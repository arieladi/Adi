// SPDX-License-Identifier: AGPL-3.0-or-later
#include "adi/store.hpp"
#include "adi/audio/io_audit.hpp"
#include "adi/gesture_navigation_detail.hpp"
#include "adi/gesture_capture_detail.hpp"
#include "adi/applied_batches_detail.hpp"
#include "adi/ops.hpp"
#include "adi/history_content.hpp"
#include "adi/media/media_ops.hpp"
#include "adi/media/publish_file.hpp"
#include "adi/media/published_rollback.hpp"
#include "adi/media/collect_export.hpp"
#include "adi/media/blake3.hpp"
#include <SQLiteCpp/SQLiteCpp.h>
#include <SQLiteCpp/Backup.h>
#include <sqlite3.h>
#include <utf8proc.h>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <stdexcept>
#include <limits>
#include <map>
#include <functional>
#include <set>
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
    fs::path staged, targetFolder;
    bool privateFolder;
    bool replacing;
    std::map<fs::path,Move> sources;
    std::set<std::string> destinations;
    std::vector<fs::path> createdDirectories;
    std::vector<Publication> published;
    bool keepPublished=false;
    bool durable=true;

    UntitledMedia(fs::path stageFolder,fs::path target,bool ownsFolder,bool replace)
        :staged(std::move(stageFolder)),targetFolder(std::move(target)),privateFolder(ownsFolder),replacing(replace){}

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
        const auto resolved=fs::weakly_canonical(primary);
        if(resolved!=primary)return false; // a linked file or directory remains an external reference
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
        const auto relative=primary.lexically_relative(newFolder);
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

struct StoreFileSave::Data {
    StoreSaveKind kind = StoreSaveKind::copy;
    StoreFileCollision collision = StoreFileCollision::Fail;
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
    std::optional<Store::CopyState> copy;
    std::unique_ptr<Store> retired;
    bool cancelled = false;
};

StoreFileSave::StoreFileSave() = default;
StoreFileSave::~StoreFileSave() = default;
StoreFileSave::StoreFileSave(StoreFileSave&&) noexcept = default;
StoreFileSave& StoreFileSave::operator=(StoreFileSave&&) noexcept = default;

StoreFileResult Store::prepareFileSave(const fs::path& output, StoreSaveKind kind, StoreFileSave& job,
                                       StoreFileCollision collision, const media::FileIdentity* expectedTarget) {
    if (audio::onAudioThread) return {false, {}, {}};
    if (job.data_) return {false, "save job was already prepared", {}};
    if (closed_ || !db_) return {false, "project is closed", {}};
    try {
        if (kind != StoreSaveKind::copy && kind != StoreSaveKind::saveAs && kind != StoreSaveKind::firstSave)
            throw std::runtime_error("unknown save kind");
        const auto owner = appliedSource(*this);
        if (detail::appliedSourceStore(owner) != this || detail::appliedDispatchActive(*this))
            throw std::runtime_error("saving requires the idle project owner");
        if (!fileLease_ || !sqlite3_get_autocommit(db_->getHandle()))
            throw std::runtime_error("save needs a project file with no open transaction");
        if (kind != StoreSaveKind::copy) {
            if (major_ != kSchemaMajor) throw std::runtime_error("use Save a Copy for an unsupported schema major");
            if (sqlite3_next_stmt(db_->getHandle(), nullptr))
                throw std::runtime_error("finish database statements before Save As");
            const auto change = gesture::detail::beginSourceChange(*this);
            if (!change.status.ok()) throw std::runtime_error(change.status.message);
        }
        auto state = std::make_unique<StoreFileSave::Data>();
        state->kind = kind;
        state->collision = collision;
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
                                          state.collision, snapshot, stop, phaseProgress, this);
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

StoreFileResult Store::saveCopy(const std::filesystem::path& output,StoreFileCollision collision) const {
    auto result=saveCopyImpl(output,collision);
    return {result.ok,std::move(result.error),std::move(result.warning)};
}
Store::CopyState Store::saveCopyImpl(const std::filesystem::path& output,StoreFileCollision collision) const {
    if(audio::onAudioThread)return {false,{},{}};
    try {
        if(closed_ || !db_)return {false,"project is closed",{}};
        if(!sqlite3_get_autocommit(db_->getHandle()))return {false,"finish the current transaction before saving a copy",{}};
        const auto sourceFolder = major_ == kSchemaMajor ? media::projectFolder(*this) : fs::path{};
        return writeFileCopy(output, sourceFolder, fileLease_, major_, collision,
            [&](SQLite::Database& destination) {
                SQLite::Backup backup(destination, *db_);
                if (backup.executeStep() != SQLITE_DONE) throw std::runtime_error("database snapshot busy or incomplete");
            });
    } catch (const std::exception& e) { return {false, e.what(), {}}; }
}

Store::CopyState Store::writeFileCopy(const fs::path& output, const fs::path& sourceFolder,
    const std::shared_ptr<media::ProjectFileLease>& sourceLease, int major, StoreFileCollision collision,
    const std::function<void(SQLite::Database&)>& snapshot, std::stop_token stop, const StoreFileSave::Progress& progress,
    const StoreFileSave* pending) {
    try {
        checkSaveCancellation(stop);
        const auto target = saveDestination(output, sourceLease, collision);
        Stage stage(target.parent_path());const auto staged=stage.path/"project.adi";
        const bool firstSave = pending && pending->data_->kind == StoreSaveKind::firstSave;
        const bool privateFolder = firstSave && sourceFolder.filename() == pending->data_->sourcePath.stem();
        const auto targetFolder=fs::canonical(target.parent_path());
        if(privateFolder && localPath(targetFolder.lexically_relative(sourceFolder)))
            return {false,"choose a location outside the temporary project folder",{}};
        std::optional<UntitledMedia> mediaToMove;
        if(firstSave && sourceFolder!=targetFolder)
            mediaToMove.emplace(stage.path/"media",targetFolder,privateFolder,
                                collision==StoreFileCollision::ReplaceConfirmed);
        {
            SQLite::Database destination(media::pathUtf8(staged),SQLite::OPEN_READWRITE|SQLite::OPEN_CREATE);
            snapshot(destination);
            checkSaveCancellation(stop);
            if (progress) progress({StoreSavePhase::relocating});
            // Rebase between the folders as SQLite names them, the bases that
            // resolveMedia() joins rel_path onto (the stage sits inside the
            // copy's folder). On unix SQLite resolves symlinks (macOS: /var is
            // /private/var) and ".." climbs physical parents, so a path made
            // relative to fs::absolute()'s spelling climbs the wrong distance.
            if(major==kSchemaMajor)rebaseHistory(destination,sourceFolder,media::projectFolder(destination).parent_path(),
                                                  mediaToMove ? &*mediaToMove : nullptr,stop);
            destination.exec("PRAGMA wal_checkpoint(TRUNCATE)");
            destination.exec("PRAGMA journal_mode=DELETE");
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
    catch(const std::exception& e){return {false,e.what(),{}};}
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
    if (audio::onAudioThread || !job.data_ || job.data_->kind == StoreSaveKind::copy ||
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
StoreFileResult Store::collectAndExport(const std::filesystem::path& output) const {
    if(closed_ || !db_)return {false,"project is closed",{}};
    if(!sqlite3_get_autocommit(db_->getHandle()))return {false,"finish the current transaction before exporting",{}};
    const auto result=media::collectExport(media::projectFolder(*this)/path_.filename(),output);
    return {result.ok,result.error,result.warning};
}
}
