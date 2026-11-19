// SPDX-License-Identifier: GPL-3.0-or-later
//
// Undo, redo, and the branching tree. SPEC §8.2, ADR-0003, ADR-0021, ADR-0030.
//
// The three that justify putting the log in the file at all:
//   * undo SURVIVES A RESTART -- the thing an in-memory UndoManager cannot do
//   * a transaction undoes AS ONE, however many ops it made
//   * undoing then doing something new FORKS rather than destroying

#include "temp_directory.hpp"

#include "adi/applied_batches.hpp"
#include "adi/audio/wav_file.hpp"
#include "adi/history.hpp"
#include "adi/media/media_ops.hpp"
#include "adi/ops.hpp"
#include "adi/store.hpp"
#include "adi/take_lane_ops.hpp"

#include <SQLiteCpp/SQLiteCpp.h>

#include <array>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <memory>
#include <string>
#include <utility>
#include <vector>

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

void testLoopPassUndoGroups() {
    section("CT-02 loop passes share one commit and undo separately");
    Scratch sc("loop_pass_groups");
    const auto path = sc / "p.adi";
    auto s = project(path);
    if (!s) return;
    OpJournal j(*s);
    History h(*s);
    auto request = [](const char* type, Payload payload) {
        OpRequest op;
        op.opType = type;
        op.payload = std::move(payload);
        return op;
    };
    const auto clip = [&](int id, const char* name) {
        return request("clip.create", {{"id",id},{"track",1},{"kind","audio"},{"name",name},{"pos",0},{"length",960}});
    };
    auto track = mkTrack(1,"Recorded");
    track.payload["kind"] = "audio";
    const std::array initial{track, clip(1,"Before recording")};
    check(j.commit(initial).ok, "the track and its original main clip commit");
    const auto baseline = h.head();
    const auto baselineCount = j.count();
    const auto laneCount = [&] { return s->db().execAndGet("SELECT COUNT(*) FROM lanes").getInt(); };
    const auto mediaCount = [&] { return s->db().execAndGet("SELECT COUNT(*) FROM media_files").getInt(); };
    const auto mainName = [&] { return s->db().execAndGet("SELECT name FROM clips WHERE id=1").getString(); };

    const auto audioPath = sc / "capture.wav";
    {
        std::array<float,64> silence{};
        audio::WavWriter wave(audioPath,48000,1,audio::WavFormat::Float32);
        wave.write(silence.data(),static_cast<std::uint32_t>(silence.size()));
        wave.close();
    }
    OpRequest mediaRequest;
    const auto imported = media::prepareImport(*s,audioPath,7,0,mediaRequest);
    check(imported.ok && !imported.existing, "prepare one recorded media row for all four takes");
    if (!imported.ok) return;
    std::vector<OpRequest> passes;
    std::array<std::size_t,4> ends{};
    for (int pass = 1; pass <= 4; ++pass) {
        const auto name = "Pass " + std::to_string(pass);
        if (pass == 1) {
            mediaRequest.label = "Record " + name;
            mediaRequest.selBefore = Payload{{"clips", {pass}}};
            passes.push_back(std::move(mediaRequest));
        }
        auto lane = createTakeLane(*s, pass+10, 1, name);
        lane.label = "Record " + name;
        lane.selBefore = Payload{{"clips", {pass}}};
        passes.push_back(std::move(lane));
        passes.push_back(clip(pass+20, name.c_str()));
        passes.push_back(request("clip.attachAudio", {{"id",pass+20},{"media",imported.mediaId},{"frames",64}}));
        passes.push_back(request("clip.setLane", {{"id",pass+20},{"lane",pass+10}}));
        passes.push_back(request("clip.setName", {{"id",1},{"name",name}}));
        ends[static_cast<std::size_t>(pass-1)] = passes.size();
    }

    struct Recorder final : AppliedObserver {
        std::vector<std::shared_ptr<const AppliedBatch>> batches;
        void committed(std::shared_ptr<const AppliedBatch> batch) override { batches.push_back(std::move(batch)); }
        void sourceInvalidated(const AppliedSource&, AppliedSourceInvalidation) override {}
    };
    auto recorder = std::make_shared<Recorder>();
    const auto subscription = appliedSource(*s).observe(recorder);
    check(subscription.ok, "observe durable applied batches");
    SQLite::Database outside(path.string(), SQLite::OPEN_READONLY);
    check(!j.commitGrouped(passes, {}).ok &&
          !j.commitGrouped(passes, std::array<std::size_t,2>{passes.size(),passes.size()}).ok &&
          !j.commitGrouped(passes, std::array<std::size_t,1>{passes.size()-1}).ok &&
          h.head() == baseline && recorder->batches.empty(),
          "empty, repeated or incomplete group ends cannot change the project");

    s->db().exec("CREATE TRIGGER refuse_last_pass BEFORE UPDATE OF name ON clips "
                 "WHEN NEW.name='Pass 4' BEGIN SELECT RAISE(ABORT,'last pass refused'); END;");
    const auto late = j.commitGrouped(passes, ends);
    check(!late.ok && late.seqs.empty() && late.groupTxnIds.empty() && late.txnId == 0 &&
          laneCount() == 0 && mediaCount() == 0 && mainName() == "Before recording" && h.head() == baseline &&
          j.count() == baselineCount && recorder->batches.empty(),
          "a handler failure in the last pass rolls back every preceding pass and publishes nothing");
    s->db().exec("DROP TRIGGER refuse_last_pass");
    CommitOptions refusal;
    refusal.beforeCommit = [](SQLite::Database& db, std::int64_t, std::string& error) {
        db.exec("UPDATE project SET name='Uncommitted recorder metadata'");
        error = "publication refused";
        return false;
    };
    check(!j.commitGrouped(passes, ends, refusal).ok && laneCount() == 0 && mediaCount() == 0 &&
          s->db().execAndGet("SELECT name FROM project").getString() == "Untitled" &&
          recorder->batches.empty(), "a refused publication rolls back its metadata and all pass operations");

    int hooks = 0;
    std::int64_t firstHookTxn = 0;
    CommitOptions options;
    options.expectHead = baseline;
    options.beforeCommit = [&](SQLite::Database& db, std::int64_t txnId, std::string&) {
        ++hooks;
        firstHookTxn = txnId;
        return db.execAndGet("SELECT COUNT(*) FROM lanes").getInt() == 4 &&
               db.execAndGet("SELECT COUNT(*) FROM media_files").getInt() == 1 &&
               db.execAndGet("SELECT COUNT(DISTINCT txn_id) FROM ops").getInt() == 5 &&
               outside.execAndGet("SELECT COUNT(*) FROM lanes").getInt() == 0 &&
               outside.execAndGet("SELECT COUNT(*) FROM media_files").getInt() == 0;
    };
    const auto result = j.commitGrouped(passes, ends, options);
    check(result.ok && hooks == 1 && result.txnId == firstHookTxn && result.groupTxnIds.size() == ends.size() &&
          result.seqs.size() == passes.size() && outside.execAndGet("SELECT COUNT(*) FROM lanes").getInt() == 4,
          "one SQL commit publishes all pass groups together to an independent reader");
    if (!result.ok || result.groupTxnIds.size() != ends.size()) return;
    const auto batch = recorder->batches.empty() ? nullptr : recorder->batches.front();
    bool notice = recorder->batches.size() == 1 && batch && batch->beforeHead() == baseline &&
                  batch->afterHead() == h.head() && batch->operations().size() == passes.size();
    if (notice) {
        std::size_t index = 0;
        for (std::size_t group = 0; group < ends.size(); ++group)
            for (; index < ends[group]; ++index)
                notice = notice && batch->operations()[index].txnId == result.groupTxnIds[group];
    }
    check(notice, "one applied-batch notice carries each operation's actual Undo ID");

    for (int pass = 4; pass > 0; --pass) {
        const auto step = h.nextUndo();
        check(step && step->txnId == result.groupTxnIds[static_cast<std::size_t>(pass-1)] &&
              step->opCount == (pass == 1 ? 6 : 5) && step->label == "Record Pass " + std::to_string(pass),
              "Undo names the last completed pass and its operations");
        const auto undone = h.undo();
        check(undone.ok && undone.selBefore == Payload{{"clips",{pass}}} && laneCount() == pass-1 &&
              mediaCount() == (pass == 1 ? 0 : 1) &&
              mainName() == (pass == 1 ? "Before recording" : "Pass " + std::to_string(pass-1)),
              "one Undo removes just its take and restores the preceding main clip");
    }
    check(h.head() == baseline, "all passes undo back to the original head");
    for (int pass = 1; pass <= 4; ++pass)
        check(h.redo().ok && laneCount() == pass && mediaCount() == 1 &&
              mainName() == "Pass " + std::to_string(pass),
              "redo restores one take and its main clip at a time");
    check(h.head() == result.seqs.back() && j.count() == baselineCount + static_cast<std::int64_t>(passes.size()),
          "Undo and Redo preserve the durable pass journal");
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
    section("OPS.md 10: ephemeral ops are outside the tree");
    Scratch sc("ephemeral");
    auto s = project(sc / "p.adi");
    if (!s) return;
    OpJournal j(*s);
    History h(*s);

    check(j.commit(mkTrack(1, "Real")).ok, "a real edit");
    const auto headAfterEdit = h.head();

    // No ephemeral op is registered yet, so simulate one as the journal writes
    // it: in the log, tagged, with no parent, and not moving the head.
    s->db().exec(
        "INSERT INTO ops(txn_id, parent_seq, ts_utc, actor, op_type, label, tags) "
        "VALUES (99, NULL, 0, 'user', 'transport.play', 'Play', 'ephemeral')");

    check(h.head() == headAfterEdit, "an ephemeral op does not move the head");
    const auto step = h.nextUndo();
    check(step && step->label == "Create Real",
          "Ctrl-Z still lands on the last real EDIT, not on Play");
    check(j.count() == 2, "but it is in the log -- the audit trail keeps it");
    const auto audited = s->db()
        .execAndGet("SELECT COUNT(*) FROM ops WHERE tags='ephemeral'").getInt();
    check(audited == 1, "and is queryable as ephemeral");
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
    testLoopPassUndoGroups();
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
