// SPDX-License-Identifier: GPL-3.0-or-later
#include "temp_directory.hpp"
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

void timeline() {
    Fixture f("branches");
    History history(*f.store); OpJournal journal(*f.store);
    auto empty = history.readTimeline();
    check(empty.error.empty() && empty.steps.empty() && !empty.headSeq && empty.currentBranchId == 1 &&
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
    try { timeline(); errors(); }
    catch (const std::exception& error) { check(false, std::string("exception: ") + error.what()); }
    std::printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
