// SPDX-License-Identifier: GPL-3.0-or-later

#include "adi/store.hpp"

#include "adi/schema_sql.hpp"  // generated from docs/format/schema.sql
#include "adi/version.hpp"

#include <SQLiteCpp/SQLiteCpp.h>

#include <chrono>
#include <map>
#include <stdexcept>
#include <system_error>

namespace adi {
namespace {

std::int64_t nowUtcMicros() {
    using namespace std::chrono;
    return duration_cast<microseconds>(system_clock::now().time_since_epoch()).count();
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
    static const std::vector<MigrationStep> steps = {
        // 1.1 (ADR-0136): the lock on embedded media. The triggers refuse NEW
        // writes only; a 1.0 file's embedded rows stay until extracted.
        {1, {"media_never_embedded_insert", "media_never_embedded_update",
             "media_blobs_forbidden"}},
        // 1.2 (ADR-0131): remarks.
        {2, {"remarks", "idx_remarks_target"}},
        // 1.3 (ADR-0128, ADR-0140): history snapshots.
        {3, {"history_snapshots", "idx_hsnap_seq"}},
    };
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

bool migrateToCurrent(SQLite::Database& db, int fromMinor, std::string& error) {
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
        SQLite::Transaction txn(db);
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
        }
        // Bookkeeping, then the version LAST: until this statement runs, the
        // file still says what it was, and a failure above rolls back to that.
        SQLite::Statement meta(db,
            "UPDATE adi_meta SET value = ? WHERE key = 'schema_minor'");
        meta.bind(1, std::to_string(kSchemaMinor));
        meta.exec();
        db.exec("PRAGMA user_version = " + std::to_string(kUserVersion));
        txn.commit();
        return true;
    } catch (const std::exception& e) {
        error = e.what();
        return false;
    }
}

const char* toString(StoreError e) {
    switch (e) {
        case StoreError::Ok:            return "ok";
        case StoreError::AlreadyExists: return "a file already exists at that path";
        case StoreError::CannotCreate:  return "could not create the database";
        case StoreError::CannotOpen:    return "could not open the database";
        case StoreError::NotAnAdiFile:  return "not a .adi file (application_id mismatch)";
        case StoreError::SchemaTooNew:  return "schema is newer than this build; opened read-only";
        case StoreError::SchemaCorrupt: return "schema present but unreadable";
        case StoreError::SqlError:      return "sql error";
        case StoreError::MigrationFailed:
            return "an older 1.x file could not be upgraded; it was left unchanged";
    }
    return "unknown";
}

Store::Store(std::unique_ptr<SQLite::Database> db, std::filesystem::path p, bool ro)
    : db_(std::move(db)), path_(std::move(p)), readOnly_(ro) {}

Store::~Store() {
    // Best effort. close() is where the result can actually be reported, which
    // is why it is public and why callers who care should call it.
    try {
        close();
    } catch (...) {
    }
}

bool Store::applySessionPragmas() {
    // SPEC §3.2. WAL is what makes autosave-during-playback cheap and lets the
    // render path read a consistent snapshot while the user keeps editing.
    try {
        if (!readOnly_) db_->exec("PRAGMA journal_mode = WAL");
        db_->exec("PRAGMA synchronous = NORMAL");
        db_->exec("PRAGMA foreign_keys = ON");
        db_->exec("PRAGMA busy_timeout = 5000");
        return true;
    } catch (const SQLite::Exception& e) {
        lastError_ = e.what();
        return false;
    }
}

std::unique_ptr<Store> Store::create(const std::filesystem::path& path, StoreError& err) {
    std::error_code ec;
    if (std::filesystem::exists(path, ec)) {
        err = StoreError::AlreadyExists;
        return nullptr;
    }

    try {
        auto db = std::make_unique<SQLite::Database>(
            path.string(), SQLite::OPEN_READWRITE | SQLite::OPEN_CREATE);

        // SPEC §3.1. page_size must be set before the first write, so it goes
        // first — after any table exists it is silently ignored, which is the
        // sort of thing that is discovered years later in a profile.
        db->exec("PRAGMA page_size = 4096");
        db->exec("PRAGMA encoding = 'UTF-8'");
        db->exec("PRAGMA application_id = " + std::to_string(kApplicationId));
        db->exec("PRAGMA user_version = " + std::to_string(kUserVersion));
        db->exec("PRAGMA foreign_keys = ON");

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

        auto store = std::unique_ptr<Store>(new Store(std::move(db), path, false));
        if (!store->applySessionPragmas()) {
            err = StoreError::SqlError;
            return nullptr;
        }
        err = StoreError::Ok;
        return store;
    } catch (const SQLite::Exception&) {
        // Do not leave a half-built file behind for someone to open later.
        std::filesystem::remove(path, ec);
        err = StoreError::CannotCreate;
        return nullptr;
    }
}

std::unique_ptr<Store> Store::open(const std::filesystem::path& path, StoreError& err,
                                   bool readOnly) {
    std::unique_ptr<SQLite::Database> db;
    try {
        db = std::make_unique<SQLite::Database>(
            path.string(), readOnly ? SQLite::OPEN_READONLY : SQLite::OPEN_READWRITE);
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
                db = std::make_unique<SQLite::Database>(path.string(), SQLite::OPEN_READONLY);
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
    int openedMinor = minor;
    if (major == kSchemaMajor && minor < kSchemaMinor) {
        if (!effectiveReadOnly) {
            std::string why;
            if (!migrateToCurrent(*db, minor, why)) {
                err = StoreError::MigrationFailed;
                return nullptr;
            }
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

    auto store = std::unique_ptr<Store>(new Store(std::move(db), path, effectiveReadOnly));
    store->major_ = major;
    store->minor_ = openedMinor;
    store->upgradedFrom_ = upgradedFrom;
    if (!store->applySessionPragmas()) {
        err = StoreError::SqlError;
        return nullptr;
    }
    err = result;
    return store;
}

bool Store::close() {
    if (closed_ || !db_) return true;
    closed_ = true;

    if (readOnly_) {
        db_.reset();
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
