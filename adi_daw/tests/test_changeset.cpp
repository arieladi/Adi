// SPDX-License-Identifier: GPL-3.0-or-later
//
// The Propose-tier changeset: ADR-0145 d9, AI-AGENT §6, ADR-0148.
//
// What must hold, and what each section proves:
//   * a preview runs the ops and leaves the file exactly as it was;
//   * the diff shows what Apply will do, in text and as before/after values;
//   * Apply is one transaction, one undo step, actor agent, and its
//     agent_requests row lives or dies with the ops;
//   * a changeset built on a head that moved is refused as stale;
//   * the guardrails refuse what the agent may not emit, and the op cap holds.

#include "temp_directory.hpp"

#include "adi/changeset.hpp"
#include "adi/digest.hpp"
#include "adi/history.hpp"
#include "adi/ops.hpp"
#include "adi/store.hpp"
#include "adi/textproj_store.hpp"

#include <SQLiteCpp/SQLiteCpp.h>
#include <sqlite3.h>

#include <chrono>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>

namespace {

namespace fs = std::filesystem;
using namespace adi;
using namespace adi::agent;

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

constexpr std::int64_t kT0 = 1790253296000000;

struct Fixture {
    adi::test::TempDirectory temp;   // destroyed after the store
    fs::path path;
    std::unique_ptr<Store> store;
    explicit Fixture(const char* n) : temp("changeset", n), path(temp.path() / "p.adi") {
        StoreError e = StoreError::Ok;
        store = Store::create(path, e);
        if (!store) return;
        store->db().exec("INSERT INTO project(id,name) VALUES (1,'Song')");
        OpJournal j(*store);
        for (const auto& [id, name] : {std::pair<int, const char*>{1, "Keys"}, {2, "Bass"}}) {
            OpRequest r;
            r.opType = "track.create";
            r.payload = {{"id", id}, {"kind", "audio"}, {"name", name}, {"index", id - 1}};
            r.label = std::string("Create ") + name;
            j.commit(r);
        }
        // A device with a parameter, written directly: no op creates a plugin
        // instance with parameter rows yet, and the review list must show one.
        store->db().exec("INSERT INTO device_chains(id, track_id) VALUES (1, 2)");
        store->db().exec("INSERT INTO devices(id, chain_id, ord, name) VALUES (7, 1, 0, 'EQ')");
        store->db().exec("INSERT INTO plugin_params(device_id, param_id, normalized_value) "
                         "VALUES (7, 'gain', 0.5)");
    }
    SQLite::Database& db() { return store->db(); }
};

OpRequest op(const char* type, Payload p, const char* label = "") {
    OpRequest r;
    r.opType = type;
    r.payload = std::move(p);
    r.label = label;
    return r;
}

Changeset threeOps(Store& s) {
    Changeset cs = begin(s, "make the bass sit lower and cut the EQ", "model-x 1.0", kT0);
    cs.ops.push_back(op("mixer.setVolume", {{"id", 2}, {"db", -6.0}}, "Bass -6 dB"));
    cs.ops.push_back(op("track.rename", {{"id", 2}, {"name", "Bass (quiet)"}}, "Rename Bass"));
    cs.ops.push_back(op("device.setParam", {{"dev", 7}, {"param", "gain"}, {"norm", 0.25}},
                        "EQ gain 0.25"));
    return cs;
}

std::int64_t count(Store& s, const char* sql) { return s.db().execAndGet(sql).getInt64(); }

std::string fileBytes(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

// --- the guardrails ---------------------------------------------------------------------

void testGuardrails() {
    section("the guardrails (AI-AGENT 6): what the agent may emit, and the cap");
    check(agentMayEmit("mixer.setVolume") && agentMayEmit("device.setParam") &&
              agentMayEmit("remark.add"),
          "project edits are allowed");
    check(!agentMayEmit("transport.play") && !agentMayEmit("transport.seek"),
          "transport is performance, not an edit: refused");
    check(agentMayEmit("media.unlink") && agentMayEmit("media.relink"),
          "changing a media REFERENCE is allowed");
    check(!agentMayEmit("media.import"), "asserting what is on disk is not");
    check(!agentMayEmit("media.delete") && !agentMayEmit("no.suchOp"),
          "an op nobody registered is refused");

    Fixture f("guard");
    if (!f.store) return;
    Changeset empty = begin(*f.store, "nothing", "m", kT0);
    check(preview(*f.store, empty).refusal == Refusal::Empty, "an empty changeset is refused");

    Changeset big = threeOps(*f.store);
    Limits two;
    two.maxOps = 2;
    const auto pr = preview(*f.store, big, two);
    check(pr.refusal == Refusal::TooManyOps, "three ops over a cap of two: preview refuses: " + pr.error);
    const auto ar = apply(*f.store, big, two);
    check(ar.refusal == Refusal::TooManyOps && count(*f.store, "SELECT COUNT(*) FROM ops") == 2,
          "and so does apply, writing nothing");

    Changeset transport = begin(*f.store, "play it", "m", kT0);
    transport.ops.push_back(op("transport.play", Payload::object()));
    check(apply(*f.store, transport).refusal == Refusal::OpNotAllowed,
          "a transport op in a changeset is refused at apply");

    Changeset bad = begin(*f.store, "typo", "m", kT0);
    bad.ops.push_back(op("mixer.setVolume", {{"id", 2}, {"decibels", -6.0}}));
    check(preview(*f.store, bad).refusal == Refusal::Invalid,
          "a payload the schema would reject is refused before anything runs");
}

// --- preview ---------------------------------------------------------------------------

void testPreviewDoesNotWrite() {
    section("preview: the ops run, the diff is produced, the file is not written");
    Fixture f("preview");
    if (!f.store) return;
    Store& s = *f.store;
    const auto digestBefore = digestProject(s).text;
    const auto opsBefore = count(s, "SELECT COUNT(*) FROM ops");
    const auto headBefore = OpJournal(s).headSeq();
    s.db().exec("PRAGMA wal_checkpoint(TRUNCATE)");
    const auto fileBefore = fileBytes(f.path);

    const auto p = preview(s, threeOps(s));
    check(p.ok, "the preview runs: " + p.error);
    check(digestProject(s).text == digestBefore, "the project is exactly as it was");
    check(count(s, "SELECT COUNT(*) FROM ops") == opsBefore && OpJournal(s).headSeq() == headBefore,
          "no op was logged and the head did not move");
    check(count(s, "SELECT normalized_value * 100 FROM plugin_params WHERE device_id = 7") == 50,
          "the parameter is still 0.5");
    s.db().exec("PRAGMA wal_checkpoint(TRUNCATE)");
    check(fileBytes(f.path) == fileBefore, "the .adi's bytes are identical after a checkpoint");
    {
        // Another reader of the same file sees nothing either.
        SQLite::Database other(f.path.string(), SQLite::OPEN_READONLY);
        check(other.execAndGet("SELECT name FROM tracks WHERE id = 2").getString() == "Bass",
              "a second connection still reads 'Bass'");
    }

    // The text diff: the projection before and after.
    check(p.unifiedDiff.rfind("--- before\n+++ after\n@@ ", 0) == 0,
          "a unified diff with the usual headers:\n" + p.unifiedDiff);
    check(p.unifiedDiff.find("\n-trk Bass\n") != std::string::npos &&
              p.unifiedDiff.find("\n+trk \"Bass (quiet)\"\n") != std::string::npos &&
              p.unifiedDiff.find("\n+  vol -6.0\n") != std::string::npos,
          "the rename and the fader show in it:\n" + p.unifiedDiff);
    check(p.textBefore == textproj::projectStore(s).text && p.textAfter != p.textBefore,
          "`before` is the file as it is now, and `after` differs from it");

    // The structured list: before and after values, the parameter's especially.
    check(p.changes.size() == 3, "one entry per queued op");
    if (p.changes.size() == 3) {
        const auto& vol = p.changes[0];
        check(vol.label == "Bass -6 dB" && vol.targetKind == "mixer" && vol.targetId == 2,
              "the fader: label and target");
        check(vol.before && vol.before->at("db").get<double>() == 0.0 &&
                  vol.after && vol.after->at("db").get<double>() == -6.0,
              "the fader: 0 dB before, -6 dB after");
        const auto& gain = p.changes[2];
        check(gain.targetKind == "device" && gain.targetId == 7, "the parameter's target is device 7");
        check(gain.before && gain.before->at("norm").get<double>() == 0.5 &&
                  gain.after && gain.after->at("norm").get<double>() == 0.25,
              "the parameter: 0.5 before, 0.25 after -- the value the text projection "
              "cannot show until devices are projected");
    }

    // A creation has no before; a deletion no after.
    Changeset cd = begin(s, "add and remove", "m", kT0);
    cd.ops.push_back(op("track.create", {{"id", 3}, {"kind", "audio"}, {"name", "Pad"}}));
    cd.ops.push_back(op("track.delete", {{"id", 1}}));
    const auto p2 = preview(s, cd);
    check(p2.ok && p2.changes.size() == 2 && !p2.changes[0].before && p2.changes[0].after &&
              p2.changes[1].before && !p2.changes[1].after &&
              p2.changes[1].before->at("name") == "Keys",
          "a creation shows only after, a deletion only what it removes");
    check(count(s, "SELECT COUNT(*) FROM tracks") == 2, "and neither happened");
}

// --- apply ------------------------------------------------------------------------------

void testApplyIsOneStep() {
    section("apply: one transaction, actor agent, the request beside it, one undo");
    Fixture f("apply");
    if (!f.store) return;
    Store& s = *f.store;
    const auto cs = threeOps(s);
    const auto p = preview(s, cs);
    const auto opsBefore = count(s, "SELECT COUNT(*) FROM ops");

    const auto r = apply(s, cs);
    check(r.ok, "applied: " + r.error);
    check(textproj::projectStore(s).text == p.textAfter, "the result is what the preview showed");
    check(count(s, "SELECT COUNT(*) FROM ops") == opsBefore + 3, "three op rows");
    check(count(s, ("SELECT COUNT(DISTINCT txn_id) FROM ops WHERE seq > " +
                    std::to_string(opsBefore)).c_str()) == 1,
          "all three share ONE txn_id (AI-AGENT 6.2)");
    check(count(s, "SELECT COUNT(*) FROM ops WHERE actor = 'agent' AND actor_detail = 'model-x 1.0'") == 3,
          "every row says agent, and which model");
    SQLite::Statement req(s.db(),
        "SELECT request, actor_detail, created_utc FROM agent_requests WHERE txn_id = ?");
    req.bind(1, r.txnId);
    check(req.executeStep() && req.getColumn(0).getString() == "make the bass sit lower and cut the EQ" &&
              req.getColumn(1).getString() == "model-x 1.0" && req.getColumn(2).getInt64() == kT0,
          "the agent_requests row: the request, the model, the time passed in (6.8)");

    History h(s);
    check(h.undo().ok, "one Ctrl-Z");
    check(count(s, "SELECT COUNT(*) FROM tracks WHERE name = 'Bass'") == 1 &&
              count(s, "SELECT volume_db FROM mixer_strip WHERE track_id = 2") == 0 &&
              count(s, "SELECT normalized_value * 100 FROM plugin_params WHERE device_id = 7") == 50,
          "undoes all of it");
    check(!h.canUndo() || h.nextUndo()->actor != "agent", "and the step before it is not the agent's");
}

void testStale() {
    section("apply refuses a changeset whose head moved");
    Fixture f("stale");
    if (!f.store) return;
    Store& s = *f.store;
    const auto cs = threeOps(s);
    check(preview(s, cs).ok, "previewed on the current head");

    // The user keeps working while the proposal waits.
    OpJournal j(s);
    j.commit(op("track.setMute", {{"id", 1}, {"muted", true}}, "Mute Keys"));
    const auto opsNow = count(s, "SELECT COUNT(*) FROM ops");

    const auto r = apply(s, cs);
    check(!r.ok && r.refusal == Refusal::Stale, "refused as stale: " + r.error);
    check(r.error.find("stale") != std::string::npos, "and the message says so");
    check(count(s, "SELECT COUNT(*) FROM ops") == opsNow &&
              count(s, "SELECT COUNT(*) FROM agent_requests") == 0,
          "nothing was written");
    check(preview(s, cs).refusal == Refusal::Stale, "preview refuses it too");
    check(apply(s, threeOps(s)).ok, "rebuilt on the current head, it applies");
}

void testRequestRowIsInTheTransaction() {
    section("the request row lives or dies with the ops (SPEC 8.7)");
    {
        Fixture f("fail_op");
        if (!f.store) return;
        Store& s = *f.store;
        auto cs = threeOps(s);
        cs.ops.push_back(op("clip.delete", {{"id", 999}}, "a clip that is not there"));
        const auto r = apply(s, cs);
        check(!r.ok, "a failing fourth op fails the apply: " + r.error);
        check(count(s, "SELECT COUNT(*) FROM agent_requests") == 0,
              "and leaves no request row behind");
        check(count(s, "SELECT COUNT(*) FROM tracks WHERE name = 'Bass'") == 1 &&
                  count(s, "SELECT COUNT(*) FROM ops WHERE actor = 'agent'") == 0,
              "or any of the first three ops");
    }
    {
        Fixture f("fail_row");
        if (!f.store) return;
        Store& s = *f.store;
        // The txn id the apply will get is already taken in agent_requests --
        // a state only a bad writer produces, and exactly the failure that
        // must take the ops down with it.
        const auto next = count(s, "SELECT IFNULL(MAX(txn_id), 0) + 1 FROM ops");
        s.db().exec("INSERT INTO agent_requests(txn_id, request, created_utc) VALUES (" +
                    std::to_string(next) + ", 'someone else', 0)");
        const auto r = apply(s, threeOps(s));
        check(!r.ok, "a request row that cannot be written fails the apply");
        check(count(s, "SELECT COUNT(*) FROM ops WHERE actor = 'agent'") == 0 &&
                  count(s, "SELECT COUNT(*) FROM tracks WHERE name = 'Bass'") == 1,
              "and no op was committed without its request");
        check(count(s, "SELECT COUNT(*) FROM sqlite_temp_master WHERE name = 'adi_agent_request'") == 0,
              "the connection's temporary trigger is gone afterwards");
    }
}

void testMediaUnlinkLeavesTheFile() {
    section("unlinking media changes a reference and never the file (6.4)");
    Fixture f("media");
    if (!f.store) return;
    Store& s = *f.store;
    const auto audio = f.temp.path() / "kick.wav";
    { std::ofstream(audio, std::ios::binary) << "RIFF"; }
    s.db().exec("INSERT INTO media_files(id, hash_blake3, orig_name, rel_path) "
                "VALUES (4, '" + std::string(64, 'a') + "', 'kick.wav', 'kick.wav')");
    Changeset cs = begin(s, "remove the unused kick", "m", kT0);
    cs.ops.push_back(op("media.unlink", {{"id", 4}}));
    const auto r = apply(s, cs);
    check(r.ok, "media.unlink applies: " + r.error);
    check(count(s, "SELECT COUNT(*) FROM media_files") == 0, "the reference is gone");
    check(fs::exists(audio) && fs::file_size(audio) == 4, "the file on disk is untouched");
}

// --- the diff itself --------------------------------------------------------------------------

void testUnifiedDiff() {
    section("unifiedDiff: Myers, three lines of context");
    check(unifiedDiff("a\nb\n", "a\nb\n").empty(), "equal texts: no diff");
    const std::string a = "1\n2\n3\n4\n5\n6\n7\n8\n9\n10\n";
    const std::string b = "1\n2\n3\n4\nfive\n6\n7\n8\n9\n10\n11\n";
    const std::string expect =
        "--- before\n+++ after\n"
        "@@ -2,9 +2,10 @@\n"
        " 2\n 3\n 4\n-5\n+five\n 6\n 7\n 8\n 9\n 10\n+11\n";
    check(unifiedDiff(a, b) == expect, "two nearby changes, one hunk:\n" + unifiedDiff(a, b));
    const std::string far = "x\n1\n2\n3\n4\n5\n6\n7\n8\n9\n10\ny\n";
    const std::string farB = "X\n1\n2\n3\n4\n5\n6\n7\n8\n9\n10\nY\n";
    const auto d = unifiedDiff(far, farB);
    check(d == "--- before\n+++ after\n@@ -1,4 +1,4 @@\n-x\n+X\n 1\n 2\n 3\n"
               "@@ -9,4 +9,4 @@\n 8\n 9\n 10\n-y\n+Y\n",
          "two distant changes, two hunks:\n" + d);
    check(unifiedDiff("", "new\n") == "--- before\n+++ after\n@@ -0,0 +1 @@\n+new\n",
          "an empty before: " + unifiedDiff("", "new\n"));
}

void testJson() {
    section("the JSON changeset adi_tool propose reads");
    Fixture f("json");
    if (!f.store) return;
    std::string err;
    const Payload j = {{"request", "quieter"},
                       {"actorDetail", "model-x"},
                       {"createdUtc", kT0},
                       {"ops", {{{"op", "mixer.setVolume"}, {"payload", {{"id", 2}, {"db", -3.0}}},
                                 {"label", "Bass -3"}}}}};
    const auto cs = changesetFromJson(*f.store, j, err);
    check(cs && cs->ops.size() == 1 && cs->request == "quieter" && cs->baseHead == OpJournal(*f.store).headSeq(),
          "parsed, on the current head when none is given: " + err);
    Payload noReq = j;
    noReq.erase("request");
    check(!changesetFromJson(*f.store, noReq, err), "a changeset without its request is refused");
    Payload old = j;
    old["baseHead"] = 1;
    const auto staleCs = changesetFromJson(*f.store, old, err);
    check(staleCs && apply(*f.store, *staleCs).refusal == Refusal::Stale,
          "an explicit older baseHead is honoured -- and is stale");
}

// --- a large project: what the preview costs --------------------------------------------------------

void testLargeProject() {
    section("a large project: the preview's cost, against a backup copy (ADR-0148)");
    Fixture f("large");
    if (!f.store) return;
    Store& s = *f.store;
    constexpr int kTracks = 200, kClipsPerTrack = 50;
    {
        SQLite::Transaction txn(s.db());
        for (int t = 3; t < 3 + kTracks; ++t) {
            s.db().exec("INSERT INTO tracks(id, kind, name, index_in_parent) VALUES (" +
                        std::to_string(t) + ", 'midi', 'T" + std::to_string(t) + "', " +
                        std::to_string(t) + ")");
            s.db().exec("INSERT INTO mixer_strip(track_id) VALUES (" + std::to_string(t) + ")");
            for (int c = 0; c < kClipsPerTrack; ++c) {
                const int id = t * 1000 + c;
                s.db().exec("INSERT INTO clips(id, track_id, kind, name, time_base, pos_ticks, "
                            "length_ticks) VALUES (" + std::to_string(id) + ", " +
                            std::to_string(t) + ", 'midi', 'C" + std::to_string(c) + "', 0, " +
                            std::to_string(static_cast<long long>(c) * 23063040) + ", 23063040)");
            }
        }
        txn.commit();
    }
    Changeset cs = begin(s, "one fader", "m", kT0);
    cs.ops.push_back(op("mixer.setVolume", {{"id", 100}, {"db", -2.5}}));

    using clock = std::chrono::steady_clock;
    const auto t0 = clock::now();
    const auto p = preview(s, cs);
    const auto previewMs = std::chrono::duration<double, std::milli>(clock::now() - t0).count();
    check(p.ok, "the preview runs on " + std::to_string(kTracks * kClipsPerTrack) + " clips");
    check(p.unifiedDiff.find("+  vol -2.5\n") != std::string::npos, "and finds the one change");

    const auto t1 = clock::now();
    const auto projMs = [&] {
        const auto a = textproj::projectStore(s);
        (void)a;
        return std::chrono::duration<double, std::milli>(clock::now() - t1).count();
    }();

    // The alternative ADR-0148 rejected: copy the file, commit on the copy.
    const auto copy = f.temp.path() / "copy.adi";
    const auto t2 = clock::now();
    {
        SQLite::Database dst(copy.string(), SQLite::OPEN_READWRITE | SQLite::OPEN_CREATE);
        sqlite3_backup* b = sqlite3_backup_init(dst.getHandle(), "main", s.db().getHandle(), "main");
        if (b) {
            sqlite3_backup_step(b, -1);
            sqlite3_backup_finish(b);
        }
    }
    const auto backupMs = std::chrono::duration<double, std::milli>(clock::now() - t2).count();
    std::printf("  info  %d clips: preview %.1f ms (one projection %.1f ms); a backup copy "
                "alone %.1f ms, before any projection\n",
                kTracks * kClipsPerTrack, previewMs, projMs, backupMs);
    check(previewMs < 20000.0, "the preview finishes in bounded time");
}

int runAll() {
    std::printf("adi_changeset_tests -- ADR-0145 d9, AI-AGENT 6, ADR-0148\n\n");
    testGuardrails();
    testPreviewDoesNotWrite();
    testApplyIsOneStep();
    testStale();
    testRequestRowIsInTheTransaction();
    testMediaUnlinkLeavesTheFile();
    testUnifiedDiff();
    testJson();
    testLargeProject();
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
