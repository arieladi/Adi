// SPDX-License-Identifier: GPL-3.0-or-later
//
// The UI's write half. ADR-0124, ADR-0021, ADR-0180.
//
// Every edit the UI makes is an op. This wraps `OpJournal` so a component says
// what it did and not how the journal is spelled: it fills in the actor, it
// turns a batch into ONE undo step, and it hands back a result a UI can act on.
// It owns no project state and caches nothing, so it is the write half of the
// same rule `SnapshotReader` is the read half of.
//
// ONE UNDO STEP IS A BATCH, AND THAT IS ALREADY THE ENGINE'S RULE.
// `OpJournal::commit` takes a span and applies it as one transaction: however
// many ops it contains they share a txn_id and revert together, and a failure
// anywhere rolls back every op AND its log rows, so a half-applied batch cannot
// be left describing itself as complete. There is nothing for this type to add
// and no RAII scope to invent — `submit(span)` is the mechanism.
//
// WHAT THIS DELIBERATELY DOES NOT DO: COALESCE A DRAG.
//
// A fader drag emits many values and must leave ONE undo entry. An earlier
// draft of this type did that here, with a "replace the last op" helper guarded
// by a hand-written list of ops it judged safe. That was wrong twice over, and
// the second reason is the one worth recording:
//
//   1. The list was derived from the wrong test. It asked "does this op's
//      inverse refuse when the row is missing?" The criterion the codebase
//      actually states is different — `ops_catalog.cpp` says of the summing ops
//      that "an op that finds none reads the DDL's defaults, so 'no row' and
//      'a row of defaults' are the same state and the inverse of either
//      restores it." The question is whether the default the inverse writes
//      EQUALS the pre-op state, not whether the builder returns false. Several
//      inverse builders substitute a default and commit — `summingInverse` is
//      one, and it backs `group.setSummingDrive`, which is itself a coalescable
//      dragged dial. A whitelist built on the wrong discriminator admits
//      exactly the ops it was written to exclude.
//
//   2. It was a re-derivation of something that already has an owner.
//      ADR-0124 is where value edits and their openers are decided, and
//      `ParamOps` implements it: a first edit emits its opener so undo lands on
//      a value rather than on a row that did not exist. ADR-0181 d5 states the
//      rule plainly — every value edit goes through the capture. A second,
//      quieter coalescer in the UI layer is how the two disagree, and undo
//      corruption is silent.
//
// So: parameter drags route through `ParamOps`, not through this type. When
// the UI needs to coalesce something that is NOT a parameter, that is a
// question for win with ADR-0124's criterion in hand — not a list maintained
// here. Step 7.1 ships the submit path and stops.

#pragma once

#include "adi/ops.hpp"

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace adi {
class Store;
}

namespace adi::ui {

/// What the caller gets back. A narrowed `CommitResult`: the fields a UI can
/// actually act on, and nothing invented.
///
/// There is deliberately no "engine impact" summary of a batch here.
/// `EngineImpact` is a plain enum with no documented ordering, so "the
/// strongest impact in the batch" would be a ranking this file made up, and a
/// new enumerator would silently change it. A caller that needs the impact
/// asks the catalogue about a specific op type.
struct SubmitResult {
    bool ok = false;
    std::int64_t txnId = 0;
    std::vector<std::int64_t> seqs;          ///< one per op, in order
    std::string error;
    std::vector<ValidationIssue> issues;
    /// ADR-0153: refused because the undo head had moved. Nothing was written,
    /// and the UI should re-read and re-ask rather than retrying blindly.
    bool stale = false;
    std::optional<std::int64_t> headFound;

    explicit operator bool() const noexcept { return ok; }
};

/// Submits ops on behalf of one actor.
///
/// Holds a `Store&` and nothing else: no snapshot, no row id it looked up, no
/// cached selection. Construct it where the store lives and pass it down.
class OpSubmitter {
public:
    explicit OpSubmitter(Store& store, Actor actor = Actor::User, std::string actorDetail = {})
        : store_(store), actor_(actor), actorDetail_(std::move(actorDetail)) {}

    OpSubmitter(const OpSubmitter&) = delete;
    OpSubmitter& operator=(const OpSubmitter&) = delete;

    /// One op, one undo step.
    SubmitResult submit(OpRequest request) {
        stamp(request);
        return run(OpJournal(store_).commit(request));
    }

    /// Several ops as ONE undo step. Empty is a no-op that reports success
    /// without opening a transaction, because "the user selected nothing and
    /// pressed delete" is not an error and must not leave an empty undo entry.
    ///
    /// This does NOT strip `selBefore` from the ops after the first. A draft of
    /// this file did, and a planted defect proved the line dead: `ops.cpp`
    /// writes the column only when `i == 0`, so ADR-0021's "first op of a
    /// transaction only" is the journal's guarantee and re-implementing it here
    /// bought a second place for the two to disagree. The suite still checks
    /// the guarantee, because this type leans on it.
    SubmitResult submit(std::vector<OpRequest> requests) {
        if (requests.empty()) {
            SubmitResult none;
            none.ok = true;
            return none;
        }
        for (auto& r : requests) stamp(r);
        return run(OpJournal(store_).commit(std::span<const OpRequest>(requests)));
    }

    [[nodiscard]] Actor actor() const noexcept { return actor_; }
    [[nodiscard]] const std::string& actorDetail() const noexcept { return actorDetail_; }

private:
    /// Fills in who is acting, without overwriting a caller that said so. A UI
    /// component should not have to name the actor on every edit, and an agent
    /// submitting through its own submitter must not be relabelled User.
    void stamp(OpRequest& r) const {
        r.actor = actor_;
        if (r.actorDetail.empty()) r.actorDetail = actorDetail_;
    }

    static SubmitResult run(CommitResult c) {
        SubmitResult s;
        s.ok = c.ok;
        s.txnId = c.txnId;
        s.seqs = std::move(c.seqs);
        s.error = std::move(c.error);
        s.issues = std::move(c.issues);
        s.stale = c.stale;
        s.headFound = c.headFound;
        return s;
    }

    Store& store_;
    Actor actor_;
    std::string actorDetail_;
};

}  // namespace adi::ui
