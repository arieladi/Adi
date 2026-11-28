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
//   SPEC §3.2  local WAL and synchronized project journaling
//   SPEC §3.3  THE CLOSE POLICY -- a cleanly closed .adi is exactly ONE file
//   SPEC §11   a newer major schema opens read-only, never read-write
//   ADR-0009   one blob per editable object, never wider

#pragma once

#include "adi/blob.hpp"
#include "adi/media/publish_file.hpp"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

namespace SQLite {
class Database;
}

namespace adi {
namespace media { struct FileIdentity; class ProjectFileLease; }
class Store;
namespace control { [[nodiscard]] std::string projectFileConsentBinding(const Store&); }
namespace freeze {bool validated(Store&) noexcept;void remember(Store&) noexcept;}
namespace detail {struct AppliedDomain; struct AppliedSourceAccess;}
namespace gesture::detail {struct Domain; struct DomainAccess; class NavigationPermit;}

// SPEC §2. 0x41444931 == 'ADI1'.
inline constexpr std::int32_t kApplicationId = 1094994225;
inline constexpr int kSchemaMajor = 1;
inline constexpr int kSchemaMinor = 23;   // 1.1: ADR-0136; 1.2: remarks (ADR-0131); 1.3: history snapshots (ADR-0128); 1.4: routes, agent requests (ADR-0146); 1.5: the plug-in panel (ADR-0154); 1.6: op clients and clocks (ADR-0161); 1.7: group summing (ADR-0174); 1.8: tuning systems (ADR-0178); 1.9: 44.1 kHz project default (ADR-0203 d18); 1.10: marker notes/prompts (N25); 1.11: split stereo pan (N12); 1.13: retained musical Warp fade domains; 1.14: track recording options; 1.15: return send taps (ADR-0203 d3); 1.16: local clip properties (N33); 1.18: whole-group freeze scope; 1.19: undo checkpoint (ADR-0203 d13); 1.20: content-addressed checkpoint rows (HI-02); 1.21: stored history baselines (HI-02); 1.22: device key taps (DEV-05/MX-24); 1.23: individual send taps (ADR-0245 d10)
inline constexpr int kUserVersion = kSchemaMajor * 1000 + kSchemaMinor;
inline constexpr int kSessionSchemaUserVersion = 1026; // TM-SV-02, reserved until 1.24 and 1.25 land

enum class StoreError {
    Ok,
    AlreadyExists,   // create() on a path that exists -- never silently truncate
    CannotCreate,
    CannotOpen,
    ExclusiveAccessRequired,
    NotAnAdiFile,    // wrong application_id: a SQLite file, but not ours
    SchemaTooNew,    // major > ours; opened read-only instead (SPEC §11)
    SchemaCorrupt,
    SqlError,
    MigrationFailed, // an older 1.x file could not be upgraded; it is unchanged (ADR-0144)
    SyncJournalUnavailable, // the writable connection could not leave WAL safely
    MigrationBackupFailed, // the safety copy could not be published, so no upgrade ran
};

const char* toString(StoreError);

// Discard a closed provisional project still owned by this launch, with no edits or recovery files.
bool discardUntouchedProject(const std::filesystem::path&, const media::FileIdentity&);

enum class StoreFileCollision { Fail, ReplaceConfirmed };

struct StoreFileResult {
    bool ok = false;
    std::string error;
    std::string warning;
};

struct ProjectBackup {
    std::filesystem::path path;
    std::int64_t createdUtc = 0; // microseconds
    media::FileRevision revision{};
    unsigned collisionIndex = 0;
    [[nodiscard]] std::shared_ptr<media::ProjectFileLease> leaseForTrash() const;
};

namespace backup {
struct Job;
StoreFileResult prepare(Store&, std::int64_t, Job&);
StoreFileResult recover(const std::filesystem::path&, const std::filesystem::path&, std::stop_token);
}

enum class StoreSaveKind { copy, saveAs, firstSave, sessionCopy };
/// Backups and relocation preserve identity. Shared copies always remove private audit data.
enum class StoreCopyPolicy { share, shareWithUndo, preserve };
enum class StoreSavePhase { copying, relocating, publishing };
struct StoreSaveProgress {
    StoreSavePhase phase = StoreSavePhase::copying;
    int pagesCopied = 0;
    int totalPages = 0;
};

/// One prepared save. The owner pins the source snapshot, a file worker writes
/// it, and the owner may then adopt the published copy for Save As.
class StoreFileSave {
public:
    using Progress = std::function<void(StoreSaveProgress)>;
    StoreFileSave();
    ~StoreFileSave();
    StoreFileSave(StoreFileSave&&) noexcept;
    StoreFileSave& operator=(StoreFileSave&&) noexcept;
    StoreFileSave(const StoreFileSave&) = delete;
    StoreFileSave& operator=(const StoreFileSave&) = delete;

    [[nodiscard]] StoreFileResult write(std::stop_token = {}, const Progress& = {});
    /// Inspect after the file worker returns.
    [[nodiscard]] bool cancelled() const noexcept;
    /// File worker, after adoption. Folds the detached source's WAL.
    [[nodiscard]] std::string closeReplacedProject();

private:
    struct Data;
    std::unique_ptr<Data> data_;
    friend class Store;
    friend StoreFileResult backup::prepare(Store&, std::int64_t, backup::Job&);
    friend StoreFileResult backup::recover(const std::filesystem::path&, const std::filesystem::path&, std::stop_token);
};

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

    /// Create a new project with its singleton project row at 44.1 kHz,
    /// before the first undoable edit. Fails if the path exists — overwriting someone's
    /// project is not a thing this function will do quietly.
    static std::unique_ptr<Store> create(const std::filesystem::path&, StoreError&);

    /// Application New: create the project row, Main, two MIDI and two audio
    /// tracks, mixer strips and 120 BPM / 4/4 maps at the history root.
    /// Returns, sends and effects can be added after New or loaded from an
    /// existing project. New adds no ops or protected UI undo step.
    static std::unique_ptr<Store> createNewProject(const std::filesystem::path&, StoreError&);
    /// Capture the caller's app recording defaults for the four initial
    /// tracks in the history root. Invalid choices create no file.
    static std::unique_ptr<Store> createNewProject(const std::filesystem::path&, StoreError&,
        std::string_view recordQuantize, bool keepMonitoringLatency);

    /// Schema-only creation for converters and low-level fixtures that supply
    /// their own project rows. Application New commands use create().
    static std::unique_ptr<Store> createEmpty(const std::filesystem::path&, StoreError&);
    /// Test seam: the next createEmpty throws after the schema and its seed
    /// have run, before they commit, so a test can prove a failed create
    /// rolls back and leaves no file behind. One-shot; tests only.
    static void failNextCreateBeforeCommitForTest() noexcept;

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
    /// The headless changeset writer must be the only holder of this project.
    static std::unique_ptr<Store> openForHeadlessApply(const std::filesystem::path&, StoreError&);

    /// SPEC §3.3. Checkpoints the WAL and returns the journal to DELETE mode, so
    /// what is left on disk is exactly one self-contained file. Idempotent; the
    /// destructor calls it. Safe to call explicitly to find out whether it
    /// worked, which the destructor cannot tell you.
    bool close();

    /// Owner thread, outside any SQL transaction. Snapshot committed WAL data
    /// into a new file. Existing files require ReplaceConfirmed, supplied only
    /// after the save dialog's explicit confirmation. Active Store files and
    /// aliases refuse replacement. Shared copies start new undo history by default.
    [[nodiscard]] StoreFileResult saveCopy(const std::filesystem::path&, StoreFileCollision=StoreFileCollision::Fail,
                                          StoreCopyPolicy=StoreCopyPolicy::share,
                                          std::stop_token={}) const;
    /// A session works on its own project. Give the staged copy a fresh
    /// identity before publishing it and refuse an existing destination.
    [[nodiscard]] StoreFileResult saveSessionCopy(const std::filesystem::path&) const;
    /// File-worker operations use their own read connection. The active
    /// project and the recovery source are kept in place.
    [[nodiscard]] static StoreFileResult writeProjectBackup(const std::filesystem::path& project,
                                                           std::int64_t createdUtc, std::stop_token = {});
    /// Managed copies for this project, newest first. Missing folders have no copies.
    [[nodiscard]] static std::vector<ProjectBackup> projectBackups(const std::filesystem::path& project,
                                                                   bool requireSourceIdentity = false);
    [[nodiscard]] static StoreFileResult restoreProjectBackup(const std::filesystem::path& project,
                                                             const std::filesystem::path& backup,
                                                             std::filesystem::path& restored, std::stop_token = {},
                                                             const std::filesystem::path& recoveryFolder = {});
    /// Save a copy, then switch this same Store object to the new writable file.
    /// Existing OpJournal/History references remain valid. Requires no live SQL
    /// statements. Refresh ProjectView/Session and the window's filename after
    /// success. A failed switch retains the source and a completed copy. When
    /// given, expectedTarget binds the confirmation to the existing file.
    [[nodiscard]] StoreFileResult saveAs(const std::filesystem::path&, StoreFileCollision=StoreFileCollision::Fail,
                                        const media::FileIdentity* expectedTarget=nullptr);
    /// Name a newly created, unnamed project. Project-local media and history
    /// follow the file; the original is discarded only after the switch.
    [[nodiscard]] StoreFileResult saveUntitledAs(const std::filesystem::path&,
                                                StoreFileCollision=StoreFileCollision::Fail,
                                                const media::FileIdentity* expectedTarget=nullptr);
    /// Retry releasing an unnamed project's old files once playback and
    /// waveform readers have switched away from them.
    void finishUntitledSave() noexcept;
    /// Owner preflight for a file worker. The private read connection pins the
    /// committed database (including WAL) at invocation.
    [[nodiscard]] StoreFileResult prepareFileSave(const std::filesystem::path&, StoreSaveKind, StoreFileSave&,
                                                  StoreFileCollision = StoreFileCollision::Fail,
                                                  const media::FileIdentity* expectedTarget = nullptr,
                                                  StoreCopyPolicy = StoreCopyPolicy::share);
    /// Owner finalization for Save As. Retains the completed copy and the
    /// current Store if the source changed since preflight.
    [[nodiscard]] StoreFileResult adoptFileSave(StoreFileSave&);
    /// Copy current and undo/branch media into audio/, verify their hashes and
    /// save the new locations atomically. Owner thread; no live SQL statements
    /// or transaction. Refresh readers after success. No undo step is appended.
    [[nodiscard]] StoreFileResult collectAllAndSave();
    /// Portable ZIP of a committed snapshot. The destination must not exist.
    [[nodiscard]] StoreFileResult collectAndExport(const std::filesystem::path&, bool keepUndoHistory = false) const;

    [[nodiscard]] bool readOnly() const { return readOnly_; }
    /// Stable across Save As and copies using the preserve policy. Empty on
    /// an unassigned read-only legacy project or a schema without an identity.
    [[nodiscard]] std::string projectUuid() const;
    [[nodiscard]] bool synchronizedLocation() const noexcept { return synchronizedLocation_; }
    [[nodiscard]] const std::filesystem::path& path() const { return path_; }
    [[nodiscard]] int schemaMajor() const { return major_; }
    [[nodiscard]] int schemaMinor() const { return minor_; }
    /// The minor the file had before this open upgraded it; nullopt when no
    /// upgrade happened (ADR-0144).
    [[nodiscard]] std::optional<int> upgradedFromMinor() const { return upgradedFrom_; }
    [[nodiscard]] const std::filesystem::path& upgradeBackupPath() const { return upgradeBackup_; }

    [[nodiscard]] SQLite::Database& db() { return *db_; }
    [[nodiscard]] const SQLite::Database& db() const { return *db_; }

    // The open document's starting state, retained only for this visit.
    // A point with no sequence refers to the original, pre-edit state.
    struct OpeningHistoryPoint { std::optional<std::int64_t> seq; };
    void retainOpeningHistory(std::optional<std::int64_t> seq) noexcept {
        openingHistory_ = OpeningHistoryPoint{seq};
    }
    [[nodiscard]] std::optional<OpeningHistoryPoint> openingHistory() const noexcept {
        return openingHistory_;
    }

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

    /// Refuses a replacement when this data has retained curve ranges; use a captured row edit for data and ranges together.
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

    /// Empty when the reader supports every essential extension in the file.
    /// Owner-side read before playback/render/export, with the extension names
    /// and minimum reader versions in the diagnostic.
    [[nodiscard]] std::string essentialExtensionProblem(int readerVersion = kUserVersion) const;

    /// Last operation diagnostic, including an essential-extension warning on open.
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
    friend std::string control::projectFileConsentBinding(const Store&);
    friend struct gesture::detail::DomainAccess;
    std::shared_ptr<gesture::detail::Domain> gestureDomain_;
    friend struct detail::AppliedSourceAccess;
    std::shared_ptr<detail::AppliedDomain> appliedDomain_;
    friend bool freeze::validated(Store&) noexcept;
    friend void freeze::remember(Store&) noexcept;
    std::int64_t frozenValidationChanges_=-1,frozenValidationDataVersion_=-1;
    std::int64_t frozenValidationSchema_=-1,frozenValidationTempSchema_=-1;

    Store(std::unique_ptr<SQLite::Database>, std::filesystem::path, bool readOnly,
          std::shared_ptr<media::ProjectFileLease>);
    static std::unique_ptr<Store> openWithLease(const std::filesystem::path&, StoreError&,
                                               bool readOnly, bool exclusive);
    struct CopyState;
    static std::filesystem::path copyBeforeMigration(SQLite::Database&, int fromMinor, std::int64_t now,
                                                      const std::shared_ptr<media::ProjectFileLease>&);
    friend class StoreFileSave;
    CopyState saveCopyImpl(const std::filesystem::path&, StoreFileCollision, StoreCopyPolicy,
                           std::stop_token, StoreSaveKind = StoreSaveKind::copy) const;
    static void renewProjectUuid(SQLite::Database&);
    static CopyState writeFileCopy(const std::filesystem::path&, const std::filesystem::path&,
        const std::shared_ptr<media::ProjectFileLease>&, int, StoreFileCollision, StoreSaveKind,
        const std::function<void(SQLite::Database&)>&, std::stop_token = {}, const StoreFileSave::Progress& = {},
        const StoreFileSave* pending = nullptr, StoreCopyPolicy = StoreCopyPolicy::preserve);
    StoreFileResult switchToCopy(const std::filesystem::path&, const CopyState&, gesture::detail::NavigationPermit&,
        std::unique_ptr<Store>& retired, const StoreFileSave* pending = nullptr);
    StoreFileResult saveAsImpl(const std::filesystem::path&,StoreFileCollision,StoreSaveKind,
                               const media::FileIdentity* expectedTarget);
    std::function<bool()> untitledCleanup_;
    bool untitledCleanupReady_=false;
    bool applySessionPragmas();
    // Source invalidation has already completed; fold WAL and release SQLite.
    bool closeFinalized();

    // SQLite closes before its native shared file lease is released.
    std::shared_ptr<media::ProjectFileLease> fileLease_;
    std::unique_ptr<SQLite::Database> db_;
    std::filesystem::path path_;
    bool readOnly_ = false;
    bool closed_ = false;
    bool synchronizedLocation_ = false;
    int major_ = kSchemaMajor;
    int minor_ = kSchemaMinor;
    std::optional<int> upgradedFrom_;
    mutable std::string lastError_;
    std::string clientId_;
    std::string clientLabel_;
    std::filesystem::path upgradeBackup_;
    std::optional<OpeningHistoryPoint> openingHistory_;
};

// --- upgrading an older 1.x file (ADR-0144, SPEC §11) -------------------------
//
// Public so tests can drive it on a connection they instrument. Store::open is
// the only production caller.

/// One step per minor: the schema objects (tables, indexes, triggers) that
/// minor added, by name. Minor 1.9 also rebuilds project to change its default. Their DDL is taken from the embedded schema.sql, so a
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
