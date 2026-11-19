// SPDX-License-Identifier: GPL-3.0-or-later
//
// Undo, redo, and the branching tree. SPEC §8.2, ADR-0003, ADR-0021, ADR-0030.
//
// The three that justify putting the log in the file at all:
//   * undo SURVIVES A RESTART -- the thing an in-memory UndoManager cannot do
//   * a transaction undoes AS ONE, however many ops it made
//   * undoing then doing something new FORKS rather than destroying

#include "temp_directory.hpp"

#include "adi/history.hpp"
#include "adi/ops.hpp"
#include "adi/store.hpp"

#include <SQLiteCpp/SQLiteCpp.h>

#include <array>
#include <cstdio>
#include <exception>
#include <filesystem>
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

struct Scratch {
    adi::test::TempDirectory temp; // destroyed after store/file members
    fs::path dir;
    explicit Scratch(const char* n)
        : temp("history", n), dir(temp.path()) {
    }
    fs::path operator/(const char* l) const { return dir / l; }
};

std::unique_ptr<Store> project(const fs::path& p) {
    StoreError e = StoreError::Ok;
    auto st = Store::createEmpty(p, e);
    if (st) st->db().exec("INSERT INTO project(id,name) VALUES (1,'Untitled')");
    return st;
}

OpRequest mkTrack(std::int64_t id, const char* name) {
    OpRequest r;
    r.opType = "track.create";
    r.payload = {{"id", id}, {"kind", "midi"}, {"name", name}};
    r.label = std::string("Create ") + name;
    return r;
}

std::string trackName(Store& s, std::int64_t id) {
    SQLite::Statement st(s.db(), "SELECT name FROM tracks WHERE id = ?");
    st.bind(1, id);
    return st.executeStep() ? st.getColumn(0).getString() : std::string("<gone>");
}

int trackCount(Store& s) {
    return s.db().execAndGet("SELECT COUNT(*) FROM tracks").getInt();
}

// --- the basics ---------------------------------------------------------------

void testUndoRedoRoundTrip() {
    section("undo / redo");
    Scratch sc("basic");
    auto s = project(sc / "p.adi");
    if (!s) return;
    OpJournal j(*s);
    History h(*s);

    check(!h.canUndo(), "a fresh project has nothing to undo");
    check(!h.canRedo(), "and nothing to redo");

    check(j.commit(mkTrack(1, "Keys")).ok, "created a track");
    check(h.canUndo(), "now there is something to undo");
    check(!h.canRedo(), "but still nothing to redo");

    const auto step = h.nextUndo();
    check(step && step->label == "Create Keys",
          "the undo menu shows the label the caller gave");

    auto r = h.undo();
    check(r.ok, "undo succeeds: " + r.error);
    check(trackCount(*s) == 0, "the track is gone");
    check(!h.canUndo(), "back at the root, nothing left to undo");
    check(h.canRedo(), "and redo is now available");

    r = h.redo();
    check(r.ok, "redo succeeds: " + r.error);
    check(trackCount(*s) == 1, "the track is back");
    check(trackName(*s, 1) == "Keys", "with the same name");
    check(h.canUndo() && !h.canRedo(), "and we are at the tip again");
}

void testTransactionUndoesAsOne() {
    section("a transaction undoes as one (OPS.md 6.4)");
    Scratch sc("txn");
    auto s = project(sc / "p.adi");
    if (!s) return;
    OpJournal j(*s);
    History h(*s);

    OpRequest a = mkTrack(1, "A"), b = mkTrack(2, "B"), c = mkTrack(3, "C");
    a.label = "Add three tracks";
    const OpRequest batch[] = {a, b, c};
    check(j.commit(batch).ok, "batch of three committed");
    check(trackCount(*s) == 3, "three tracks exist");

    const auto step = h.nextUndo();
    check(step && step->opCount == 3, "the undo step covers all three ops");
    check(step && step->label == "Add three tracks",
          "and is labelled by the transaction, not the last op");

    const auto r = h.undo();
    check(r.ok, "one undo: " + r.error);
    check(r.opsApplied == 3, "applied three inverses");
    check(trackCount(*s) == 0,
          "all three are gone from ONE undo -- an agent's forty edits are one Ctrl-Z");

    check(h.redo().ok, "one redo brings them all back");
    check(trackCount(*s) == 3, "three again");
}

void testSurvivesRestart() {
    section("undo survives a restart -- the whole point (RATIONALE 5.3)");
    Scratch sc("restart");
    const auto p = sc / "p.adi";
    {
        auto s = project(p);
        if (!s) return;
        OpJournal j(*s);
        check(j.commit(mkTrack(1, "Yesterday")).ok, "committed");
        check(s->close(), "closed cleanly");
    }
    StoreError e = StoreError::Ok;
    auto s = Store::open(p, e);
    check(s != nullptr, "reopened");
    if (!s) return;
    History h(*s);

    check(h.canUndo(), "the undo is still available after a full close and reopen");
    const auto step = h.nextUndo();
    check(step && step->label == "Create Yesterday", "with its label intact");
    check(h.undo().ok, "and it applies");
    check(trackCount(*s) == 0,
          "an edit from a previous session was undone -- a QUndoStack cannot do this");
}

// --- the tree -------------------------------------------------------------------

void testForkPreservesTheAbandonedLine() {
    section("SPEC 8.2: undo then do something new FORKS");
    Scratch sc("fork");
    auto s = project(sc / "p.adi");
    if (!s) return;
    OpJournal j(*s);
    History h(*s);

    check(j.commit(mkTrack(1, "First")).ok, "first");
    check(j.commit(mkTrack(2, "Second")).ok, "second");
    check(trackCount(*s) == 2, "two tracks");

    check(h.undo().ok, "undo the second");
    check(trackCount(*s) == 1, "one track");
    check(h.canRedo(), "the second is redoable");

    const auto before = h.branches().size();

    // Committing from here diverges from the line that held "Second".
    check(j.commit(mkTrack(3, "Instead")).ok, "commit something else instead");
    check(trackCount(*s) == 2, "two tracks again, but a different second");
    check(trackName(*s, 3) == "Instead", "the new work is present");
    check(trackName(*s, 2) == "<gone>", "and the abandoned work is not");

    const auto after = h.branches();
    check(after.size() == before + 1,
          "the abandoned line was saved as a branch rather than destroyed");

    // The abandoned ops are still in the log -- nothing was deleted.
    const auto stillThere = s->db()
        .execAndGet("SELECT COUNT(*) FROM ops WHERE op_type='track.create'").getInt();
    check(stillThere == 3, "all three creates are still in the log, saw " +
                               std::to_string(stillThere));

    // Redo follows the NEW line, not the abandoned one.
    check(h.undo().ok, "undo the new work");
    const auto redoStep = h.nextRedo();
    check(redoStep.has_value(), "there is something to redo");
    check(h.redo().ok, "redo");
    check(trackName(*s, 3) == "Instead",
          "redo followed the most recent line, not the abandoned one");
}

void testUndoRedoStack() {
    section("multi-step undo and redo");
    Scratch sc("stack");
    auto s = project(sc / "p.adi");
    if (!s) return;
    OpJournal j(*s);
    History h(*s);

    for (int i = 1; i <= 4; ++i)
        check(j.commit(mkTrack(i, ("T" + std::to_string(i)).c_str())).ok,
              "created T" + std::to_string(i));
    check(trackCount(*s) == 4, "four tracks");

    for (int i = 0; i < 4; ++i) check(h.undo().ok, "undo " + std::to_string(i + 1));
    check(trackCount(*s) == 0, "all undone");
    check(!h.canUndo(), "at the root");

    for (int i = 0; i < 4; ++i) check(h.redo().ok, "redo " + std::to_string(i + 1));
    check(trackCount(*s) == 4, "all redone");
    check(!h.canRedo(), "at the tip");
    check(trackName(*s, 4) == "T4", "and in the right order");
}

// --- the properties that are easy to get wrong ------------------------------------

void testUndoDoesNotGrowTheLog() {
    section("ADR-0030: undo moves the head, it does not append");
    Scratch sc("nogrow");
    auto s = project(sc / "p.adi");
    if (!s) return;
    OpJournal j(*s);
    History h(*s);

    check(j.commit(mkTrack(1, "One")).ok, "committed");
    const auto after = j.count();

    for (int i = 0; i < 5; ++i) {
        check(h.undo().ok, "undo");
        check(h.redo().ok, "redo");
    }
    check(j.count() == after,
          "five undo/redo cycles added no log rows -- otherwise toggling Ctrl-Z "
          "would grow the file without bound, saw " + std::to_string(j.count()));
}

void testEphemeralSkipped() {
    section("OPS.md 10: ephemeral ops stay outside the tree while undone");
    Scratch sc("ephemeral");
    auto s = project(sc / "p.adi");
    if (!s) return;
    OpJournal j(*s);
    History h(*s);

    check(j.commit(mkTrack(1, "Real")).ok, "a real edit");
    check(j.commit(mkTrack(2, "Original")).ok, "a second edit to leave in the redo tail");
    const auto originalTip = h.head();
    check(h.undo().ok, "undo the second edit");
    const auto fork = h.head();
    const auto before = h.branches().size();

    OpRequest play, stop;
    play.opType = "transport.play";
    stop.opType = "transport.stop";
    play.payload = Payload::object();
    stop.payload = Payload::object();
    for (const auto& op : {play, stop, play, stop})
        check(j.commit(op).ok, "Play or Stop commits to the audit log while undone");
    const std::array transport{play, stop};
    check(j.commit(transport).ok, "a whole batch of Play and Stop commits");
    check(h.branches().size() == before, "Play and Stop while undone create no history branches");
    check(h.head() == fork, "an ephemeral batch does not move the head");
    const auto step = h.nextUndo();
    check(step && step->label == "Create Real",
          "Undo still lands on the last real edit");
    const auto redo = h.nextRedo();
    check(redo && redo->label == "Create Original", "the existing redo tail survives performance");
    check(j.count() == 8, "the audit trail keeps two edits and six performance ops");
    const auto audited = s->db()
        .execAndGet("SELECT COUNT(*) FROM ops WHERE tags='ephemeral' AND parent_seq IS NULL AND inverse IS NULL")
        .getInt();
    check(audited == 6, "ephemeral audit rows have no parent or inverse");
    if (!originalTip) return;

    const auto beforeMixed = h.branches().size();
    const std::array mixed{play, mkTrack(3, "Mixed")};
    check(j.commit(mixed).ok && trackCount(*s) == 2, "mixed Play and edit applies a real edit");
    check(h.branches().size() == beforeMixed + 1, "a mixed batch preserves the branch it diverges from");
    SQLite::Statement original(s->db(), "SELECT 1 FROM op_branches WHERE is_current=0 AND head_seq=?");
    original.bind(1, *originalTip);
    check(original.executeStep(), "the preserved branch points to the original redo tip");
    original.reset();

    const auto mixedTip = h.head();
    check(h.undo().ok && h.head() == fork && trackName(*s, 3) == "<gone>",
          "Undo of the mixed batch reverts its durable edit");
    if (!mixedTip) return;
    SQLite::Statement pin(s->db(),
        "INSERT INTO op_branches(name,head_seq,created_utc,is_current,created_by) VALUES('Kept mix',?,0,0,'user')");
    pin.bind(1, *mixedTip);
    pin.exec();
    const auto beforePinned = h.branches().size();
    check(j.commit(mkTrack(4, "Alternate")).ok, "a subsequent edit diverges from the pinned tip");
    check(h.branches().size() == beforePinned, "a fork preserves an existing named tip without a duplicate branch");
    check(h.undo().ok && h.redo().ok && trackName(*s, 4) == "Alternate",
          "redo follows the latest durable line");
}

void testSelBeforeIsCarried() {
    section("ADR-0021 7.5: the selection hint");
    Scratch sc("sel");
    auto s = project(sc / "p.adi");
    if (!s) return;
    OpJournal j(*s);
    History h(*s);

    OpRequest r = mkTrack(1, "Sel");
    Payload sel;
    sel["clips"] = {92, 93};
    r.selBefore = sel;
    check(j.commit(r).ok, "committed with a selection hint");

    const auto u = h.undo();
    check(u.ok, "undo: " + u.error);
    check(u.selBefore.has_value(), "undo returns what was selected beforehand");
    check(u.selBefore && (*u.selBefore)["clips"][1] == 93,
          "and it survived CBOR intact");

    // It is ADVISORY. It must never have reached a handler.
    check(trackCount(*s) == 0, "the undo itself did the right thing regardless");
}

void testUndoIsAtomic() {
    section("an undo that cannot complete changes nothing");
    Scratch sc("atomic");
    auto s = project(sc / "p.adi");
    if (!s) return;
    OpJournal j(*s);
    History h(*s);

    OpRequest a = mkTrack(1, "A"), b = mkTrack(2, "B");
    const OpRequest batch[] = {a, b};
    check(j.commit(batch).ok, "batch committed");
    check(trackCount(*s) == 2, "two tracks");

    // Corrupt one inverse so undoing the transaction must fail partway. The
    // other op's inverse has already been applied by then, and it must be
    // rolled back with it.
    s->db().exec("UPDATE ops SET inverse = NULL WHERE seq = 1");

    const auto headBefore = h.head();
    const auto r = h.undo();
    check(!r.ok, "the undo fails");
    check(r.error.find("no stored inverse") != std::string::npos,
          "and says why: " + r.error);
    check(trackCount(*s) == 2,
          "BOTH tracks are still there -- the partial undo rolled back, saw " +
              std::to_string(trackCount(*s)));
    check(h.head() == headBefore, "and the head did not move");
}

void testReadOnlyRefuses() {
    section("a read-only project refuses to undo");
    Scratch sc("ro");
    const auto p = sc / "p.adi";
    {
        auto s = project(p);
        if (!s) return;
        OpJournal j(*s);
        j.commit(mkTrack(1, "X"));
        s->close();
    }
    StoreError e = StoreError::Ok;
    auto s = Store::open(p, e, /*readOnly=*/true);
    check(s != nullptr && s->readOnly(), "opened read-only");
    if (!s) return;
    History h(*s);
    check(h.canUndo(), "it can still SEE that an undo exists");
    const auto r = h.undo();
    check(!r.ok, "but refuses to perform it");
}

}  // namespace

int runAll() {
    std::printf("adi_history_tests -- SPEC 8.2, ADR-0003/0021/0030\n\n");
    testUndoRedoRoundTrip();
    testTransactionUndoesAsOne();
    testSurvivesRestart();
    testForkPreservesTheAbandonedLine();
    testUndoRedoStack();
    testUndoDoesNotGrowTheLog();
    testEphemeralSkipped();
    testSelBeforeIsCarried();
    testUndoIsAtomic();
    testReadOnlyRefuses();
    std::printf("\n%s -- %d checks, %d failure(s)\n", g_failures ? "FAILED" : "PASS", g_checks,
                g_failures);
    return g_failures ? 1 : 0;
}

int main() {
    // Unbuffered, so the last line before a crash survives. On Windows a
    // crashing test binary loses its whole block-buffered stdout, and the
    // harness then prints a blank line where a failure should be -- which is
    // how adi_device_tests' 383 KB overrun looked like a harness glitch for
    // two runs before anyone ran the binary directly.
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    // OpRegistry::instance() throws if the catalogue is malformed (OPS.md 3).
    // Uncaught, that is abort() -- on Windows a modal dialog that blocks the
    // run rather than reporting it. Catch it and say what is wrong.
    try {
        return runAll();
    } catch (const std::exception& e) {
        std::printf("\nFAILED -- exception escaped: %s\n", e.what());
        return 1;
    }
}
