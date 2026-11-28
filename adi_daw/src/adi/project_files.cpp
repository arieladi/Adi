// SPDX-License-Identifier: AGPL-3.0-or-later
#include "adi/store.hpp"
#include "adi/project_backup.hpp"
#include "adi/audio/io_audit.hpp"
#include "adi/gesture_navigation_detail.hpp"
#include "adi/gesture_capture_detail.hpp"
#include "adi/applied_batches_detail.hpp"
#include "adi/ops.hpp"
#include "adi/history_content.hpp"
#include "adi/history_roots.hpp"
#include "adi/media/media_ops.hpp"
#include "adi/media/publish_file.hpp"
#include "adi/media/published_rollback.hpp"
#include "adi/media/collect_export.hpp"
#include "adi/media/blake3.hpp"
#include <SQLiteCpp/SQLiteCpp.h>
#include <SQLiteCpp/Backup.h>
#include <sqlite3.h>
#include <utf8proc.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <charconv>
#include <cstdio>
#include <cstdlib>
#include <stdexcept>
#include <limits>
#include <map>
#include <functional>
#include <set>
#include <thread>
#include <tuple>
#include <vector>

namespace adi {
namespace {
namespace fs=std::filesystem;
struct SaveCancelled {};

void checkSaveCancellation(std::stop_token stop) {
    if (stop.stop_requested()) throw SaveCancelled{};
}

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
// A first save keeps paths within the project, but does not collect external
// samples. With an older, flat staging layout, audio/ may belong to many songs.
bool localPath(const fs::path& relative) {
    return !relative.empty() && !relative.is_absolute() && *relative.begin()!=".." && relative!=".";
}
std::string collectionNameKey(const fs::path& filename);
struct UntitledMedia {
    struct Move {
        std::optional<media::FileIdentity> identity;
        fs::path destination;
    };
    struct Publication {
        fs::path path;
        std::optional<media::FileIdentity> identity;
    };
    fs::path staged, canonicalSource, targetFolder;
    bool privateFolder;
    bool replacing;
    std::map<fs::path,Move> sources;
    std::set<std::string> destinations;
    std::vector<fs::path> createdDirectories;
    std::vector<Publication> published;
    bool keepPublished=false;
    bool durable=true;

    UntitledMedia(fs::path stageFolder,fs::path source,fs::path target,bool ownsFolder,bool replace)
        :staged(std::move(stageFolder)),canonicalSource(std::move(source)),targetFolder(std::move(target)),
         privateFolder(ownsFolder),replacing(replace){}

    void makeDirectory(const fs::path& directory) {
        if(directory==targetFolder)return;
        makeDirectory(directory.parent_path());
        if(fs::create_directory(directory))createdDirectories.push_back(directory);
        else if(!fs::is_directory(fs::symlink_status(directory)))
            throw std::runtime_error("project media folder is not an ordinary directory");
    }
    Move& reserveDestination(const fs::path& relative) {
        if(const auto found=sources.find(relative);found!=sources.end())return found->second;
        auto candidate=relative;
        if(replacing){
            const auto exists=[](const fs::path& path){return fs::symlink_status(path).type()!=fs::file_type::not_found;};
            auto key=collectionNameKey(candidate);
            // A reservation may be for missing media with no staged file yet.
            for(unsigned suffix=2;destinations.contains(key) || exists(targetFolder/candidate) || exists(staged/candidate);++suffix){
                auto name=relative.stem();
                name+=fs::path(" ("+std::to_string(suffix)+")");
                name+=relative.extension();
                candidate=relative.parent_path()/name;
                key=collectionNameKey(candidate);
            }
            destinations.insert(std::move(key));
        }
        return sources.emplace(relative,Move{std::nullopt,std::move(candidate)}).first->second;
    }
    void stage(const fs::path& source,Move& move) {
        if(move.identity)return;
        std::error_code identityError;
        const auto identity=media::readFileIdentity(source,identityError);
        if(!identity)throw fs::filesystem_error("identify project media",source,identityError);
        const auto copy=staged/move.destination;
        fs::create_directories(copy.parent_path());
        fs::copy_file(source,copy);
        const auto originalHash=media::blake3File(source),copiedHash=media::blake3File(copy);
        if(!originalHash || !copiedHash || originalHash.hex!=copiedHash.hex ||
           media::readFileIdentity(source,identityError)!=identity)
            throw std::runtime_error("project media changed during first save: "+media::pathUtf8(source));
        move.identity=identity;
    }
    bool relocate(Payload& paths,const fs::path& oldFolder,const fs::path& primary) {
        const auto relative=primary.lexically_relative(oldFolder);
        if(!localPath(relative))return false;
        if(!privateFolder && *relative.begin()!="audio" && *relative.begin()!="Samples")return false;
        // Compare with the physical source folder so a Windows short or differently cased
        // parent name is not mistaken for a file or directory linked outside the project.
        const auto resolved=fs::weakly_canonical(primary);
        if(resolved!=(canonicalSource/relative).lexically_normal())return false;
        const auto status=fs::symlink_status(primary);
        if(!fs::is_regular_file(status) && status.type()!=fs::file_type::not_found)
            throw std::runtime_error("project media is not an ordinary file: "+media::pathUtf8(primary));
        auto& move=reserveDestination(relative);
        if(fs::is_regular_file(status))stage(primary,move);
        paths["rel_path"]=media::pathUtf8(move.destination);
        paths["abs_path_hint"]=nullptr;
        return true;
    }
    void publish() {
        for(const auto& [relative,move]:sources) {
            (void)relative;
            if(!move.identity)continue;
            const auto destination=targetFolder/move.destination;
            makeDirectory(destination.parent_path());
            published.push_back({destination,{}});
            const auto result=media::publishFile(staged/move.destination,destination);
            if(result.status!=media::PublishStatus::published){
                published.pop_back();
                throw std::runtime_error("publish project media: "+result.error);
            }
            published.back().identity=result.identity;
            durable=durable && result.durabilityConfirmed;
        }
        for (const auto &directory : createdDirectories)
            if (media::syncPublishedDirectory(directory.parent_path()))
                durable = false;
    }
    ~UntitledMedia(){
        // This save owns the paths it published and the directories it
        // created. Exclude a file that has been replaced in the meantime.
        if(keepPublished)return;
        for(const auto& file:published)
            if(file.identity)(void)media::removePublishedFile(file.path,*file.identity);
        for(auto at=createdDirectories.rbegin();at!=createdDirectories.rend();++at){
            std::error_code ec;
            fs::remove(*at,ec);
        }
    }
};
// Relocation changes path representation, not the media's hash/identity. Apply
// the same transform to captured import/relink payloads on every history branch.
bool rebasePaths(Payload& paths,const fs::path& oldFolder,const fs::path& newFolder,
                 const std::string& original,UntitledMedia* untitled=nullptr) {
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
        if(!paths.at("abs_path_hint").is_null()) {
            const auto hint=media::pathFromUtf8(paths.at("abs_path_hint").get<std::string>());
            if(primary.empty() || (!fs::exists(primary,ec) && fs::exists(hint,ec)))primary=hint;
        }
        if(primary.empty() && !original.empty())primary=oldFolder/"audio"/media::pathFromUtf8(original).filename();
    }
    if(!primary.empty()) {
        if(!primary.is_absolute())throw std::runtime_error("media path hint is not absolute");
        if(untitled && untitled->relocate(paths,oldFolder,primary))return paths!=before;
#ifdef _WIN32
        // On Windows one folder has several spellings: a short (8.3) name, another case. primary is built
        // from the old folder as it was given and newFolder comes from the staged copy, and the two may
        // spell a shared parent differently, as under a runner's temporary folder. Compared as text they
        // then share no parent and the stored path climbs out through the alias. Compare physical paths.
        std::error_code aliasError,folderError;
        const auto physical=fs::weakly_canonical(primary,aliasError);
        const auto physicalFolder=fs::weakly_canonical(newFolder,folderError);
        const auto relative=(aliasError?primary:physical).lexically_relative(folderError?newFolder:physicalFolder);
#else
        const auto relative=primary.lexically_relative(newFolder);
#endif
        if(!relative.empty() && !relative.is_absolute()) {
            paths["rel_path"]=media::pathUtf8(relative);
            if(*relative.begin()==".." && paths.at("abs_path_hint").is_null())
                paths["abs_path_hint"]=media::pathUtf8(primary);
        }
        else {paths["rel_path"]=nullptr;paths["abs_path_hint"]=media::pathUtf8(primary);}
    }
    return paths!=before;
}
using PathTransform=std::function<bool(Payload&,const std::string&,const std::string&)>;
// Visit both sides of every captured media op, even on inactive branches.
// Callers own the SQL transaction when writing; a read-only pass gathers files.
void visitPaths(SQLite::Database& db,const PathTransform& transform,bool write,std::stop_token stop = {}) {
    {
        SQLite::Statement rows(db,"SELECT id,rel_path,abs_path_hint,orig_name,hash_blake3,missing FROM media_files WHERE embedded=0");
        SQLite::Statement update(db,"UPDATE media_files SET rel_path=?,abs_path_hint=?,missing=? WHERE id=?");
        while(rows.executeStep()) {
            checkSaveCancellation(stop);
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
    history_detail::visitCheckpointState(db,[&](Payload& state){
        checkSaveCancellation(stop);
        bool changed=false;
        for(auto& table:state.at("tables")){
            if(table.at("name")!="media_files")continue;
            const auto names=table.at("columns").get<std::vector<std::string>>();
            std::map<std::string,std::size_t> columns;for(std::size_t i=0;i<names.size();++i)columns.emplace(names[i],i);
            for(auto& row:table.at("rows")){
                checkSaveCancellation(stop);
                const auto value=[&](const char* name)->Payload&{return row.at(columns.at(name)).at(1);};
                if(value("embedded")!=0)continue;
                Payload paths={{"rel_path",value("rel_path")},{"abs_path_hint",value("abs_path_hint")},{"missing",value("missing")}};
                if(!transform(paths,value("orig_name").get<std::string>(),value("hash_blake3").get<std::string>()))continue;
                changed=true;
                if(write)for(const auto* field:{"rel_path","abs_path_hint","missing"}){
                    const auto& cell=paths.at(field);const char* type=cell.is_null()?"null":cell.is_string()?"text":"int";
                    row.at(columns.at(field))=Payload::array({type,cell});
                }
            }
        }
        return changed;
    },write);
    SQLite::Statement rows(db,"SELECT seq,payload,inverse FROM ops WHERE op_type IN ('media.import','media.unlink','media.relink')");
    while(rows.executeStep()) {
        checkSaveCancellation(stop);
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
void rebaseHistory(SQLite::Database& db,const fs::path& oldFolder,const fs::path& newFolder,
                   UntitledMedia* untitled, std::stop_token stop) {
    if(oldFolder==newFolder)return;
    SQLite::Transaction transaction(db);
    visitPaths(db,[&](Payload& paths,const std::string& original,const std::string&){
        return rebasePaths(paths,oldFolder,newFolder,original,untitled);
    },true,stop);
    transaction.commit();
}
// Only the private stage is writable here. A shared project is a new identity
// so it cannot pick up this computer's existing project/server authorization.
Payload prepareSharedCopy(SQLite::Database& db, StoreCopyPolicy policy, std::stop_token stop) {
    const auto version = db.execAndGet("PRAGMA user_version").getInt();
    if (version / 1000 != kSchemaMajor || version > kUserVersion)
        throw std::runtime_error("this project format is too new to share safely");
    if (version < kUserVersion) {
        std::string error;
        if (!migrateToCurrent(db, version % 1000, error))
            throw std::runtime_error("cannot upgrade the shared copy: " + error);
    }
    db.exec("PRAGMA foreign_keys=ON; PRAGMA secure_delete=ON");
    SQLite::Transaction transaction(db);
    db.exec("INSERT INTO adi_meta(key,value) VALUES('project_uuid',lower(hex(randomblob(16)))) "
            "ON CONFLICT(key) DO UPDATE SET value=excluded.value;"
            "DELETE FROM session_lock;"
            "DELETE FROM adi_meta WHERE key IN ('history.undo.collaboration','history.undo.uploaded_through');"
            "UPDATE remote_hosts SET enabled=0 WHERE enabled<>0;");
    Payload reportedLatency;
    if (policy == StoreCopyPolicy::share) {
        // A measurement tied to the exact captured state still describes the
        // shared project. The fresh Undo baseline gives that state a new head.
        {
            SQLite::Statement measured(db,
                "SELECT s.value,b.head_seq FROM session_state s,op_branches b "
                "WHERE s.key='devices.reported_latency' AND b.is_current=1");
            if (measured.executeStep()) {
                auto report = Payload::parse(measured.getColumn(0).getString(), nullptr, false);
                const auto head = measured.getColumn(1);
                const auto sourceHead = head.isNull() ? Payload(nullptr) : Payload(head.getInt64());
                if (report.contains("version") && report["version"] == 1 &&
                    report.contains("history_head") && report["history_head"] == sourceHead)
                    reportedLatency = std::move(report);
            }
        }
        db.exec("DELETE FROM session_state WHERE key='devices.reported_latency';"
                "DELETE FROM adi_meta WHERE key GLOB 'history.daily.*';"
                "DELETE FROM history_origin_root; DELETE FROM history_roots;"
                "DELETE FROM history_checkpoint_content; DELETE FROM history_checkpoint;"
                "DELETE FROM history_state_manifests; DELETE FROM history_state_rows;"
                "DELETE FROM history_snapshots; DELETE FROM op_branches;"
                "DELETE FROM ops; DELETE FROM op_clients;"
                "DELETE FROM sqlite_sequence WHERE name='ops';"
                "INSERT INTO op_branches VALUES(1,'main',NULL,0,1,'system');"
                "DELETE FROM state_blobs WHERE NOT EXISTS "
                "(SELECT 1 FROM plugin_state WHERE state_hash=state_blobs.hash_blake3);");
    } else {
        // An agent's supplied operation label can repeat the request. Keep the
        // operation and semantic tags, and give the entry a generic Undo label.
        db.exec("UPDATE ops SET label='Agent edit', actor_detail='' WHERE actor='agent' "
                "OR txn_id IN(SELECT txn_id FROM agent_requests);");
        history_detail::visitCheckpointState(db, [stop](Payload& state) {
            checkSaveCancellation(stop);
            for (auto& table : state.at("tables")) {
                if (table.at("name") != "remote_hosts") continue;
                const auto& columns = table.at("columns");
                const auto enabled = std::find(columns.begin(), columns.end(), "enabled");
                if (enabled == columns.end()) break;
                const auto index = static_cast<std::size_t>(std::distance(columns.begin(), enabled));
                bool edited = false;
                for (auto& row : table.at("rows")) {
                    checkSaveCancellation(stop);
                    auto& value = row.at(index).at(1);
                    if (value == 0) continue;
                    value = 0;
                    edited = true;
                }
                return edited;
            }
            return false;
        }, true);
        history_detail::pruneCheckpointContent(db, [stop] { checkSaveCancellation(stop); });
    }
    db.exec("DELETE FROM agent_requests");
    transaction.commit();
    return reportedLatency;
}
std::error_code projectDestinationReady(const fs::path& target) {
    // A closed backup must never inherit another database's WAL or rollback
    // journal. Do not checkpoint/delete someone else's recovery material.
    for(const auto* suffix:{"-wal","-shm","-journal"}){
        auto sidecar=target;sidecar+=suffix;std::error_code error;
        const auto status=fs::symlink_status(sidecar,error);
        if(error==std::errc::no_such_file_or_directory)continue;
        if(error)return error;
        if(status.type()!=fs::file_type::not_found)return std::make_error_code(std::errc::device_or_resource_busy);
    }
    return {};
}
bool validCollision(StoreFileCollision collision) noexcept {
    return collision==StoreFileCollision::Fail || collision==StoreFileCollision::ReplaceConfirmed;
}
fs::path saveDestination(const fs::path& output, const std::shared_ptr<media::ProjectFileLease>& source,
                         StoreFileCollision collision) {
    if (!validCollision(collision)) throw std::runtime_error("unknown save collision mode");
    const auto target = fs::absolute(output).lexically_normal();
    if (!fs::is_directory(target.parent_path())) throw std::runtime_error("output directory does not exist");
    if (source) {
        std::error_code error;
        if (fs::equivalent(source->path(), target, error) || fs::weakly_canonical(source->path()) == fs::weakly_canonical(target))
            throw std::runtime_error("destination is the active project");
    }
    const auto status = fs::symlink_status(target);
    if (collision == StoreFileCollision::Fail && status.type() != fs::file_type::not_found)
        throw std::runtime_error("output already exists");
    if (status.type() != fs::file_type::not_found && !fs::is_regular_file(status))
        throw std::runtime_error("destination is not a regular file");
    if (projectDestinationReady(target)) throw std::runtime_error("destination is open or has unrecovered SQLite sidecars");
    return target;
}

bool pathHasIdentity(const fs::path& path, const media::FileIdentity& expected) noexcept {
    std::error_code error;
    return media::readFileIdentity(path, error) == expected;
}
std::string joinedWarning(std::string first,const std::string& next) {
    if(next.empty())return first;
    if(!first.empty())first+="; ";
    first+=next;return first;
}
std::string collectionNameKey(const fs::path& filename) {
    const auto name = media::pathUtf8(filename);
    // Reserve aliases too, so a collected ZIP can be unpacked on a platform
    // that folds case or normalizes filenames. Keep the original spelling.
    utf8proc_uint8_t* data = nullptr;
    const auto flags = static_cast<utf8proc_option_t>(UTF8PROC_STABLE | UTF8PROC_COMPOSE | UTF8PROC_CASEFOLD);
    const auto size = utf8proc_map(reinterpret_cast<const utf8proc_uint8_t*>(name.data()),
                                  static_cast<utf8proc_ssize_t>(name.size()), &data, flags);
    const std::unique_ptr<utf8proc_uint8_t, decltype(&std::free)> owned(data, &std::free);
    return size < 0 ? name : std::string(reinterpret_cast<const char*>(data), static_cast<std::size_t>(size));
}
using CollectionNames = std::map<std::string, fs::path>;
fs::path publishCollectedMedia(const fs::path& directory, const fs::path& stage, const fs::path& source,
                               const std::string& original, const std::string& hash,
                               CollectionNames& names, media::detail::PublishedRollback& owned) {
    auto fallbackExtension = media::pathUtf8(source.extension());
    if (fallbackExtension.empty() || fallbackExtension.size() > 16 ||
        fallbackExtension.find_first_not_of(".abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789") != std::string::npos)
        fallbackExtension = ".bin";
    const auto name = media::detail::portableFilename(original, "Sample-" + hash.substr(0, 8) + fallbackExtension);
    const auto filename = media::pathFromUtf8(name);
    const auto stem = media::pathUtf8(filename.stem());
    const auto extension = media::pathUtf8(filename.extension());
    const auto matches = [&](const fs::path& target) {
        if (!fs::is_regular_file(fs::symlink_status(target))) return false;
        const auto actual = media::blake3File(target);
        return actual && actual.hex == hash;
    };
    const auto staged = stage / hash;
    for (unsigned number = 1; number <= 10000; ++number) {
        const auto leaf = number == 1 ? filename
            : media::pathFromUtf8(stem + " (" + std::to_string(number) + ")" + extension);
        const auto key = collectionNameKey(leaf);
        if (const auto occupied = names.find(key); occupied != names.end()) {
            if (!occupied->second.empty() && matches(occupied->second))
                return occupied->second;
            continue;
        }
        const auto target = directory / leaf;
        if (fs::symlink_status(target).type() != fs::file_type::not_found) {
            names.emplace(key, target);
            if (matches(target)) return target;
            continue;
        }
        if (!fs::exists(staged)) {
            fs::copy_file(source, staged);
            if (!matches(staged)) throw std::runtime_error("hash mismatch or unreadable media: " + media::pathUtf8(source));
        }
        owned.files.push_back({target, {}});
        const auto result = media::publishFile(staged, target, {}, media::PublishPolicy::VerifiedCopyFallback);
        if (result.status == media::PublishStatus::published) {
            owned.files.back().identity = result.identity;
            names.emplace(key, target);
            return target;
        }
        owned.files.pop_back();
        if (result.status != media::PublishStatus::exists) throw std::runtime_error("publish media: " + result.error);
        names.emplace(key, target);
        if (matches(target)) {
            fs::remove(staged);
            return target;
        }
    }
    throw std::runtime_error("too many filename collisions for " + original);
}
}
struct Store::CopyState : StoreFileResult {
    std::optional<media::FileIdentity> identity;
    std::map<fs::path,media::FileIdentity> originalMedia;
    bool durable=false;
    CopyState(bool value,std::string why={},std::string note={},std::optional<media::FileIdentity> file={})
        :StoreFileResult{value,std::move(why),std::move(note)},identity(file){}
};

namespace {
using ProjectRevision = std::pair<std::int64_t, std::int64_t>;
ProjectRevision readRevision(SQLite::Database &db) {
    return {db.execAndGet("SELECT COALESCE(MAX(seq),0) FROM ops").getInt64(),
            db.execAndGet("SELECT COALESCE(head_seq,0) FROM op_branches WHERE is_current=1").getInt64()};
}
}

bool discardUntouchedProject(const fs::path& path, const media::FileIdentity& expected) {
    std::error_code error;
    const auto lease = media::ProjectFileLease::openExclusive(path, error);
    if (!lease || lease->identity() != expected)
        return false;
    try {
        // A clean Store close folds its owned WAL. Any surviving sidecar may contain someone
        // else's recovery data, so keep the entire set without even opening SQLite on it.
        if (projectDestinationReady(lease->path()))
            return false;
        const auto revision = media::readFileRevision(lease->path(), error);
        if (!revision || revision->identity != expected)
            return false;
        {
            // An edit followed by Undo still leaves an op. Performance-only gestures do not count.
            SQLite::Database db(media::pathUtf8(lease->path()), SQLite::OPEN_READONLY);
            if (db.execAndGet("SELECT EXISTS(SELECT 1 FROM ops WHERE tags <> 'ephemeral') OR "
                              "EXISTS(SELECT 1 FROM history_snapshots WHERE auto = 0)").getInt() != 0)
                return false;
        }
        return lease->removeExclusive(*revision, error, [&] { return projectDestinationReady(lease->path()); });
    } catch (const SQLite::Exception&) {
        return false;
    }
}

namespace {
struct BackupImageStamp {
    std::string sourceName;
    std::int64_t utcMicros = 0;
    std::optional<unsigned> collisionIndex;
};
}

struct StoreFileSave::Data {
    StoreSaveKind kind = StoreSaveKind::copy;
    StoreFileCollision collision = StoreFileCollision::Fail;
    StoreCopyPolicy copyPolicy = StoreCopyPolicy::share;
    fs::path sourcePath, sourceFolder, target;
    std::shared_ptr<media::ProjectFileLease> sourceLease;
    media::FileIdentity sourceIdentity;
    std::optional<media::FileIdentity> expectedTarget;
    int major = 0;
    AppliedSource owner;
    std::uint64_t sequence = 0;
    ProjectRevision sourceRevision;
    gesture::detail::StoreWriteStamp stamp{};
    std::unique_ptr<SQLite::Database> reader;
    std::optional<BackupImageStamp> backup;
    std::optional<Store::CopyState> copy;
    bool recoveringBackup = false;
    std::unique_ptr<Store> retired;
    bool cancelled = false;
};

StoreFileSave::StoreFileSave() = default;
StoreFileSave::~StoreFileSave() = default;
StoreFileSave::StoreFileSave(StoreFileSave&&) noexcept = default;
StoreFileSave& StoreFileSave::operator=(StoreFileSave&&) noexcept = default;

StoreFileResult Store::prepareFileSave(const fs::path& output, StoreSaveKind kind, StoreFileSave& job,
                                       StoreFileCollision collision, const media::FileIdentity* expectedTarget,
                                       StoreCopyPolicy copyPolicy) {
    if (audio::onAudioThread) return {false, {}, {}};
    if (job.data_) return {false, "save job was already prepared", {}};
    if (closed_ || !db_) return {false, "project is closed", {}};
    try {
        if (kind != StoreSaveKind::copy && kind != StoreSaveKind::saveAs &&
            kind != StoreSaveKind::firstSave && kind != StoreSaveKind::sessionCopy)
            throw std::runtime_error("unknown save kind");
        if (kind == StoreSaveKind::sessionCopy) {
            if (major_ != kSchemaMajor) throw std::runtime_error("session copies need a supported schema major");
            if (collision != StoreFileCollision::Fail)
                throw std::runtime_error("session copies cannot replace an existing project");
        }
        const auto owner = appliedSource(*this);
        if (detail::appliedSourceStore(owner) != this || detail::appliedDispatchActive(*this))
            throw std::runtime_error("saving requires the idle project owner");
        if (!fileLease_ || !sqlite3_get_autocommit(db_->getHandle()))
            throw std::runtime_error("save needs a project file with no open transaction");
        if (kind != StoreSaveKind::copy) {
            if (major_ != kSchemaMajor) throw std::runtime_error("use a newer ADI version to save this project");
            if (sqlite3_next_stmt(db_->getHandle(), nullptr))
                throw std::runtime_error("finish database statements before Save As");
            const auto change = gesture::detail::beginSourceChange(*this);
            if (!change.status.ok()) throw std::runtime_error(change.status.message);
        }
        auto state = std::make_unique<StoreFileSave::Data>();
        state->kind = kind;
        state->collision = collision;
        state->copyPolicy = kind == StoreSaveKind::copy ? copyPolicy : StoreCopyPolicy::preserve;
        state->target = saveDestination(output, fileLease_, collision);
        if (collision == StoreFileCollision::ReplaceConfirmed) {
            if (fs::is_regular_file(fs::symlink_status(state->target))) {
                std::error_code error;
                state->expectedTarget = media::readFileIdentity(state->target, error);
                if (!state->expectedTarget) throw std::runtime_error("cannot identify save destination: " + error.message());
                if (expectedTarget && state->expectedTarget != *expectedTarget)
                    throw std::runtime_error("save destination changed after replacement confirmation");
            } else {
                if (expectedTarget) throw std::runtime_error("save destination disappeared after replacement confirmation");
                // Confirmation on an empty path never covers a later arrival.
                state->collision = StoreFileCollision::Fail;
            }
        }
        const auto* path = sqlite3_db_filename(db_->getHandle(), "main");
        if (!path || !*path) throw std::runtime_error("cannot locate the project database");
        state->sourcePath = media::pathFromUtf8(path);
        state->sourceFolder = state->sourcePath.parent_path();
        state->sourceLease = fileLease_;
        state->sourceIdentity = fileLease_->identity();
        state->major = major_;
        state->owner = owner;
        state->sequence = owner.sequence();
        const auto before = gesture::detail::storeWriteStamp(*this);
        if (!before || !pathHasIdentity(state->sourcePath, state->sourceIdentity))
            throw std::runtime_error("could not verify the open project's source");

        // Use a private, serialized handle. Only this new handle crosses to
        // the worker; the Store's writer and owner statements stay here.
        state->reader = std::make_unique<SQLite::Database>(media::pathUtf8(state->sourcePath),
            SQLite::OPEN_READONLY | SQLITE_OPEN_FULLMUTEX);
        state->reader->exec("BEGIN DEFERRED");
        {
            // BEGIN alone does not choose a WAL snapshot. Read the schema so
            // a later writer cannot move the image between backup steps.
            SQLite::Statement pin(*state->reader, "SELECT rootpage FROM main.sqlite_schema LIMIT 1");
            (void)pin.executeStep();
        }
        if (!pathHasIdentity(state->sourcePath, state->sourceIdentity) || before != gesture::detail::storeWriteStamp(*this) ||
            owner.sequence() != state->sequence)
            throw std::runtime_error("project changed while pinning the save snapshot; try again");
        if (kind == StoreSaveKind::firstSave) state->sourceRevision = readRevision(*state->reader);
        state->stamp = *before;
        job.data_ = std::move(state);
        return {true, {}, {}};
    } catch (const std::exception& error) {
        return {false, error.what(), {}};
    }
}

StoreFileResult StoreFileSave::write(std::stop_token stop, const Progress& progress) {
    if (audio::onAudioThread || !data_ || !data_->reader) return {false, "save job has no pinned source", {}};
    auto& state = *data_;
    auto reader = std::move(state.reader);
    try {
        int totalPages = 0, copiedPages = 0;
        const Progress phaseProgress = progress ? Progress{[&](StoreSaveProgress step) {
            progress({step.phase, copiedPages, totalPages});
        }} : Progress{};
        const auto snapshot = [&](SQLite::Database& destination) {
            {
                SQLite::Backup backup(destination, *reader);
                constexpr int pagesPerStep = 128;
                int result = SQLITE_OK;
                while (result == SQLITE_OK) {
                    checkSaveCancellation(stop);
                    result = backup.executeStep(pagesPerStep);
                    if (result != SQLITE_OK && result != SQLITE_DONE)
                        throw std::runtime_error("database snapshot busy or incomplete");
                    if (progress) {
                        totalPages = backup.getTotalPageCount();
                        copiedPages = totalPages - backup.getRemainingPageCount();
                        progress({StoreSavePhase::copying, copiedPages, totalPages});
                    }
                }
            }
            // Release the pinned WAL before relocation and publication.
            reader.reset();
        };
        state.copy = Store::writeFileCopy(state.target, state.sourceFolder, state.sourceLease, state.major,
                                          state.collision, state.kind, snapshot, stop, phaseProgress, this,
                                          state.copyPolicy);
        return *state.copy;
    } catch (const SaveCancelled&) {
        state.cancelled = true;
        return {false, "save cancelled", {}};
    } catch (const std::exception& error) {
        return {false, error.what(), {}};
    }
}

bool StoreFileSave::cancelled() const noexcept { return data_ && data_->cancelled; }

std::string StoreFileSave::closeReplacedProject() {
    if (audio::onAudioThread || !data_ || !data_->retired) return {};
    auto retired = std::move(data_->retired);
    const bool folded = retired->closeFinalized();
    data_->sourceLease.reset();
    return folded ? std::string{} : "old project is still in use; its WAL could not be folded";
}

fs::path Store::copyBeforeMigration(SQLite::Database& source, int fromMinor, std::int64_t now,
                                    const std::shared_ptr<media::ProjectFileLease>& sourceLease) {
    // The migration connection holds the writer lock. Read the committed
    // image, including pages in a WAL, without using the writer as backup source.
    const auto folder = media::projectFolder(source);
    const auto backupPath = folder / ("Before upgrade from 1." + std::to_string(fromMinor) + "-" +
                                      std::to_string(now) + ".adi");
    SQLite::Database reader(sqlite3_db_filename(source.getHandle(), "main"), SQLite::OPEN_READONLY);
    const auto copied = writeFileCopy(backupPath, folder, sourceLease,
        kSchemaMajor, StoreFileCollision::Fail, StoreSaveKind::copy, [&](SQLite::Database& destination) {
            SQLite::Backup backup(destination, reader);
            if (backup.executeStep() != SQLITE_DONE)
                throw std::runtime_error("pre-upgrade database snapshot was incomplete");
        });
    if (!copied.ok || !copied.durable)
        throw std::runtime_error("could not publish a durable safety copy");
    return backupPath;
}

StoreFileResult Store::saveCopy(const std::filesystem::path& output, StoreFileCollision collision,
                               StoreCopyPolicy copyPolicy, std::stop_token stop) const {
    auto result=saveCopyImpl(output,collision,copyPolicy,stop);
    return {result.ok,std::move(result.error),std::move(result.warning)};
}

StoreFileResult Store::saveSessionCopy(const std::filesystem::path& output) const {
    auto result = saveCopyImpl(output, StoreFileCollision::Fail, StoreCopyPolicy::preserve, {},
                               StoreSaveKind::sessionCopy);
    return {result.ok, std::move(result.error), std::move(result.warning)};
}

namespace {
constexpr auto backupOrigin = "backup_origin";
constexpr auto backupCreated = "backup_created_utc";

std::string backupBaseName(const fs::path& project) {
    return media::detail::portableFilename(media::pathUtf8(project.stem()), "Project");
}

std::string backupTimestamp(std::int64_t createdUtc) {
    using namespace std::chrono;
    const sys_time<microseconds> instant{microseconds{createdUtc}};
    const auto midnight = floor<days>(instant);
    if (midnight < sys_days{year{1970}/January/1} || midnight > sys_days{year{9999}/December/31})
        throw std::runtime_error("backup time is outside the supported range");
    const year_month_day date{midnight};
    const hh_mm_ss clock{instant - midnight};
    char name[48];
    std::snprintf(name, sizeof(name), "%04d-%02u-%02u_%02d-%02d-%02d.%06dZ",
                  static_cast<int>(date.year()), static_cast<unsigned>(date.month()), static_cast<unsigned>(date.day()),
                  static_cast<int>(clock.hours().count()), static_cast<int>(clock.minutes().count()),
                  static_cast<int>(clock.seconds().count()), static_cast<int>(clock.subseconds().count()));
    return name;
}

struct BackupGeneration {
    std::string uuid, created;
    [[nodiscard]] bool known() const { return !uuid.empty() || !created.empty(); }
    [[nodiscard]] bool matches(const BackupGeneration& other) const {
        return !uuid.empty() && !other.uuid.empty() ? uuid == other.uuid :
            !created.empty() && created == other.created;
    }
};

BackupGeneration backupGeneration(SQLite::Database& database) {
    SQLite::Statement values(database,
        "SELECT COALESCE((SELECT value FROM main.adi_meta WHERE key='project_uuid'),''),"
        "COALESCE((SELECT value FROM main.adi_meta WHERE key='created_utc'),'')");
    if (!values.executeStep()) return {};
    return {values.getColumn(0).getString(), values.getColumn(1).getString()};
}

BackupGeneration sourceBackupGeneration(const fs::path& project) {
    try {
        SQLite::Database reader(media::pathUtf8(project), SQLite::OPEN_READONLY);
        if (reader.execAndGet("PRAGMA application_id").getInt() == kApplicationId)
            return backupGeneration(reader);
    } catch (const SQLite::Exception&) {
        // A corrupt original still needs the backups identified by its filename.
    }
    return {};
}

void clearBackupMarkers(SQLite::Database& database) {
    database.exec("DELETE FROM main.adi_meta WHERE key IN "
                  "('backup_origin','backup_created_utc','backup_source','backup_utc','backup_collision')");
}

void verifyBackupImage(SQLite::Database& database) {
    SQLite::Statement check(database, "PRAGMA quick_check(1)");
    if (!check.executeStep() || check.getColumn(0).getString() != "ok" || check.executeStep())
        throw std::runtime_error("the project copy failed its SQLite integrity check");
}

void stampBackupImage(SQLite::Database& database, const BackupImageStamp& metadata) {
    SQLite::Transaction finish(database);
    database.exec("DELETE FROM main.session_lock");
    clearBackupMarkers(database);
    SQLite::Statement stamp(database, "INSERT OR REPLACE INTO main.adi_meta(key,value) VALUES(?,?),(?,?),('backup_collision',?)");
    stamp.bind(1, backupOrigin);
    stamp.bind(2, metadata.sourceName);
    stamp.bind(3, backupCreated);
    stamp.bind(4, std::to_string(metadata.utcMicros));
    stamp.bind(5, std::to_string(metadata.collisionIndex.value_or(0)));
    stamp.exec();
    finish.commit();
    verifyBackupImage(database);
}

std::optional<BackupImageStamp> readBackupStamp(SQLite::Database& database) {
    if (database.execAndGet("PRAGMA application_id").getInt() != kApplicationId) return {};
    SQLite::Statement marker(database,
        "SELECT COALESCE((SELECT value FROM main.adi_meta WHERE key=?),"
        "(SELECT value FROM main.adi_meta WHERE key='backup_source')),"
        "COALESCE((SELECT value FROM main.adi_meta WHERE key=?),"
        "(SELECT value FROM main.adi_meta WHERE key='backup_utc')),"
        "(SELECT value FROM main.adi_meta WHERE key='backup_collision')");
    marker.bind(1, backupOrigin);
    marker.bind(2, backupCreated);
    if (!marker.executeStep()) return {};
    BackupImageStamp result;
    result.sourceName = marker.getColumn(0).getString();
    const auto sourceName = media::pathFromUtf8(result.sourceName);
    if (sourceName.empty() || sourceName.filename() != sourceName || sourceName == "." || sourceName == "..") return {};
    const auto stamp = marker.getColumn(1).getString();
    const auto parsed = std::from_chars(stamp.data(), stamp.data() + stamp.size(), result.utcMicros);
    if (parsed.ec != std::errc{} || parsed.ptr != stamp.data() + stamp.size()) return {};
    if (!marker.getColumn(2).isNull()) {
        const auto collision = marker.getColumn(2).getString();
        unsigned ordinal = 0;
        const auto parsedCollision = std::from_chars(collision.data(), collision.data() + collision.size(), ordinal);
        if (parsedCollision.ec != std::errc{} || parsedCollision.ptr != collision.data() + collision.size() || ordinal >= 1000)
            return {};
        result.collisionIndex = ordinal;
    }
    return result;
}

std::optional<BackupImageStamp> readOwnedBackup(SQLite::Database& database, const fs::path& project,
                                               const BackupGeneration& generation) {
    auto stamp = readBackupStamp(database);
    if (!stamp || stamp->sourceName != media::pathUtf8(project.filename()) ||
        (generation.known() && !generation.matches(backupGeneration(database)))) return {};
    return stamp;
}

void backUpDatabase(SQLite::Database& destination, SQLite::Database& source, std::stop_token stop) {
    using namespace std::chrono_literals;
    // Each step takes its own short source read lock. SQLite restarts from the
    // latest committed image when another connection writes between steps.
    SQLite::Backup backup(destination, source);
    auto deadline = std::chrono::steady_clock::now() + 30s;
    int fewestRemaining = std::numeric_limits<int>::max();
    while (true) {
        checkSaveCancellation(stop);
        const auto state = backup.executeStep(128);
        if (state == SQLITE_DONE) return;
        const auto now = std::chrono::steady_clock::now();
        const auto remaining = backup.getRemainingPageCount();
        if (state == SQLITE_OK && remaining < fewestRemaining) {
            fewestRemaining = remaining;
            deadline = now + 30s;
        }
        if (now >= deadline)
            throw std::runtime_error("the project stayed too busy to make a backup");
        if (state == SQLITE_BUSY || state == SQLITE_LOCKED) std::this_thread::sleep_for(10ms);
    }
}

fs::path numberedAdiFile(const fs::path& folder, const std::string& stem, unsigned number) {
    const auto suffix = number == 1 ? std::string{} : " (" + std::to_string(number) + ")";
    return folder / media::pathFromUtf8(stem + suffix + ".adi");
}
std::optional<unsigned> ownedBackupCollision(const fs::path& file, const BackupImageStamp& metadata) {
    const auto origin = media::pathFromUtf8(metadata.sourceName);
    auto utc = backupTimestamp(metadata.utcMicros);
    const auto stem = backupBaseName(origin) + " - " + utc;
    const auto actual = media::pathUtf8(file.stem());
    std::optional<unsigned> ordinal;
    if (actual == stem) ordinal = 0;
    if (const auto prefixed = stem + " ("; actual.starts_with(prefixed) && actual.ends_with(')')) {
        const std::string_view number(actual.data() + prefixed.size(), actual.size() - prefixed.size() - 1);
        unsigned selected = 0;
        const auto parsed = std::from_chars(number.data(), number.data() + number.size(), selected);
        if (parsed.ec == std::errc{} && parsed.ptr == number.data() + number.size() && selected >= 2 && selected <= 1000)
            ordinal = selected - 1;
    }
    if (ordinal && (!metadata.collisionIndex || metadata.collisionIndex == ordinal)) return ordinal;
    // The original queue put bracketed UTC and its collision ordinal in names.
    if (!metadata.collisionIndex) return {};
    utc[10] = ' ';
    utc.erase(16, 1);
    utc.erase(13, 1);
    auto legacy = media::pathUtf8(origin.stem()) + " [" + utc + "]";
    if (*metadata.collisionIndex != 0) legacy += "-" + std::to_string(*metadata.collisionIndex + 1);
    return actual == legacy ? metadata.collisionIndex : std::nullopt;
}
} // namespace

StoreFileResult Store::writeProjectBackup(const fs::path& project, std::int64_t createdUtc, std::stop_token stop) {
    try {
        checkSaveCancellation(stop);
        StoreError error{};
        auto source = open(project, error, true);
        if (!source) return {false, toString(error), {}};
        if (source->major_ != kSchemaMajor) return {false, "this project version cannot be backed up", {}};
        if (source->db().execAndGet("SELECT EXISTS(SELECT 1 FROM main.adi_meta "
                                    "WHERE key IN ('backup_origin','backup_source'))").getInt() != 0)
            return {false, "this file is already a project backup", {}};
        const auto base = media::projectFolder(*source);
        const auto folder = base / "Backup";
        if (!fs::create_directory(folder) && !fs::is_directory(fs::symlink_status(folder)))
            return {false, "Backup must be a regular directory in the project folder", {}};
        const auto stem = backupBaseName(project) + " - " + backupTimestamp(createdUtc);
        for (unsigned number = 1; number <= 1000; ++number) {
            checkSaveCancellation(stop);
            const auto target = numberedAdiFile(folder, stem, number);
            if (fs::symlink_status(target).type() != fs::file_type::not_found) continue;
            auto copied = writeFileCopy(target, base, source->fileLease_, source->major_, StoreFileCollision::Fail,
                StoreSaveKind::copy,
                [&](SQLite::Database& destination) {
                    backUpDatabase(destination, source->db(), stop);
                    stampBackupImage(destination, {media::pathUtf8(project.filename()), createdUtc, number - 1});
                }, stop);
            if (copied.ok || fs::symlink_status(target).type() == fs::file_type::not_found) return copied;
        }
        return {false, "could not reserve a name for the project backup", {}};
    } catch (const SaveCancelled&) {
        return {false, "project backup cancelled", {}};
    } catch (const std::exception& error) {
        return {false, error.what(), {}};
    }
}

std::vector<ProjectBackup> Store::projectBackups(const fs::path& project, bool requireSourceIdentity) {
    std::vector<ProjectBackup> files;
    const auto generation = sourceBackupGeneration(project);
    if (requireSourceIdentity && !generation.known()) return files;
    std::error_code error;
    const auto folder = fs::weakly_canonical(project, error).parent_path() / "Backup";
    if (error || !fs::is_directory(fs::symlink_status(folder, error))) return files;
    const auto prefix = backupBaseName(project) + " - ";
    const auto earlierPrefix = media::pathUtf8(project.stem()) + " [";
    for (fs::directory_iterator file(folder, error), end; !error && file != end; file.increment(error)) {
        std::error_code entryError;
        const auto& path = file->path();
        const auto name = media::pathUtf8(path.filename());
        if (path.extension() != ".adi" || (!name.starts_with(prefix) && !name.starts_with(earlierPrefix)) ||
            !fs::is_regular_file(file->symlink_status(entryError))) continue;
        if (projectDestinationReady(path)) continue;
        const auto revision = media::readFileRevision(path, entryError);
        if (!revision) continue;
        try {
            std::optional<BackupImageStamp> stamp;
            {
                SQLite::Database reader(media::pathUtf8(path), SQLite::OPEN_READONLY);
                stamp = readOwnedBackup(reader, project, generation);
            }
            const auto collision = stamp ? ownedBackupCollision(path, *stamp) : std::nullopt;
            if (collision && media::readFileRevision(path, entryError) == revision)
                files.push_back({path, stamp->utcMicros, *revision, *collision});
        } catch (const std::exception&) {
            // An incomplete or unreadable file is not offered or rotated.
        }
    }
    std::ranges::sort(files, [](const auto& left, const auto& right) {
        return std::tie(left.createdUtc, left.collisionIndex, left.path) >
               std::tie(right.createdUtc, right.collisionIndex, right.path);
    });
    return files;
}

std::shared_ptr<media::ProjectFileLease> ProjectBackup::leaseForTrash() const {
    std::error_code error;
    const auto lease = media::ProjectFileLease::openExclusive(path, error);
    if (!lease || lease->identity() != revision.identity || projectDestinationReady(path) ||
        media::readFileRevision(path, error) != revision)
        return {};
    return lease;
}

StoreFileResult Store::restoreProjectBackup(const fs::path& project, const fs::path& backup,
                                           fs::path& restored, std::stop_token stop,
                                           const fs::path& recoveryFolder) {
    restored.clear();
    try {
        checkSaveCancellation(stop);
        const auto folder = fs::weakly_canonical(project).parent_path();
        const auto backupFolder = folder / "Backup";
        std::error_code folderError;
        // Write and listing use this directory even when the volume resolves a different spelling.
        // A link substituted for the project's Backup directory is still refused.
        if (!fs::is_directory(fs::symlink_status(backupFolder, folderError)) ||
            !fs::equivalent(backup.parent_path(), backupFolder, folderError) ||
            !fs::is_regular_file(fs::symlink_status(backup)))
            return {false, "choose a backup from this project's Backup folder", {}};
        StoreError error{};
        auto source = open(backup, error, true);
        if (!source) return {false, toString(error), {}};
        if (source->major_ != kSchemaMajor || !readOwnedBackup(source->db(), project, sourceBackupGeneration(project)))
            return {false, "this file is not a backup of the project", {}};
        const auto stem = backupBaseName(project) + " - Recovered";
        const auto destinationFolder = recoveryFolder.empty() ? folder : fs::canonical(recoveryFolder);
        for (unsigned number = 1; number <= 1000; ++number) {
            checkSaveCancellation(stop);
            const auto target = numberedAdiFile(destinationFolder, stem, number);
            if (fs::symlink_status(target).type() != fs::file_type::not_found) continue;
            auto copied = writeFileCopy(target, media::projectFolder(*source), source->fileLease_, source->major_,
                StoreFileCollision::Fail, StoreSaveKind::copy, [&](SQLite::Database& destination) {
                    backUpDatabase(destination, source->db(), stop);
                    clearBackupMarkers(destination);
                    verifyBackupImage(destination);
                }, stop);
            if (copied.ok) restored = target;
            if (copied.ok || fs::symlink_status(target).type() == fs::file_type::not_found) return copied;
        }
        return {false, "could not reserve a name for the recovered project", {}};
    } catch (const SaveCancelled&) {
        return {false, "project recovery cancelled", {}};
    } catch (const std::exception& error) {
        return {false, error.what(), {}};
    }
}

namespace backup {

fs::path directoryFor(const fs::path& project) {
    if (project.empty()) return {};
    std::error_code error;
    const auto canonical = fs::weakly_canonical(project, error);
    return (error ? project : canonical).parent_path() / "Backup";
}

bool isBackup(Store& store) {
    if (store.schemaMajor() != kSchemaMajor || !appliedSource(store).alive()) return false;
    try {
        return readBackupStamp(store.db()).has_value();
    } catch (const SQLite::Exception&) {
        return false;
    }
}

StoreFileResult prepare(Store& store, std::int64_t utcMicros, Job& job) {
    if (audio::onAudioThread || !appliedSource(store).alive() || store.schemaMajor() != kSchemaMajor)
        return {false, "backups need an open supported project", {}};
    if (job.file.data_) return {false, "backup was already prepared", {}};
    try {
        if (isBackup(store)) return {false, "this file is already a backup", {}};
        if (store.synchronizedLocation()) return {false, "synced projects use the incremental file backup", {}};
        const auto source = fs::canonical(store.path());
        if (!fs::is_regular_file(source)) return {false, "backups need a saved project file", {}};
        const auto directory = directoryFor(source);
        fs::create_directory(directory);
        if (!fs::is_directory(fs::symlink_status(directory)))
            return {false, "the project's Backup folder is not a directory", {}};
        const auto stem = backupBaseName(source) + " - " + backupTimestamp(utcMicros);
        for (unsigned number = 1; number <= 1000; ++number) {
            auto target = numberedAdiFile(directory, stem, number);
            if (fs::symlink_status(target).type() != fs::file_type::not_found || projectDestinationReady(target))
                continue;
            auto result = store.prepareFileSave(target, StoreSaveKind::copy, job.file, StoreFileCollision::Fail,
                                                nullptr, StoreCopyPolicy::preserve);
            if (!result.ok) return result;
            job.file.data_->backup = BackupImageStamp{media::pathUtf8(source.filename()), utcMicros, number - 1};
            job.path = std::move(target);
            job.project = source;
            return result;
        }
        return {false, "could not reserve a name for the project backup", {}};
    } catch (const std::exception& error) {
        return {false, error.what(), {}};
    }
}

std::vector<ProjectBackup> expiredBackups(const fs::path& project) {
    auto backups = Store::projectBackups(project, true);
    constexpr std::size_t keep = 10;
    if (backups.size() <= keep) return {};
    backups.erase(backups.begin(), backups.begin() + static_cast<std::ptrdiff_t>(keep));
    return backups;
}

Result write(Job& job, std::stop_token stop) {
    if (job.path.empty()) return {{false, "backup was not prepared", {}}, {}, {}};
    Result result{job.file.write(stop), job.path, {}};
    job.file = {};
    if (result.file.ok && result.file.warning.empty() && !stop.stop_requested()) {
        try {
            result.expired = expiredBackups(job.project);
        } catch (const std::exception& error) {
            result.file.warning = joinedWarning(std::move(result.file.warning),
                "could not check backup retention: " + std::string(error.what()));
        }
    }
    return result;
}

StoreFileResult recover(const fs::path& path, const fs::path& destination, std::stop_token stop) {
    if (stop.stop_requested()) return {false, "recovery cancelled", {}};
    try {
        if (!fs::is_regular_file(fs::symlink_status(path)))
            return {false, "choose a regular ADI backup file", {}};
        StoreError error{};
        auto source = Store::open(path, error, true);
        if (!source) return {false, "could not open backup: " + std::string(toString(error)), {}};
        if (!isBackup(*source)) return {false, "the selected file is not an ADI backup", {}};
        StoreFileSave file;
        auto result = source->prepareFileSave(destination, StoreSaveKind::copy, file, StoreFileCollision::Fail,
                                              nullptr, StoreCopyPolicy::preserve);
        if (!result.ok) return result;
        file.data_->recoveringBackup = true;
        return file.write(stop);
    } catch (const std::exception& error) {
        return {false, error.what(), {}};
    }
}

} // namespace backup

Store::CopyState Store::saveCopyImpl(const std::filesystem::path& output, StoreFileCollision collision,
                                     StoreCopyPolicy copyPolicy, std::stop_token stop, StoreSaveKind kind) const {
    if(audio::onAudioThread)return {false,{},{}};
    try {
        if(closed_ || !db_)return {false,"project is closed",{}};
        if (kind == StoreSaveKind::sessionCopy && major_ != kSchemaMajor)
            return {false, "session copies need a supported schema major", {}};
        if(!sqlite3_get_autocommit(db_->getHandle()))return {false,"finish the current transaction before saving a copy",{}};
        const auto sourceFolder = major_ == kSchemaMajor ? media::projectFolder(*this) : fs::path{};
        return writeFileCopy(output, sourceFolder, fileLease_, major_, collision, kind,
            [&](SQLite::Database& destination) {
                backUpDatabase(destination, *db_, stop);
            }, stop, {}, nullptr, copyPolicy);
    } catch (const SaveCancelled&) { return {false, "save cancelled", {}}; }
    catch (const std::exception& e) { return {false, e.what(), {}}; }
}

Store::CopyState Store::writeFileCopy(const fs::path& output, const fs::path& sourceFolder,
    const std::shared_ptr<media::ProjectFileLease>& sourceLease, int major, StoreFileCollision collision, StoreSaveKind kind,
    const std::function<void(SQLite::Database&)>& snapshot, std::stop_token stop, const StoreFileSave::Progress& progress,
    const StoreFileSave* pending, StoreCopyPolicy copyPolicy) {
    try {
        checkSaveCancellation(stop);
        const auto target = saveDestination(output, sourceLease, collision);
        const bool firstSave = pending && pending->data_->kind == StoreSaveKind::firstSave;
        const bool privateFolder = firstSave && sourceFolder.filename() == pending->data_->sourcePath.stem();
        const auto targetFolder=fs::canonical(target.parent_path());
        const auto canonicalSource=firstSave?fs::weakly_canonical(sourceFolder):fs::path{};
        if(privateFolder && localPath(targetFolder.lexically_relative(canonicalSource)))
            return {false,"choose a location outside the temporary project folder",{}};
        Stage stage(target.parent_path());const auto staged=stage.path/"project.adi";
        std::optional<UntitledMedia> mediaToMove;
        if(firstSave && canonicalSource!=targetFolder)
            mediaToMove.emplace(stage.path/"media",canonicalSource,targetFolder,privateFolder,
                                collision==StoreFileCollision::ReplaceConfirmed);
        {
            auto database = std::make_unique<SQLite::Database>(media::pathUtf8(staged),
                SQLite::OPEN_READWRITE | SQLite::OPEN_CREATE);
            auto& destination = *database;
            snapshot(destination);
            if (kind == StoreSaveKind::sessionCopy) renewProjectUuid(destination);
            checkSaveCancellation(stop);
            if (progress) progress({StoreSavePhase::relocating});
            const bool sharing = copyPolicy != StoreCopyPolicy::preserve;
            Payload reportedLatency;
            if (sharing) {
                sqlite3_progress_handler(destination.getHandle(), 10000, [](void* token) {
                    return static_cast<const std::stop_token*>(token)->stop_requested() ? 1 : 0;
                }, &stop);
                reportedLatency = prepareSharedCopy(destination, copyPolicy, stop);
            }
            // Rebase between the folders as SQLite names them, the bases that
            // resolveMedia() joins rel_path onto (the stage sits inside the
            // copy's folder). On unix SQLite resolves symlinks (macOS: /var is
            // /private/var) and ".." climbs physical parents, so a path made
            // relative to fs::absolute()'s spelling climbs the wrong distance.
            if (major == kSchemaMajor) {
                rebaseHistory(destination, sourceFolder, media::projectFolder(destination).parent_path(),
                              mediaToMove ? &*mediaToMove : nullptr, stop);
                // Save As, a sharing copy and a session fork yield ordinary projects.
                if (pending && pending->data_->backup)
                    stampBackupImage(destination, *pending->data_->backup);
                else if (pending || sharing || kind == StoreSaveKind::sessionCopy)
                    clearBackupMarkers(destination);
                if (pending && pending->data_->recoveringBackup)
                    verifyBackupImage(destination);
            }
            if (copyPolicy == StoreCopyPolicy::share) {
                Store stagedStore(std::move(database), staged, false, {});
                {
                    SQLite::Transaction transaction(destination);
                    const auto baseline = history_detail::seedShareBaseline(stagedStore);
                    if (!reportedLatency.is_null()) {
                        reportedLatency["history_head"] = baseline;
                        SQLite::Statement measured(destination,
                            "INSERT INTO session_state(key,value) VALUES('devices.reported_latency',?)");
                        measured.bind(1, reportedLatency.dump());
                        measured.exec();
                    }
                    transaction.commit();
                }
                checkSaveCancellation(stop);
                destination.exec("VACUUM");
                if (!stagedStore.closeFinalized()) throw std::runtime_error(stagedStore.lastError());
            } else {
                if (sharing) destination.exec("VACUUM");
                destination.exec("PRAGMA wal_checkpoint(TRUNCATE)");
                destination.exec("PRAGMA journal_mode=DELETE");
            }
        }
        if (progress) progress({StoreSavePhase::publishing});
        checkSaveCancellation(stop);
        media::PublishResult result;
        if(collision==StoreFileCollision::ReplaceConfirmed){
            bool mediaPublished=false;
            media::ReplaceHooks hooks;
            hooks.validateTarget=[&](const fs::path& destination){
                const auto error=projectDestinationReady(destination);
                if(!error && mediaToMove && !mediaPublished){
                    mediaToMove->publish();
                    mediaPublished=true;
                }
                return error;
            };
            if (pending) hooks.expectedTarget = pending->data_->expectedTarget;
            result=media::replaceFile(staged,target,hooks);
        }else{
            if(projectDestinationReady(target))return {false,"destination acquired unrecovered SQLite sidecars",{}};
            if(mediaToMove)mediaToMove->publish();
            result=media::publishFile(staged,target,{},media::PublishPolicy::AtomicOnly);
        }
        if(result.status!=media::PublishStatus::published)return {false,"publish project: "+result.error,{}};
        if(mediaToMove)mediaToMove->keepPublished=true;
        auto warning=major==kSchemaMajor?std::string{}:std::string("newer schema copied verbatim; media paths were not relocated");
        warning=joinedWarning(std::move(warning),result.warning);
        if(!result.durabilityConfirmed && result.warning.empty())warning=joinedWarning(std::move(warning),"file published; durability could not be confirmed");
        CopyState copied{true,{},std::move(warning),result.identity};
        copied.durable=result.durabilityConfirmed && (!mediaToMove || mediaToMove->durable);
        if(firstSave && targetFolder.has_relative_path() &&
           media::syncPublishedDirectory(targetFolder.parent_path()))
            copied.durable=false;
        if(firstSave && !copied.durable)
            copied.warning=joinedWarning(std::move(copied.warning),"the unnamed file was kept because durability could not be confirmed");
        if(mediaToMove && privateFolder)
            for(const auto& [relative,move]:mediaToMove->sources)
                if(move.identity)copied.originalMedia.emplace(relative,*move.identity);
        return copied;
    } catch (const SaveCancelled&) { throw; }
    catch (const std::exception& e) {
        // SQLite interrupts the private staging statements on cancellation.
        checkSaveCancellation(stop);
        return {false, e.what(), {}};
    }
}
StoreFileResult Store::saveAs(const std::filesystem::path& output,StoreFileCollision collision,
                              const media::FileIdentity* expectedTarget) {
    return saveAsImpl(output,collision,StoreSaveKind::saveAs,expectedTarget);
}
StoreFileResult Store::saveUntitledAs(const std::filesystem::path& output,StoreFileCollision collision,
                                      const media::FileIdentity* expectedTarget) {
    return saveAsImpl(output,collision,StoreSaveKind::firstSave,expectedTarget);
}
StoreFileResult Store::saveAsImpl(const std::filesystem::path& output,StoreFileCollision collision,StoreSaveKind kind,
                                  const media::FileIdentity* expectedTarget) {
    StoreFileSave job;
    auto prepared = prepareFileSave(output, kind, job, collision, expectedTarget);
    if (!prepared.ok) return prepared;
    auto written = job.write();
    if (!written.ok) return written;
    auto result = adoptFileSave(job);
    if (result.ok) result.warning = joinedWarning(std::move(result.warning), job.closeReplacedProject());
    return result;
}

StoreFileResult Store::adoptFileSave(StoreFileSave& job) {
    if (audio::onAudioThread || !job.data_ ||
        (job.data_->kind != StoreSaveKind::saveAs && job.data_->kind != StoreSaveKind::firstSave) ||
        !job.data_->copy || !job.data_->copy->ok || job.data_->retired)
        return {false, "save job has no published replacement", {}};
    if (closed_ || !db_ || detail::appliedSourceStore(job.data_->owner) != this || detail::appliedDispatchActive(*this))
        return {false, "copy saved but the original project is no longer active", job.data_->copy->warning};
    if (!sqlite3_get_autocommit(db_->getHandle()) || sqlite3_next_stmt(db_->getHandle(), nullptr))
        return {false, "copy saved but database transactions or statements are still open", job.data_->copy->warning};
    auto change = gesture::detail::beginSourceChange(*this);
    if (!change.status.ok()) return {false, change.status.message, "copy saved; original project remains active"};
    return switchToCopy(job.data_->target, *job.data_->copy, change.permit, job.data_->retired, &job);
}

StoreFileResult Store::switchToCopy(const fs::path& output, const CopyState& copied,
    gesture::detail::NavigationPermit& permit, std::unique_ptr<Store>& retired, const StoreFileSave* pending) {
    StoreError error{};
    auto replacement=Store::open(fs::absolute(output),error);
    if(!replacement || error!=StoreError::Ok || replacement->readOnly())
        return {false,"copy saved but could not switch projects: "+std::string(toString(error)),{}};
    if(!copied.identity || !replacement->fileLease_ || replacement->fileLease_->identity()!=*copied.identity)
        return {false,"copy saved but destination changed before switching",copied.warning};
    StoreFileResult saved{true, {}, copied.warning};
    std::unique_ptr<SQLite::Database> admission;
    std::optional<SQLite::Transaction> writerGate;
    std::function<bool()> cleanup;
    try {
        detail::prepareAppliedReplacement(*replacement,*this);
        if (pending) {
            const auto& source = *pending->data_;
            // Refuse a competing writer immediately without changing the
            // active Store's normal busy timeout or transaction state.
            if (!readOnly_) {
                admission = std::make_unique<SQLite::Database>(media::pathUtf8(source.sourcePath), SQLite::OPEN_READWRITE, 0);
                writerGate.emplace(*admission, SQLite::TransactionBehavior::IMMEDIATE);
            }
            const auto applied = appliedSource(*this);
            const auto stamp = gesture::detail::storeWriteStamp(*this);
            if (!source.owner.sameConnection(applied) || source.sequence != applied.sequence() ||
                !stamp || source.stamp != *stamp || !fileLease_ ||
                fileLease_->identity() != source.sourceIdentity || !pathHasIdentity(source.sourcePath, source.sourceIdentity))
                return {false, "copy saved but the original project changed before switching", copied.warning};
            if (!pathHasIdentity(output, *copied.identity))
                return {false, "copy saved but destination changed before switching", copied.warning};
        }
        auto sourceLease = pending && pending->data_->kind == StoreSaveKind::firstSave && copied.durable
            ? fileLease_ : nullptr;
        if(sourceLease) {
            const auto& oldFolder = pending->data_->sourceFolder;
            const bool privateFolder = oldFolder.filename() == path_.stem();
            cleanup=[sourceLease=std::move(sourceLease),oldFolder,privateFolder,
                              sourceRevision=pending->data_->sourceRevision, files=copied.originalMedia]() mutable {
                std::error_code cleanupError;
                if(sourceLease) {
                    if(projectDestinationReady(sourceLease->path()))return false;
                    const auto validate=[&]() -> std::error_code {
                        if(const auto sidecars=projectDestinationReady(sourceLease->path()))return sidecars;
                        SQLite::Database source(media::pathUtf8(sourceLease->path()),SQLite::OPEN_READONLY);
                        if(readRevision(source)!=sourceRevision)return std::make_error_code(std::errc::state_not_recoverable);
                        return {};
                    };
                    if(!sourceLease->removeCreated(cleanupError,validate)){
                        // A durable edit appeared since the backup. Keep its project and media.
                        return cleanupError==std::errc::state_not_recoverable;
                    }
                    sourceLease.reset();
                }
                for(auto file=files.begin();file!=files.end();) {
                    const auto path=oldFolder/file->first;
#if defined(_WIN32)
                    cleanupError=media::removePublishedFile(path,file->second);
#else
                    // Linux and macOS only remove owned recordings in the app-created Untitled folder.
                    if(privateFolder && media::readFileIdentity(path,cleanupError)==file->second)fs::remove(path,cleanupError);
#endif
                    if(cleanupError && cleanupError!=std::errc::no_such_file_or_directory){++file;continue;}
                    for(auto parent=path.parent_path();parent!=oldFolder;parent=parent.parent_path()){
                        if(!fs::remove(parent,cleanupError) || cleanupError)break;
                    }
                    file=files.erase(file);
                }
                if(privateFolder && files.empty())fs::remove(oldFolder,cleanupError);
                return files.empty();
            };
        }
    } catch(const std::exception& exception) {
        return {false,"copy saved but could not switch projects: "+std::string(exception.what()),{}};
    }
    const auto finished=gesture::detail::finishSourceChange(permit);
    if(!finished.ok())return {false,finished.message,"copy saved; original project remains active"};
    gesture::detail::invalidateSource(*this);
    detail::invalidateAppliedDomain(*this,AppliedSourceInvalidation::Replaced);
    db_.swap(replacement->db_);path_.swap(replacement->path_);
    fileLease_.swap(replacement->fileLease_);
    appliedDomain_.swap(replacement->appliedDomain_);
    gestureDomain_.swap(replacement->gestureDomain_);
    detail::rebindAppliedDomain(*this);
    std::swap(readOnly_,replacement->readOnly_);std::swap(major_,replacement->major_);std::swap(minor_,replacement->minor_);
    std::swap(synchronizedLocation_,replacement->synchronizedLocation_);
    upgradedFrom_.swap(replacement->upgradedFrom_);lastError_.clear();
    if (cleanup) {
        untitledCleanupReady_ = false;
        untitledCleanup_ = std::move(cleanup);
    }
    retired = std::move(replacement);
    // The separate writer gate rolls back before the retired handle can be
    // sent to a cleanup worker.
    return saved;
}
void Store::finishUntitledSave() noexcept {
    untitledCleanupReady_=true;
    try {
        if(untitledCleanup_ && untitledCleanup_())untitledCleanup_={};
    } catch(...) {}
}
}

namespace adi {
StoreFileResult Store::collectAllAndSave() {
    if(detail::appliedDispatchActive(*this))return {false,"reentrant collection during applied-batch delivery",{}};
    media::detail::PublishedRollback files;
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
        struct Source {
            std::string hash;
            fs::path path;
            std::string original;
        };
        std::vector<Source> sources;
        // Resolve before publishing any new basename that a later source
        // might otherwise pick up through resolveMedia's audio/ fallback.
        for (const auto& [hash, rows] : candidates) {
            std::string error;
            fs::path source;
            std::string original;
            for (const auto& row : rows) {
                if (original.empty()) original = row.at("orig_name").get<std::string>();
                source = media::resolveMedia(folder, row, error);
                if (!source.empty()) break;
            }
            if (source.empty()) throw std::runtime_error(error);
            if (original.empty()) original = media::pathUtf8(source.filename());
            sources.push_back({hash, std::move(source), std::move(original)});
        }
        std::map<std::string,std::string> locations;
        if(!sources.empty()) {
            if(fs::is_symlink(fs::symlink_status(audio)))throw std::runtime_error("audio/ must not be a symlink");
            fs::create_directories(audio);
            CollectionNames names;
            for (const auto& item : fs::directory_iterator(audio)) {
                const auto [found, inserted] = names.try_emplace(collectionNameKey(item.path().filename()), item.path());
                if (!inserted)
                    found->second.clear(); // existing ambiguities are left alone
            }
            Stage stage(audio);
            for (const auto& [hash, path, original] : sources) {
                const auto target = publishCollectedMedia(audio, stage.path, path, original, hash, names, files);
                locations[hash] = media::pathUtf8(target.lexically_relative(folder));
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
    }catch(const std::exception& e){return {false,e.what(),files.rollback()};}
}
StoreFileResult Store::collectAndExport(const std::filesystem::path& output, bool keepUndoHistory) const {
    if(closed_ || !db_)return {false,"project is closed",{}};
    if(!sqlite3_get_autocommit(db_->getHandle()))return {false,"finish the current transaction before exporting",{}};
    const auto result=media::collectExport(media::projectFolder(*this)/path_.filename(),output,{},keepUndoHistory);
    return {result.ok,result.error,result.warning};
}
}
