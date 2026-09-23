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
    };
    [[nodiscard]] std::vector<Branch> branches() const;

    /// Move to another branch's head. The project is rewound to the common
    /// ancestor and replayed forward, so this is a real navigation, not a label
    /// change. The branch left keeps its head, so nothing becomes unreachable.
    Result switchToBranch(std::int64_t branchId);

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
    };

    struct SnapshotResult {
        bool ok = false;
        std::string error;
        std::int64_t id = 0;
        std::string name;   // as stored -- the default name when none was given
    };

    /// Names the current head. An empty `name` becomes
    /// defaultSnapshotName(project name, createdUtc, utcOffsetMinutes), the
    /// local time being UTC plus the offset the CALLER supplies (ADR-0128 d4).
    SnapshotResult takeSnapshot(const std::string& name, std::int64_t createdUtc,
                                int utcOffsetMinutes, bool automatic = false);

    /// Renames in place. Not an op and not undoable (ADR-0128 d5). An empty name
    /// is refused rather than defaulted: a rename is always a typed name.
    Result renameSnapshot(std::int64_t snapshotId, const std::string& newName);

    /// Every snapshot, oldest first (created_utc, then id).
    [[nodiscard]] std::vector<Snapshot> listSnapshots() const;

    /// ADR-0128 d2: never discards. Moves the head to the snapshot's point,
    /// rewinding to the common ancestor and replaying forward, which works
    /// across branches. Whatever the head's line held beyond that point stays
    /// reachable: if no other branch already holds its tip, a branch named
    /// "before revert to '<name>'" is created for it, stamped `nowUtc`.
    Result revertToSnapshot(std::int64_t snapshotId, std::int64_t nowUtc);

    /// "<Project Name> <YYYY-MM-DD HH:MM>" at UTC + `utcOffsetMinutes`. An
    /// unnamed project is "Untitled". Pure: exposed so the format is testable.
    static std::string defaultSnapshotName(const std::string& projectName,
                                           std::int64_t utcMicros, int utcOffsetMinutes);

private:
    Store& store_;
    OpJournal journal_;
};

}  // namespace adi
