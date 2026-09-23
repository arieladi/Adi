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
inline constexpr int kSchemaMinor = 1;   // 1.1: ADR-0136
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

private:
    Store(std::unique_ptr<SQLite::Database>, std::filesystem::path, bool readOnly);
    bool applySessionPragmas();

    std::unique_ptr<SQLite::Database> db_;
    std::filesystem::path path_;
    bool readOnly_ = false;
    bool closed_ = false;
    int major_ = kSchemaMajor;
    int minor_ = kSchemaMinor;
    mutable std::string lastError_;
};

}  // namespace adi
