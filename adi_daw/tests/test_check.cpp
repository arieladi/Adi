// SPDX-License-Identifier: GPL-3.0-or-later
//
// adi_tool check. ADR-0029, ADR-0033.
//
// Every check here is planted with the corruption it exists to find, because a
// check nobody has watched reject something is a comment. The corruptions are
// written with raw SQL on purpose: they are states the op layer cannot produce,
// which is precisely why a checker is needed — they arrive from a crash, a bad
// merge, a third-party writer, or a future version of us with a bug.

#include "temp_directory.hpp"

#include "adi/blob.hpp"
#include "adi/check.hpp"
#include "adi/ops.hpp"
#include "adi/store.hpp"

#include <SQLiteCpp/SQLiteCpp.h>

#include <algorithm>
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

struct Fixture {
    adi::test::TempDirectory temp; // destroyed after store/file members
    fs::path dir;
    std::unique_ptr<Store> store;
    explicit Fixture(const char* n)
        : temp("check", n), dir(temp.path()) {
        StoreError e = StoreError::Ok;
        store = Store::create(dir / "p.adi", e);
        if (!store) return;
        // A small but real project: a track, a clip, and some notes.
        OpJournal j(*store);
        OpRequest r;
        r.opType = "track.create";
        r.payload = {{"id", 1}, {"kind", "midi"}, {"name", "Keys"}};
        j.commit(r);
        r.opType = "clip.create";
        r.payload = {{"id", 5}, {"track", 1}, {"kind", "midi"}, {"pos", 0},
                     {"length", 23063040}};
        j.commit(r);
        r.opType = "note.insert";
        r.payload = {{"clip", 5}, {"note", 1}, {"start", 0}, {"dur", 1441440},
                     {"key", 60}};
        j.commit(r);
    }
    SQLite::Database& db() { return store->db(); }
};

/// Does the report contain a finding with this code?
bool has(const CheckReport& r, const char* code) {
    return std::any_of(r.findings.begin(), r.findings.end(),
                       [&](const Finding& f) { return f.code == code; });
}

std::string describe(const CheckReport& r) {
    std::string s;
    for (const auto& f : r.findings) s += f.code + "(" + f.where + ") ";
    return s.empty() ? "<none>" : s;
}

// --- baseline --------------------------------------------------------------------

void testCleanProject() {
    section("a healthy project is clean");
    Fixture f("clean");
    if (!f.store) return;
    const auto r = checkProject(*f.store);
    check(r.checksRun >= 15, "a useful number of checks ran, saw " +
                                 std::to_string(r.checksRun));
    check(r.errors == 0, "no errors: " + describe(r));
    check(r.warnings == 0, "no warnings: " + describe(r));
    check(r.clean(), "clean() agrees");
}

// --- the polymorphic references, which SQLite cannot enforce -------------------

void testDanglingPolyRef() {
    section("ref.dangling -- the gap ADR-0029 named");
    Fixture f("dangling");
    if (!f.store) return;

    // A routing row into track 1, then track 1 is removed behind SQLite's back.
    // A FOREIGN KEY cannot express (kind, id), so nothing stops this.
    f.db().exec("INSERT INTO routing(id, src_kind, src_id, dst_kind, dst_id, kind) "
                "VALUES (1, 'track', 1, 'track', 999, 'main')");
    const auto r = checkProject(*f.store);
    check(has(r, "ref.dangling"), "a routing endpoint naming a missing track is caught");
    check(r.errors >= 1, "and it is an error, not a warning");

    // The message has to say which row and which target, or it is useless.
    const auto it = std::find_if(r.findings.begin(), r.findings.end(),
                                 [](const Finding& x) { return x.code == "ref.dangling"; });
    check(it != r.findings.end() && it->where.find("routing#1") != std::string::npos,
          "the finding names the offending row: " +
              (it != r.findings.end() ? it->where : std::string("<none>")));
    check(it != r.findings.end() && it->detail.find("999") != std::string::npos,
          "and the missing target");
}

void testExternalKindsAreNotErrors() {
    section("hw_in / hw_out are not rows, and not errors");
    Fixture f("external");
    if (!f.store) return;
    // SPEC §6.7: a hardware port index is resolved against the current audio
    // device. Failing to resolve is NORMAL -- the project opened in another
    // studio -- so the checker must not call it corruption.
    f.db().exec("INSERT INTO routing(id, src_kind, src_id, dst_kind, dst_id, kind) "
                "VALUES (1, 'hw_in', 7, 'track', 1, 'main')");
    f.db().exec("INSERT INTO routing(id, src_kind, src_id, dst_kind, dst_id, kind) "
                "VALUES (2, 'track', 1, 'hw_out', 99, 'main')");
    const auto r = checkProject(*f.store);
    check(!has(r, "ref.dangling"),
          "hardware endpoints are not reported as dangling: " + describe(r));
    check(r.errors == 0, "and the project is clean");
}

void testUnknownKindIsAWarning() {
    section("an unknown reference kind warns rather than errors");
    Fixture f("unknown");
    if (!f.store) return;
    // ui_view.scope_kind has no CHECK, so a kind from a future version can
    // legitimately appear. ADR-0012 says unknown data is preserved, not
    // rejected -- so this is a warning: we cannot verify it, not it is wrong.
    f.db().exec("INSERT INTO ui_view(scope_kind, scope_id, key, value) "
                "VALUES ('chordTrack', 3, 'height', '120')");
    const auto r = checkProject(*f.store);
    check(has(r, "ref.unknownKind"), "the unknown kind is reported: " + describe(r));
    check(r.errors == 0, "as a WARNING, not an error -- we cannot verify it, "
                         "which is not the same as it being wrong");
    check(r.warnings >= 1, "and the warning count reflects it");
}

// --- inside the blobs -------------------------------------------------------------

void testMalformedBlob() {
    section("blob.malformed -- opaque to SQL, checkable only here");
    Fixture f("blob");
    if (!f.store) return;
    check(checkProject(*f.store).clean(), "clean to begin with");

    // Truncate the notes blob mid-record. SQLite is perfectly happy; a reader
    // is not.
    f.db().exec("UPDATE event_streams SET data = X'414E4F5401002800' "
                "WHERE stream_kind = 'notes'");
    const auto r = checkProject(*f.store);
    check(has(r, "blob.malformed"), "a truncated stream is caught: " + describe(r));
    check(r.errors >= 1, "as an error");
}

void testRecSizeNotReleased() {
    section("a rec_size no writer ever released (ADR-0023)");
    Fixture f("recsize");
    if (!f.store) return;
    // rec_size 29 lands mid-field. ADR-0023 made StreamReader reject it; this
    // confirms the checker surfaces it rather than the corruption sitting there
    // until something tries to play the clip.
    f.db().exec("UPDATE event_streams SET data = X'414E4F5401001D000100000001000000' "
                "WHERE stream_kind = 'notes'");
    const auto r = checkProject(*f.store);
    check(has(r, "blob.malformed"),
          "a rec_size matching no released version is caught: " + describe(r));
}

void testOrphanExpression() {
    section("expression.orphan -- a reference from one blob into another");
    Fixture f("orphan");
    if (!f.store) return;

    // A pressure curve for note 42, which does not exist in clip 5. Nothing in
    // SQL can see this: both sides are blobs.
    f.db().exec("INSERT INTO note_expression(clip_id, note_id, dimension, data) "
                "VALUES (5, 42, 1, X'41455850010018000000000001000000')");
    const auto r = checkProject(*f.store);
    check(has(r, "expression.orphan"),
          "an expression curve for a nonexistent note is caught: " + describe(r));

    // And the one that DOES exist is not reported.
    f.db().exec("INSERT INTO note_expression(clip_id, note_id, dimension, data) "
                "VALUES (5, 1, 2, X'41455850010018000000000001000000')");
    const auto r2 = checkProject(*f.store);
    const auto orphans = std::count_if(
        r2.findings.begin(), r2.findings.end(),
        [](const Finding& x) { return x.code == "expression.orphan"; });
    check(orphans == 1, "only the orphan is reported, not the valid one, saw " +
                            std::to_string(orphans));
}

// --- the history tree ---------------------------------------------------------------

void testHistoryChecks() {
    section("the undo tree");
    Fixture f("history");
    if (!f.store) return;

    // No current branch. Two current is prevented by the partial unique index
    // from ADR-0026, so this is the reachable half of that invariant.
    f.db().exec("UPDATE op_branches SET is_current = 0");
    check(has(checkProject(*f.store), "history.currentBranch"),
          "zero current branches is caught");
    f.db().exec("UPDATE op_branches SET is_current = 1 WHERE id = 1");
    check(checkProject(*f.store).clean(), "and clean once restored");

    // A head pointing at an op that is not there. SQLite ENFORCES this one --
    // head_seq is a real FOREIGN KEY -- so planting it needs foreign_keys off.
    // The check still earns its place: a file written by a third party, or by
    // us with the pragma off, can arrive in this state, and the checker is the
    // only thing that would notice.
    f.db().exec("PRAGMA foreign_keys = OFF");
    f.db().exec("UPDATE op_branches SET head_seq = 9999 WHERE id = 1");
    check(has(checkProject(*f.store), "history.danglingHead"),
          "a head_seq naming no op is caught");
    f.db().exec("UPDATE op_branches SET head_seq = (SELECT MAX(seq) FROM ops) WHERE id = 1");
    f.db().exec("PRAGMA foreign_keys = ON");

    // An ephemeral op inside the tree. ADR-0030 puts them outside it, and one
    // with a parent would make undo walk into a transport action.
    f.db().exec("INSERT INTO ops(txn_id, parent_seq, ts_utc, actor, op_type, tags) "
                "VALUES (99, 1, 0, 'user', 'transport.play', 'ephemeral')");
    check(has(checkProject(*f.store), "history.ephemeralInTree"),
          "an ephemeral op with a parent is caught");
    f.db().exec("DELETE FROM ops WHERE txn_id = 99");

    // A cycle. Undo would not terminate. Both ends are real ops, so foreign
    // keys are satisfied and SQLite has no opinion -- a cycle is exactly the
    // shape of corruption a relational constraint cannot describe.
    f.db().exec("UPDATE ops SET parent_seq = (SELECT MAX(seq) FROM ops) "
                "WHERE seq = (SELECT MIN(seq) FROM ops)");
    const auto r = checkProject(*f.store);
    check(has(r, "history.cycle"),
          "a parent_seq loop is caught before undo hangs on it: " + describe(r));
}

// --- time and media -------------------------------------------------------------------

void testTimeBaseMismatch() {
    section("time.baseMismatch -- the half of SPEC 4.1 clips do not CHECK");
    Fixture f("time");
    if (!f.store) return;
    // SPEC 4.1 has four ways to get this wrong, and `clips` now CHECKs three of
    // them: `time_base = 1 OR pos_ns IS NULL` stops a musical clip carrying a
    // nanosecond position, and ADR-0037's placement CHECK stops a clip with no
    // position in the domain it declares. SQLite gets the credit for those.
    for (const char* sql : {"UPDATE clips SET pos_ns = 123456 WHERE id = 5",
                            "UPDATE clips SET pos_ticks = NULL WHERE id = 5"}) {
        bool rejected = false;
        try {
            f.db().exec(sql);
        } catch (const std::exception&) {
            rejected = true;
        }
        check(rejected, std::string("SQLite itself rejects: ") + sql);
    }

    // The fourth it does NOT cover: a LINEAR clip that also carries a tick
    // position. Equally invalid under SPEC 4.1, equally silent, and only this
    // checker sees it.
    //
    // Worth keeping even though three quarters of the rule is now schema-level.
    // A CHECK constraint protects files THIS writer creates; `adi_tool check`
    // runs against files a third-party implementation wrote, and nothing
    // obliges that implementation to have used our DDL. For an interchange
    // format a runtime checker is not redundant with a constraint.
    f.db().exec("UPDATE clips SET time_base = 1, pos_ns = 1000 WHERE id = 5");
    check(has(checkProject(*f.store), "time.baseMismatch"),
          "a linear clip that also carries pos_ticks is caught");
}

void testNoInitialTempo() {
    section("time.noInitialTempo -- legal SQL, meaningless music");
    Fixture f("tempo");
    if (!f.store) return;
    f.db().exec("INSERT INTO tempo_map(pos_ticks, bpm) VALUES (46126080, 140.0)");
    const auto r = checkProject(*f.store);
    check(has(r, "time.noInitialTempo"),
          "tempo events with none at tick 0 is reported: " + describe(r));
    check(r.errors == 0, "as a warning -- it is recoverable, not corrupt");

    f.db().exec("INSERT INTO tempo_map(pos_ticks, bpm) VALUES (0, 120.0)");
    check(!has(checkProject(*f.store), "time.noInitialTempo"),
          "and clears once a tempo at 0 exists");
}

void testMediaEmbedding() {
    section("ADR-0136 -- media is never embedded: refused in 1.1, reported in 1.0");
    Fixture f("media");
    if (!f.store) return;
    f.db().exec("INSERT INTO media_files(id, hash_blake3) VALUES (1, 'abc123')");
    auto refused = [&](const char* sql) {
        try { f.db().exec(sql); return false; } catch (const std::exception&) { return true; }
    };
    check(refused("UPDATE media_files SET embedded = 1 WHERE id = 1"),
          "a 1.1 file refuses marking media embedded");
    check(refused("INSERT INTO media_files(id, hash_blake3, embedded) VALUES (2, 'def', 1)"),
          "and refuses inserting it embedded");
    check(refused("INSERT INTO media_blobs(media_id, chunk_index, data) VALUES (1, 0, X'00')"),
          "and refuses any blob chunk");
    check(!has(checkProject(*f.store), "media.embedded"), "a clean file reports nothing");

    // A 1.0-shaped file: the lock is not there, so the check is the guard.
    f.db().exec("DROP TRIGGER media_never_embedded_insert");
    f.db().exec("DROP TRIGGER media_never_embedded_update");
    f.db().exec("DROP TRIGGER media_blobs_forbidden");
    f.db().exec("UPDATE media_files SET embedded = 1 WHERE id = 1");
    check(has(checkProject(*f.store), "media.embedded"),
          "a 1.0 file marked embedded is an error");
    f.db().exec("UPDATE media_files SET embedded = 0 WHERE id = 1");
    f.db().exec("INSERT INTO media_blobs(media_id, chunk_index, data) VALUES (1, 0, X'00')");
    check(has(checkProject(*f.store), "media.embedded"),
          "and so are blob chunks without the flag");
}

}  // namespace

int main() {
    // Unbuffered, so the last line before a crash survives. On Windows a
    // crashing test binary loses its whole block-buffered stdout, and the
    // harness then prints a blank line where a failure should be -- which is
    // how adi_device_tests' 383 KB overrun looked like a harness glitch for
    // two runs before anyone ran the binary directly.
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::printf("adi_check_tests -- ADR-0029/0033, what SQLite cannot enforce\n\n");
    try {
        testCleanProject();
        testDanglingPolyRef();
        testExternalKindsAreNotErrors();
        testUnknownKindIsAWarning();
        testMalformedBlob();
        testRecSizeNotReleased();
        testOrphanExpression();
        testHistoryChecks();
        testTimeBaseMismatch();
        testNoInitialTempo();
        testMediaEmbedding();
    } catch (const std::exception& e) {
        std::printf("\nFAILED -- exception escaped: %s\n", e.what());
        return 1;
    }
    std::printf("\n%s -- %d checks, %d failure(s)\n", g_failures ? "FAILED" : "PASS", g_checks,
                g_failures);
    return g_failures ? 1 : 0;
}
