// SPDX-License-Identifier: GPL-3.0-or-later
//
// History snapshots: ADR-0128, ADR-0140, SPEC 8.6.
//
// The property that matters is ADR-0128 d2, "revert never discards", and it is
// tested the only way that means anything: revert, do new work, then actually
// GO BACK to what was reverted away from and find it intact. A branch row that
// merely exists proves nothing if the line it names cannot be reached.

#include "temp_directory.hpp"

#include "adi/digest.hpp"
#include "adi/history.hpp"
#include "adi/ops.hpp"
#include "adi/store.hpp"

#include <SQLiteCpp/SQLiteCpp.h>

#include <cstdio>
#include <exception>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>

namespace {

namespace fs = std::filesystem;
using namespace adi;

int g_failures = 0;
int g_checks = 0;

void check(bool cond, const std::string& what) {
    ++g_checks;
    if (!cond) {
        ++g_failures;
        std::printf("  FAIL  %s\n", what.c_str());
    }
}
void section(const char* s) { std::printf("[%s]\n", s); }

constexpr std::int64_t kT0 = 1790253296000000;   // 2026-09-24 12:34:56 UTC

struct Fixture {
    adi::test::TempDirectory temp;   // destroyed after the store
    std::unique_ptr<Store> store;
    explicit Fixture(const char* n, const char* projectName = "My Song")
        : temp("snapshots", n) {
        StoreError e = StoreError::Ok;
        store = Store::create(temp.path() / "p.adi", e);
        if (!store) return;
        SQLite::Statement st(store->db(), "INSERT INTO project(id,name) VALUES (1,?)");
        st.bind(1, projectName);
        st.exec();
    }
};

bool mk(Store& s, std::int64_t id, const char* name) {
    OpJournal j(s);
    OpRequest r;
    r.opType = "track.create";
    r.payload = {{"id", id}, {"kind", "midi"}, {"name", name}};
    r.label = std::string("Create ") + name;
    return j.commit(r).ok;
}

/// Track names in id order, "A,B,C": the whole visible state of these tests.
std::string tracks(Store& s) {
    SQLite::Statement st(s.db(), "SELECT name FROM tracks ORDER BY id");
    std::string out;
    while (st.executeStep()) out += (out.empty() ? "" : ",") + st.getColumn(0).getString();
    return out;
}

std::int64_t count(Store& s, const char* sql) {
    return s.db().execAndGet(sql).getInt64();
}

/// The branch whose head is `seq`, other than the current one.
std::optional<std::int64_t> branchHolding(History& h, std::optional<std::int64_t> seq) {
    for (const auto& b : h.branches())
        if (!b.isCurrent && b.headSeq == seq) return b.id;
    return std::nullopt;
}

// --- the automatic name (ADR-0128 d4) ------------------------------------------------

void testDefaultName() {
    section("the automatic name: [Project Name] [YYYY-MM-DD HH:MM], local time passed in");
    const auto n = [](const char* p, std::int64_t t, int off) {
        return History::defaultSnapshotName(p, t, off);
    };
    check(n("My Song", kT0, 0) == "My Song 2026-09-24 12:34", "UTC: " + n("My Song", kT0, 0));
    check(n("My Song", kT0, 180) == "My Song 2026-09-24 15:34",
          "the caller's offset is applied (+03:00): " + n("My Song", kT0, 180));
    check(n("My Song", kT0, -600) == "My Song 2026-09-24 02:34",
          "and a negative one (-10:00): " + n("My Song", kT0, -600));
    check(n("X", 1798759800000000, 120) == "X 2027-01-01 01:30",
          "crossing midnight into a new year: " + n("X", 1798759800000000, 120));
    check(n("X", 1709165700000000, 0) == "X 2024-02-29 00:15",
          "a leap day: " + n("X", 1709165700000000, 0));
    check(n("X", -30000000, 0) == "X 1969-12-31 23:59",
          "before the epoch, floored not truncated: " + n("X", -30000000, 0));
    check(n("", kT0, 0) == "Untitled 2026-09-24 12:34", "an unnamed project: " + n("", kT0, 0));
}

// --- take, list, rename --------------------------------------------------------------

void testTakeListRename() {
    section("take, list and rename -- metadata, not ops (ADR-0128 d5)");
    Fixture f("take");
    if (!f.store) return;
    History h(*f.store);
    Store& s = *f.store;

    const auto root = h.takeSnapshot("", kT0, 60, /*automatic=*/true);
    check(root.ok && root.name == "My Song 2026-09-24 13:34",
          "an empty name defaults from the project name and the time given: " + root.name +
              root.error);
    mk(s, 1, "A");
    const auto opsBefore = count(s, "SELECT COUNT(*) FROM ops");
    const auto a = h.takeSnapshot("verse done", kT0 + 1, 0);
    check(a.ok && a.name == "verse done", "a typed name is kept");
    check(count(s, "SELECT COUNT(*) FROM ops") == opsBefore, "taking a snapshot appends no op");

    const auto list = h.listSnapshots();
    check(list.size() == 2, "two snapshots listed");
    if (list.size() == 2) {
        check(!list[0].opSeq && list[0].automatic && list[0].createdUtc == kT0,
              "the first names the root, is automatic, and carries the time passed in");
        check(list[1].opSeq == h.head() && !list[1].automatic && list[1].branchId == 1,
              "the second names the head, on the current branch");
    }

    check(h.renameSnapshot(a.id, "verse final").ok, "renamed");
    check(h.listSnapshots().back().name == "verse final", "the new name is listed");
    check(count(s, "SELECT COUNT(*) FROM ops") == opsBefore, "renaming appends no op");
    check(h.undo().ok && tracks(s).empty(), "undo acts on the last EDIT, not on the rename");
    check(h.listSnapshots().back().name == "verse final", "and the rename survives it");
    check(!h.renameSnapshot(a.id, "").ok, "an empty rename is refused");
    check(!h.renameSnapshot(999, "x").ok, "renaming a missing snapshot is refused");
}

// --- revert never discards (ADR-0128 d2) -------------------------------------------------

void testRevertThenEditKeepsTheOldLine() {
    section("revert, then a new edit -- and the old line is still reachable");
    Fixture f("revert");
    if (!f.store) return;
    History h(*f.store);
    Store& s = *f.store;

    mk(s, 1, "A");
    mk(s, 2, "B");
    const auto snapSeq = h.head();
    const auto snap = h.takeSnapshot("two tracks", kT0, 0);
    const auto atSnap = digestProject(s).text;
    mk(s, 3, "C");
    mk(s, 4, "D");
    const auto oldTip = h.head();
    const auto opsBefore = count(s, "SELECT COUNT(*) FROM ops");

    const auto r = h.revertToSnapshot(snap.id, kT0 + 5);
    check(r.ok, "revert runs: " + r.error);
    check(tracks(s) == "A,B", "the project is as it was at the snapshot: " + tracks(s));
    check(digestProject(s).text == atSnap, "byte for byte, by the replay digest");
    check(h.head() == snapSeq, "the head is the snapshot's point");
    check(count(s, "SELECT COUNT(*) FROM ops") == opsBefore, "revert appends nothing and deletes nothing");

    // Until the next edit, the old line is the redo line -- the same state an
    // undo leaves -- so it is reachable without a branch row of its own.
    check(h.nextRedo().has_value(), "C is redoable from the snapshot's point");

    check(mk(s, 5, "E"), "a new edit after the revert");
    check(tracks(s) == "A,B,E", "lands on the reverted state: " + tracks(s));
    const auto kept = branchHolding(h, oldTip);
    check(kept.has_value(), "and the edit forked the old line into a branch holding its tip");

    // Reachable means reachable: go there.
    const auto sw = h.switchToBranch(kept.value_or(-1));
    check(sw.ok, "switch to the kept branch: " + sw.error);
    check(tracks(s) == "A,B,C,D", "C and D are back, E is not: " + tracks(s));
    check(h.head() == oldTip, "at the old tip");
    check(h.undo().ok && tracks(s) == "A,B,C", "and undo walks that line as before");
}

void testRevertOnTheSameLineMakesOneBranch() {
    section("reverting to an earlier point on the same line keeps redo, and forks once");
    Fixture f("sameline");
    if (!f.store) return;
    History h(*f.store);
    Store& s = *f.store;

    mk(s, 1, "A");
    const auto snap = h.takeSnapshot("one", kT0, 0);
    mk(s, 2, "B");
    const auto tip = h.head();
    const auto branchesBefore = h.branches().size();

    check(h.revertToSnapshot(snap.id, kT0).ok && tracks(s) == "A", "reverted to A");
    check(h.branches().size() == branchesBefore,
          "no branch yet: the line is exactly where redo from here leads");
    check(h.canRedo() && h.redo().ok && tracks(s) == "A,B", "redo still reaches B");
    check(h.revertToSnapshot(snap.id, kT0).ok, "revert again");
    check(mk(s, 3, "C") && tracks(s) == "A,C", "a new edit forks");
    int holding = 0;
    for (const auto& b : h.branches()) holding += (b.headSeq == tip) ? 1 : 0;
    check(holding == 1, "and exactly one branch holds B's line, saw " + std::to_string(holding));
}

void testSnapshotOnAnotherBranch() {
    section("a snapshot on another branch: rewind to the common ancestor, replay across");
    Fixture f("otherbranch");
    if (!f.store) return;
    History h(*f.store);
    Store& s = *f.store;

    mk(s, 1, "A");
    mk(s, 2, "B");
    const auto snap = h.takeSnapshot("with B", kT0, 0);
    const auto bSeq = h.head();
    check(h.undo().ok, "undo B");
    check(mk(s, 3, "C"), "and do C instead: B's line forks off");
    const auto cSeq = h.head();
    check(branchHolding(h, bSeq).has_value(), "B's line is now another branch");
    check(tracks(s) == "A,C", "A,C now");

    const auto r = h.revertToSnapshot(snap.id, kT0 + 7);
    check(r.ok, "revert across branches: " + r.error);
    check(tracks(s) == "A,B", "A,B: C rewound, B replayed: " + tracks(s));
    check(r.opsApplied == 2, "exactly two ops moved -- nothing below the ancestor, saw " +
                                 std::to_string(r.opsApplied));
    check(h.head() == bSeq, "the head is on B");
    check(h.listSnapshots().front().branchId == 1,
          "the snapshot still records the branch it was taken on");

    // Here redo from B leads nowhere near C, so the revert itself must name
    // C's line -- the current branch's head is leaving it.
    const auto kept = branchHolding(h, cSeq);
    check(kept.has_value(), "C's line was kept, by the revert itself");
    SQLite::Statement nm(s.db(), "SELECT name, created_utc FROM op_branches WHERE id = ?");
    nm.bind(1, kept.value_or(-1));
    check(nm.executeStep() && nm.getColumn(0).getString() == "before revert to 'with B'" &&
              nm.getColumn(1).getInt64() == kT0 + 7,
          "named for the snapshot, stamped with the time passed in");
    check(h.switchToBranch(kept.value_or(-1)).ok && tracks(s) == "A,C",
          "and is reachable: A,C again: " + tracks(s));
}

void testRevertToRoot() {
    section("a snapshot of the empty project");
    Fixture f("root");
    if (!f.store) return;
    History h(*f.store);
    Store& s = *f.store;
    const auto empty = h.takeSnapshot("", kT0, 0);
    mk(s, 1, "A");
    mk(s, 2, "B");
    const auto tip = h.head();
    check(h.revertToSnapshot(empty.id, kT0).ok && tracks(s).empty(), "reverts to nothing");
    check(!h.head().has_value(), "the head is the root");
    check(h.redo().ok && tracks(s) == "A", "and redo leads back up the line");
    check(h.revertToSnapshot(empty.id, kT0).ok, "again");
    check(branchHolding(h, tip) == std::nullopt, "no branch was needed: redo reaches it");
    check(h.revertToSnapshot(empty.id, kT0).ok, "reverting to where the head already is is a no-op");
}

// --- refusals -------------------------------------------------------------------------

void testRevertIsAtomic() {
    section("a revert that cannot complete changes nothing");
    Fixture f("atomic");
    if (!f.store) return;
    History h(*f.store);
    Store& s = *f.store;
    mk(s, 1, "A");
    const auto snap = h.takeSnapshot("A", kT0, 0);
    mk(s, 2, "B");
    mk(s, 3, "C");
    // B's inverse is lost (a state only corruption produces). The rewind
    // undoes C first, then fails at B -- and C's undo must not survive it.
    s.db().exec("UPDATE ops SET inverse = NULL WHERE seq = (SELECT MAX(seq) FROM ops) - 1");
    const auto head = h.head();
    const auto branches = h.branches().size();
    const auto r = h.revertToSnapshot(snap.id, kT0);
    check(!r.ok, "the revert fails: " + r.error);
    check(tracks(s) == "A,B,C", "the project is untouched: " + tracks(s));
    check(h.head() == head && h.branches().size() == branches,
          "the head did not move and no branch was created");
    check(!h.revertToSnapshot(999, kT0).ok, "a missing snapshot is refused");
}

void testReadOnly() {
    section("a read-only project refuses to take, rename or revert");
    adi::test::TempDirectory temp("snapshots", "ro");
    const auto path = temp.path() / "p.adi";
    std::int64_t id = 0;
    {
        StoreError e = StoreError::Ok;
        auto s = Store::create(path, e);
        if (!s) return;
        s->db().exec("INSERT INTO project(id,name) VALUES (1,'R')");
        mk(*s, 1, "A");
        History h(*s);
        id = h.takeSnapshot("", kT0, 0).id;
        mk(*s, 2, "B");
    }
    StoreError e = StoreError::Ok;
    auto s = Store::open(path, e, /*readOnly=*/true);
    if (!s) return;
    History h(*s);
    check(h.listSnapshots().size() == 1, "a read-only project still lists its snapshots");
    check(!h.takeSnapshot("x", kT0, 0).ok, "but cannot take one");
    check(!h.renameSnapshot(id, "x").ok, "or rename one");
    check(!h.revertToSnapshot(id, kT0).ok && tracks(*s) == "A,B", "or revert");
}

int runAll() {
    std::printf("adi_snapshots_tests -- ADR-0128, ADR-0140, SPEC 8.6\n\n");
    testDefaultName();
    testTakeListRename();
    testRevertThenEditKeepsTheOldLine();
    testRevertOnTheSameLineMakesOneBranch();
    testSnapshotOnAnotherBranch();
    testRevertToRoot();
    testRevertIsAtomic();
    testReadOnly();
    std::printf("\n%s -- %d checks, %d failure(s)\n", g_failures ? "FAILED" : "PASS", g_checks,
                g_failures);
    return g_failures ? 1 : 0;
}

}  // namespace

int main() {
    // Unbuffered, so the last line before a crash survives (see test_history).
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    try {
        return runAll();
    } catch (const std::exception& e) {
        std::printf("\nFAILED -- exception escaped: %s\n", e.what());
        return 1;
    }
}
