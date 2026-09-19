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
    /// change.
    Result switchToBranch(std::int64_t branchId);

private:
    Store& store_;
    OpJournal journal_;
};

}  // namespace adi
