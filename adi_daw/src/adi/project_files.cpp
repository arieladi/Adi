// SPDX-License-Identifier: AGPL-3.0-or-later
#include "adi/store.hpp"
#include "adi/gesture_navigation_detail.hpp"
#include "adi/applied_batches_detail.hpp"
#include "adi/ops.hpp"
#include "adi/media/media_ops.hpp"
#include "adi/media/publish_file.hpp"
#include "adi/media/collect_export.hpp"
#include "adi/media/blake3.hpp"
#include <SQLiteCpp/SQLiteCpp.h>
#include <SQLiteCpp/Backup.h>
#include <sqlite3.h>
#include <atomic>
#include <chrono>
#include <stdexcept>
#include <limits>
#include <map>
#include <functional>

namespace adi {
namespace {
namespace fs=std::filesystem;
struct Stage {
    fs::path path;
    explicit Stage(const fs::path& parent) {
        static std::atomic<unsigned long long> serial{0};
        const auto stamp=std::chrono::steady_clock::now().time_since_epoch().count();
        for(unsigned attempt=0;attempt<128;++attempt) {
            const auto candidate=parent/(".adi-save-"+std::to_string(stamp)+"-"+std::to_string(serial.fetch_add(1)));
            std::error_code ec;
            if(fs::create_directory(candidate,ec)){path=candidate;return;}
            if(ec && ec!=std::errc::file_exists)throw fs::filesystem_error("create save stage",candidate,ec);
        }
        throw std::runtime_error("cannot reserve a save stage");
    }
    ~Stage(){std::error_code ec;fs::remove_all(path,ec);}
};
// Relocation changes path representation, not the media's hash/identity. Apply
// the same transform to captured import/relink payloads on every history branch.
bool rebasePaths(Payload& paths,const fs::path& oldFolder,const fs::path& newFolder,
                 const std::string& original) {
    if(oldFolder==newFolder)return false;
    auto before=paths;
    fs::path primary;
    if(!paths.at("rel_path").is_null()) {
        const auto relative=media::pathFromUtf8(paths.at("rel_path").get<std::string>());
        if(relative.is_absolute())throw std::runtime_error("media rel_path is absolute");
        primary=(oldFolder/relative).lexically_normal();
    }
    // Keep the old implicit audio/ fallback when it is the first existing
    // candidate. No audio hashing/decoding is performed by Save As or Copy.
    std::error_code ec;
    if(primary.empty() || !fs::exists(primary,ec)) {
        if(!original.empty()) {
            const auto fallback=oldFolder/"audio"/media::pathFromUtf8(original).filename();
            if(fs::exists(fallback,ec))primary=fallback;
        }
        if(primary.empty() && !paths.at("abs_path_hint").is_null())
            primary=media::pathFromUtf8(paths.at("abs_path_hint").get<std::string>());
        if(primary.empty() && !original.empty())primary=oldFolder/"audio"/media::pathFromUtf8(original).filename();
    }
    if(!primary.empty()) {
        if(!primary.is_absolute())throw std::runtime_error("media path hint is not absolute");
        const auto relative=primary.lexically_relative(newFolder);
        if(!relative.empty() && !relative.is_absolute())paths["rel_path"]=media::pathUtf8(relative);
        else {paths["rel_path"]=nullptr;paths["abs_path_hint"]=media::pathUtf8(primary);}
    }
    return paths!=before;
}
using PathTransform=std::function<bool(Payload&,const std::string&,const std::string&)>;
// Visit both sides of every captured media op, even on inactive branches.
// Callers own the SQL transaction when writing; a read-only pass gathers files.
void visitPaths(SQLite::Database& db,const PathTransform& transform,bool write) {
    {
        SQLite::Statement rows(db,"SELECT id,rel_path,abs_path_hint,orig_name,hash_blake3,missing FROM media_files WHERE embedded=0");
        SQLite::Statement update(db,"UPDATE media_files SET rel_path=?,abs_path_hint=?,missing=? WHERE id=?");
        while(rows.executeStep()) {
            Payload paths={{"rel_path",rows.getColumn(1).isNull()?Payload(nullptr):Payload(rows.getColumn(1).getString())},
                {"abs_path_hint",rows.getColumn(2).isNull()?Payload(nullptr):Payload(rows.getColumn(2).getString())},
                {"missing",rows.getColumn(5).getInt64()}};
            if(!transform(paths,rows.getColumn(3).getString(),rows.getColumn(4).getString()) || !write)continue;
            update.reset();
            if(paths["rel_path"].is_null())update.bind(1);else update.bind(1,paths["rel_path"].get<std::string>());
            if(paths["abs_path_hint"].is_null())update.bind(2);else update.bind(2,paths["abs_path_hint"].get<std::string>());
            update.bind(3,paths["missing"].get<std::int64_t>());
            update.bind(4,rows.getColumn(0).getInt64());update.exec();
        }
    }
    SQLite::Statement rows(db,"SELECT seq,payload,inverse FROM ops WHERE op_type IN ('media.import','media.unlink','media.relink')");
    while(rows.executeStep()) {
        for(int column=1;column<=2;++column) {
            const auto bytes=rows.getColumn(column);
            if(bytes.isNull())continue;
            std::string error;
            auto captured=decodeFromLog({static_cast<const std::byte*>(bytes.getBlob()),static_cast<std::size_t>(bytes.getBytes())},error);
            if(!captured)throw std::runtime_error("cannot relocate captured media history: "+error);
            bool changed=false;
            if(captured->is_object() && captured->contains("row")) {
                auto& row=captured->at("row");
                changed=transform(row,row.at("orig_name").get<std::string>(),row.at("hash_blake3").get<std::string>());
            }else if(captured->is_object() && captured->contains("paths"))
                changed=transform(captured->at("paths"),{},captured->at("hash").get<std::string>());
            if(!changed || !write)continue;
            const auto encoded=encodePayload(*captured);
            SQLite::Statement update(db,column==1?"UPDATE ops SET payload=? WHERE seq=?":"UPDATE ops SET inverse=? WHERE seq=?");
            if(encoded.size()>static_cast<std::size_t>(std::numeric_limits<int>::max()))throw std::runtime_error("captured media history too large");
            update.bind(1,encoded.data(),static_cast<int>(encoded.size()));update.bind(2,rows.getColumn(0).getInt64());update.exec();
        }
    }
}
void rebaseHistory(SQLite::Database& db,const fs::path& oldFolder,const fs::path& newFolder) {
    if(oldFolder==newFolder)return;
    SQLite::Transaction transaction(db);
    visitPaths(db,[&](Payload& paths,const std::string& original,const std::string&){
        return rebasePaths(paths,oldFolder,newFolder,original);
    },true);
    transaction.commit();
}
struct CollectedFiles {
    std::vector<fs::path> owned;
    bool committed=false;
    ~CollectedFiles(){if(!committed)for(const auto& file:owned){std::error_code ec;fs::remove(file,ec);}}
};
std::string collectionName(const std::string& hash,const fs::path& source) {
    auto extension=media::pathUtf8(source.extension());
    if(extension.empty() || extension.size()>16 || extension.find_first_not_of(".abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789")!=std::string::npos)
        extension=".bin";
    return hash+extension;
}
}
StoreFileResult Store::saveCopy(const std::filesystem::path& output) const {
    try {
        if(closed_ || !db_)return {false,"project is closed",{}};
        if(!sqlite3_get_autocommit(db_->getHandle()))return {false,"finish the current transaction before saving a copy",{}};
        const auto target=fs::absolute(output).lexically_normal();
        if(!fs::is_directory(target.parent_path()))return {false,"output directory does not exist",{}};
        if(fs::exists(target) || fs::is_symlink(fs::symlink_status(target)))return {false,"output already exists",{}};
        Stage stage(target.parent_path());const auto staged=stage.path/"project.adi";
        {
            SQLite::Database destination(media::pathUtf8(staged),SQLite::OPEN_READWRITE|SQLite::OPEN_CREATE);
            {
                SQLite::Backup backup(destination,*db_);
                if(backup.executeStep()!=SQLITE_DONE)throw std::runtime_error("database snapshot busy or incomplete");
            }
            // Rebase between the folders as SQLite names them, the bases that
            // resolveMedia() joins rel_path onto (the stage sits inside the
            // copy's folder). On unix SQLite resolves symlinks (macOS: /var is
            // /private/var) and ".." climbs physical parents, so a path made
            // relative to fs::absolute()'s spelling climbs the wrong distance.
            if(major_==kSchemaMajor)rebaseHistory(destination,media::projectFolder(*this),media::projectFolder(destination).parent_path());
            destination.exec("PRAGMA wal_checkpoint(TRUNCATE)");
            destination.exec("PRAGMA journal_mode=DELETE");
        }
        const auto result=media::publishFile(staged,target);
        if(result.status!=media::PublishStatus::published)return {false,"publish project: "+result.error,{}};
        return {true,{},major_==kSchemaMajor?std::string{}:std::string("newer schema copied verbatim; media paths were not relocated")};
    }catch(const std::exception& e){return {false,e.what(),{}};}
}
StoreFileResult Store::saveAs(const std::filesystem::path& output) {
    if(detail::appliedDispatchActive(*this))return {false,"reentrant Save As during applied-batch delivery",{}};
    if(closed_ || !db_)return {false,"project is closed",{}};
    auto change=gesture::detail::beginSourceChange(*this);
    if(!change.status.ok())return {false,change.status.message,{}};
    if(major_!=kSchemaMajor)return {false,"use Save a Copy for an unsupported schema major",{}};
    if(sqlite3_next_stmt(db_->getHandle(),nullptr))return {false,"finish database statements before Save As",{}};
    const auto copied=saveCopy(output);
    if(!copied.ok)return copied;
    StoreError error{};
    auto replacement=Store::open(fs::absolute(output),error);
    if(!replacement || error!=StoreError::Ok || replacement->readOnly())
        return {false,"copy saved but could not switch projects: "+std::string(toString(error)),{}};
    try {
        detail::prepareAppliedReplacement(*replacement,*this);
    } catch(const std::exception& exception) {
        return {false,"copy saved but could not switch projects: "+std::string(exception.what()),{}};
    }
    const auto finished=gesture::detail::finishSourceChange(change.permit);
    if(!finished.ok())return {false,finished.message,"copy saved; original project remains active"};
    gesture::detail::invalidateSource(*this);
    detail::invalidateAppliedDomain(*this,AppliedSourceInvalidation::Replaced);
    db_.swap(replacement->db_);path_.swap(replacement->path_);
    appliedDomain_.swap(replacement->appliedDomain_);
    gestureDomain_.swap(replacement->gestureDomain_);
    detail::rebindAppliedDomain(*this);
    std::swap(readOnly_,replacement->readOnly_);std::swap(major_,replacement->major_);std::swap(minor_,replacement->minor_);
    upgradedFrom_.swap(replacement->upgradedFrom_);lastError_.clear();
    const bool folded=replacement->closeFinalized();
    return {true,{},folded?std::string{}:std::string("old project is still in use; its WAL could not be folded")};
}
}

namespace adi {
StoreFileResult Store::collectAllAndSave() {
    if(detail::appliedDispatchActive(*this))return {false,"reentrant collection during applied-batch delivery",{}};
    try {
        if(closed_ || !db_)return {false,"project is closed",{}};
        if(readOnly_ || major_!=kSchemaMajor)return {false,"project is read-only or unsupported",{}};
        if(!sqlite3_get_autocommit(db_->getHandle()) || sqlite3_next_stmt(db_->getHandle(),nullptr))
            return {false,"finish database transactions and statements before collecting",{}};
        if(db_->execAndGet("SELECT count(*) FROM media_files WHERE embedded<>0").getInt64()!=0 ||
           db_->execAndGet("SELECT count(*) FROM media_blobs").getInt64()!=0)
            return {false,"extract legacy embedded media before collecting",{}};
        // Hold a write transaction across resolution/copying so a second writer
        // cannot append media between the gather and location update passes.
        db_->exec("BEGIN IMMEDIATE");
        struct Transaction {
            SQLite::Database& db;bool committed=false;
            ~Transaction(){if(!committed)try{db.exec("ROLLBACK");}catch(...) {}}
        } transaction{*db_};
        std::map<std::string,std::vector<Payload>> candidates;
        visitPaths(*db_,[&](Payload& paths,const std::string& original,const std::string& hash){
            if(hash.size()!=64 || hash.find_first_not_of("0123456789abcdef")!=std::string::npos)
                throw std::runtime_error("invalid media hash in pool or history");
            auto row=paths;row["orig_name"]=original;row["hash_blake3"]=hash;
            candidates[hash].push_back(std::move(row));return false;
        },false);
        const auto folder=media::projectFolder(*this),audio=folder/"audio";
        std::map<std::string,std::string> locations;
        CollectedFiles files;
        if(!candidates.empty()) {
            if(fs::is_symlink(fs::symlink_status(audio)))throw std::runtime_error("audio/ must not be a symlink");
            fs::create_directories(audio);
            Stage stage(audio);
            for(const auto& [hash,rows]:candidates) {
                fs::path source;std::string error;
                for(const auto& row:rows) {
                    source=media::resolveMedia(folder,row,error);
                    if(!source.empty())break;
                }
                if(source.empty())throw std::runtime_error(error);
                const auto target=audio/media::pathFromUtf8(collectionName(hash,source));
                auto verify=[&](const fs::path& file){
                    const auto actual=media::blake3File(file);
                    if(!actual || actual.hex!=hash)throw std::runtime_error("hash mismatch or unreadable media: "+media::pathUtf8(file));
                };
                if(fs::is_symlink(fs::symlink_status(target)))throw std::runtime_error("collected target must not be a symlink");
                if(fs::exists(target))verify(target);
                else {
                    const auto staged=stage.path/"media.tmp";
                    fs::copy_file(source,staged);verify(staged);
                    files.owned.push_back(target);
                    const auto result=media::publishFile(staged,target);
                    if(result.status!=media::PublishStatus::published) {
                        files.owned.pop_back();
                        if(result.status!=media::PublishStatus::exists)throw std::runtime_error("publish media: "+result.error);
                        verify(target);fs::remove(staged);
                    }
                }
                locations[hash]=media::pathUtf8(target.lexically_relative(folder));
            }
        }
        visitPaths(*db_,[&](Payload& paths,const std::string&,const std::string& hash){
            const auto before=paths;
            paths["rel_path"]=locations.at(hash);paths["abs_path_hint"]=nullptr;paths["missing"]=0;
            if(paths.contains("embedded"))paths["embedded"]=0;
            return paths!=before;
        },true);
        db_->exec("COMMIT");transaction.committed=true;files.committed=true;
        const auto status=sqlite3_wal_checkpoint_v2(db_->getHandle(),nullptr,SQLITE_CHECKPOINT_PASSIVE,nullptr,nullptr);
        return {true,{},status==SQLITE_OK?std::string{}:std::string("collection saved; WAL checkpoint is pending")};
    }catch(const std::exception& e){return {false,e.what(),{}};}
}
StoreFileResult Store::collectAndExport(const std::filesystem::path& output) const {
    if(closed_ || !db_)return {false,"project is closed",{}};
    if(!sqlite3_get_autocommit(db_->getHandle()))return {false,"finish the current transaction before exporting",{}};
    const auto result=media::collectExport(media::projectFolder(*this)/path_.filename(),output);
    return {result.ok,result.error,{}};
}
}
