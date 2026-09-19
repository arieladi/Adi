// SPDX-License-Identifier: GPL-3.0-or-later
//
// Op codec, registry and journal. OPS.md §3–§8, ADR-0003, ADR-0021, ADR-0025.
//
// The two that matter most:
//   * ATOMICITY -- there must be no state where the project changed and the log
//     did not, and none where the log grew and the project did not.
//   * DETERMINISM -- identical payloads encode to identical bytes, or ADR-0007's
//     text projection and ADR-0021's replay test both become impossible.

#include "adi/ops.hpp"
#include "adi/store.hpp"

#include <SQLiteCpp/SQLiteCpp.h>

#include <cmath>
#include <cstdio>
#include <exception>
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
    explicit Scratch(const char* name) {
        dir = fs::temp_directory_path() / ("adi_ops_test_" + std::string(name));
        std::error_code ec;
        fs::remove_all(dir, ec);
        fs::create_directories(dir, ec);
    }
    ~Scratch() {
        std::error_code ec;
        fs::remove_all(dir, ec);
    }
    fs::path operator/(const char* leaf) const { return dir / leaf; }
};

std::unique_ptr<Store> freshProject(const fs::path& p) {
    StoreError err = StoreError::Ok;
    auto st = Store::create(p, err);
    if (st) st->db().exec("INSERT INTO project(id,name) VALUES (1,'Untitled')");
    return st;
}

// --- registry ----------------------------------------------------------------

void testRegistry() {
    section("registry invariants (OPS.md 3)");
    const auto& reg = OpRegistry::instance();   // throws if malformed
    check(!reg.all().empty(), "registry has ops");
    check(reg.find("track.create") != nullptr, "track.create is registered");
    check(reg.find("nope.missing") == nullptr, "an unregistered name is not found");

    // The real registry is well-formed by construction -- reaching this line
    // proves it, since instance() throws otherwise. What is worth testing is
    // that selfCheck can actually FAIL, or it is decoration.
    constexpr Field f[] = {{"id", FieldType::Int, true}};
    const OpDescriptor broken[] = {
        {"Bad.Name", "", Scope::Edit, EngineImpact::None, f, false, false,
         nullptr, nullptr, ""},
        {"dup.op", "", Scope::Edit, EngineImpact::None, f, false, false,
         nullptr, nullptr, "no.such.op"},
        {"dup.op", "", Scope::Edit, EngineImpact::None, f, false, false,
         nullptr, nullptr, ""},
        {"ok.ephemeralMisplaced", "", Scope::Edit, EngineImpact::None, f, false,
         /*ephemeral=*/true, nullptr, nullptr, ""},
    };
    const auto bad = OpRegistry::selfCheck(broken);
    const auto has = [&](const char* frag) {
        for (const auto& b : bad)
            if (b.find(frag) != std::string::npos) return true;
        return false;
    };
    check(!bad.empty(), "selfCheck rejects a malformed registry");
    check(has("domain.verb"), "catches a name that breaks the naming rule");
    check(has("no apply handler"), "catches a write op with no handler");
    check(has("duplicate op name"), "catches a duplicate name");
    check(has("not registered"), "catches an inverse that names a missing op");
    check(has("ephemeral outside"), "catches ephemeral outside transport/session");
}

// --- codec -------------------------------------------------------------------

void testCodecDeterminism() {
    section("ADR-0025 deterministic encoding");

    // Same content, different INSERTION order. If these differ, the text
    // projection cannot be stable and ADR-0021's replay test cannot exist.
    Payload a, b;
    a["z"] = 1; a["aa"] = 2; a["id"] = 5765760;
    b["id"] = 5765760; b["aa"] = 2; b["z"] = 1;
    check(encodePayload(a) == encodePayload(b),
          "insertion order does not change the encoding");

    // Encoding twice gives the same bytes.
    check(encodePayload(a) == encodePayload(a), "encoding is stable across calls");

    // i64 beyond a double's exact range. This is the ADR-0025 argument for CBOR
    // over JSON: our tick positions are i64 at 5765760 PPQ and exceed 2^53.
    const std::int64_t big = 4611686018427387904LL;   // 2^62
    Payload t;
    t["pos"] = big;
    std::string err;
    const auto rt = decodeFromLog(encodePayload(t), err);
    check(rt.has_value(), "large i64 payload decodes");
    check(rt && rt->at("pos").get<std::int64_t>() == big,
          "i64 beyond 2^53 survives exactly -- a double would not");

    // nlohmann narrows a double to float32 when that is lossless, so the byte
    // count depends on the VALUE. That is still deterministic; assert the
    // values survive rather than the widths.
    for (double d : {0.0, 1.0, 0.1, -6.5, 1e-300}) {
        Payload p;
        p["v"] = d;
        const auto back = decodeFromLog(encodePayload(p), err);
        check(back && back->at("v").get<double>() == d,
              "double " + std::to_string(d) + " round-trips bit-exactly");
    }
}

void testUnknownFieldDirections() {
    section("closed on submission, preserving from the log (OPS.md 3 vs 8)");
    const auto* d = OpRegistry::instance().find("track.rename");

    Payload good;
    good["id"] = 1;
    good["name"] = "Keys";
    check(validateSubmission(*d, good).empty(), "a valid payload validates");

    // From a CALLER: an unknown field is a typo or a version mismatch. Silently
    // accepting it means they believe they set something they did not.
    Payload typo = good;
    typo["nmae"] = "Keys";
    const auto issues = validateSubmission(*d, typo);
    check(issues.size() == 1 && issues[0].key == "nmae",
          "an unknown field from a caller is REJECTED");

    Payload missing;
    missing["id"] = 1;
    check(!validateSubmission(*d, missing).empty(), "a missing required field is rejected");

    Payload wrongType;
    wrongType["id"] = "one";
    wrongType["name"] = "Keys";
    check(!validateSubmission(*d, wrongType).empty(), "a wrongly-typed field is rejected");

    Payload nonFinite;
    nonFinite["id"] = 1;
    nonFinite["name"] = "Keys";
    check(validateSubmission(*d, nonFinite).empty(), "control: finite payload passes");

    // From the LOG: an unknown field is a NEWER build's, and dropping it would
    // corrupt a log we are only carrying.
    Payload fromFuture;
    fromFuture["id"] = 1;
    fromFuture["name"] = "Keys";
    fromFuture["swingAmount"] = 0.62;      // a field this build never heard of
    const auto encoded = encodePayload(fromFuture);
    std::string err;
    const auto decoded = decodeFromLog(encoded, err);
    check(decoded.has_value(), "a newer build's op decodes");
    check(decoded && decoded->contains("swingAmount"),
          "an unknown field from the LOG is PRESERVED");
    check(encodePayload(*decoded) == encoded,
          "and re-encoding it is byte-identical -- nothing is stripped in transit");
}

void testNonFiniteRejected() {
    section("non-finite values");
    constexpr Field f[] = {{"v", FieldType::Real, true}};
    const OpDescriptor d{"test.real", "", Scope::Edit, EngineImpact::None, f,
                         false, false, nullptr, nullptr, ""};
    Payload nan;
    nan["v"] = std::nan("");
    check(!validateSubmission(d, nan).empty(),
          "NaN is rejected rather than written into a project file");
    Payload inf;
    inf["v"] = std::numeric_limits<double>::infinity();
    check(!validateSubmission(d, inf).empty(), "infinity is rejected");
    Payload fine;
    fine["v"] = -6.5;
    check(validateSubmission(d, fine).empty(), "a finite real passes");
}

// --- journal -----------------------------------------------------------------

void testApplyAndLog() {
    section("apply + log in one transaction (ADR-0003)");
    Scratch s("apply");
    auto st = freshProject(s / "p.adi");
    check(st != nullptr, "project created");
    if (!st) return;
    OpJournal j(*st);

    OpRequest r;
    r.opType = "track.create";
    r.payload = {{"id", 1}, {"kind", "midi"}, {"name", "Keys"}};
    r.label = "Create track";
    const auto res = j.commit(r);
    check(res.ok, "track.create commits: " + res.error);
    check(res.seqs.size() == 1, "one op row written");

    const auto n = st->db().execAndGet("SELECT COUNT(*) FROM tracks").getInt();
    check(n == 1, "the track exists");
    check(j.count() == 1, "the log has exactly one entry");

    // The inverse was captured BEFORE apply and stored.
    const auto logged = j.recent(1);
    check(logged.size() == 1, "the op reads back");
    check(logged[0].opType == "track.create", "op_type round-trips");
    check(logged[0].payload.at("name") == "Keys", "payload round-trips through CBOR");
    check(logged[0].inverse.has_value(), "an inverse was stored");
    check(logged[0].inverse && logged[0].inverse->at("id") == 1, "the inverse names the track");
}

void testAtomicity() {
    section("atomicity: no half-applied batch");
    Scratch s("atomic");
    auto st = freshProject(s / "p.adi");
    if (!st) return;
    OpJournal j(*st);

    OpRequest ok1;
    ok1.opType = "track.create";
    ok1.payload = {{"id", 1}, {"kind", "midi"}, {"name", "A"}};
    check(j.commit(ok1).ok, "first track created");

    // A batch where the SECOND op fails. Nothing from it may survive -- not the
    // first op's mutation, and not its log row.
    const std::int64_t opsBefore = j.count();
    OpRequest a, b;
    a.opType = "track.create";
    a.payload = {{"id", 2}, {"kind", "audio"}, {"name", "B"}};
    b.opType = "track.rename";
    b.payload = {{"id", 999}, {"name", "ghost"}};   // no such track
    const OpRequest batch[] = {a, b};
    const auto res = j.commit(batch);

    check(!res.ok, "a batch with a failing op does not commit");
    const auto tracks = st->db().execAndGet("SELECT COUNT(*) FROM tracks").getInt();
    check(tracks == 1, "the first op's mutation was rolled back too, saw " +
                           std::to_string(tracks) + " tracks");
    check(j.count() == opsBefore,
          "and no log rows were left behind describing work that did not happen");

    // Validation failure must not even open a transaction.
    OpRequest badPayload;
    badPayload.opType = "track.create";
    badPayload.payload = {{"id", 3}};   // missing required 'kind'
    const auto vres = j.commit(badPayload);
    check(!vres.ok && !vres.issues.empty(), "an invalid payload is rejected with issues");
    check(st->db().execAndGet("SELECT COUNT(*) FROM tracks").getInt() == 1,
          "a rejected payload changed nothing");
    check(j.count() == opsBefore, "and wrote no log row");
}

void testInverseShapes() {
    section("the three inverse shapes (OPS.md 6.2)");
    Scratch s("inverse");
    auto st = freshProject(s / "p.adi");
    if (!st) return;
    OpJournal j(*st);

    OpRequest create;
    create.opType = "track.create";
    create.payload = {{"id", 1}, {"kind", "midi"}, {"name", "Original"}};
    check(j.commit(create).ok, "created");

    // SYMMETRIC: the inverse is the same op with the previous value.
    OpRequest rename;
    rename.opType = "track.rename";
    rename.payload = {{"id", 1}, {"name", "Renamed"}};
    check(j.commit(rename).ok, "renamed");
    auto logged = j.recent(1);
    check(logged[0].inverse && logged[0].inverse->at("name") == "Original",
          "symmetric inverse carries the value being overwritten");

    // Applying the inverse really does undo it.
    OpRequest undo;
    undo.opType = "track.rename";
    undo.payload = *logged[0].inverse;
    check(j.commit(undo).ok, "the inverse applies");
    const auto name =
        st->db().execAndGet("SELECT name FROM tracks WHERE id=1").getString();
    check(name == "Original", "applying the inverse restored the old name, got " + name);

    // STATE CAPTURE: delete captures enough to rebuild.
    OpRequest del;
    del.opType = "track.delete";
    del.payload = {{"id", 1}};
    check(j.commit(del).ok, "deleted");
    logged = j.recent(1);
    check(st->db().execAndGet("SELECT COUNT(*) FROM tracks").getInt() == 0, "track is gone");
    check(logged[0].inverse && logged[0].inverse->at("kind") == "midi",
          "the capture kept the track's kind");
    check(logged[0].inverse && logged[0].inverse->at("name") == "Original",
          "and its name");

    // And the capture is sufficient: replay it through the paired op.
    OpRequest restore;
    restore.opType = "track.create";
    restore.payload = *logged[0].inverse;
    const auto rr = j.commit(restore);
    check(rr.ok, "the captured state replays through track.create: " + rr.error);
    check(st->db().execAndGet("SELECT name FROM tracks WHERE id=1").getString() == "Original",
          "the track came back intact");
}

void testCallerAllocatedIds() {
    section("ADR-0021 7.3 caller-allocated ids");
    Scratch s("ids");
    auto st = freshProject(s / "p.adi");
    if (!st) return;
    OpJournal j(*st);

    OpRequest c;
    c.opType = "track.create";
    c.payload = {{"id", 77}, {"kind", "midi"}, {"name", "Seventy-seven"}};
    check(j.commit(c).ok, "created with a caller-chosen id");
    check(st->db().execAndGet("SELECT id FROM tracks").getInt64() == 77,
          "the id is the one the caller chose, not one SQLite picked");

    // Delete then recreate from the capture: the id must be identical, or every
    // later op referencing it points at nothing.
    OpRequest d;
    d.opType = "track.delete";
    d.payload = {{"id", 77}};
    check(j.commit(d).ok, "deleted");
    const auto cap = *j.recent(1)[0].inverse;
    OpRequest again;
    again.opType = "track.create";
    again.payload = cap;
    check(j.commit(again).ok, "recreated");
    check(st->db().execAndGet("SELECT id FROM tracks").getInt64() == 77,
          "redo restores the SAME id -- this is what makes the log replayable");

    // A colliding id is rejected rather than silently reassigned.
    OpRequest clash;
    clash.opType = "track.create";
    clash.payload = {{"id", 77}, {"kind", "audio"}, {"name", "Clash"}};
    check(!j.commit(clash).ok, "a duplicate id is rejected, not reassigned");
}

void testAgentAttribution() {
    section("SPEC 8.1 attribution");
    Scratch s("actor");
    auto st = freshProject(s / "p.adi");
    if (!st) return;
    OpJournal j(*st);

    OpRequest u;
    u.opType = "track.create";
    u.payload = {{"id", 1}, {"kind", "midi"}, {"name", "Mine"}};
    j.commit(u);

    OpRequest a;
    a.opType = "track.rename";
    a.payload = {{"id", 1}, {"name", "Agent's"}};
    a.actor = Actor::Agent;
    a.actorDetail = "adi-agent/claude-opus-5";
    check(j.commit(a).ok, "agent op commits");

    const auto n = st->db()
        .execAndGet("SELECT COUNT(*) FROM ops WHERE actor='agent'").getInt();
    check(n == 1, "agent ops are queryable by actor");
    const auto detail = st->db()
        .execAndGet("SELECT actor_detail FROM ops WHERE actor='agent'").getString();
    check(detail == "adi-agent/claude-opus-5", "the model is recorded, not just 'agent'");
}

void testBatchSharesTxn() {
    section("a batch is one txn_id (OPS.md 6.4)");
    Scratch s("txn");
    auto st = freshProject(s / "p.adi");
    if (!st) return;
    OpJournal j(*st);

    OpRequest a, b, c;
    a.opType = "track.create"; a.payload = {{"id",1},{"kind","midi"},{"name","A"}};
    b.opType = "track.create"; b.payload = {{"id",2},{"kind","midi"},{"name","B"}};
    c.opType = "track.setMute"; c.payload = {{"id",1},{"muted",true}};
    const OpRequest batch[] = {a, b, c};
    const auto res = j.commit(batch);
    check(res.ok, "batch commits: " + res.error);
    check(res.seqs.size() == 3, "three rows");

    const auto distinct = st->db()
        .execAndGet("SELECT COUNT(DISTINCT txn_id) FROM ops").getInt();
    check(distinct == 1, "all three share one txn_id, so undo reverts them as one");
}

void testPersistsAcrossReopen() {
    section("the log survives a close (ADR-0003)");
    Scratch s("persist");
    const auto p = s / "p.adi";
    {
        auto st = freshProject(p);
        if (!st) return;
        OpJournal j(*st);
        OpRequest r;
        r.opType = "track.create";
        r.payload = {{"id", 1}, {"kind", "midi"}, {"name", "Persistent"}};
        check(j.commit(r).ok, "committed");
        check(st->close(), "closed cleanly");
    }
    StoreError err = StoreError::Ok;
    auto re = Store::open(p, err);
    check(re != nullptr, "reopened");
    if (!re) return;
    OpJournal j2(*re);
    check(j2.count() == 1, "the op log is still there after a restart");
    const auto logged = j2.recent(1);
    check(logged.size() == 1 && logged[0].payload.at("name") == "Persistent",
          "and its payload still decodes -- this is what in-memory undo cannot do");
}

}  // namespace

int runAll() {
    std::printf("adi_ops_tests -- OPS.md 3-8, ADR-0003/0021/0025\n\n");
    try {
        testRegistry();
    } catch (const std::exception& e) {
        std::printf("  FAIL  registry threw at startup: %s\n", e.what());
        ++g_failures;
    }
    testCodecDeterminism();
    testUnknownFieldDirections();
    testNonFiniteRejected();
    testApplyAndLog();
    testAtomicity();
    testInverseShapes();
    testCallerAllocatedIds();
    testAgentAttribution();
    testBatchSharesTxn();
    testPersistsAcrossReopen();
    std::printf("\n%s -- %d checks, %d failure(s)\n", g_failures ? "FAILED" : "PASS", g_checks,
                g_failures);
    return g_failures ? 1 : 0;
}

int main() {
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
