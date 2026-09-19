// SPDX-License-Identifier: GPL-3.0-or-later
//
// The round-trip corpus. ADR-0021 §7.4.
//
// This is the test the whole op design exists to pass:
//
//     Apply an op log to an empty project twice, under deliberately different
//     UI state, and the two projects must be identical.
//
// It is what separates a log that is REPLAYABLE from one that is merely
// undoable, and replayability is what makes the log a real account of the
// project rather than a convenience for Ctrl-Z. An op that reads ambient state
// (ADR-0021 §7.2) fails here and nowhere else: it will look fine in every unit
// test, because a unit test does not vary the ambience.
//
// The oracle is `digestProject`, which deliberately EXCLUDES the UI tables. That
// exclusion is what makes a match meaningful — the two projects have different
// UI state by construction, so matching proves the ops did not consume it.

#include "adi/digest.hpp"
#include "adi/history.hpp"
#include "adi/ops.hpp"
#include "adi/store.hpp"

#include <SQLiteCpp/SQLiteCpp.h>

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <string>
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
    fs::path dir;
    explicit Scratch(const char* n) {
        dir = fs::temp_directory_path() / ("adi_replay_" + std::string(n));
        std::error_code ec;
        fs::remove_all(dir, ec);
        fs::create_directories(dir, ec);
    }
    ~Scratch() {
        std::error_code ec;
        fs::remove_all(dir, ec);
    }
    fs::path operator/(const char* l) const { return dir / l; }
};

std::unique_ptr<Store> blank(const fs::path& p) {
    StoreError e = StoreError::Ok;
    return Store::create(p, e);
}

// --- the corpus ---------------------------------------------------------------
//
// One scripted session, exercising every op that has a handler and all three
// inverse shapes. Every value is a literal: no op here reads anything it was not
// handed, which is the property under test.

std::vector<OpRequest> corpus() {
    std::vector<OpRequest> v;

    auto op = [&](const char* type, Payload p, const char* label,
                  Actor a = Actor::User, const char* detail = "") {
        OpRequest r;
        r.opType = type;
        r.payload = std::move(p);
        r.label = label;
        r.actor = a;
        r.actorDetail = detail;
        v.push_back(std::move(r));
    };

    op("project.setName", {{"name", "Round Trip"}}, "Name the project");
    op("track.create", {{"id", 10}, {"kind", "midi"}, {"name", "Keys"}}, "Add Keys");
    op("track.create", {{"id", 11}, {"kind", "audio"}, {"name", "Vox"}}, "Add Vox");
    op("track.create", {{"id", 12}, {"kind", "group"}, {"name", "Bus"}}, "Add Bus");
    // Ids are caller-allocated and deliberately NOT sequential, so a handler
    // that quietly used rowid order instead would diverge.
    op("track.create", {{"id", 7}, {"kind", "midi"}, {"name", "Drums"},
                        {"parent", 12}, {"index", 3}}, "Add Drums under Bus");
    op("track.rename", {{"id", 11}, {"name", "Lead Vocal"}}, "Rename Vox");
    op("track.setMute", {{"id", 7}, {"muted", true}}, "Mute Drums");
    op("track.setMute", {{"id", 10}, {"muted", false}}, "Unmute Keys");
    // An agent-authored edit, so attribution is exercised too.
    op("track.rename", {{"id", 10}, {"name", "Rhodes"}}, "Agent renames Keys",
       Actor::Agent, "adi-agent/claude-opus-5");
    op("track.delete", {{"id", 11}}, "Delete Lead Vocal");
    op("project.setName", {{"name", "Round Trip Final"}}, "Rename the project");
    return v;
}

/// Applies the corpus one transaction at a time, so undo has several steps to
/// walk rather than one big one.
bool applyCorpus(Store& s) {
    OpJournal j(s);
    for (const auto& r : corpus()) {
        const auto res = j.commit(r);
        if (!res.ok) {
            std::printf("  (corpus op %s failed: %s)\n", r.opType.c_str(), res.error.c_str());
            return false;
        }
    }
    return true;
}

/// Ambient state that MUST NOT reach an op. Set differently in each project.
void setAmbience(Store& s, int variant) {
    auto& db = s.db();
    SQLite::Statement st(db, "INSERT INTO session_state(key, value) VALUES (?,?)");
    const auto put = [&](const char* k, const std::string& v) {
        st.reset();
        st.bind(1, k);
        st.bind(2, v);
        st.exec();
    };
    if (variant == 0) {
        put("selection", "clip:1,clip:2,track:10");
        put("playhead_ticks", "0");
        put("grid_ticks", "1441440");
        put("zoom_x", "1.0");
    } else {
        put("selection", "track:7");
        put("playhead_ticks", "99532800");
        put("grid_ticks", "720720");
        put("zoom_x", "4.75");
    }
}

// --- the tests -----------------------------------------------------------------

void testReplayUnderDifferentAmbience() {
    section("ADR-0021: the same log, twice, under different UI state");
    Scratch sc("replay");

    auto a = blank(sc / "a.adi");
    auto b = blank(sc / "b.adi");
    check(a && b, "two blank projects");
    if (!a || !b) return;

    // Different selection, playhead, grid and zoom in each. If any op reads
    // ambient state, this is where it shows.
    setAmbience(*a, 0);
    setAmbience(*b, 1);

    check(applyCorpus(*a), "corpus applied to A");
    check(applyCorpus(*b), "corpus applied to B");

    const auto da = digestProject(*a);
    const auto db_ = digestProject(*b);

    check(da.tableCount > 25, "the digest covers the project tier, saw " +
                                  std::to_string(da.tableCount) + " tables");
    check(da.rowCount > 0, "and some rows, saw " + std::to_string(da.rowCount));
    check(da.text == db_.text,
          "the two projects are IDENTICAL despite different UI state\n"
          "        A " + da.fingerprint + "  B " + db_.fingerprint);

    if (da.text != db_.text) {
        // A diff is worth far more than a hash mismatch when this ever fires.
        std::vector<std::string> la, lb;
        auto split = [](const std::string& s, std::vector<std::string>& out) {
            std::string cur;
            for (char c : s) { if (c == '\n') { out.push_back(cur); cur.clear(); } else cur += c; }
        };
        split(da.text, la);
        split(db_.text, lb);
        for (std::size_t i = 0; i < std::max(la.size(), lb.size()) && i < 400; ++i) {
            const std::string x = i < la.size() ? la[i] : "<none>";
            const std::string y = i < lb.size() ? lb[i] : "<none>";
            if (x != y) std::printf("        A: %s\n        B: %s\n", x.c_str(), y.c_str());
        }
    }

    // And the ambience really was different -- otherwise the test proves nothing.
    const auto sa = a->db().execAndGet(
        "SELECT value FROM session_state WHERE key='selection'").getString();
    const auto sb = b->db().execAndGet(
        "SELECT value FROM session_state WHERE key='selection'").getString();
    check(sa != sb, "the two projects really did have different selections");
}

void testDigestIgnoresRowidOrder() {
    section("the oracle itself: content order, not storage order");
    Scratch sc("order");
    auto a = blank(sc / "a.adi");
    auto b = blank(sc / "b.adi");
    if (!a || !b) return;

    OpJournal ja(*a), jb(*b);
    const char* names[] = {"Alpha", "Beta", "Gamma"};
    const std::int64_t ids[] = {30, 20, 10};

    // A inserts ascending, B descending. Same set, different rowid assignment.
    for (int i = 0; i < 3; ++i) {
        OpRequest r;
        r.opType = "track.create";
        r.payload = {{"id", ids[i]}, {"kind", "midi"}, {"name", names[i]}};
        ja.commit(r);
    }
    for (int i = 2; i >= 0; --i) {
        OpRequest r;
        r.opType = "track.create";
        r.payload = {{"id", ids[i]}, {"kind", "midi"}, {"name", names[i]}};
        jb.commit(r);
    }

    check(digestProject(*a).text == digestProject(*b).text,
          "insertion order does not change the digest");

    // And the oracle is not just always-equal: a real difference must show.
    OpRequest extra;
    extra.opType = "track.rename";
    extra.payload = {{"id", 10}, {"name", "Different"}};
    jb.commit(extra);
    check(digestProject(*a).text != digestProject(*b).text,
          "but a genuine difference DOES change it -- the oracle discriminates");
}

void testDigestExclusions() {
    section("the oracle excludes exactly what it should");
    Scratch sc("excl");
    auto s = blank(sc / "p.adi");
    if (!s) return;
    const auto tables = digestedTables(*s);
    const auto has = [&](const char* t) {
        return std::find(tables.begin(), tables.end(), t) != tables.end();
    };
    check(!has("ops"), "the log is excluded -- replay makes new seqs and timestamps");
    check(!has("op_branches"), "and so are the branch heads");
    check(!has("session_state"), "UI state is excluded -- the replay test VARIES it");
    check(!has("ui_view") && !has("window_state"), "as are the other UI tables");
    check(!has("adi_meta") && !has("session_lock"), "and the volatile metadata");

    check(has("tracks") && has("clips") && has("routing"), "but the music is included");
    check(has("event_streams") && has("note_expression"), "including the blobs");
    check(has("media_files"), "and the media pool");
}

void testUndoAllRestoresTheEmptyProject() {
    section("undo everything == never did anything");
    Scratch sc("undoall");
    auto s = blank(sc / "p.adi");
    if (!s) return;

    const auto empty = digestProject(*s);
    check(applyCorpus(*s), "corpus applied");
    const auto full = digestProject(*s);
    check(empty.text != full.text, "the corpus changed the project");

    History h(*s);
    int steps = 0;
    while (h.canUndo() && steps < 100) {
        const auto r = h.undo();
        if (!r.ok) { check(false, "undo failed: " + r.error); break; }
        ++steps;
    }
    check(steps == static_cast<int>(corpus().size()),
          "undid every transaction, " + std::to_string(steps) + " steps");
    check(digestProject(*s).text == empty.text,
          "the project is byte-identical to the one that never had anything done to it");
}

void testUndoAllRedoAllIsIdentity() {
    section("undo everything then redo everything == identity");
    Scratch sc("undoredo");
    auto s = blank(sc / "p.adi");
    if (!s) return;

    check(applyCorpus(*s), "corpus applied");
    const auto before = digestProject(*s);

    History h(*s);
    int undone = 0;
    while (h.canUndo() && undone < 100) { h.undo(); ++undone; }
    int redone = 0;
    while (h.canRedo() && redone < 100) { h.redo(); ++redone; }

    check(undone == redone, "redid as many steps as were undone (" +
                                std::to_string(undone) + "/" + std::to_string(redone) + ")");
    check(digestProject(*s).text == before.text,
          "and the project came back byte-identical -- inverses really do invert");
}

void testReplayAcrossAClose() {
    section("replay survives a close and reopen");
    Scratch sc("persist");
    const auto pa = sc / "a.adi";
    const auto pb = sc / "b.adi";

    {
        auto a = blank(pa);
        if (!a) return;
        check(applyCorpus(*a), "corpus applied to A");
        check(a->close(), "A closed cleanly");
    }
    {
        // B does the same work but in two sessions, closing in between -- a
        // WAL checkpoint boundary in the middle of the log.
        auto b = blank(pb);
        if (!b) return;
        OpJournal j(*b);
        const auto ops = corpus();
        for (std::size_t i = 0; i < ops.size() / 2; ++i) j.commit(ops[i]);
        check(b->close(), "B closed mid-corpus");
    }
    {
        StoreError e = StoreError::Ok;
        auto b = Store::open(pb, e);
        check(b != nullptr, "B reopened");
        if (!b) return;
        OpJournal j(*b);
        const auto ops = corpus();
        for (std::size_t i = ops.size() / 2; i < ops.size(); ++i) {
            const auto r = j.commit(ops[i]);
            check(r.ok, "second half op " + ops[i].opType + ": " + r.error);
        }
        check(b->close(), "B closed again");
    }

    StoreError e = StoreError::Ok;
    auto a = Store::open(pa, e, true);
    auto b = Store::open(pb, e, true);
    check(a && b, "both reopened for comparison");
    if (!a || !b) return;
    check(digestProject(*a).text == digestProject(*b).text,
          "a log applied in one session and in two produce identical projects");
}

void testAgentAndUserProduceTheSameProject() {
    section("who ran the op does not change what it did");
    Scratch sc("actor");
    auto a = blank(sc / "a.adi");
    auto b = blank(sc / "b.adi");
    if (!a || !b) return;

    OpJournal ja(*a), jb(*b);
    for (int i = 0; i < 2; ++i) {
        OpRequest r;
        r.opType = "track.create";
        r.payload = {{"id", 1}, {"kind", "midi"}, {"name", "T"}};
        r.actor = i ? Actor::Agent : Actor::User;
        r.actorDetail = i ? "adi-agent/claude-opus-5" : "";
        (i ? jb : ja).commit(r);
    }
    check(digestProject(*a).text == digestProject(*b).text,
          "an agent's edit and a user's edit leave the project in the same state");
    // Attribution still differs -- in the log, which the digest excludes.
    const auto agentOps = b->db()
        .execAndGet("SELECT COUNT(*) FROM ops WHERE actor='agent'").getInt();
    check(agentOps == 1, "while the log still records who did it");
}

}  // namespace

int main() {
    std::printf("adi_replay_tests -- ADR-0021 7.4, the round-trip corpus\n\n");
    testDigestExclusions();
    testDigestIgnoresRowidOrder();
    testReplayUnderDifferentAmbience();
    testUndoAllRestoresTheEmptyProject();
    testUndoAllRedoAllIsIdentity();
    testReplayAcrossAClose();
    testAgentAndUserProduceTheSameProject();
    std::printf("\n%s -- %d checks, %d failure(s)\n", g_failures ? "FAILED" : "PASS", g_checks,
                g_failures);
    return g_failures ? 1 : 0;
}
