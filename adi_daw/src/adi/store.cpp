// SPDX-License-Identifier: GPL-3.0-or-later

#include "adi/store.hpp"
#include "adi/appdata.hpp"
#include "adi/media/publish_file.hpp"
#include "adi/audio/io_audit.hpp"
#include "adi/gesture_navigation_detail.hpp"
#include "adi/applied_batches_detail.hpp"
#include "adi/track_recording.hpp"

#include "adi/schema_sql.hpp"  // generated from docs/format/schema.sql
#include "adi/version.hpp"

#include <SQLiteCpp/SQLiteCpp.h>

#include <atomic>
#include <chrono>
#include <map>
#include <random>
#include <stdexcept>
#include <system_error>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#elif defined(__APPLE__)
#include <sys/mount.h>
#elif defined(__linux__)
#include <linux/magic.h>
#include <sys/vfs.h>
// Older Linux headers lack some filesystem IDs. Keep recognizing the same mounts.
#ifndef CEPH_SUPER_MAGIC
#define CEPH_SUPER_MAGIC 0x00c36400
#endif
#ifndef CIFS_SUPER_MAGIC
#define CIFS_SUPER_MAGIC 0xFF534D42
#endif
#ifndef SMB2_SUPER_MAGIC
#define SMB2_SUPER_MAGIC 0xFE534D42
#endif
#endif

namespace adi {
namespace {

std::string sqlitePath(const std::filesystem::path& path) {
    const auto bytes=path.generic_u8string();
    return {reinterpret_cast<const char*>(bytes.data()),bytes.size()};
}

namespace fs = std::filesystem;

bool insideFolder(const fs::path& path, const fs::path& configuredRoot) {
    if (!configuredRoot.is_absolute()) return false;
    std::error_code error;
    const auto root = fs::canonical(configuredRoot, error);
    if (error) return false;
    auto part = path.begin();
    for (const auto& component : root) {
        if (part == path.end()) return false;
#if defined(_WIN32)
        if (CompareStringOrdinal(part->c_str(), -1, component.c_str(), -1, TRUE) != CSTR_EQUAL) return false;
#else
        if (*part != component) return false;
#endif
        ++part;
    }
    return true;
}

bool networkFolder(const fs::path& folder) {
#if defined(_WIN32)
    const auto& name = folder.native();
    if (name.starts_with(L"\\\\?\\UNC\\") ||
        (name.starts_with(L"\\\\") && !name.starts_with(L"\\\\?\\") && !name.starts_with(L"\\\\.\\")))
        return true;
    std::wstring volume(32768, L'\0');
    return GetVolumePathNameW(folder.c_str(), volume.data(), static_cast<DWORD>(volume.size())) &&
           GetDriveTypeW(volume.c_str()) == DRIVE_REMOTE;
#elif defined(__APPLE__)
    struct statfs volume{};
    return ::statfs(folder.c_str(), &volume) == 0 && (volume.f_flags & MNT_LOCAL) == 0;
#elif defined(__linux__)
    struct statfs volume{};
    if (::statfs(folder.c_str(), &volume) != 0) return false;
    switch (static_cast<std::uint32_t>(volume.f_type)) {
        case NFS_SUPER_MAGIC:
        case SMB_SUPER_MAGIC:
        case SMB2_SUPER_MAGIC:
        case CIFS_SUPER_MAGIC:
        case CEPH_SUPER_MAGIC:
        case AFS_SUPER_MAGIC:
        case NCP_SUPER_MAGIC:
        case CODA_SUPER_MAGIC:
            return true;
        default:
            return false;
    }
#else
    (void)folder;
    return false;
#endif
}

bool synchronizedProject(const fs::path& path) {
    if (path.empty() || path == fs::path(":memory:")) return false;
    std::error_code error;
    const auto canonical = fs::canonical(path, error);
    if (error) return false;
    for (const char* key : {"OneDrive", "OneDriveConsumer", "OneDriveCommercial", "ADI_SYNC_PROJECTS"})
        if (insideFolder(canonical, appdata::environmentPath(key))) return true;
#if defined(__APPLE__)
    const auto library = appdata::environmentPath("HOME") / "Library";
    if (insideFolder(canonical, library / "Mobile Documents" / "com~apple~CloudDocs") ||
        insideFolder(canonical, library / "CloudStorage")) return true;
#endif
    return networkFolder(canonical.parent_path());
}

void prepareSynchronizedJournal(SQLite::Database& db) {
    // EXTRA also syncs the containing directory when DELETE unlinks the rollback journal.
    db.exec("PRAGMA synchronous = EXTRA");
    if (db.execAndGet("PRAGMA journal_mode = DELETE").getString() != "delete")
        throw std::runtime_error("SQLite did not enter rollback journal mode");
}

std::int64_t nowUtcMicros() {
    using namespace std::chrono;
    return duration_cast<microseconds>(system_clock::now().time_since_epoch()).count();
}

std::string newClientId();

std::string readProjectUuid(SQLite::Database& db) {
    SQLite::Statement row(db, "SELECT value FROM adi_meta WHERE key='project_uuid'");
    return row.executeStep() && !row.getColumn(0).isNull() ? row.getColumn(0).getString() : std::string{};
}

void writeProjectUuid(SQLite::Database& db, bool replace = false) {
    if (!replace && !readProjectUuid(db).empty()) return;
    auto hex = newClientId();
    hex[12] = '4';
    const char nibble = hex[16];
    const int bits = nibble <= '9' ? nibble - '0' : nibble - 'a' + 10;
    hex[16] = "89ab"[bits & 3];
    const auto uuid = hex.substr(0, 8) + '-' + hex.substr(8, 4) + '-' + hex.substr(12, 4) +
                      '-' + hex.substr(16, 4) + '-' + hex.substr(20);
    SQLite::Statement row(db, replace
        ? "INSERT INTO adi_meta(key,value) VALUES('project_uuid',?) "
          "ON CONFLICT(key) DO UPDATE SET value=excluded.value"
        : "INSERT INTO adi_meta(key,value) VALUES('project_uuid',?) "
          "ON CONFLICT(key) DO UPDATE SET value=excluded.value "
          "WHERE adi_meta.value IS NULL OR adi_meta.value=''");
    row.bind(1, uuid);
    row.exec();
}

std::vector<std::byte> toBytes(const void* p, std::size_t n) {
    std::vector<std::byte> v(n);
    if (n) std::memcpy(v.data(), p, n);
    return v;
}

/// Every object in the embedded schema.sql, by name: its type and the exact
/// statement SQLite stored for it. Built once, from an in-memory database, so a
/// migration creates each object with the same statement a fresh file does.
struct SchemaObject {
    std::string type;
    std::string sql;
};
const std::map<std::string, SchemaObject>& currentSchemaObjects() {
    static const std::map<std::string, SchemaObject> objects = [] {
        std::map<std::string, SchemaObject> out;
        SQLite::Database mem(":memory:", SQLite::OPEN_READWRITE | SQLite::OPEN_CREATE);
        mem.exec(std::string(kSchemaSql));
        SQLite::Statement st(mem,
            "SELECT name, type, sql FROM sqlite_master WHERE sql IS NOT NULL");
        while (st.executeStep())
            out[st.getColumn(0).getString()] = {st.getColumn(1).getString(),
                                                st.getColumn(2).getString()};
        return out;
    }();
    return objects;
}

}  // namespace

const std::vector<MigrationStep>& migrationSteps() {
    // One entry per minor, in order, each naming what that minor ADDED. A new
    // minor appends a row here and freezes the previous schema.sql under
    // docs/format/history/ -- validate_schema check 9 fails until both exist.
    static const std::vector<MigrationStep> steps = [] {
        std::vector<MigrationStep> released = {
        // 1.1 (ADR-0136): the lock on embedded media. The triggers refuse NEW
        // writes only; a 1.0 file's embedded rows stay until extracted.
        {1, {"media_never_embedded_insert", "media_never_embedded_update",
             "media_blobs_forbidden"}},
        // 1.2 (ADR-0131): remarks.
        {2, {"remarks", "idx_remarks_target"}},
        // 1.3 (ADR-0128, ADR-0140): history snapshots.
        {3, {"history_snapshots", "idx_hsnap_seq"}},
        // 1.4 (ADR-0146): the expression route a device plays with, and the
        // agent's request text beside its transaction (AI-AGENT §6.8).
        {4, {"device_expression_routes", "agent_requests"}},
        // 1.5 (ADR-0154): the plug-in panel a device shows.
        {5, {"device_panels", "device_panel_params", "idx_panel_ord"}},
        // 1.6 (ADR-0161): who wrote each op, and its Lamport clock.
        {6, {"op_clients", "op_clocks", "idx_opclk_lamport"}},
        // 1.7 (ADR-0174): native analog summing on a group.
        {7, {"group_summing"}},
        // 1.8 (ADR-0178): tuning systems, and a key's scale over one.
        {8, {"tuning_systems", "tuning_degrees", "key_map_tunings", "key_map_degrees"}},
        // 1.9 (ADR-0203 d18): project default changes; no objects added.
        {9, {}},
        // 1.10 (N25): literal marker notes/prompts.
        {10, {"marker_details"}},
        // 1.11: Split Stereo Pan, additive settings (ADR-0203 d9).
        {11, {"mixer_pan_settings"}},
        {12, {"clip_playback_details","automation_segment_ranges"}}, // continuous fade handles (N17)
        {13, {"clip_warp_fade_domains"}}, // retained musical Warp fades (N17)
        // 1.14: per-track Rec-Q / Play-Q and monitoring-latency policy.
        {14, {"track_recording_options"}},
        // 1.15: persistent return-owned send taps (ADR-0203 d3).
        {15, {"return_send_taps"}},
        {16, {"clip_properties"}},
        {17, {"frozen_renders"}},
        {18, {"frozen_groups"}},
        // 1.19 (ADR-0203 d13): retained undo root and replay checkpoint.
        {19, {"history_checkpoint"}},
        {20, {"history_state_rows", "history_state_manifests", "history_checkpoint_content"}},
        {21, {"history_roots", "history_origin_root"}},
        {22, {"routing_key_taps"}},
        {23, {"routing_send_taps"}}, // ADR-0245 d10: a tap on an individual send
        };
        // The Session branch stages the exact 1.26 DDL over 1.23 in tests.
        // Only the real format bump activates this step. The contiguous
        // registry and frozen-history checks still require 1.24 and 1.25.
        if constexpr (kUserVersion >= kSessionSchemaUserVersion)
            released.push_back({kSessionSchemaUserVersion % 1000,
                {"scenes", "clip_slots", "idx_clip_slots_scene", "clip_launch",
                 "clip_slots_validate_insert", "clip_slots_validate_update",
                 "clips_validate_session_insert", "clips_validate_session_update",
                 "scenes_delete_clips"}});
        return released;
    }();
    return steps;
}

std::vector<std::string> tablesAddedAfter(int fromMinor) {
    std::vector<std::string> out;
    const auto& objects = currentSchemaObjects();
    for (const auto& step : migrationSteps()) {
        if (step.toMinor <= fromMinor) continue;
        for (const auto& name : step.objects) {
            const auto it = objects.find(name);
            if (it != objects.end() && it->second.type == "table") out.push_back(name);
        }
    }
    return out;
}

namespace {
bool migrateSchema(SQLite::Database& db, int fromMinor, std::string& error,
                   const std::function<void()>& beforeUpgrade = {}) {
    const auto& steps = migrationSteps();
    // The table must cover every minor exactly once, in order; a gap would
    // leave a file claiming a version whose objects it lacks.
    for (std::size_t i = 0; i < steps.size(); ++i) {
        if (steps[i].toMinor != static_cast<int>(i) + 1) {
            error = "migration table is not contiguous at minor " + std::to_string(i + 1);
            return false;
        }
    }
    if (steps.empty() || steps.back().toMinor != kSchemaMinor) {
        error = "migration table does not reach minor " + std::to_string(kSchemaMinor);
        return false;
    }
    if (fromMinor < 0 || fromMinor >= kSchemaMinor) {
        error = "nothing to upgrade from minor " + std::to_string(fromMinor);
        return false;
    }

    try {
        const auto& objects = currentSchemaObjects();
        db.setBusyTimeout(5000);
        SQLite::Transaction txn(db, SQLite::TransactionBehavior::IMMEDIATE);
        const auto version = db.execAndGet("PRAGMA user_version").getInt();
        // Another opener can finish the migration while we acquire the writer lock.
        if (version == kUserVersion) return true;
        if (version != kSchemaMajor * 1000 + fromMinor)
            throw std::runtime_error("project version changed before upgrade");
        if (beforeUpgrade) beforeUpgrade();
        for (const auto& step : steps) {
            if (step.toMinor <= fromMinor) continue;
            for (const auto& name : step.objects) {
                const auto it = objects.find(name);
                if (it == objects.end()) {
                    error = "schema.sql has no object '" + name + "' for minor " +
                            std::to_string(step.toMinor);
                    return false;   // rolls back
                }
                db.exec(it->second.sql);
            }
            if (step.toMinor == 9) {
                // SQLite cannot ALTER a column's default. No core table points
                // at project; refuse an extension that would cascade on DROP.
                if (db.execAndGet("SELECT count(*) FROM sqlite_master m, "
                        "pragma_foreign_key_list(m.name) f WHERE m.type='table' "
                        "AND f.\"table\" COLLATE NOCASE='project'").getInt() != 0)
                    throw std::runtime_error("cannot migrate project default with extension foreign keys");
                std::vector<std::string> adjuncts;
                SQLite::Statement extra(db, "SELECT sql FROM sqlite_master WHERE tbl_name='project' "
                    "AND type IN ('index','trigger') AND sql IS NOT NULL ORDER BY rowid");
                while (extra.executeStep()) adjuncts.push_back(extra.getColumn(0).getString());
                db.exec("CREATE TEMP TABLE adi_project_upgrade AS SELECT * FROM project");
                db.exec("DROP TABLE project");
                db.exec(objects.at("project").sql);
                // A nonstandard extra column fails here and rolls everything
                // back, rather than silently discarding its data.
                db.exec("INSERT INTO project SELECT * FROM temp.adi_project_upgrade");
                db.exec("DROP TABLE temp.adi_project_upgrade");
                for (const auto& sql : adjuncts) db.exec(sql);
            }
        }
        // Bookkeeping, then the version LAST: until this statement runs, the
        // file still says what it was, and a failure above rolls back to that.
        SQLite::Statement meta(db,
            "UPDATE adi_meta SET value = ? WHERE key = 'schema_minor'");
        meta.bind(1, std::to_string(kSchemaMinor));
        meta.exec();
        writeProjectUuid(db);
        db.exec("PRAGMA user_version = " + std::to_string(kUserVersion));
        txn.commit();
        return true;
    } catch (const std::exception& e) {
        error = e.what();
        return false;
    }
}

} // namespace

bool migrateToCurrent(SQLite::Database& db, int fromMinor, std::string& error) {
    return migrateSchema(db, fromMinor, error);
}

const char* toString(StoreError e) {
    switch (e) {
        case StoreError::Ok:            return "ok";
        case StoreError::AlreadyExists: return "a file already exists at that path";
        case StoreError::CannotCreate:  return "could not create the database";
        case StoreError::CannotOpen:    return "could not open the database";
        case StoreError::ExclusiveAccessRequired: return "the project requires exclusive access for headless Apply";
        case StoreError::NotAnAdiFile:  return "not a .adi file (application_id mismatch)";
        case StoreError::SchemaTooNew:  return "schema is newer than this build; opened read-only";
        case StoreError::SchemaCorrupt: return "schema present but unreadable";
        case StoreError::SqlError:      return "sql error";
        case StoreError::MigrationBackupFailed:
            return "could not save a safety copy before upgrade. The project was left unchanged.";
        case StoreError::MigrationFailed:
            return "an older 1.x file could not be upgraded; it was left unchanged";
        case StoreError::SyncJournalUnavailable:
            return "could not prepare single-file journaling in the synced or network folder";
    }
    return "unknown";
}

namespace {
// ADR-0161: 128 random bits, as 32 lowercase hex characters. Uniqueness is the
// whole requirement -- the id orders ties between two clients' equal Lamport
// clocks -- so it is drawn fresh for every Store rather than stored anywhere.
std::string newClientId() {
    std::random_device rd;
    static constexpr char kHex[] = "0123456789abcdef";
    std::string id;
    id.reserve(32);
    for (int word = 0; word < 4; ++word) {
        const std::uint32_t v = rd();
        for (int nibble = 0; nibble < 8; ++nibble)
            id.push_back(kHex[(v >> (4 * nibble)) & 0xFu]);
    }
    return id;
}

bool isClientId(const std::string& id) {
    if (id.size() != 32) return false;
    for (const char c : id)
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
    return true;
}
}  // namespace

Store::Store(std::unique_ptr<SQLite::Database> db, std::filesystem::path p, bool ro,
             std::shared_ptr<media::ProjectFileLease> lease)
    : fileLease_(std::move(lease)), db_(std::move(db)), path_(std::move(p)), readOnly_(ro), clientId_(newClientId()) {
    appliedDomain_ = detail::makeAppliedDomain(*this);
}

bool Store::setClientId(std::string id) {
    if (!isClientId(id)) return false;
    clientId_ = std::move(id);
    return true;
}

std::string Store::projectUuid() const {
    try {
        if (db_) return readProjectUuid(*db_);
    } catch (const SQLite::Exception&) {
        // A read-only open of an unsupported major may have different metadata.
    }
    return {};
}

void Store::renewProjectUuid(SQLite::Database& db) {
    writeProjectUuid(db, true);
}

Store::~Store() {
    // Best effort. close() is where the result can actually be reported, which
    // is why it is public and why callers who care should call it.
    try {
        gesture::detail::invalidateSource(*this);
        detail::invalidateAppliedDomain(*this, AppliedSourceInvalidation::Closed);
        closeFinalized();
        if(untitledCleanupReady_)finishUntitledSave();
    } catch (...) {
    }
}

bool Store::applySessionPragmas() {
    // Local WAL lets a worker pin a snapshot while the owner keeps editing.
    // In a sync or network folder, each completed transaction belongs in the
    // main file. SQLite recovers any previous WAL before leaving WAL mode.
    try {
        synchronizedLocation_ = synchronizedProject(path_);
        db_->exec("PRAGMA busy_timeout = 5000");
        if (!readOnly_ && synchronizedLocation_) {
            prepareSynchronizedJournal(*db_);
        } else {
            if (!readOnly_) db_->exec("PRAGMA journal_mode = WAL");
            db_->exec("PRAGMA synchronous = NORMAL");
        }
        db_->exec("PRAGMA foreign_keys = ON");
        return true;
    } catch (const std::exception& e) {
        lastError_ = e.what();
        return false;
    }
}

std::unique_ptr<Store> Store::create(const std::filesystem::path& path, StoreError& err) {
    auto store = createEmpty(path, err);
    if (!store) return nullptr;
    try {
        // Initialization is the history's root, not a project.setName op.
        store->db().exec("INSERT INTO project(id,name) VALUES(1,'Untitled')");
        return store;
    } catch (const SQLite::Exception&) {
        const auto lease=store->fileLease_;
        store.reset();
        std::error_code ec;
        if(lease)(void)lease->removeCreated(ec);
        err = StoreError::CannotCreate;
        return nullptr;
    }
}

std::unique_ptr<Store> Store::createNewProject(const std::filesystem::path& path, StoreError& err) {
    return createNewProject(path,err,"1/16",true);
}
std::unique_ptr<Store> Store::createNewProject(const std::filesystem::path& path, StoreError& err,
    std::string_view recordChoice,bool keepMonitoringLatency) {
    std::vector<OpRequest> captured;
    try {captured=trackRecordingDefaults(0,recordChoice,keepMonitoringLatency);}
    catch(const std::exception&){err=StoreError::CannotCreate;return nullptr;}
    auto store = create(path, err);
    if (!store) return nullptr;
    try {
        SQLite::Transaction root(store->db());
        // SC-437: New has four source tracks and Main. A saved project or template
        // supplies its own tracks, devices and routing when opened.
        // Store each colour so inserting or grouping a track does not recolour the rest.
        // The numbered names display as 1 MIDI … 4 Audio and follow later track order.
        SQLite::Statement seedTracks(store->db(),
            "INSERT INTO tracks(id,kind,name,index_in_parent,monitor_mode,color) VALUES"
            "(1,'master','Main',4,0,6),(2,'midi','# MIDI',0,2,0),(3,'midi','# MIDI',1,2,1),"
            "(4,'audio','# Audio',2,:audioMonitor,2),(5,'audio','# Audio',3,:audioMonitor,3)");
        seedTracks.bind(1, kNewAudioTrackMonitorMode);
        seedTracks.exec();
        store->db().exec("INSERT INTO mixer_strip(track_id) SELECT id FROM tracks;"
            "INSERT INTO tempo_map(id,pos_ticks,bpm) VALUES(1,0,120);"
            "INSERT INTO time_signature_map(id,pos_ticks,numerator,denominator) VALUES(1,0,4,4);");
        SQLite::Statement recording(store->db(),
            "INSERT INTO track_recording_options(track_id,record_enabled,record_grid,keep_monitoring_latency) "
            "SELECT id,?,?,? FROM tracks WHERE id BETWEEN 2 AND 5");
        recording.bind(1,captured.front().payload.at("on").get<bool>()?1:0);
        recording.bind(2,captured.front().payload.value("grid",std::string("1/16")));
        recording.bind(3,keepMonitoringLatency?1:0);recording.exec();
        root.commit();
        return store;
    } catch (const SQLite::Exception&) {
        const auto lease=store->fileLease_;
        store.reset();
        std::error_code ec;
        if(lease)(void)lease->removeCreated(ec);
        err = StoreError::CannotCreate;
        return nullptr;
    }
}

namespace {
std::atomic<bool> failNextCreate{false};
}  // namespace

void Store::failNextCreateBeforeCommitForTest() noexcept {
    failNextCreate.store(true, std::memory_order_relaxed);
}

std::unique_ptr<Store> Store::createEmpty(const std::filesystem::path& path, StoreError& err) {
    if(audio::onAudioThread){err=StoreError::CannotCreate;return nullptr;}
    std::error_code ec;
    std::shared_ptr<media::ProjectFileLease> lease;
    if (std::filesystem::exists(path, ec)) {
        err = StoreError::AlreadyExists;
        return nullptr;
    }

    try {
        if(!path.empty() && path!=std::filesystem::path(":memory:")){
            lease=media::ProjectFileLease::createShared(path,ec);
            if(!lease){err=ec==std::errc::file_exists?StoreError::AlreadyExists:StoreError::CannotCreate;return nullptr;}
        }
        auto db = std::make_unique<SQLite::Database>(
            sqlitePath(lease?lease->path():path), SQLite::OPEN_READWRITE | SQLite::OPEN_CREATE);

        // SPEC §3.1. page_size must be set before the first write, so it goes
        // first — after any table exists it is silently ignored, which is the
        // sort of thing that is discovered years later in a profile.
        db->exec("PRAGMA page_size = 4096");
        db->exec("PRAGMA encoding = 'UTF-8'");
        db->exec("PRAGMA application_id = " + std::to_string(kApplicationId));
        db->exec("PRAGMA user_version = " + std::to_string(kUserVersion));
        db->exec("PRAGMA foreign_keys = ON");

        // ONE transaction for the schema and its seed. Before this, each of the
        // schema's ~100 statements committed on its own, in the rollback
        // journal with synchronous=FULL (WAL starts below), so a new project
        // cost ~100 journal create/flush/delete cycles. On Windows that was
        // most of every store-making test suite's time: adi_mixer_tests, which
        // makes 71 stores, spent 112 of its 145 s waiting on the disk. Same
        // bytes either way, and a create that fails part-way now leaves no
        // half-built schema behind. foreign_keys is set above because that
        // pragma is a no-op inside a transaction (schema.sql repeats it; that
        // repeat is harmless).
        SQLite::Transaction create(*db);

        // The DDL is the generated copy of docs/format/schema.sql, so there is
        // exactly one source of truth and the validator tests the same bytes.
        db->exec(std::string(kSchemaSql));

        const auto now = std::to_string(nowUtcMicros());
        SQLite::Statement meta(*db, "UPDATE adi_meta SET value = ? WHERE key = ?");
        const std::pair<const char*, std::string> seed[] = {
            {"created_utc", now},
            {"modified_utc", now},
            {"created_by", std::string("adi_tool ") + kVersion},
            {"modified_by", std::string("adi_tool ") + kVersion},
        };
        for (const auto& [k, v] : seed) {
            meta.reset();
            meta.bind(1, v);
            meta.bind(2, k);
            meta.exec();
        }
        writeProjectUuid(*db);
        if (failNextCreate.exchange(false, std::memory_order_relaxed))
            throw SQLite::Exception("injected create fault (test seam)", 1 /* SQLITE_ERROR */);
        create.commit();

        auto store = std::unique_ptr<Store>(new Store(std::move(db), path, false,lease));
        if (!store->applySessionPragmas()) {
            err = store->synchronizedLocation() ? StoreError::SyncJournalUnavailable : StoreError::SqlError;
            // As the catch below: no schema-only file left at the user's path.
            // Close SQLite first (on Windows it holds the file without
            // FILE_SHARE_DELETE); removeCreated still keeps a file another
            // reader has already opened.
            store.reset();
            if(lease)(void)lease->removeCreated(ec);
            return nullptr;
        }
        err = StoreError::Ok;
        return store;
    } catch (const std::exception&) {
        // Do not leave a half-built file behind for someone to open later.
        if(lease)(void)lease->removeCreated(ec);
        err = StoreError::CannotCreate;
        return nullptr;
    }
}

std::unique_ptr<Store> Store::open(const std::filesystem::path& path, StoreError& err,
                                   bool readOnly) {
    return openWithLease(path, err, readOnly, false);
}

std::unique_ptr<Store> Store::openForHeadlessApply(const std::filesystem::path& path, StoreError& err) {
    return openWithLease(path, err, false, true);
}

std::unique_ptr<Store> Store::openWithLease(const std::filesystem::path& path, StoreError& err,
                                           bool readOnly, bool exclusive) {
    if(audio::onAudioThread){err=StoreError::CannotOpen;return nullptr;}
    std::shared_ptr<media::ProjectFileLease> lease;
    std::error_code leaseError;
    if(!path.empty() && path!=std::filesystem::path(":memory:")){
        lease = exclusive ? media::ProjectFileLease::openExclusive(path, leaseError)
                          : media::ProjectFileLease::openShared(path, leaseError);
        if (!lease) {
            err = exclusive && leaseError != std::errc::no_such_file_or_directory
                ? StoreError::ExclusiveAccessRequired : StoreError::CannotOpen;
            return nullptr;
        }
    }
    std::unique_ptr<SQLite::Database> db;
    try {
        db = std::make_unique<SQLite::Database>(
            sqlitePath(lease?lease->path():path), readOnly ? SQLite::OPEN_READONLY : SQLite::OPEN_READWRITE);
    } catch (const SQLite::Exception&) {
        err = StoreError::CannotOpen;
        return nullptr;
    }

    // SPEC §2: verify application_id BEFORE trusting any table. A SQLite file
    // with the wrong id is not a .adi, and parsing it partially to find out is
    // exactly what the check exists to avoid.
    std::int32_t appId = 0;
    std::int32_t userVersion = 0;
    try {
        appId = SQLite::Statement(*db, "PRAGMA application_id").executeStep()
                    ? db->execAndGet("PRAGMA application_id").getInt()
                    : 0;
        userVersion = db->execAndGet("PRAGMA user_version").getInt();
    } catch (const SQLite::Exception&) {
        err = StoreError::SchemaCorrupt;
        return nullptr;
    }

    if (appId != kApplicationId) {
        err = StoreError::NotAnAdiFile;
        return nullptr;
    }

    const int major = userVersion / 1000;
    const int minor = userVersion % 1000;

    // SPEC §11. A newer MAJOR opens read-only and never read-write: this build
    // does not know what a newer major means, and writing to it would destroy
    // whatever it did not understand. A newer MINOR is fine — unknown tables and
    // columns survive precisely because we never rewrite the file wholesale.
    bool effectiveReadOnly = readOnly;
    StoreError result = StoreError::Ok;
    if (major > kSchemaMajor) {
        if (!readOnly) {
            try {
                db = std::make_unique<SQLite::Database>(sqlitePath(path), SQLite::OPEN_READONLY);
            } catch (const SQLite::Exception&) {
                err = StoreError::CannotOpen;
                return nullptr;
            }
        }
        effectiveReadOnly = true;
        result = StoreError::SchemaTooNew;
    }

    // ADR-0144: an older minor of OUR major. Opened for writing, it is upgraded
    // now, before anything else touches it, so every writer after this point
    // can assume the current schema. Opened read-only, it is never written:
    // the tables it lacks get empty TEMP stand-ins on this connection -- the one
    // place readers are protected, rather than a guard in every reader.
    // A newer minor is never touched (SPEC §11): `minor < kSchemaMinor` only.
    std::optional<int> upgradedFrom;
    std::filesystem::path upgradeBackup;
    int openedMinor = minor;
    if (major == kSchemaMajor && minor < kSchemaMinor) {
        if (!effectiveReadOnly) {
            // Refuse a still-busy WAL before any schema writes can succeed and leave a
            // newer on-disk project after the application reports a failed open.
            if (synchronizedProject(path)) {
                try {
                    prepareSynchronizedJournal(*db);
                } catch (const std::exception&) {
                    err = StoreError::SyncJournalUnavailable;
                    return nullptr;
                }
            }
            std::string why;
            bool copying = false;
            if (!migrateSchema(*db, minor, why, [&] {
                copying = true;
                upgradeBackup = copyBeforeMigration(*db, minor, nowUtcMicros(), lease);
                copying = false;
            })) {
                err = copying ? StoreError::MigrationBackupFailed : StoreError::MigrationFailed;
                return nullptr;
            }
            if (!upgradeBackup.empty())
                upgradedFrom = minor;
            openedMinor = kSchemaMinor;
        } else {
            try {
                const auto& objects = currentSchemaObjects();
                for (const auto& table : tablesAddedAfter(minor)) {
                    std::string sql = objects.at(table).sql;
                    const std::string prefix = "CREATE TABLE ";
                    if (sql.rfind(prefix, 0) != 0) throw std::runtime_error("unexpected DDL for " + table);
                    db->exec("CREATE TEMP TABLE " + sql.substr(prefix.size()));
                }
            } catch (const std::exception&) {
                err = StoreError::SqlError;
                return nullptr;
            }
        }
    }

    auto store = std::unique_ptr<Store>(new Store(std::move(db), path, effectiveReadOnly,lease));
    store->major_ = major;
    store->minor_ = openedMinor;
    store->upgradedFrom_ = upgradedFrom;
    store->upgradeBackup_ = std::move(upgradeBackup);
    if (!store->applySessionPragmas()) {
        err = store->synchronizedLocation() ? StoreError::SyncJournalUnavailable : StoreError::SqlError;
        return nullptr;
    }
    if (!effectiveReadOnly && major == kSchemaMajor && minor == kSchemaMinor) {
        try {
            writeProjectUuid(store->db());
        } catch (const std::exception&) {
            err = StoreError::SqlError;
            return nullptr;
        }
    }
    store->lastError_ = store->essentialExtensionProblem();
    err = result;
    return store;
}

std::string Store::essentialExtensionProblem(int readerVersion) const {
    if (!db_ || audio::onAudioThread)
        return {};
    try {
        SQLite::Statement exists(*db_, "SELECT 1 FROM main.sqlite_master "
                                        "WHERE type='table' AND name='extensions'");
        if (!exists.executeStep())
            return {};
        SQLite::Statement rows(*db_, "SELECT ns, key, min_reader, scope_kind, scope_id FROM main.extensions "
                                      "WHERE criticality='essential' ORDER BY ns, key, min_reader, id");
        std::string problem;
        while (rows.executeStep()) {
            const auto ns = rows.getColumn(0).getString();
            const auto key = rows.getColumn(1).getString();
            const auto minimum = rows.getColumn(2).getInt64();
            if (ns == "org.adidaw.session" && key == "clip_slots" &&
                readerVersion >= kSessionSchemaUserVersion && readerVersion >= minimum &&
                rows.getColumn(3).getString() == "project" && rows.getColumn(4).isNull())
                continue;
            problem += problem.empty() ? "Unsupported essential project extension " : ", ";
            problem += ns + "/" + key;
            if (minimum > 0)
                problem += " (minimum reader " + std::to_string(minimum / 1000) + "." +
                           std::to_string(minimum % 1000) + ")";
        }
        return problem;
    } catch (const std::exception& error) {
        return "Could not inspect essential project extensions: " + std::string(error.what());
    }
}

bool Store::close() {
    if (detail::appliedDispatchActive(*this)) {
        lastError_ = "reentrant Store close during applied-batch delivery";
        return false;
    }
    if (closed_ || !db_) return true;
    auto change = gesture::detail::beginSourceChange(*this);
    if (!change.status.ok()) { lastError_ = change.status.message; return false; }
    const auto finished = gesture::detail::finishSourceChange(change.permit);
    if (!finished.ok()) { lastError_ = finished.message; return false; }
    gesture::detail::invalidateSource(*this);
    detail::invalidateAppliedDomain(*this, AppliedSourceInvalidation::Closed);
    return closeFinalized();
}

bool Store::closeFinalized() {
    if (closed_ || !db_) return true;
    closed_ = true;

    if (readOnly_ || synchronizedLocation_) {
        db_.reset();
        fileLease_.reset();
        return true;
    }

    // SPEC §3.3 — the WAL sidecar rule, and the single most user-visible thing
    // in this file.
    //
    // While open, SQLite keeps foo.adi-wal and foo.adi-shm beside the database.
    // Someone who copies or emails only foo.adi from that state gets a project
    // that opens CLEANLY but is missing everything since the last checkpoint —
    // strictly worse than an error, because nothing tells them.
    //
    // So: checkpoint everything into the main file, then leave WAL mode, which
    // deletes the sidecars. A cleanly closed .adi is exactly one file.
    bool ok = true;
    try {
        db_->exec("PRAGMA wal_checkpoint(TRUNCATE)");
        db_->exec("PRAGMA journal_mode = DELETE");
    } catch (const SQLite::Exception& e) {
        lastError_ = e.what();
        ok = false;
    }
    db_.reset();
    fileLease_.reset();
    return ok;
}

// --- blob persistence --------------------------------------------------------

bool Store::putEventStream(std::int64_t clipId, const std::string& streamKind,
                           std::span<const std::byte> blob) {
    try {
        SQLite::Statement st(*db_,
            "INSERT INTO event_streams(clip_id, stream_kind, data) VALUES (?,?,?) "
            "ON CONFLICT(clip_id, stream_kind, IFNULL(channel,-1), IFNULL(controller,-1)) "
            "DO UPDATE SET data = excluded.data");
        st.bind(1, clipId);
        st.bind(2, streamKind);
        st.bindNoCopy(3, blob.data(), static_cast<int>(blob.size()));
        st.exec();
        return true;
    } catch (const SQLite::Exception& e) {
        lastError_ = e.what();
        return false;
    }
}

std::optional<std::vector<std::byte>>
Store::getEventStream(std::int64_t clipId, const std::string& streamKind) const {
    try {
        SQLite::Statement st(*db_,
            "SELECT data FROM event_streams WHERE clip_id = ? AND stream_kind = ?");
        st.bind(1, clipId);
        st.bind(2, streamKind);
        if (!st.executeStep()) return std::nullopt;
        const auto col = st.getColumn(0);
        return toBytes(col.getBlob(), static_cast<std::size_t>(col.getBytes()));
    } catch (const SQLite::Exception& e) {
        lastError_ = e.what();
        return std::nullopt;
    }
}

std::optional<std::vector<std::byte>>
Store::getStateBlob(const std::string& hashBlake3) const {
    try {
        SQLite::Statement st(*db_, "SELECT data FROM state_blobs WHERE hash_blake3 = ?");
        st.bind(1, hashBlake3);
        if (!st.executeStep()) return std::nullopt;
        const auto col = st.getColumn(0);
        return toBytes(col.getBlob(), static_cast<std::size_t>(col.getBytes()));
    } catch (const SQLite::Exception& e) {
        lastError_ = e.what();
        return std::nullopt;
    }
}

bool Store::putNoteExpression(std::int64_t clipId, std::uint64_t noteId, int dimension,
                              std::span<const std::byte> blob) {
    try {
        SQLite::Statement st(*db_,
            "INSERT INTO note_expression(clip_id, note_id, dimension, data) VALUES (?,?,?,?) "
            "ON CONFLICT(clip_id, note_id, dimension) DO UPDATE SET data = excluded.data");
        st.bind(1, clipId);
        st.bind(2, static_cast<std::int64_t>(noteId));
        st.bind(3, dimension);
        st.bindNoCopy(4, blob.data(), static_cast<int>(blob.size()));
        st.exec();
        return true;
    } catch (const SQLite::Exception& e) {
        lastError_ = e.what();
        return false;
    }
}

std::optional<std::vector<std::byte>>
Store::getNoteExpression(std::int64_t clipId, std::uint64_t noteId, int dimension) const {
    try {
        SQLite::Statement st(*db_,
            "SELECT data FROM note_expression "
            "WHERE clip_id = ? AND note_id = ? AND dimension = ?");
        st.bind(1, clipId);
        st.bind(2, static_cast<std::int64_t>(noteId));
        st.bind(3, dimension);
        if (!st.executeStep()) return std::nullopt;
        const auto col = st.getColumn(0);
        return toBytes(col.getBlob(), static_cast<std::size_t>(col.getBytes()));
    } catch (const SQLite::Exception& e) {
        lastError_ = e.what();
        return std::nullopt;
    }
}

bool Store::putAutomationData(std::int64_t laneId, std::optional<std::int64_t> clipId,
                              std::span<const std::byte> blob) {
    try {
        SQLite::Statement ranges(*db_,"SELECT 1 FROM automation_segment_ranges r JOIN automation_data d ON d.id=r.data_id WHERE d.lane_id=? AND d.clip_id IS ? LIMIT 1");
        ranges.bind(1,laneId);if(clipId)ranges.bind(2,*clipId);else ranges.bind(2);
        if(ranges.executeStep()){lastError_="replace automation with its captured curve ranges";return false;}
        SQLite::Statement st(*db_,
            "INSERT INTO automation_data(lane_id, clip_id, data) VALUES (?,?,?) "
            "ON CONFLICT(lane_id, IFNULL(clip_id,-1)) DO UPDATE SET data = excluded.data");
        st.bind(1, laneId);
        if (clipId) st.bind(2, *clipId); else st.bind(2);   // bind(i) binds NULL
        st.bindNoCopy(3, blob.data(), static_cast<int>(blob.size()));
        st.exec();
        return true;
    } catch (const SQLite::Exception& e) {
        lastError_ = e.what();
        return false;
    }
}

std::optional<std::vector<std::byte>>
Store::getAutomationData(std::int64_t laneId, std::optional<std::int64_t> clipId) const {
    try {
        SQLite::Statement st(*db_,
            "SELECT data FROM automation_data "
            "WHERE lane_id = ? AND IFNULL(clip_id,-1) = ?");
        st.bind(1, laneId);
        st.bind(2, clipId ? *clipId : static_cast<std::int64_t>(-1));
        if (!st.executeStep()) return std::nullopt;
        const auto col = st.getColumn(0);
        return toBytes(col.getBlob(), static_cast<std::size_t>(col.getBytes()));
    } catch (const SQLite::Exception& e) {
        lastError_ = e.what();
        return std::nullopt;
    }
}

}  // namespace adi
