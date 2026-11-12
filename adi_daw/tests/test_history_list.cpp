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
    check(policy.ok && compacted.ok && compacted.baseSeq == 5 && journal.count() == 3,
          "real compaction retains the two-step low-water budget and structural root");
    const auto digest = digestProject(*f.store).text;
    const auto changes = f.store->db().execAndGet("SELECT total_changes()").getInt64();
    const auto view = history.readTimeline();
    check(view.error.empty() && view.baseSeq == 5 && view.headSeq == 7 && view.steps.size() == 2 &&
              std::all_of(view.steps.begin(), view.steps.end(), [](const auto& step) {
                  return step.step.firstSeq > 5 && step.step.lastSeq > 5 && step.step.opCount == 1 &&
                      step.applied && step.onCurrentLine;
              }), "compacted timeline exposes its base and omits the retained root from undo steps");
    if (!view.error.empty() || view.steps.size() != 2) return;
    check(view.steps[0].step.label == "7" && view.steps[1].step.label == "6" &&
              view.steps[1].parentSeq == view.baseSeq,
          "visible steps preserve newest-first labels and the first visible parent is the hidden base");
    check(f.store->db().execAndGet("SELECT count(*) FROM ops WHERE seq=5 AND parent_seq IS NULL AND inverse IS NULL").getInt() == 1,
          "omitted row remains a real inverse-less structural root in the persisted journal");
    (void)history.readTimeline();
    check(f.store->db().execAndGet("SELECT total_changes()").getInt64() == changes && journal.count() == 3 &&
              history.head() == view.headSeq && digestProject(*f.store).text == digest,
          "repeated compacted metadata reads perform zero SQL changes and preserve state, log and head");
    check(uncompacted.baseSeq == 0 && uncompacted.steps.size() == 7 && uncompacted.steps.back().step.firstSeq == 1,
          "a held pre-compaction value retains its original base and complete steps");
    bool undone = true;
    for (int i = 0; i < 2; ++i) undone = history.undo().ok && undone;
    const auto atBase = history.readTimeline();
    check(undone && atBase.error.empty() && atBase.baseSeq == 5 && atBase.headSeq == 5 &&
              !history.canUndo() && !history.undo().ok && projectName(f) == "5",
          "undo reaches the nonzero checkpoint head without offering its structural row");
    check(atBase.steps.size() == 2 && std::all_of(atBase.steps.begin(), atBase.steps.end(), [](const auto& step) {
                  return !step.applied && step.onCurrentLine && has(step.branchIds, 1);
              }) && branch(atBase, 1) && branch(atBase, 1)->headSeq == 5 &&
              history.nextRedo() && history.nextRedo()->firstSeq == 6,
          "at-base branch head and both visible redo steps remain usable");
    bool redone = true;
    for (int i = 0; i < 2; ++i) redone = history.redo().ok && redone;
    const auto restored = history.readTimeline();
    check(redone && projectName(f) == "7" && restored.headSeq == 7 && restored.steps.size() == 2 &&
              std::all_of(restored.steps.begin(), restored.steps.end(), [](const auto& step) { return step.applied; }),
          "redo returns every visible compacted step and the project to the original head");
    (void)rename(f, "8"); (void)rename(f, "9");
    const auto advanced = history.readTimeline();
    check(advanced.error.empty() && advanced.baseSeq == 7 && advanced.headSeq == 9 && advanced.steps.size() == 2 &&
              advanced.steps.back().parentSeq == 7 && view.baseSeq == 5 && view.headSeq == 7 &&
              view.steps.back().step.firstSeq == 6,
          "a later real compaction advances new reads without mutating a held timeline value");
    // Listing is deliberately metadata-only, including its new checkpoint read.
    // Navigation was exercised above before these opaque blobs were corrupted.
    f.store->db().exec("UPDATE history_checkpoint SET state=X'FF'");
    f.store->db().exec("UPDATE ops SET payload=X'FF' WHERE op_type<>'history.root'");
    const auto metadata = history.readTimeline();
    check(metadata.error.empty() && metadata.baseSeq == 7 && metadata.steps.size() == 2,
          "checkpoint state and forward payload blobs are not decoded by timeline listing");
    const auto path = f.store->path(); f.store.reset();
    StoreError error = StoreError::Ok; f.store = Store::open(path, error, true);
    if (!f.store) throw std::runtime_error(toString(error));
    const auto readonlyChanges = f.store->db().execAndGet("SELECT total_changes()").getInt64();
    const auto readonly = History(*f.store).readTimeline();
    check(readonly.error.empty() && readonly.baseSeq == 7 && readonly.headSeq == 9 && readonly.steps.size() == 2 &&
              readonly.steps.back().parentSeq == 7 && branch(readonly, 1) && branch(readonly, 1)->headSeq == 9,
          "read-only reopen exposes the persisted compacted base, steps and branch head");
    check(f.store->db().execAndGet("SELECT total_changes()").getInt64() == readonlyChanges &&
              OpJournal(*f.store).count() == 3 && view.baseSeq == 5 && view.steps.size() == 2,
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
    check(compacted.ok && compacted.baseSeq == 8 && view.error.empty() && view.baseSeq == 8 &&
              view.steps.size() == 1 && journal.count() == 3 && !find(view, committed[2].txnId),
          "a compacted two-op batch leaves no misleading one-op public undo step");
    const auto* newest = find(view, committed[4].txnId);
    const auto* hidden = find(view, committed[3].txnId);
    check(newest && !hidden && newest->step.opCount == 2 && newest->step.firstSeq == 9 && newest->step.lastSeq == 10 &&
              newest->step.label == "Batch 5" && newest->step.actor == "agent" &&
              newest->step.actorDetail == "whole transaction" && newest->parentSeq == 8,
          "retained batches preserve whole bounds, first-op metadata and hidden-root ancestry");
    check(history.undo().opsApplied == 2 && !history.canUndo() &&
              projectName(f) == "8" && history.head() == 8,
          "each listed compacted batch is exactly one real undo step down to the base");
    const auto atBase = history.readTimeline();
    check(atBase.error.empty() && atBase.baseSeq == 8 && atBase.steps.size() == 1 &&
              std::all_of(atBase.steps.begin(), atBase.steps.end(), [](const auto& step) {
                  return !step.applied && step.onCurrentLine && step.step.opCount == 2;
              }) && history.redo().opsApplied == 2 && projectName(f) == "10",
          "whole batch redo flags at the checkpoint match real two-op replay");
}

void compactedAnchors() {
    Fixture f("compacted_snapshot");
    History history(*f.store);
    names(f, 1, 2);
    const auto pinned = history.takeSnapshot("Pinned base", 1, 0);
    names(f, 3, 7);
    const auto compacted = history.setUndoRetentionPolicy(budget(2));
    const auto view = history.readTimeline();
    const auto *anchor = snapshot(view, pinned.id);
    check(pinned.ok && compacted.ok && !compacted.protectedHistory && compacted.baseSeq == 6 &&
              view.error.empty() && view.steps.size() == 1 && anchor && anchor->opSeq == 2 &&
              anchor->storedStateHash,
          "stored snapshot stays named and addressable while only the active suffix appears as "
          "undo steps");
    check(std::none_of(view.steps.begin(), view.steps.end(),
                       [&](const auto &step) { return has(step.snapshotIds, pinned.id); }),
          "stored snapshot is never attached to an unrelated retained step");
    check(
        history.revertToSnapshot(pinned.id, 2, History::StoredRestore::SessionReloadPrepared).ok &&
            projectName(f) == "2" && !history.canUndo(),
        "prepared navigation restores stored snapshot without an undo step");
    const auto reverted = history.readTimeline();
    check(
        reverted.error.empty() && reverted.baseSeq == 2 && reverted.headSeq == 2 &&
            snapshot(reverted, pinned.id)->storedStateHash && reverted.steps.size() == 1 &&
            !reverted.steps[0].applied && !reverted.steps[0].onCurrentLine,
        "stored point has its own baseline and leaves the other branch suffix visible as inactive");
    Fixture origin("origin_snapshot");
    History root(*origin.store);
    const auto point = root.takeSnapshot("Before edits", 1, 0);
    names(origin, 1, 5);
    const auto cap = root.setUndoRetentionPolicy(budget(2));
    const auto originView = root.readTimeline();
    check(point.ok && cap.ok && !cap.protectedHistory && cap.baseSeq == 4 &&
              originView.error.empty() && originView.steps.size() == 1 &&
              snapshot(originView, point.id) && snapshot(originView, point.id)->storedStateHash,
          "original null-root snapshot is stored independently and does not protect the whole log");
}
void compactedFork() {
    Fixture f("compacted_fork");
    History history(*f.store);
    names(f, 1, 6);
    check(history.undo().ok && history.undo().ok && history.undo().ok, "fork timeline rewinds");
    const auto first = rename(f, "B1"), last = rename(f, "B2");
    const auto compacted = history.setUndoRetentionPolicy(budget(2));
    const auto view = history.readTimeline();
    std::int64_t old = 0;
    for (const auto &entry : view.branches)
        if (!entry.isCurrent)
            old = entry.id;
    const auto *newest = find(view, last.txnId);
    check(compacted.ok && !compacted.protectedHistory && compacted.baseSeq == first.seqs.back() &&
              view.error.empty() && view.steps.size() == 1 && old != 0 && newest &&
              newest->applied && newest->parentSeq == first.seqs.back(),
          "compacted fork exposes one active step and independently stored dead-end metadata");
    check(history.undo().ok && !history.canUndo() && projectName(f) == "B1" && history.nextRedo() &&
              history.nextRedo()->txnId == last.txnId,
          "retained branch undoes to its own baseline and keeps real redo");
    check(history.switchToBranch(old, History::StoredRestore::SessionReloadPrepared).ok &&
              projectName(f) == "6",
          "prepared branch navigation opens the stored dead end");
    const auto switched = history.readTimeline();
    check(switched.error.empty() && switched.baseSeq == 6 && switched.steps.size() == 1 &&
              !switched.steps[0].applied && !switched.steps[0].onCurrentLine &&
              branch(switched, old)->storedStateHash,
          "stored dead end is not an applied undo step on the active suffix");
    check(history.switchToBranch(1, History::StoredRestore::SessionReloadPrepared).ok &&
              projectName(f) == "B1" && history.redo().ok && projectName(f) == "B2",
          "returning to the retained branch restores its baseline and redo tail");
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
        const auto compacted = history.setUndoRetentionPolicy(budget(3));
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
