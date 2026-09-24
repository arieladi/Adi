// SPDX-License-Identifier: GPL-3.0-or-later
//
// Remarks: ADR-0131 d2-d4, SPEC 6.8, OPS.md 9.12.
//
// Each op is checked the way test_catalog checks the rest: apply it, look at
// the row, undo it, look again, redo it. The row is compared column by column,
// because a remark.remove whose inverse forgot one column (the author, the
// time, the resolved flag) would still "bring the remark back" -- as a
// different remark.

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

struct Fixture {
    adi::test::TempDirectory temp;   // destroyed after the store
    std::unique_ptr<Store> store;
    explicit Fixture(const char* n) : temp("remarks", n) {
        StoreError e = StoreError::Ok;
        store = Store::create(temp.path() / "p.adi", e);
        if (!store) return;
        store->db().exec("INSERT INTO project(id,name) VALUES (1,'Remarks')");
        OpJournal j(*store);
        OpRequest r;
        r.opType = "track.create";
        r.payload = {{"id", 1}, {"kind", "audio"}, {"name", "Bass"}};
        j.commit(r);
    }
    SQLite::Database& db() { return store->db(); }
};

CommitResult run(Store& s, const char* type, Payload p, Actor actor = Actor::User) {
    OpJournal j(s);
    OpRequest r;
    r.opType = type;
    r.payload = std::move(p);
    r.actor = actor;
    r.label = type;
    return j.commit(r);
}

/// Every column of one remark, rendered, or "<none>".
std::string row(Store& s, std::int64_t id) {
    SQLite::Statement st(s.db(),
        "SELECT target_kind, target_id, IFNULL(param_id, '~'), author, actor_detail, "
        "text, created_utc, resolved FROM remarks WHERE id = ?");
    st.bind(1, id);
    if (!st.executeStep()) return "<none>";
    std::string out;
    for (int i = 0; i < 8; ++i) out += st.getColumn(i).getString() + "|";
    return out;
}

Payload trackRemark(std::int64_t id, const char* text) {
    return {{"id", id}, {"kind", "track"}, {"target", 1}, {"author", "user"},
            {"text", text}, {"created", 1790000000000000}};
}

// --- the registry ---------------------------------------------------------------

void testRegistered() {
    section("the four ops are catalogued (OPS.md 9.12)");
    const auto& reg = OpRegistry::instance();
    for (const char* n : {"remark.add", "remark.edit", "remark.resolve", "remark.remove"}) {
        const auto* d = reg.find(n);
        check(d != nullptr, std::string(n) + " is registered");
        if (!d) continue;
        check(d->scope == Scope::Edit && !d->ephemeral,
              std::string(n) + " is an undoable edit, not a transport op");
        check(d->engineImpact == EngineImpact::None,
              std::string(n) + " has no engine impact -- the graph never reads a remark");
        check(d->buildInverse != nullptr, std::string(n) + " builds an inverse");
    }
    const auto* add = reg.find("remark.add");
    const auto* rem = reg.find("remark.remove");
    check(add && add->inverseOp == "remark.remove", "remark.add pairs with remark.remove");
    check(rem && rem->inverseOp == "remark.add", "remark.remove's capture replays through remark.add");
}

// --- each op, applied, undone, redone ----------------------------------------------

void testAdd() {
    section("remark.add, and its paired inverse");
    Fixture f("add");
    if (!f.store) return;
    History h(*f.store);

    auto p = trackRemark(10, "the low end is muddy");
    p["detail"] = "Adi";
    check(run(*f.store, "remark.add", p).ok, "a track remark is added");
    const std::string added = row(*f.store, 10);
    check(added == "track|1|~|user|Adi|the low end is muddy|1790000000000000|0|",
          "every column is the payload's: " + added);

    check(h.undo().ok && row(*f.store, 10) == "<none>", "undo removes it");
    check(h.redo().ok && row(*f.store, 10) == added, "redo puts back the identical row");

    auto dup = trackRemark(10, "again");
    check(!run(*f.store, "remark.add", dup).ok,
          "an id that exists is refused, not reassigned (OPS.md 7.3)");
}

void testRemoveCapturesEverything() {
    section("remark.remove captures every column");
    Fixture f("remove");
    if (!f.store) return;
    History h(*f.store);

    // The awkward row: an agent's remark, on a device parameter, resolved.
    // Every column that a lazy capture could drop has a non-default value.
    f.db().exec("INSERT INTO device_chains(id, track_id) VALUES (1, 1)");
    f.db().exec("INSERT INTO devices(id, chain_id, ord, name) VALUES (7, 1, 0, 'EQ')");
    Payload p = {{"id", 20},        {"kind", "device"},  {"target", 7},
                 {"param", "gain"}, {"author", "agent"}, {"detail", "model-x"},
                 {"text", "cut 3 dB at 250 Hz?"}, {"created", 1790000000000042},
                 {"resolved", true}};
    check(run(*f.store, "remark.add", p, Actor::Agent).ok, "an agent's parameter remark is added");
    const std::string before = row(*f.store, 20);
    check(before == "device|7|gain|agent|model-x|cut 3 dB at 250 Hz?|1790000000000042|1|",
          "as written: " + before);

    check(run(*f.store, "remark.remove", {{"id", 20}}).ok, "it is removed");
    check(row(*f.store, 20) == "<none>", "and is gone");
    check(h.undo().ok, "undo runs");
    check(row(*f.store, 20) == before,
          "undo restores it column for column: " + row(*f.store, 20));
    check(!run(*f.store, "remark.remove", {{"id", 99}}).ok, "removing a missing remark fails");
}

void testEditAndResolve() {
    section("remark.edit and remark.resolve are symmetric");
    Fixture f("edit");
    if (!f.store) return;
    History h(*f.store);
    run(*f.store, "remark.add", trackRemark(30, "first draft"));

    check(run(*f.store, "remark.edit", {{"id", 30}, {"text", "second draft"}}).ok, "edited");
    check(row(*f.store, 30).find("|second draft|") != std::string::npos, "the text changed");
    check(h.undo().ok && row(*f.store, 30).find("|first draft|") != std::string::npos,
          "undo restores the old text");
    check(h.redo().ok && row(*f.store, 30).find("|second draft|") != std::string::npos,
          "redo applies it again");

    check(run(*f.store, "remark.resolve", {{"id", 30}, {"resolved", true}}).ok, "resolved");
    check(row(*f.store, 30).ends_with("|1|"), "resolved = 1");
    check(h.undo().ok && row(*f.store, 30).ends_with("|0|"), "undo reopens it");
    check(!run(*f.store, "remark.edit", {{"id", 31}, {"text", "x"}}).ok,
          "editing a missing remark fails rather than doing nothing");
}

// --- the constraints, through the op layer ---------------------------------------------

void testRefusals() {
    section("what the schema refuses, the op refuses, and the log stays clean");
    Fixture f("refuse");
    if (!f.store) return;
    OpJournal j(*f.store);
    const auto logBefore = j.count();

    auto onTrack = trackRemark(40, "x");
    onTrack["param"] = "gain";
    check(!run(*f.store, "remark.add", onTrack).ok, "a parameter on a track remark is refused");

    auto remote = trackRemark(41, "x");
    remote["author"] = "remote";
    check(!run(*f.store, "remark.add", remote).ok, "an author outside user|agent is refused");

    check(!run(*f.store, "remark.add", trackRemark(42, "")).ok, "empty text is refused");

    auto noTime = trackRemark(43, "x");
    noTime.erase("created");
    const auto r = run(*f.store, "remark.add", noTime);
    check(!r.ok && !r.issues.empty(),
          "a remark without `created` fails validation -- the handler has no clock to fall back on");

    check(j.count() == logBefore, "none of the refusals left a log row");
    check(f.db().execAndGet("SELECT COUNT(*) FROM remarks").getInt() == 0, "or a remark");
}

// --- ADR-0131: the target is not a foreign key --------------------------------------------

void testSurvivesTargetDelete() {
    section("deleting the target keeps the remark; undo re-anchors it");
    Fixture f("orphan");
    if (!f.store) return;
    History h(*f.store);
    run(*f.store, "remark.add", trackRemark(50, "keep this take"));
    check(run(*f.store, "track.delete", {{"id", 1}}).ok, "the track is deleted");
    check(row(*f.store, 50) != "<none>", "its remark is still there -- nothing cascades");
    check(h.undo().ok, "undo the delete");
    check(f.db().execAndGet("SELECT COUNT(*) FROM tracks WHERE id = 1").getInt() == 1 &&
              row(*f.store, 50).starts_with("track|1|"),
          "the track is back and the remark points at it again");
}

// --- OPS.md 7: no ambient state -----------------------------------------------------------

void testDeterministic() {
    section("OPS.md 7: the same remark ops, two projects, two actors, one result");
    Fixture a("det_a");
    Fixture b("det_b");
    if (!a.store || !b.store) return;
    for (auto* s : {a.store.get(), b.store.get()}) {
        const Actor who = (s == a.store.get()) ? Actor::User : Actor::Script;
        run(*s, "remark.add", trackRemark(60, "one"), who);
        run(*s, "remark.edit", {{"id", 60}, {"text", "two"}}, who);
        run(*s, "remark.resolve", {{"id", 60}, {"resolved", true}}, who);
        run(*s, "remark.add", trackRemark(61, "three"), who);
    }
    check(row(*a.store, 60) == row(*b.store, 60) && row(*a.store, 61) == row(*b.store, 61),
          "the rows are identical");
    check(row(*a.store, 60).find("|1790000000000000|") != std::string::npos,
          "created_utc is the payload's, not a clock's");
    const auto da = digestProject(*a.store);
    const auto db = digestProject(*b.store);
    check(da.text.find("remarks") != std::string::npos, "the replay digest covers remarks");
    check(da.text == db.text, "and the two digests are equal");
}

int runAll() {
    std::printf("adi_remarks_tests -- ADR-0131, SPEC 6.8, OPS.md 9.12\n\n");
    testRegistered();
    testAdd();
    testRemoveCapturesEverything();
    testEditAndResolve();
    testRefusals();
    testSurvivesTargetDelete();
    testDeterministic();
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
