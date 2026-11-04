// SPDX-License-Identifier: AGPL-3.0-or-later
#include "adi/store.hpp"
#include "adi/ops.hpp"
#include "adi/media/media_ops.hpp"
#include "adi/media/publish_file.hpp"
#include <SQLiteCpp/SQLiteCpp.h>
#include <SQLiteCpp/Backup.h>
#include <sqlite3.h>
#include <atomic>
#include <chrono>
#include <stdexcept>
#include <limits>

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
bool rebaseOp(Payload& payload,const fs::path& oldFolder,const fs::path& newFolder) {
    if(!payload.is_object())return false;
    if(payload.contains("row") && payload["row"].is_object() && payload["row"].contains("hash_blake3")) {
        auto& row=payload["row"];
        return rebasePaths(row,oldFolder,newFolder,row.at("orig_name").get<std::string>());
    }
    if(payload.contains("paths") && payload.contains("hash"))
        return rebasePaths(payload["paths"],oldFolder,newFolder,{});
    return false;
}
void rebaseHistory(SQLite::Database& db,const fs::path& oldFolder,const fs::path& newFolder) {
    if(oldFolder==newFolder)return;
    SQLite::Transaction transaction(db);
    {
        SQLite::Statement rows(db,"SELECT id,rel_path,abs_path_hint,orig_name FROM media_files WHERE embedded=0");
        SQLite::Statement update(db,"UPDATE media_files SET rel_path=?,abs_path_hint=? WHERE id=?");
        while(rows.executeStep()) {
            Payload paths={{"rel_path",rows.getColumn(1).isNull()?Payload(nullptr):Payload(rows.getColumn(1).getString())},
                {"abs_path_hint",rows.getColumn(2).isNull()?Payload(nullptr):Payload(rows.getColumn(2).getString())}};
            if(!rebasePaths(paths,oldFolder,newFolder,rows.getColumn(3).getString()))continue;
            update.reset();
            if(paths["rel_path"].is_null())update.bind(1);else update.bind(1,paths["rel_path"].get<std::string>());
            if(paths["abs_path_hint"].is_null())update.bind(2);else update.bind(2,paths["abs_path_hint"].get<std::string>());
            update.bind(3,rows.getColumn(0).getInt64());update.exec();
        }
    }
    SQLite::Statement rows(db,"SELECT seq,op_type,payload,inverse FROM ops WHERE op_type IN ('media.import','media.unlink','media.relink')");
    while(rows.executeStep()) {
        for(int column=2;column<=3;++column) {
            const auto bytes=rows.getColumn(column);
            if(bytes.isNull())continue;
            std::string error;
            auto captured=decodeFromLog({static_cast<const std::byte*>(bytes.getBlob()),static_cast<std::size_t>(bytes.getBytes())},error);
            if(!captured)throw std::runtime_error("cannot relocate captured media history: "+error);
            if(!rebaseOp(*captured,oldFolder,newFolder))continue;
            const auto encoded=encodePayload(*captured);
            SQLite::Statement update(db,column==2?"UPDATE ops SET payload=? WHERE seq=?":"UPDATE ops SET inverse=? WHERE seq=?");
            if(encoded.size()>static_cast<std::size_t>(std::numeric_limits<int>::max()))throw std::runtime_error("captured media history too large");
            update.bind(1,encoded.data(),static_cast<int>(encoded.size()));update.bind(2,rows.getColumn(0).getInt64());update.exec();
        }
    }
    transaction.commit();
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
            if(major_==kSchemaMajor)rebaseHistory(destination,media::projectFolder(*this),target.parent_path());
            destination.exec("PRAGMA wal_checkpoint(TRUNCATE)");
            destination.exec("PRAGMA journal_mode=DELETE");
        }
        const auto result=media::publishFile(staged,target);
        if(result.status!=media::PublishStatus::published)return {false,"publish project: "+result.error,{}};
        return {true,{},major_==kSchemaMajor?std::string{}:std::string("newer schema copied verbatim; media paths were not relocated")};
    }catch(const std::exception& e){return {false,e.what(),{}};}
}
StoreFileResult Store::saveAs(const std::filesystem::path& output) {
    if(closed_ || !db_)return {false,"project is closed",{}};
    if(major_!=kSchemaMajor)return {false,"use Save a Copy for an unsupported schema major",{}};
    if(sqlite3_next_stmt(db_->getHandle(),nullptr))return {false,"finish database statements before Save As",{}};
    const auto copied=saveCopy(output);
    if(!copied.ok)return copied;
    StoreError error{};
    auto replacement=Store::open(fs::absolute(output),error);
    if(!replacement || error!=StoreError::Ok || replacement->readOnly())
        return {false,"copy saved but could not switch projects: "+std::string(toString(error)),{}};
    db_.swap(replacement->db_);path_.swap(replacement->path_);
    std::swap(readOnly_,replacement->readOnly_);std::swap(major_,replacement->major_);std::swap(minor_,replacement->minor_);
    upgradedFrom_.swap(replacement->upgradedFrom_);lastError_.clear();
    const bool folded=replacement->close();
    return {true,{},folded?std::string{}:std::string("old project is still in use; its WAL could not be folded")};
}
}
