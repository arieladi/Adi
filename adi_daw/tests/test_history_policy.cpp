// SPDX-License-Identifier: AGPL-3.0-or-later
#include "adi/history.hpp"
#include "adi/history_policy.hpp"
#include "adi/store.hpp"
#include "temp_directory.hpp"
#include <SQLiteCpp/SQLiteCpp.h>
#include <cstdio>
#include <stdexcept>
namespace {
using namespace adi;
int checks = 0, failures = 0;
void check(bool value, const char *message) {
    ++checks;
    if (!value) {
        ++failures;
        std::printf("FAIL: %s\n", message);
    }
}
constexpr std::int64_t day = 86400000000, minute = 60000000;
struct Project {
    test::TempDirectory folder{"history_policy", "project"};
    std::unique_ptr<Store> store;
    Project() {
        StoreError error{};
        store = Store::create(folder.path() / "song.adi", error);
        if (!store)
            throw std::runtime_error(toString(error));
    }
    void edit(const char *name) {
        OpRequest request;
        request.opType = "project.setName";
        request.payload = {{"name", name}};
        const auto result = OpJournal(*store).commit(request);
        if (!result.ok)
            throw std::runtime_error(result.error);
    }
};
void daily() {
    Project p;
    History history(*p.store);
    DailyHistorySnapshots service(*p.store);
    const auto now = 20000 * day + 60 * minute;
    check(service.poll(now, 0).status == DailyHistorySnapshots::Status::Unchanged &&
              history.listSnapshots().empty(),
          "opening an unchanged project creates no daily snapshot");
    p.edit("One");
    const auto ops = OpJournal(*p.store).count();
    auto first = service.poll(now, 0);
    check(first.status == DailyHistorySnapshots::Status::Created && first.snapshotId > 0,
          "first edit creates the day's automatic snapshot");
    check(history.listSnapshots().size() == 1 && history.listSnapshots()[0].automatic &&
              history.listSnapshots()[0].opSeq == OpJournal(*p.store).headSeq(),
          "automatic snapshot names the actual committed history head");
    check(OpJournal(*p.store).count() == ops, "daily snapshot is metadata and adds no undo step");
    p.edit("Two");
    auto updated = service.poll(now + minute, 0);
    check(updated.status == DailyHistorySnapshots::Status::Updated &&
              updated.snapshotId == first.snapshotId && history.listSnapshots().size() == 1,
          "later edits refresh the same day's point instead of creating duplicate snapshots");
    check(history.listSnapshots()[0].name == History::defaultSnapshotName("Two", now + minute, 0),
          "updated automatic name uses supplied project name, time and offset");
    check(service.poll(now + day, 0).status == DailyHistorySnapshots::Status::Unchanged &&
              history.listSnapshots().size() == 1,
          "an open but unchanged next day makes no automatic snapshot");
    p.edit("Three");
    auto second = service.poll(now + day, 0);
    check(second.status == DailyHistorySnapshots::Status::Created && second.snapshotId != first.snapshotId,
          "a changed next day gets its own automatic point");
    check(history.renameSnapshot(second.snapshotId, "Keep this").ok,
          "automatic snapshot can be promoted by a user rename");
    bool pinned = false;
    for (const auto &snapshot : history.listSnapshots())
        if (snapshot.id == second.snapshotId)
            pinned = !snapshot.automatic && snapshot.name == "Keep this";
    check(pinned, "a renamed automatic snapshot becomes named and cannot be evicted automatically");
    p.edit("Four");
    auto afterRename = service.poll(now + day + minute, 0);
    check(afterRename.status == DailyHistorySnapshots::Status::Created &&
              afterRename.snapshotId != second.snapshotId && history.listSnapshots().size() == 3,
          "new automatic point preserves the promoted name and leaves only one automatic entry for the date");
    check(history.undo().ok &&
              service.poll(now + day + 2 * minute, 0).status == DailyHistorySnapshots::Status::Updated,
          "undo is a project change observed by the daily service");
    check(history.redo().ok &&
              service.poll(now + day + 3 * minute, 0).status == DailyHistorySnapshots::Status::Updated,
          "redo refreshes the automatic point without adding history");
    DailyHistorySnapshots reopenedService(*p.store);
    check(reopenedService.poll(now + 2 * day, 0).status == DailyHistorySnapshots::Status::Unchanged,
          "a newly opened document service does not treat existing log rows as new edits");
}
void dates() {
    Project p;
    DailyHistorySnapshots service(*p.store);
    const auto utc = 20000 * day + 23 * 60 * minute;
    p.edit("East");
    const auto east = service.poll(utc, 120);
    p.edit("West");
    const auto west = service.poll(utc + minute, -120);
    p.edit("East again");
    const auto same = service.poll(utc + 2 * minute, 120);
    check(east.status == DailyHistorySnapshots::Status::Created &&
              west.status == DailyHistorySnapshots::Status::Created &&
              same.status == DailyHistorySnapshots::Status::Updated && same.snapshotId == east.snapshotId,
          "daily identity follows the supplied local date even when timezone or clock moves back");
    p.edit("Before epoch");
    const auto negative = service.poll(-1, 0);
    check(negative.status == DailyHistorySnapshots::Status::Created &&
              negative.name == History::defaultSnapshotName("Before epoch", -1, 0),
          "pre-epoch fractional times floor to the previous calendar day");
    p.edit("Same pre-epoch day");
    const auto sameNegative = service.poll(-minute, 0);
    check(sameNegative.status == DailyHistorySnapshots::Status::Updated &&
              sameNegative.snapshotId == negative.snapshotId,
          "negative fractional timestamps and whole negative minutes share the same previous date");
    check(service.poll(utc, 2000).status == DailyHistorySnapshots::Status::Failed,
          "invalid UTC offset is refused");
}
void budgets() {
    Project p;
    History history(*p.store);
    HistorySnapshotPolicy policy;
    policy.maxSnapshots = 3;
    const auto named = history.takeSnapshot("Named", 1, 0, false, policy);
    const auto old = history.takeSnapshot("Old auto", 2, 0, true, policy);
    const auto newer = history.takeSnapshot("New auto", 3, 0, true, policy);
    const auto another = history.takeSnapshot("Another name", 4, 0, false, policy);
    check(named.ok && old.ok && newer.ok && another.ok && history.listSnapshots().size() == 3,
          "snapshot budget evicts an automatic point to make room atomically");
    bool keptNamed = false, keptNewer = false, keptOld = false;
    for (const auto &snapshot : history.listSnapshots()) {
        keptNamed = keptNamed || snapshot.id == named.id;
        keptNewer = keptNewer || snapshot.id == newer.id;
        keptOld = keptOld || snapshot.id == old.id;
    }
    check(keptNamed && keptNewer && !keptOld,
          "oldest automatic is selected while named and newer points remain");
    check(history.renameSnapshot(newer.id, "Promoted").ok, "newer automatic point can be named");
    check(!history.takeSnapshot("Blocked", 5, 0, false, policy).ok && history.listSnapshots().size() == 3,
          "an all-named budget refuses a new point without deleting any named snapshot");
    check(history.removeSnapshot(newer.id).ok && !history.removeSnapshot(999).ok,
          "explicit user removal can free a slot and nonexistent removal fails");
    check(history.takeSnapshot("Room", 6, 0, false, policy).ok, "a freed slot accepts another named point");
    HistorySnapshotPolicy tooMany;
    tooMany.maxSnapshots = 10001;
    check(!history.takeSnapshot("Invalid local policy", 7, 0, false, tooMany).ok,
          "local configuration cannot exceed 10000 snapshots");
    tooMany.collaboration = true;
    tooMany.maxSnapshots = 100001;
    check(!history.takeSnapshot("Invalid collaboration policy", 8, 0, false, tooMany).ok,
          "collaboration configuration cannot exceed 100000 snapshots");
}
void blockedAndRollback() {
    Project p;
    History history(*p.store);
    HistorySnapshotPolicy one;
    one.maxSnapshots = 1;
    const auto named = history.takeSnapshot("Pinned", 1, 0, false, one);
    DailyHistorySnapshots service(*p.store, one);
    p.edit("Changed");
    const auto blocked = service.poll(20000 * day, 0);
    check(named.ok && blocked.status == DailyHistorySnapshots::Status::Blocked &&
              history.listSnapshots().size() == 1,
          "daily service reports a full named budget without deleting the point");
    check(history.removeSnapshot(named.id).ok &&
              service.poll(20000 * day, 0).status == DailyHistorySnapshots::Status::Created,
          "pending daily change retries when the user frees space");
    const auto remaining = history.listSnapshots();
    if (remaining.empty())
        return;
    const auto autoId = remaining[0].id;
    p.edit("Next edit");
    p.store->db().exec("CREATE TRIGGER refuse_daily BEFORE UPDATE ON history_snapshots BEGIN SELECT "
                       "RAISE(ABORT,'planted daily update failure'); END");
    check(service.poll(20000 * day + minute, 0).status == DailyHistorySnapshots::Status::Failed,
          "late metadata failure is reported");
    p.store->db().exec("DROP TRIGGER refuse_daily");
    check(service.poll(20000 * day + minute, 0).status == DailyHistorySnapshots::Status::Updated &&
              history.listSnapshots()[0].id == autoId,
          "failed daily update retains pending work and retries the same automatic row");
    p.store->db().exec("CREATE TRIGGER refuse_snapshot BEFORE INSERT ON history_snapshots BEGIN SELECT "
                       "RAISE(ABORT,'planted insert failure'); END");
    check(!history.takeSnapshot("New named", 20000 * day + 2 * minute, 0, false, one).ok &&
              history.listSnapshots().size() == 1 && history.listSnapshots()[0].id == autoId,
          "failed insertion rolls back eviction of the previous automatic snapshot");
}
void actualLimits() {
    for (const bool collaboration : {false, true}) {
        Project p;
        History history(*p.store);
        HistorySnapshotPolicy policy;
        policy.collaboration = collaboration;
        const auto maximum = collaboration ? 100000 : 10000;
        SQLite::Statement seed(
            p.store->db(),
            "WITH RECURSIVE ids(n) AS (VALUES(1) UNION ALL SELECT n+1 FROM ids WHERE n<?) INSERT INTO "
            "history_snapshots(name,op_seq,branch_id,created_utc,auto) SELECT 'Named',NULL,?,n,0 FROM ids");
        seed.bind(1, maximum);
        seed.bind(2, OpJournal(*p.store).currentBranchId());
        seed.exec();
        check(!history.takeSnapshot("One too many", maximum + 1, 0, false, policy).ok &&
                  p.store->db().execAndGet("SELECT COUNT(*) FROM history_snapshots").getInt() == maximum,
              "actual local/collaboration hard boundary preserves all named snapshots");
    }
}
void reopenAndReadonly() {
    Project p;
    std::int64_t id = 0;
    const auto now = 20000 * day;
    {
        DailyHistorySnapshots service(*p.store);
        p.edit("Before reopen");
        const auto first = service.poll(now, 0);
        id = first.snapshotId;
        check(first.status == DailyHistorySnapshots::Status::Created, "persistent daily fixture is recorded");
    }
    const auto path = p.store->path();
    p.store.reset();
    StoreError error{};
    p.store = Store::open(path, error);
    check(p.store != nullptr, "daily project reopens");
    if (!p.store)
        return;
    DailyHistorySnapshots service(*p.store);
    check(service.poll(now + minute, 0).status == DailyHistorySnapshots::Status::Unchanged,
          "reopen creates no snapshot before a new edit");
    p.edit("After reopen");
    const auto updated = service.poll(now + minute, 0);
    check(updated.status == DailyHistorySnapshots::Status::Updated && updated.snapshotId == id,
          "persistent day marker updates the same automatic row after reopen");
    p.store.reset();
    p.store = Store::open(path, error, true);
    if (!p.store)
        throw std::runtime_error("readonly daily fixture");
    DailyHistorySnapshots readonly(*p.store);
    History history(*p.store);
    check(readonly.poll(now + 2 * minute, 0).status == DailyHistorySnapshots::Status::Failed &&
              !history.removeSnapshot(id).ok && history.listSnapshots().size() == 1,
          "readonly mode refuses daily writes and explicit removal without changing metadata");
}
} // namespace
int main() {
    try {
        daily();
        dates();
        budgets();
        blockedAndRollback();
        actualLimits();
        reopenAndReadonly();
    } catch (const std::exception &error) {
        check(false, error.what());
    }
    std::printf("history policy: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
