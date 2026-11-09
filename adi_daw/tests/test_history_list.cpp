// SPDX-License-Identifier: GPL-3.0-or-later
#include "temp_directory.hpp"
#include "adi/digest.hpp"
#include "adi/history.hpp"
#include "adi/ops.hpp"
#include "adi/store.hpp"

#include <SQLiteCpp/SQLiteCpp.h>
#include <algorithm>
#include <array>
#include <cstdio>
#include <stdexcept>

namespace {
using namespace adi;
int checks = 0, failures = 0;
void check(bool ok, const std::string& what) {
    ++checks;
    if (!ok) { ++failures; std::printf("FAIL %s\n", what.c_str()); }
}
struct Fixture {
    test::TempDirectory temp;
    std::unique_ptr<Store> store;
    explicit Fixture(const char* name) : temp("history_list", name) {
        StoreError error = StoreError::Ok;
        store = Store::create(temp.path() / "song.adi", error);
        if (!store) throw std::runtime_error(toString(error));
    }
};
OpRequest request(const char* type, Payload payload, const char* label) {
    OpRequest result; result.opType = type; result.payload = std::move(payload); result.label = label;
    return result;
}
const History::TimelineStep* find(const History::Timeline& timeline, std::int64_t txn) {
    for (const auto& step : timeline.steps) if (step.step.txnId == txn) return &step;
    return nullptr;
}
bool has(const std::vector<std::int64_t>& ids, std::int64_t id) {
    return std::find(ids.begin(), ids.end(), id) != ids.end();
}
UndoRetentionPolicy budget(std::int64_t steps) {
    UndoRetentionPolicy result; result.maxTransactions = steps; return result;
}
CommitResult rename(Fixture& fixture, const std::string& name) {
    const auto result = OpJournal(*fixture.store).commit(
        request("project.setName", {{"name", name}}, name.c_str()));
    if (!result.ok) throw std::runtime_error(result.error);
    return result;
}
void names(Fixture& fixture, int first, int last) {
    for (int value = first; value <= last; ++value) (void)rename(fixture, std::to_string(value));
}
std::string projectName(Fixture& fixture) {
    return fixture.store->db().execAndGet("SELECT name FROM project WHERE id=1").getString();
}
const History::Branch* branch(const History::Timeline& timeline, std::int64_t id) {
    for (const auto& entry : timeline.branches) if (entry.id == id) return &entry;
    return nullptr;
}
const History::Snapshot* snapshot(const History::Timeline& timeline, std::int64_t id) {
    for (const auto& entry : timeline.snapshots) if (entry.id == id) return &entry;
    return nullptr;
}
bool clearedFailure(const History::Timeline& timeline) {
    return !timeline.error.empty() && timeline.baseSeq == 0 && timeline.steps.empty() &&
        timeline.branches.empty() && timeline.snapshots.empty() && !timeline.headSeq && !timeline.currentBranchId;
}

void timeline() {
    Fixture f("branches");
    History history(*f.store); OpJournal journal(*f.store);
    auto empty = history.readTimeline();
    check(empty.error.empty() && empty.baseSeq == 0 && empty.steps.empty() && !empty.headSeq && empty.currentBranchId == 1 &&
              empty.branches.size() == 1 && empty.snapshots.empty(), "fresh project has a root branch and no steps");
    const auto root = history.takeSnapshot("Root", 1, 0);
    check(root.ok, "root snapshot");
    std::array pair = {request("track.create", {{"id", 1}, {"kind", "audio"}, {"name", "A"}}, "Add two tracks"),
                       request("track.create", {{"id", 2}, {"kind", "audio"}, {"name", "B"}}, "Internal helper")};
    pair[0].actor = Actor::Agent; pair[0].actorDetail = "test actor";
    const auto a = journal.commit(pair);
    check(a.ok, "multi-op transaction");
    const auto markA = history.takeSnapshot("Pair", 2, 0);
    const auto b = journal.commit(request("track.rename", {{"id", 1}, {"name", "B name"}}, "Name A"));
    const auto c = journal.commit(request("track.rename", {{"id", 2}, {"name", "C name"}}, "Name B"));
    const auto markC = history.takeSnapshot("Automatic", 3, 0, true);
    check(markA.ok && b.ok && c.ok && markC.ok, "linear history and snapshot setup");
    const auto ephem = journal.commit(request("transport.play", Payload::object(), "Performance event"));
    check(ephem.ok, "ephemeral setup");
    check(history.undo().ok, "undo C");
    const auto d = journal.commit(request("track.rename", {{"id", 2}, {"name", "D name"}}, "Different name B"));
    check(d.ok, "forked edit D");
    const auto beforeCount = journal.count();
    auto view = history.readTimeline();
    check(view.error.empty() && view.steps.size() == 4 && view.branches.size() == 2 && view.snapshots.size() == 3,
          "every transaction across both branches and all snapshot anchors");
    if (!view.error.empty() || view.steps.size() != 4) { check(false, view.error); return; }
    check(view.steps[0].step.txnId == d.txnId && view.steps[1].step.txnId == c.txnId &&
              view.steps[2].step.txnId == b.txnId && view.steps[3].step.txnId == a.txnId,
          "newest-first journal order, one row per whole transaction");
    const auto* batch = find(view, a.txnId);
    check(batch && batch->step.opCount == 2 && batch->step.firstSeq == a.seqs.front() &&
              batch->step.lastSeq == a.seqs.back() && batch->step.label == "Add two tracks" &&
              batch->step.actor == "agent" && batch->step.actorDetail == "test actor" && !batch->parentSeq,
          "transaction label and actor come from its first op, with complete bounds");
    check(view.headSeq == d.seqs.back() && view.currentBranchId == 1 &&
              find(view, d.txnId)->parentSeq == b.seqs.back(), "current head and tree parent are explicit");
    const auto abandoned = view.branches[1].id;
    check(find(view, d.txnId)->applied && find(view, d.txnId)->onCurrentLine &&
              !find(view, c.txnId)->applied && !find(view, c.txnId)->onCurrentLine &&
              has(find(view, c.txnId)->branchIds, abandoned) && has(batch->branchIds, abandoned) &&
              has(batch->branchIds, 1), "active and alternate paths share the ancestor transaction");
    check(view.snapshots[0].id == markC.id && view.snapshots[0].automatic && view.snapshots[2].id == root.id &&
              !view.snapshots[2].opSeq && has(find(view, c.txnId)->snapshotIds, markC.id) &&
              has(batch->snapshotIds, markA.id), "snapshots newest first, root anchor retained, per-step anchors linked");
    check(journal.count() == beforeCount && history.head() == view.headSeq, "read does not append or move history");
    check(history.undo().ok, "undo D");
    const auto undone = history.readTimeline();
    check(undone.error.empty() && !find(undone, d.txnId)->applied && find(undone, d.txnId)->onCurrentLine &&
              has(find(undone, d.txnId)->branchIds, 1) && history.nextRedo()->txnId == d.txnId &&
              history.nextUndo()->txnId == b.txnId, "undone row is on current redo line, matching Ctrl-Z/Y");
    check(view.headSeq == d.seqs.back() && find(view, d.txnId)->applied, "held value does not change with runtime history");
    check(history.switchToBranch(abandoned).ok, "switch to saved branch");
    const auto switched = history.readTimeline();
    check(switched.error.empty() && switched.currentBranchId == abandoned && find(switched, c.txnId)->applied &&
              !find(switched, d.txnId)->applied, "branch switch updates applied ancestry");
    // Wall clock times can move backwards; journal sequence is still order.
    f.store->db().exec("UPDATE ops SET ts_utc=-seq");
    const auto backwardsClock = history.readTimeline();
    check(backwardsClock.error.empty() && backwardsClock.steps[0].step.txnId == d.txnId &&
              find(backwardsClock, a.txnId)->createdUtc == -a.seqs.front(), "timestamps are exposed but do not reorder steps");
    // Payloads are intentionally not needed to list history.
    f.store->db().exec("UPDATE ops SET payload=X'FF'");
    const auto metadataOnly = history.readTimeline();
    check(metadataOnly.error.empty() && metadataOnly.steps.size() == 4, "listing reads metadata without decoding payload or inverse blobs");
    const auto path = f.store->path(); f.store.reset();
    StoreError error = StoreError::Ok;
    f.store = Store::open(path, error, true);
    const auto readonly = History(*f.store).readTimeline();
    check(readonly.error.empty() && readonly.steps.size() == 4 && readonly.headSeq == c.seqs.back() &&
              readonly.snapshots.size() == 3, "history list reads persisted metadata from a read-only project");
}

void compactedTimeline() {
    Fixture f("compacted"); History history(*f.store); OpJournal journal(*f.store);
    names(f, 1, 7);
    const auto uncompacted = history.readTimeline();
    const auto policy = history.setUndoRetentionPolicy(budget(3));
    const auto compacted = history.compactUndo();
    check(policy.ok && compacted.ok && compacted.baseSeq == 4 && journal.count() == 4,
          "real compaction retains three transactions and their structural root");
    const auto digest = digestProject(*f.store).text;
    const auto changes = f.store->db().execAndGet("SELECT total_changes()").getInt64();
    const auto view = history.readTimeline();
    check(view.error.empty() && view.baseSeq == 4 && view.headSeq == 7 && view.steps.size() == 3 &&
              std::all_of(view.steps.begin(), view.steps.end(), [](const auto& step) {
                  return step.step.firstSeq > 4 && step.step.lastSeq > 4 && step.step.opCount == 1 &&
                      step.applied && step.onCurrentLine;
              }), "compacted timeline exposes its base and omits the retained root from undo steps");
    if (!view.error.empty() || view.steps.size() != 3) return;
    check(view.steps[0].step.label == "7" && view.steps[1].step.label == "6" &&
              view.steps[2].step.label == "5" && view.steps[2].parentSeq == view.baseSeq,
          "visible steps preserve newest-first labels and the first visible parent is the hidden base");
    check(f.store->db().execAndGet("SELECT count(*) FROM ops WHERE seq=4 AND parent_seq IS NULL AND inverse IS NULL").getInt() == 1,
          "omitted row remains a real inverse-less structural root in the persisted journal");
    (void)history.readTimeline();
    check(f.store->db().execAndGet("SELECT total_changes()").getInt64() == changes && journal.count() == 4 &&
              history.head() == view.headSeq && digestProject(*f.store).text == digest,
          "repeated compacted metadata reads perform zero SQL changes and preserve state, log and head");
    check(uncompacted.baseSeq == 0 && uncompacted.steps.size() == 7 && uncompacted.steps.back().step.firstSeq == 1,
          "a held pre-compaction value retains its original base and complete steps");
    bool undone = true;
    for (int i = 0; i < 3; ++i) undone = history.undo().ok && undone;
    const auto atBase = history.readTimeline();
    check(undone && atBase.error.empty() && atBase.baseSeq == 4 && atBase.headSeq == 4 &&
              !history.canUndo() && !history.undo().ok && projectName(f) == "4",
          "undo reaches the nonzero checkpoint head without offering its structural row");
    check(atBase.steps.size() == 3 && std::all_of(atBase.steps.begin(), atBase.steps.end(), [](const auto& step) {
                  return !step.applied && step.onCurrentLine && has(step.branchIds, 1);
              }) && branch(atBase, 1) && branch(atBase, 1)->headSeq == 4 &&
              history.nextRedo() && history.nextRedo()->firstSeq == 5,
          "at-base branch head and all three visible redo steps remain usable");
    bool redone = true;
    for (int i = 0; i < 3; ++i) redone = history.redo().ok && redone;
    const auto restored = history.readTimeline();
    check(redone && projectName(f) == "7" && restored.headSeq == 7 && restored.steps.size() == 3 &&
              std::all_of(restored.steps.begin(), restored.steps.end(), [](const auto& step) { return step.applied; }),
          "redo returns every visible compacted step and the project to the original head");
    (void)rename(f, "8");
    const auto advanced = history.readTimeline();
    check(advanced.error.empty() && advanced.baseSeq == 5 && advanced.headSeq == 8 && advanced.steps.size() == 3 &&
              advanced.steps.back().parentSeq == 5 && view.baseSeq == 4 && view.headSeq == 7 &&
              view.steps.back().step.firstSeq == 5,
          "a later real compaction advances new reads without mutating a held timeline value");
    // Listing is deliberately metadata-only, including its new checkpoint read.
    // Navigation was exercised above before these opaque blobs were corrupted.
    f.store->db().exec("UPDATE history_checkpoint SET state=X'FF'");
    f.store->db().exec("UPDATE ops SET payload=X'FF'");
    const auto metadata = history.readTimeline();
    check(metadata.error.empty() && metadata.baseSeq == 5 && metadata.steps.size() == 3,
          "checkpoint state and forward payload blobs are not decoded by timeline listing");
    const auto path = f.store->path(); f.store.reset();
    StoreError error = StoreError::Ok; f.store = Store::open(path, error, true);
    if (!f.store) throw std::runtime_error(toString(error));
    const auto readonlyChanges = f.store->db().execAndGet("SELECT total_changes()").getInt64();
    const auto readonly = History(*f.store).readTimeline();
    check(readonly.error.empty() && readonly.baseSeq == 5 && readonly.headSeq == 8 && readonly.steps.size() == 3 &&
              readonly.steps.back().parentSeq == 5 && branch(readonly, 1) && branch(readonly, 1)->headSeq == 8,
          "read-only reopen exposes the persisted compacted base, steps and branch head");
    check(f.store->db().execAndGet("SELECT total_changes()").getInt64() == readonlyChanges &&
              OpJournal(*f.store).count() == 4 && view.baseSeq == 4 && view.steps.size() == 3,
          "read-only listing writes nothing and held values survive their original Store closing");
}

void compactedTransactions() {
    Fixture f("compacted_batches"); History history(*f.store); OpJournal journal(*f.store);
    check(history.setUndoRetentionPolicy(budget(2)).ok, "batched timeline retention policy");
    std::array<CommitResult, 5> committed;
    for (std::size_t i = 0; i < committed.size(); ++i) {
        const auto first = std::to_string(i * 2 + 1), last = std::to_string(i * 2 + 2);
        const auto label = "Batch " + std::to_string(i + 1);
        std::array batch = {request("project.setName", {{"name", first}}, label.c_str()),
                            request("project.setName", {{"name", last}}, "Internal batch helper")};
        batch[0].actor = Actor::Agent; batch[0].actorDetail = "whole transaction";
        committed[i] = journal.commit(batch);
        if (!committed[i].ok) throw std::runtime_error(committed[i].error);
    }
    const auto compacted = history.compactUndo();
    const auto view = history.readTimeline();
    check(compacted.ok && compacted.baseSeq == 6 && view.error.empty() && view.baseSeq == 6 &&
              view.steps.size() == 2 && journal.count() == 5 && !find(view, committed[2].txnId),
          "a compacted two-op batch leaves no misleading one-op public undo step");
    const auto* newest = find(view, committed[4].txnId);
    const auto* older = find(view, committed[3].txnId);
    check(newest && older && newest->step.opCount == 2 && newest->step.firstSeq == 9 && newest->step.lastSeq == 10 &&
              newest->step.label == "Batch 5" && newest->step.actor == "agent" &&
              newest->step.actorDetail == "whole transaction" && older->step.opCount == 2 &&
              older->step.firstSeq == 7 && older->step.lastSeq == 8 && older->parentSeq == 6,
          "retained batches preserve whole bounds, first-op metadata and hidden-root ancestry");
    check(history.undo().opsApplied == 2 && history.undo().opsApplied == 2 && !history.canUndo() &&
              projectName(f) == "6" && history.head() == 6,
          "each listed compacted batch is exactly one real undo step down to the base");
    const auto atBase = history.readTimeline();
    check(atBase.error.empty() && atBase.baseSeq == 6 && atBase.steps.size() == 2 &&
              std::all_of(atBase.steps.begin(), atBase.steps.end(), [](const auto& step) {
                  return !step.applied && step.onCurrentLine && step.step.opCount == 2;
              }) && history.redo().opsApplied == 2 && history.redo().opsApplied == 2 && projectName(f) == "10",
          "whole batch redo flags at the checkpoint match real two-op replay");
}

void compactedAnchors() {
    Fixture f("compacted_snapshot"); History history(*f.store);
    names(f, 1, 2); const auto pinned = history.takeSnapshot("Pinned base", 1, 0); names(f, 3, 7);
    const auto compacted = history.setUndoRetentionPolicy(budget(2));
    const auto view = history.readTimeline();
    const auto* anchor = snapshot(view, pinned.id);
    check(pinned.ok && compacted.ok && compacted.protectedHistory && compacted.baseSeq == 2 &&
              view.error.empty() && view.baseSeq == 2 && view.steps.size() == 5 && anchor && anchor->opSeq == 2 &&
              anchor->name == "Pinned base" && anchor->branchId == 1,
          "an exact-base snapshot remains metadata even though its protected root is not a step");
    check(std::none_of(view.steps.begin(), view.steps.end(), [&](const auto& step) {
                  return step.step.firstSeq <= view.baseSeq || has(step.snapshotIds, pinned.id);
              }), "the base snapshot is never misattached to a later visible transaction");
    check(history.revertToSnapshot(pinned.id, 2).ok && projectName(f) == "2" && !history.canUndo(),
          "a named snapshot at the hidden root remains a real navigable point");
    const auto reverted = history.readTimeline();
    check(reverted.error.empty() && reverted.baseSeq == 2 && reverted.headSeq == 2 &&
              snapshot(reverted, pinned.id) && snapshot(reverted, pinned.id)->opSeq == 2 &&
              reverted.steps.size() == 5 && std::all_of(reverted.steps.begin(), reverted.steps.end(), [](const auto& step) {
                  return !step.applied && step.onCurrentLine;
              }), "snapshot navigation to the base retains its anchor and the complete visible redo line");
    Fixture root("uncompacted_root_snapshot"); History rootHistory(*root.store);
    const auto beforeEdits = rootHistory.takeSnapshot("Before edits", 1, 0); names(root, 1, 5);
    const auto protectedRoot = rootHistory.setUndoRetentionPolicy(budget(2));
    const auto rootView = rootHistory.readTimeline();
    check(beforeEdits.ok && protectedRoot.ok && protectedRoot.protectedHistory && protectedRoot.baseSeq == 0 &&
              rootView.error.empty() && rootView.baseSeq == 0 && rootView.steps.size() == 5 &&
              snapshot(rootView, beforeEdits.id) && !snapshot(rootView, beforeEdits.id)->opSeq,
          "an ordinary null-root snapshot still protects the whole log and differs from a compacted base anchor");
}

void compactedFork() {
    Fixture f("compacted_fork"); History history(*f.store);
    names(f, 1, 6);
    check(history.undo().ok && history.undo().ok && history.undo().ok, "fork timeline rewinds to its future compacted base");
    const auto first = rename(f, "B1"), last = rename(f, "B2");
    const auto compacted = history.setUndoRetentionPolicy(budget(2));
    const auto view = history.readTimeline();
    std::int64_t oldBranch = 0;
    for (const auto& entry : view.branches) if (!entry.isCurrent) oldBranch = entry.id;
    check(compacted.ok && compacted.protectedHistory && compacted.baseSeq == 3 && view.error.empty() &&
              view.baseSeq == 3 && view.steps.size() == 5 && view.branches.size() == 2 && oldBranch != 0,
          "a real fork compacts through its shared root while retaining both descendant branches");
    const auto* b1 = find(view, first.txnId); const auto* b2 = find(view, last.txnId);
    const auto oldFirst = std::find_if(view.steps.begin(), view.steps.end(), [](const auto& step) { return step.step.firstSeq == 4; });
    check(b1 && b2 && b1->parentSeq == 3 && b1->applied && b2->applied && has(b1->branchIds, 1) &&
              oldFirst != view.steps.end() && oldFirst->parentSeq == 3 && !oldFirst->applied &&
              !oldFirst->onCurrentLine && has(oldFirst->branchIds, oldBranch),
          "both first descendants name the hidden shared parent and retain correct active and alternate flags");
    check(history.undo().ok && history.undo().ok && history.head() == 3 && !history.canUndo(),
          "new branch undoes exactly to the compacted shared root");
    const auto undone = history.readTimeline();
    check(undone.error.empty() && undone.baseSeq == 3 && undone.headSeq == 3 && branch(undone, 1) &&
              branch(undone, 1)->headSeq == 3 && find(undone, first.txnId) &&
              find(undone, first.txnId)->onCurrentLine && !find(undone, first.txnId)->applied &&
              std::none_of(undone.steps.begin(), undone.steps.end(), [](const auto& step) { return step.applied; }),
          "a branch may point exactly at the hidden root while its newer redo tail remains visible");
    check(oldBranch != 0 && history.switchToBranch(oldBranch).ok && projectName(f) == "6",
          "switch to the older branch still replays through the compacted common ancestor");
    const auto switched = history.readTimeline();
    check(switched.error.empty() && switched.baseSeq == 3 && switched.currentBranchId == oldBranch &&
              branch(switched, 1) && branch(switched, 1)->headSeq == 3 && find(switched, first.txnId) &&
              !find(switched, first.txnId)->applied && !find(switched, first.txnId)->onCurrentLine &&
              find(switched, first.txnId)->branchIds.empty(),
          "an inactive branch at the base retains its stored anchor without claiming its former redo tail");
    check(history.switchToBranch(1).ok && history.head() == 3 && history.nextRedo() &&
              history.nextRedo()->txnId == first.txnId,
          "switch back to the exact-base branch recovers the real highest-child redo path");
    const auto returned = history.readTimeline();
    check(returned.error.empty() && find(returned, first.txnId) && find(returned, last.txnId) &&
              find(returned, first.txnId)->onCurrentLine && find(returned, last.txnId)->onCurrentLine &&
              history.redo().ok && history.redo().ok && projectName(f) == "B2",
          "reactivated compacted branch flags agree with replay of its retained redo tail");
}

void checkpointErrors() {
    struct Corruption { const char* sql; const char* reason; };
    const std::array corruptions = {
        Corruption{"UPDATE history_checkpoint SET base_seq=0", "zero checkpoint base"},
        Corruption{"UPDATE history_checkpoint SET base_seq=-1", "negative checkpoint base"},
        Corruption{"UPDATE history_checkpoint SET base_seq=9999", "missing checkpoint root"},
        Corruption{"UPDATE history_checkpoint SET base_seq=3", "checkpoint points to a non-root edit"},
        Corruption{"UPDATE ops SET parent_seq=3 WHERE seq=2", "checkpoint root has a parent"},
        Corruption{"UPDATE ops SET tags='ephemeral' WHERE seq=2", "checkpoint root is ephemeral"},
        Corruption{"UPDATE ops SET txn_id=(SELECT txn_id FROM ops WHERE seq=3) WHERE seq=2", "checkpoint root shares a retained transaction"},
        Corruption{"UPDATE history_checkpoint SET base_seq=3; UPDATE ops SET parent_seq=NULL WHERE seq=3", "retained edit precedes checkpoint root"}
    };
    for (const auto& corruption : corruptions) {
        Fixture f("checkpoint_error"); History history(*f.store);
        names(f, 1, 4);
        const auto compacted = history.setUndoRetentionPolicy(budget(2));
        if (!compacted.ok || compacted.baseSeq != 2) throw std::runtime_error("invalid real compaction fixture");
        // Corrupt an actual compacted journal, not a synthetic happy-path
        // checkpoint. Foreign keys are disabled only to exercise broken files.
        f.store->db().exec("PRAGMA foreign_keys=OFF");
        f.store->db().exec(corruption.sql);
        const auto changes = f.store->db().execAndGet("SELECT total_changes()").getInt64();
        const auto view = history.readTimeline();
        check(clearedFailure(view), std::string(corruption.reason) + " returns a named error with no partial timeline");
        check(f.store->db().execAndGet("SELECT total_changes()").getInt64() == changes &&
                  OpJournal(*f.store).count() == 3 && projectName(f) == "4",
              std::string(corruption.reason) + " refusal does not repair or mutate the project");
    }
}

void errors() {
    {
        Fixture f("cycle"); OpJournal journal(*f.store);
        const auto op = journal.commit(request("track.create", {{"id", 1}, {"kind", "audio"}}, "Track"));
        check(op.ok, "cycle fixture");
        f.store->db().exec("UPDATE ops SET parent_seq=seq");
        const auto view = History(*f.store).readTimeline();
        check(!view.error.empty() && view.steps.empty() && view.branches.empty(), "cycle returns a named error, no partial plausible history");
    }
    {
        Fixture f("partial"); OpJournal journal(*f.store);
        const std::array pair = {request("track.create", {{"id", 1}, {"kind", "audio"}}, "Pair"),
                                request("track.create", {{"id", 2}, {"kind", "audio"}}, "Helper")};
        const auto op = journal.commit(pair);
        check(op.ok && journal.setHeadSeq(op.seqs.front()), "partial transaction fixture");
        const auto view = History(*f.store).readTimeline();
        check(!view.error.empty() && view.steps.empty(), "partial transaction head is refused, never split into fake undo steps");
    }
    {
        Fixture f("table");
        f.store->db().exec("DROP TABLE history_snapshots");
        const auto view = History(*f.store).readTimeline();
        check(!view.error.empty() && view.steps.empty(), "missing metadata table is an error rather than empty history");
    }
    {
        Fixture f("nested");
        SQLite::Transaction transaction(f.store->db());
        const auto view = History(*f.store).readTimeline();
        check(!view.error.empty(), "listing inside an active journal transaction fails explicitly");
        check(f.store->db().execAndGet("SELECT count(*) FROM project").getInt() == 1, "listing failure does not roll back caller's transaction");
    }
}
} // namespace
int main() {
    try { timeline(); compactedTimeline(); compactedTransactions(); compactedAnchors(); compactedFork(); checkpointErrors(); errors(); }
    catch (const std::exception& error) { check(false, std::string("exception: ") + error.what()); }
    std::printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
