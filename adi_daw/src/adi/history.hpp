// SPDX-License-Identifier: GPL-3.0-or-later
//
// Undo, redo, and the branching history tree. SPEC §8.2, ADR-0003, ADR-0030.
//
// Three properties make this different from an in-memory undo stack, and they
// are the reason the op log was put in the file at all (RATIONALE §5.3):
//
//   * It SURVIVES A RESTART. Close the project, reopen it tomorrow, press
//     Ctrl-Z, and the edit from yesterday comes back.
//   * It BRANCHES. Undoing and then doing something new does not destroy the
//     line you left; that line is preserved as a branch you can return to.
//     For an AI-assisted DAW this is the difference between "try the agent's
//     arrangement" being usable and being frightening.
//   * It UNDOES A WHOLE ACTION. Ops sharing a txn_id revert together, so an
//     agent request that made forty edits is one Ctrl-Z, not forty.
//
// Undo and redo do NOT append to the log -- they move a pointer along it. See
// ADR-0030 for why, and for why that does not violate ADR-0003.

#pragma once

#include "adi/history_compaction.hpp"
#include "adi/history_policy.hpp"
#include "adi/ops.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace adi {

class Store;

class History {
public:
    explicit History(Store&);
    // A cross-baseline restore invalidates stateful runtime consumers. The
    // caller explicitly prepares a stopped/reloadable Session, then reloads it.
    enum class StoredRestore { Refuse, SessionReloadPrepared };

    /// What Ctrl-Z / Ctrl-Y would act on. Describes a whole transaction, since
    /// that is the unit both operate on.
    struct Step {
        std::int64_t txnId = 0;
        std::int64_t firstSeq = 0;
        std::int64_t lastSeq = 0;
        int opCount = 0;
        std::string label;     // what the undo menu shows
        std::string actor;     // 'user', 'agent', ...
        std::string actorDetail;
    };

    [[nodiscard]] bool canUndo() const { return nextUndo().has_value(); }
    [[nodiscard]] bool canRedo() const { return nextRedo().has_value(); }
    [[nodiscard]] std::optional<Step> nextUndo() const;
    [[nodiscard]] std::optional<Step> nextRedo() const;

    struct Result {
        bool ok = false;
        std::string error;
        std::int64_t txnId = 0;
        int opsApplied = 0;
        /// ADR-0021 §7.5. What was selected when the undone transaction began.
        /// Purely a UI hint -- restoring it is the view's business, and ignoring
        /// it is always correct.
        std::optional<Payload> selBefore;
        bool sessionReloadRequired = false;
    };

    Result undo();
    Result redo();

    /// NULL means the branch is at its root: nothing done, nothing to undo.
    [[nodiscard]] std::optional<std::int64_t> head() const;

    struct Branch {
        std::int64_t id = 0;
        std::string name;
        std::optional<std::int64_t> headSeq;
        bool isCurrent = false;
        std::int64_t createdUtc = 0;
        std::optional<std::string> storedStateHash = std::nullopt;
    };
    [[nodiscard]] std::vector<Branch> branches() const;

    /// Move to another branch's head. The project is rewound to the common
    /// ancestor and replayed forward, so this is a real navigation, not a label
    /// change. The branch left keeps its head, so nothing becomes unreachable.
    Result switchToBranch(std::int64_t branchId, StoredRestore = StoredRestore::Refuse);

    // Caps are project metadata. Setting a policy also compacts atomically.
    UndoCompactionResult setUndoRetentionPolicy(const UndoRetentionPolicy &);
    UndoCompactionResult compactUndo();
    struct ReplayBaseResult {
        bool ok = false;
        std::int64_t baseSeq = 0;
        std::string error;
        std::vector<std::int64_t> retainedSeqs; // current ancestry, oldest first
    };
    // Hold a source read transaction across this call and retained-log reads.
    // Restore the source checkpoint into a journal-empty target, preserving its
    // UI metadata. Replay exactly retainedSeqs in order; other roots/branches
    // and the current redo tail are not part of the materialized current state.
    static ReplayBaseResult restoreReplayBase(const Store &source, Store &target);
    // Open an archived snapshot into an empty target without modifying the
    // source, including a read-only source. A point still in stepwise history
    // is refused; it uses ordinary copy/navigation. No retained replay is needed.
    static ReplayBaseResult restoreStoredSnapshot(const Store &source, std::int64_t snapshotId,
                                                  Store &target);

    // --- snapshots (ADR-0128, SPEC 8.6) ---------------------------------------
    //
    // A snapshot is a name on a point in the log; it copies nothing. Taking,
    // renaming and reverting are history METADATA, not ops (ADR-0128 d5): they
    // append nothing to the log and are not undoable.
    //
    // Nothing here reads a clock or a time zone. Every time is an argument, so
    // a test -- or a replay -- gets the same names and rows every run.

    struct Snapshot {
        std::int64_t id = 0;
        std::string name;
        std::optional<std::int64_t> opSeq;   // nullopt: the root, before any op
        std::int64_t branchId = 0;           // current when taken; a record, not a pointer
        std::int64_t createdUtc = 0;         // microseconds, UTC
        bool automatic = false;
        std::optional<std::string> storedStateHash = std::nullopt;
    };

    struct TimelineStep {
        Step step; // whole transaction, label/actor from its first edit op
        std::optional<std::int64_t> parentSeq; // before this transaction
        std::int64_t createdUtc = 0;
        bool applied = false; // on the current head's ancestry
        bool onCurrentLine = false; // ancestry plus the redo line Ctrl-Y follows
        std::vector<std::int64_t> branchIds;
        std::vector<std::int64_t> snapshotIds;
    };
    struct Timeline {
        std::vector<TimelineStep> steps; // newest lastSeq first, every branch; excludes the checkpoint root
        std::vector<Branch> branches; // id order; current head moves with undo
        std::vector<Snapshot> snapshots; // newest createdUtc/id first, including root
        std::optional<std::int64_t> headSeq, currentBranchId;
        std::int64_t baseSeq = 0; // retained structural root; zero when history has not compacted
        std::string error; // no partial data on failure
    };

    /// Message-thread metadata read, outside journal mutations. One consistent
    /// read transaction; only structural hash payloads are decoded, never edit
    /// payloads, inverses or full-state blobs. Ephemeral
    /// performance/session ops and the checkpoint root are excluded from steps.
    /// Root snapshots remain in snapshots, including anchors exactly at baseSeq;
    /// branch heads and step parents retain their stored sequence values.
    /// branchIds names stored-head ancestry; the current branch also includes
    /// its redo line. No branch/snapshot metadata or undo head is changed.
    [[nodiscard]] Timeline readTimeline() const;

    struct SnapshotResult {
        bool ok = false;
        std::string error;
        std::int64_t id = 0;
        std::string name;   // as stored -- the default name when none was given
    };

    /// Names the current head. An empty `name` becomes
    /// defaultSnapshotName(project name, createdUtc, utcOffsetMinutes), the
    /// local time being UTC plus the offset the CALLER supplies (ADR-0128 d4).
    SnapshotResult takeSnapshot(const std::string &name, std::int64_t createdUtc, int utcOffsetMinutes,
                                bool automatic = false, const HistorySnapshotPolicy &policy = {});

    /// Explicit user removal of snapshot metadata; never an undo operation.
    Result removeSnapshot(std::int64_t snapshotId);

    /// Renames in place and promotes automatic points to named. Not an op and
    /// not undoable (ADR-0128 d5). An empty name is refused.
    Result renameSnapshot(std::int64_t snapshotId, const std::string& newName);

    /// Every snapshot, oldest first (created_utc, then id).
    [[nodiscard]] std::vector<Snapshot> listSnapshots() const;

    /// ADR-0128 d2: never discards. Moves the head to the snapshot's point,
    /// rewinding to the common ancestor and replaying forward, which works
    /// across branches. Whatever the head's line held beyond that point stays
    /// reachable: if no other branch already holds its tip, a branch named
    /// "before revert to '<name>'" is created for it, stamped `nowUtc`.
    Result revertToSnapshot(std::int64_t snapshotId, std::int64_t nowUtc,
                            StoredRestore = StoredRestore::Refuse);

    /// "<Project Name> <YYYY-MM-DD HH:MM>" at UTC + `utcOffsetMinutes`. An
    /// unnamed project is "Untitled". Pure: exposed so the format is testable.
    static std::string defaultSnapshotName(const std::string& projectName,
                                           std::int64_t utcMicros, int utcOffsetMinutes);

private:
    Store& store_;
    OpJournal journal_;
};

}  // namespace adi
