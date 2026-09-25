// SPDX-License-Identifier: GPL-3.0-or-later
//
// The .adi store — create, open, close, and blob persistence. SPEC §2, §3, §11.
//
// This is the persistence layer and nothing else. It has no audio, no threads
// and no JUCE, and the audio thread never reaches it (ADR-0010). What it owns
// is the part of the format that is easy to get subtly wrong:
//
//   SPEC §2    application_id is verified before any table is trusted
//   SPEC §3.1  the pragmas a new file is created with
//   SPEC §3.2  WAL while open
//   SPEC §3.3  THE CLOSE POLICY -- a cleanly closed .adi is exactly ONE file
//   SPEC §11   a newer major schema opens read-only, never read-write
//   ADR-0009   one blob per editable object, never wider

#pragma once

#include "adi/blob.hpp"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace SQLite {
class Database;
}

namespace adi {

// SPEC §2. 0x41444931 == 'ADI1'.
inline constexpr std::int32_t kApplicationId = 1094994225;
inline constexpr int kSchemaMajor = 1;
inline constexpr int kSchemaMinor = 7;   // 1.1: ADR-0136; 1.2: remarks (ADR-0131); 1.3: history snapshots (ADR-0128); 1.4: routes, agent requests (ADR-0146); 1.5: the plug-in panel (ADR-0154); 1.6: op clients and clocks (ADR-0161); 1.7: group summing (ADR-0174)
inline constexpr int kUserVersion = kSchemaMajor * 1000 + kSchemaMinor;

enum class StoreError {
    Ok,
    AlreadyExists,   // create() on a path that exists -- never silently truncate
    CannotCreate,
    CannotOpen,
    NotAnAdiFile,    // wrong application_id: a SQLite file, but not ours
    SchemaTooNew,    // major > ours; opened read-only instead (SPEC §11)
    SchemaCorrupt,
    SqlError,
    MigrationFailed, // an older 1.x file could not be upgraded; it is unchanged (ADR-0144)
};

const char* toString(StoreError);

/// An open `.adi`.
///
/// Non-copyable, non-movable: it owns a database handle and a close policy that
/// must run exactly once. Create it through `create()` or `open()`, which return
/// null on failure and set the out-parameter rather than throwing across the
/// API boundary — SQLiteCpp throws, and a format reader that lets those escape
/// makes every caller handle a vocabulary it did not choose.
class Store {
public:
    ~Store();
    Store(const Store&) = delete;
    Store& operator=(const Store&) = delete;

    /// Create a new project. Fails if the path exists — overwriting someone's
    /// project is not a thing this function will do quietly.
    static std::unique_ptr<Store> create(const std::filesystem::path&, StoreError&);

    /// Open an existing project. Verifies `application_id` before trusting any
    /// table, and downgrades to read-only if the file's major schema is newer
    /// than this build's (SPEC §11) — `err` is then `SchemaTooNew` and the
    /// returned Store is valid but `readOnly()`.
    ///
    /// An OLDER minor of the same major is upgraded in place when opened for
    /// writing: one transaction, every missing minor in order, user_version
    /// last (ADR-0144, SPEC §11). If that fails the file is unchanged, `err` is
    /// `MigrationFailed` and nothing is returned. Opened read-only, the file is
    /// never written: each table it lacks is stood in for by an empty TEMP
    /// table on this connection, so every reader sees "no rows".
    static std::unique_ptr<Store> open(const std::filesystem::path&, StoreError&,
                                       bool readOnly = false);

    /// SPEC §3.3. Checkpoints the WAL and returns the journal to DELETE mode, so
    /// what is left on disk is exactly one self-contained file. Idempotent; the
    /// destructor calls it. Safe to call explicitly to find out whether it
    /// worked, which the destructor cannot tell you.
    bool close();

    [[nodiscard]] bool readOnly() const { return readOnly_; }
    [[nodiscard]] const std::filesystem::path& path() const { return path_; }
    [[nodiscard]] int schemaMajor() const { return major_; }
    [[nodiscard]] int schemaMinor() const { return minor_; }
    /// The minor the file had before this open upgraded it; nullopt when no
    /// upgrade happened (ADR-0144).
    [[nodiscard]] std::optional<int> upgradedFromMinor() const { return upgradedFrom_; }

    [[nodiscard]] SQLite::Database& db() { return *db_; }
    [[nodiscard]] const SQLite::Database& db() const { return *db_; }

    // --- blob persistence, at the ADR-0009 granularity bound -----------------
    // One blob per editable object. `clipId` scopes a note stream; a lane and an
    // optional clip scope an automation stream. Nothing here writes a blob that
    // spans two objects, and nothing should be added that does.

    bool putEventStream(std::int64_t clipId, const std::string& streamKind,
                        std::span<const std::byte> blob);
    [[nodiscard]] std::optional<std::vector<std::byte>>
    getEventStream(std::int64_t clipId, const std::string& streamKind) const;

    bool putNoteExpression(std::int64_t clipId, std::uint64_t noteId, int dimension,
                           std::span<const std::byte> blob);
    [[nodiscard]] std::optional<std::vector<std::byte>>
    getNoteExpression(std::int64_t clipId, std::uint64_t noteId, int dimension) const;

    bool putAutomationData(std::int64_t laneId, std::optional<std::int64_t> clipId,
                           std::span<const std::byte> blob);
    [[nodiscard]] std::optional<std::vector<std::byte>>
    getAutomationData(std::int64_t laneId, std::optional<std::int64_t> clipId) const;

    /// One `state_blobs` row by hash: the bytes a device loader hands back to
    /// a plugin, or keeps beside a placeholder, byte for byte (SPEC §7.1 rule 1,
    /// ADR-0122). `nullopt` when no such row exists, which a loader reports
    /// rather than treats as empty state.
    [[nodiscard]] std::optional<std::vector<std::byte>>
    getStateBlob(const std::string& hashBlake3) const;

    /// Last SQLite message, for diagnostics when a put/get returns false.
    [[nodiscard]] const std::string& lastError() const { return lastError_; }

    // --- who writes (ADR-0161) --------------------------------------------------
    // Every op this Store commits is stamped with its client and a Lamport
    // clock (`op_clocks`), so a future remote session can order and merge two
    // logs. A client is this open Store, not a person: new and random each
    // time a Store is opened; `ops.actor` still says user or agent.

    /// 32 lowercase hex characters.
    [[nodiscard]] const std::string& clientId() const noexcept { return clientId_; }
    /// Refused, returning false, unless 32 lowercase hex characters. For tests
    /// and for the sync layer that will one day hand a client its identity.
    bool setClientId(std::string id);
    /// Shown beside the client in `op_clients.label`. Takes effect for a client
    /// the file has not seen yet, so set it before the first commit.
    void setClientLabel(std::string label) { clientLabel_ = std::move(label); }
    [[nodiscard]] const std::string& clientLabel() const noexcept { return clientLabel_; }

private:
    Store(std::unique_ptr<SQLite::Database>, std::filesystem::path, bool readOnly);
    bool applySessionPragmas();

    std::unique_ptr<SQLite::Database> db_;
    std::filesystem::path path_;
    bool readOnly_ = false;
    bool closed_ = false;
    int major_ = kSchemaMajor;
    int minor_ = kSchemaMinor;
    std::optional<int> upgradedFrom_;
    mutable std::string lastError_;
    std::string clientId_;
    std::string clientLabel_;
};

// --- upgrading an older 1.x file (ADR-0144, SPEC §11) -------------------------
//
// Public so tests can drive it on a connection they instrument. Store::open is
// the only production caller.

/// One step per minor: the schema objects (tables, indexes, triggers) that
/// minor added, by name. Their DDL is taken from the embedded schema.sql, so a
/// migrated file and a fresh one are built from the same statements.
struct MigrationStep {
    int toMinor = 0;
    std::vector<std::string> objects;
};
const std::vector<MigrationStep>& migrationSteps();

/// Upgrades a major-1 file at `fromMinor` to kSchemaMinor, in ONE transaction:
/// each step's objects in order, then adi_meta.schema_minor, then user_version
/// last. On any failure everything rolls back and `error` says why.
bool migrateToCurrent(SQLite::Database&, int fromMinor, std::string& error);

/// The tables a file at `fromMinor` lacks, which a read-only open stands in for.
std::vector<std::string> tablesAddedAfter(int fromMinor);

}  // namespace adi
