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
// IT ALSO REFRESHES THE PROJECT VIEW, AND THAT IS WHY IT EXISTS RATHER THAN
// CALLING `OpJournal` DIRECTLY. ADR-0201 makes `ProjectView` the UI's one cell
// of project state and requires a refresh "after a load and after every
// committed op, undo and redo". A commit that does not refresh leaves every
// window drawing the previous project, silently and permanently -- so the
// refresh belongs to the thing that commits, not to each of its callers.
//
// Only a SUCCESSFUL write refreshes. A refused op wrote nothing, so rebuilding
// would burn a generation and tell every component to re-read an unchanged
// project.
//
// Undo and redo live here for the same reason: they are writes, they are the
// two whose omission ADR-0201 names, and routing them anywhere else would put
// the refresh back on the call site.
//
// ONE UNDO STEP IS A BATCH, AND THAT IS ALREADY THE ENGINE'S RULE.
// `OpJournal::commit` takes a span and applies it as one transaction: however
// many ops it contains they share a txn_id and revert together, and a failure
// anywhere rolls back every op AND its log rows, so a half-applied batch cannot
// be left describing itself as complete. There is nothing for this type to add
// and no RAII scope to invent — `submit(span)` is the mechanism.
//
// A PREPARED BATCH IS THE ENGINE'S, AND ITS HELPER COMMITS IT. The engine
// prepares an edit off the callback (a split, a device paste, a built-in
// insert, a sample assignment) as ops plus the undo head it expects
// (ADR-0153), and its own `commit*` helper refuses when the project is no
// longer the file it was prepared on or the head has moved. `commitPrepared`
// runs that helper against this submitter's store and adds only what `submit`
// adds: the actor stamp, the refresh, `stale` and `headFound`. The window may
// put ONE op of its own on the end, so "split here and deactivate the right
// part" is one undo step.
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

#include "adi/engine/project_view.hpp"
#include "adi/history.hpp"
#include "adi/ops.hpp"

#include <concepts>
#include <cstdint>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

namespace adi {
class Store;
}

namespace adi::ui {

/// A batch the engine prepared (ADR-0153): its ops, and in `options` the undo
/// head it expects. `clip_edit::PreparedEdit`, `PreparedDevicePaste`,
/// `PreparedBuiltinInsert` and `PreparedSampleAssignment` all have this shape.
template <class P>
concept PreparedBatch = requires(P& p) {
    { p.requests } -> std::same_as<std::vector<OpRequest>&>;
};

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
    /// ADR-0153: the engine refused a prepared batch. Nothing was written, and
    /// the UI should re-read and re-ask rather than retrying blindly. Two
    /// causes, told apart by `headFound`: the undo head moved (`headFound` is
    /// the head the journal met), or the project is no longer the file the
    /// batch was prepared on (`headFound` empty; the helper's own check, made
    /// before any transaction). A refusal worded "the project moved" fits
    /// only the first.
    bool stale = false;
    std::optional<std::int64_t> headFound;
    /// The `ProjectView` generation after the refresh this write triggered.
    /// 0 when nothing was written, so a caller can tell "the project moved"
    /// from "it did not" without comparing snapshots.
    std::uint64_t generation = 0;

    explicit operator bool() const noexcept { return ok; }
};

/// Submits ops on behalf of one actor.
///
/// Holds a `Store&` and nothing else: no snapshot, no row id it looked up, no
/// cached selection. Construct it where the store lives and pass it down.
class OpSubmitter {
public:
    OpSubmitter(Store& store, engine::ProjectView& view, Actor actor = Actor::User,
                std::string actorDetail = {})
        : store_(store), view_(view), actor_(actor), actorDetail_(std::move(actorDetail)) {}

    OpSubmitter(const OpSubmitter&) = delete;
    OpSubmitter& operator=(const OpSubmitter&) = delete;

    /// One op, one undo step.
    SubmitResult submit(OpRequest request) {
        stamp(request);
        return run(OpJournal(store_).commit(request));
    }

    /// Undo the last transaction and rebuild the view (ADR-0201).
    ///
    /// `selBefore` comes back untouched: restoring the selection is the view's
    /// business and ignoring it is always correct.
    History::Result undo() {
        if (atRoot()) {
            History::Result none;
            none.error = "Nothing to undo";
            return none;
        }
        auto r = History(store_).undo();
        if (r.ok) view_.refresh(store_, overrides());
        return r;
    }

    History::Result redo() {
        auto r = History(store_).redo();
        if (r.ok) view_.refresh(store_, overrides());
        return r;
    }

    /// Live's Undo History (manual 5.4.2): the whole history, newest first, every branch, with its
    /// snapshots (#387). A read; nothing moves.
    [[nodiscard]] History::Timeline timeline() const { return History(store_).readTimeline(); }

    /// Back to the project as it stood just after the step `txnId`: undone or redone to it along the
    /// current line, and, for a step on another branch, that branch first (`switchToBranch`), then
    /// undone to it. The view is refreshed ONCE, at the end, when anything moved; a refusal part way
    /// refreshes for what did move and reports the refusal. Never undoes past the protected root.
    /// `txnId` 0 is the history's root, the project as created or opened: every step on the current
    /// line undone, down to the protected root where there is one.
    History::Result restore(std::int64_t txnId) {
        History history(store_);
        History::Result last;
        if (txnId == 0) {
            bool movedToRoot = false;
            last.ok = true;
            while (!atRoot() && history.canUndo()) {
                auto r = history.undo();
                if (!r.ok) {
                    last = std::move(r);
                    break;
                }
                movedToRoot = true;
            }
            if (movedToRoot) view_.refresh(store_, overrides());
            return last;
        }
        auto timeline = history.readTimeline();
        if (!timeline.error.empty()) {
            last.error = timeline.error;
            return last;
        }
        const auto find = [&timeline, txnId]() -> const History::TimelineStep * {
            for (const auto& s : timeline.steps)
                if (s.step.txnId == txnId) return &s;
            return nullptr;
        };
        const auto* target = find();
        if (target == nullptr) {
            last.error = "That step is no longer in the history.";
            return last;
        }
        bool moved = false;
        const auto finish = [&](History::Result r) {
            if (moved) view_.refresh(store_, overrides());
            return r;
        };
        if (!target->onCurrentLine) {
            if (target->branchIds.empty()) {
                last.error = "That step is on no branch to switch to.";
                return finish(last);
            }
            last = history.switchToBranch(target->branchIds.front());
            if (!last.ok) return finish(last);
            moved = true;
            timeline = history.readTimeline();
            target = find();
            if (target == nullptr || !timeline.error.empty()) {
                last.ok = false;
                last.error = "The branch switched, but the step was not found on it.";
                return finish(last);
            }
        }
        const auto seq = target->step.lastSeq;
        const bool back = target->applied;
        for (std::size_t guard = 0; guard <= timeline.steps.size() && history.head() != seq; ++guard) {
            if (back && atRoot()) break;
            auto r = back ? history.undo() : history.redo();
            if (!r.ok) return finish(r);
            moved = true;
            last = std::move(r);
        }
        last.ok = history.head() == seq;
        if (!last.ok && last.error.empty()) last.error = "The history could not reach that step.";
        return finish(last);
    }

    /// The session's overridden lanes (ADR-0162), handed to every refresh this
    /// submitter makes, a commit, an undo or a redo, so the snapshot's
    /// `automationOverridden` and each lane's `overridden` read the session's
    /// truth and not the offline view's "nothing overridden" (N26). Without
    /// one, the view is the no-session view, as `ProjectView::refresh` says.
    /// The set is read at each refresh, never copied: the session's own, which
    /// outlives this submitter's last write. An override the session finds
    /// only when it re-reads the rows, AFTER this refresh, is the document's to
    /// reconcile (`ProjectDocument::synchronise`).
    void followOverrides(const std::set<std::int64_t>& lanes) noexcept { overridden_ = &lanes; }

    /// What the undo and redo menu items show and whether they are enabled.
    /// Passed through rather than wrapped: `History::Step::label` is already
    /// "what the undo menu shows".
    [[nodiscard]] bool canUndo() const { return nextUndo().has_value(); }
    [[nodiscard]] bool canRedo() const { return History(store_).canRedo(); }
    [[nodiscard]] std::optional<History::Step> nextUndo() const {
        auto step = History(store_).nextUndo();
        return step && isRoot(*step) ? std::nullopt : step;
    }

    /// Live's rule (manual 5.4.2): creating a project is the first entry in its
    /// history and cannot be undone. Given that entry's label, this submitter
    /// treats the log's first transaction under it as the floor: undo stops
    /// there and says there is nothing to undo. Empty, nothing is protected.
    void protectRoot(std::string label) { root_ = std::move(label); }
    [[nodiscard]] std::optional<History::Step> nextRedo() const { return History(store_).nextRedo(); }

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

    /// A PREPARED batch, through the engine's own commit helper (ADR-0153).
    ///
    /// The engine prepares an edit off the callback -- `clip_edit::prepareSplit`,
    /// `prepareDevicePaste`, `prepareBuiltinInsert`, `prepareSampleAssignment` --
    /// as ops plus the undo head it expects, and its `commit*` helper checks
    /// two things the journal alone cannot: that the project is still the file
    /// the batch was prepared on, and, inside the transaction, that the head
    /// has not moved. Neither check belongs here; what belongs here is what
    /// `submit` does around a commit and no caller should repeat: the actor
    /// stamp, the refresh on success, `stale` and `headFound` on a moved head.
    /// So this runs `helper(store, prepared, actor)` against this submitter's
    /// store and hands the result to the same `run` as `submit`.
    ///
    /// `appended` is the window's ONE op of its own on the end of the batch,
    /// so "split here and deactivate the right part" is one undo step: a
    /// `clip.setMute` after `prepareSplit`. One, not many: a second op of the
    /// window's is a second thing the batch is about, and that is a prepared
    /// edit for the engine to offer.
    ///
    /// By value: a prepared batch is used once, because the head it expects is
    /// stale the moment it commits. Move it in.
    template <class Prepared, class Helper>
        requires PreparedBatch<Prepared> &&
                 std::is_invocable_r_v<CommitResult, Helper&, Store&, Prepared&, Actor>
    SubmitResult commitPrepared(Prepared prepared, Helper helper,
                                std::optional<OpRequest> appended = std::nullopt) {
        for (auto& r : prepared.requests) stamp(r);
        if (appended) {
            stamp(*appended);
            prepared.requests.push_back(std::move(*appended));
        }
        return run(helper(store_, prepared, actor_));
    }

    /// The same for a batch the helper owns but whose requests are in reach
    /// (`clip_edit::MaterialEdit::edit.requests`, for `commitMaterial`): the
    /// requests are stamped first, as `submit` stamps them, then the helper
    /// runs with this submitter's actor. Neither `commitMaterial` nor
    /// `commitPrepared` has the `(Store&, Actor)` shape itself: wrap it in a
    /// lambda that captures the edit,
    ///
    ///     sub.commitPrepared(material.edit.requests,
    ///         [&](Store& s, Actor a) { return clip_edit::commitMaterial(s, material, a); });
    ///
    /// The engine's helpers set the actor on every request and leave the
    /// detail alone, so the stamp survives the commit. Nothing to append to:
    /// an import or a bounce is the whole of what its batch is about.
    template <class Helper>
        requires std::is_invocable_r_v<CommitResult, Helper&, Store&, Actor>
    SubmitResult commitPrepared(std::vector<OpRequest>& requests, Helper helper) {
        for (auto& r : requests) stamp(r);
        return run(helper(store_, actor_));
    }

    /// And for a batch whose ops the helper keeps entirely to itself
    /// (`commitTrackBounce`, whose requests sit behind `TrackBounce::Data`):
    /// the actor goes in through the helper, again a lambda that captures the
    /// bounce and the document generation it takes. KNOWN LIMIT: this form
    /// cannot stamp `actorDetail`, so an agent's bounce lands with the detail
    /// empty until `commitTrackBounce` takes one (the engine lane's).
    template <class Helper>
        requires std::is_invocable_r_v<CommitResult, Helper&, Store&, Actor>
    SubmitResult commitPrepared(Helper helper) {
        return run(helper(store_, actor_));
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

    SubmitResult run(CommitResult c) {
        SubmitResult s;
        s.ok = c.ok;
        s.txnId = c.txnId;
        s.seqs = std::move(c.seqs);
        s.error = std::move(c.error);
        s.issues = std::move(c.issues);
        s.stale = c.stale;
        s.headFound = c.headFound;
        // A prepared batch with nothing in it reports success and no seqs, as
        // `submit` of an empty vector does: nothing was written, so nothing
        // refreshes (ADR-0201 refreshes after a COMMITTED op).
        if (s.ok && !s.seqs.empty()) s.generation = view_.refresh(store_, overrides());
        return s;
    }

    [[nodiscard]] const std::set<std::int64_t>& overrides() const noexcept {
        static const std::set<std::int64_t> none;
        return overridden_ ? *overridden_ : none;
    }

    [[nodiscard]] bool isRoot(const History::Step& step) const {
        return !root_.empty() && step.firstSeq == 1 && step.label == root_;
    }
    [[nodiscard]] bool atRoot() const {
        const auto step = History(store_).nextUndo();
        return step && isRoot(*step);
    }
    Store& store_;
    engine::ProjectView& view_;
    Actor actor_;
    std::string actorDetail_;
    std::string root_;
    const std::set<std::int64_t>* overridden_ = nullptr;
};

}  // namespace adi::ui
