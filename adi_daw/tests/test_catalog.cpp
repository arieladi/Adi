// SPDX-License-Identifier: GPL-3.0-or-later
//
// The op catalogue. OPS.md §9.
//
// Every op is checked the same way: apply it, assert the world changed, undo it,
// assert the world came back. That is a stronger check than inspecting the
// stored inverse, because it exercises the inverse the way undo actually will.
//
// The note ops get more attention than the rest: they are the first ops to touch
// a BLOB, so they are the first to exercise the ADR-0009 granularity bound
// through the op system rather than only in a unit test.

#include "adi/blob.hpp"
#include "adi/history.hpp"
#include "adi/ops.hpp"
#include "adi/store.hpp"

#include <SQLiteCpp/SQLiteCpp.h>

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
    fs::path dir;
    std::unique_ptr<Store> store;
    explicit Fixture(const char* n) {
        dir = fs::temp_directory_path() / ("adi_cat_" + std::string(n));
        std::error_code ec;
        fs::remove_all(dir, ec);
        fs::create_directories(dir, ec);
        StoreError e = StoreError::Ok;
        store = Store::create(dir / "p.adi", e);
        if (store) store->db().exec("INSERT INTO project(id,name) VALUES (1,'Cat')");
    }
    ~Fixture() {
        store.reset();
        std::error_code ec;
        fs::remove_all(dir, ec);
    }
};

std::string scalarText(Store& s, const char* sql, std::int64_t id) {
    SQLite::Statement st(s.db(), sql);
    st.bind(1, id);
    return st.executeStep() ? st.getColumn(0).getString() : std::string("<none>");
}
double scalarReal(Store& s, const char* sql, std::int64_t id) {
    SQLite::Statement st(s.db(), sql);
    st.bind(1, id);
    return st.executeStep() ? st.getColumn(0).getDouble() : -999.0;
}
/// A count with no bound parameters. An earlier version of this file reused
/// scalarInt with "WHERE ?=?" and bound only one of the two placeholders, so
/// the condition was `1 = NULL` and every count came back zero -- a test helper
/// failing in a way that looks exactly like the code being wrong.
std::int64_t countRows(Store& s, const char* sql) {
    SQLite::Statement st(s.db(), sql);
    return st.executeStep() ? st.getColumn(0).getInt64() : -1;
}

std::int64_t scalarInt(Store& s, const char* sql, std::int64_t id) {
    SQLite::Statement st(s.db(), sql);
    st.bind(1, id);
    return st.executeStep() ? st.getColumn(0).getInt64() : -999;
}

bool run(OpJournal& j, const char* type, Payload p, std::string& err) {
    OpRequest r;
    r.opType = type;
    r.payload = std::move(p);
    r.label = type;
    const auto res = j.commit(r);
    err = res.error;
    return res.ok;
}

// --- registry shape -------------------------------------------------------------

void testRegistryGrew() {
    section("the catalogue");
    const auto& reg = OpRegistry::instance();
    check(reg.all().size() >= 50, "at least 50 ops registered, saw " +
                                      std::to_string(reg.all().size()));
    // A sample across every generation route: hand-written, scalar-generated,
    // blob-touching and ephemeral.
    for (const char* n : {"project.setName", "track.create", "track.setSolo",
                          "mixer.setVolume", "note.insert", "clip.resize",
                          "routing.connect", "transport.play",
                          "project.insertTempoEvent"})
        check(reg.find(n) != nullptr, std::string(n) + " is registered");

    const auto* play = reg.find("transport.play");
    check(play && play->ephemeral, "transport.play is ephemeral");
    check(play && play->scope == Scope::Transport, "and in transport scope");
    check(play && play->fields.empty(),
          "and has an EMPTY schema -- it takes no parameters, which is legal");
    check(play && play->buildInverse == nullptr, "and has no inverse");

    const auto* vol = reg.find("mixer.setVolume");
    check(vol && vol->coalescable, "mixer.setVolume coalesces (a fader drag)");
    check(vol && vol->inverseOp.empty(), "and is symmetric, as coalescing requires");
}

// --- scalar setters ---------------------------------------------------------------

void testScalarSetters() {
    section("scalar setters, apply and undo");
    Fixture f("scalar");
    if (!f.store) return;
    auto& s = *f.store;
    OpJournal j(s);
    History h(s);
    std::string err;

    check(run(j, "track.create", {{"id", 1}, {"kind", "midi"}, {"name", "T"}}, err),
          "track created: " + err);

    // name, solo, arm and monitor mode, each applied then undone.
    check(run(j, "track.setSolo", {{"id", 1}, {"soloed", true}}, err), "solo: " + err);
    check(scalarInt(s, "SELECT soloed FROM tracks WHERE id=?", 1) == 1, "track is soloed");
    check(h.undo().ok, "undo solo");
    check(scalarInt(s, "SELECT soloed FROM tracks WHERE id=?", 1) == 0, "solo undone");

    check(run(j, "track.setArm", {{"id", 1}, {"armed", true}}, err), "arm: " + err);
    check(scalarInt(s, "SELECT record_armed FROM tracks WHERE id=?", 1) == 1, "armed");
    check(h.undo().ok, "undo arm");
    check(scalarInt(s, "SELECT record_armed FROM tracks WHERE id=?", 1) == 0, "arm undone");

    check(run(j, "track.setInput", {{"id", 1}, {"input", "in:3"}}, err), "input: " + err);
    check(scalarText(s, "SELECT input_ref FROM tracks WHERE id=?", 1) == "in:3", "input set");
    check(h.undo().ok, "undo input");
    check(scalarText(s, "SELECT IFNULL(input_ref,'<null>') FROM tracks WHERE id=?", 1) ==
              "<null>",
          "input reverts to NULL, not to empty string -- the inverse kept the type");

    // mixer_strip keys on track_id, not id. The table-driven generator has to
    // get that right or it silently edits nothing.
    check(run(j, "mixer.setVolume", {{"id", 1}, {"db", -6.5}}, err), "volume: " + err);
    check(scalarReal(s, "SELECT volume_db FROM mixer_strip WHERE track_id=?", 1) == -6.5,
          "fader moved on the right row -- mixer_strip keys on track_id");
    check(h.undo().ok, "undo volume");
    check(scalarReal(s, "SELECT volume_db FROM mixer_strip WHERE track_id=?", 1) == 0.0,
          "fader restored");

    // A missing row must fail rather than silently updating nothing.
    check(!run(j, "track.setSolo", {{"id", 999}, {"soloed", true}}, err),
          "setting a field on a nonexistent row fails");
}

// --- clips ---------------------------------------------------------------------

void testClips() {
    section("clips");
    Fixture f("clips");
    if (!f.store) return;
    auto& s = *f.store;
    OpJournal j(s);
    History h(s);
    std::string err;

    check(run(j, "track.create", {{"id", 1}, {"kind", "midi"}, {"name", "T"}}, err), "track");
    check(run(j, "clip.create",
              {{"id", 5}, {"track", 1}, {"kind", "midi"}, {"name", "Riff"},
               {"pos", 0}, {"length", 23063040}}, err),
          "clip created: " + err);
    check(scalarInt(s, "SELECT COUNT(*) FROM clips WHERE id=?", 5) == 1, "clip exists");

    check(run(j, "clip.resize", {{"id", 5}, {"pos", 5765760}, {"length", 11531520}}, err),
          "resized: " + err);
    check(scalarInt(s, "SELECT pos_ticks FROM clips WHERE id=?", 5) == 5765760, "moved");
    check(scalarInt(s, "SELECT length_ticks FROM clips WHERE id=?", 5) == 11531520, "shortened");
    check(h.undo().ok, "undo resize");
    check(scalarInt(s, "SELECT pos_ticks FROM clips WHERE id=?", 5) == 0, "pos restored");
    check(scalarInt(s, "SELECT length_ticks FROM clips WHERE id=?", 5) == 23063040,
          "length restored");

    check(run(j, "clip.delete", {{"id", 5}}, err), "deleted: " + err);
    check(scalarInt(s, "SELECT COUNT(*) FROM clips WHERE id=?", 5) == 0, "gone");
    check(h.undo().ok, "undo delete");
    check(scalarText(s, "SELECT name FROM clips WHERE id=?", 5) == "Riff",
          "the capture brought the clip back with its name");
}

// --- notes: the first ops through the blob layer -----------------------------------

void testNotes() {
    section("notes (ADR-0009 blob granularity, via ops)");
    Fixture f("notes");
    if (!f.store) return;
    auto& s = *f.store;
    OpJournal j(s);
    History h(s);
    std::string err;

    check(run(j, "track.create", {{"id", 1}, {"kind", "midi"}, {"name", "T"}}, err), "track");
    check(run(j, "clip.create",
              {{"id", 5}, {"track", 1}, {"kind", "midi"}, {"pos", 0},
               {"length", 23063040}}, err), "clip");

    const auto noteCount = [&]() -> int {
        const auto blob = s.getEventStream(5, "notes");
        if (!blob) return 0;
        StreamReader<NoteRecord> r(*blob, FourCC::Notes);
        return r.ok() ? static_cast<int>(r.count()) : -1;
    };
    const auto findNote = [&](std::uint64_t id) -> std::optional<NoteRecord> {
        const auto blob = s.getEventStream(5, "notes");
        if (!blob) return std::nullopt;
        StreamReader<NoteRecord> r(*blob, FourCC::Notes);
        if (!r.ok()) return std::nullopt;
        for (std::uint32_t i = 0; i < r.count(); ++i)
            if (auto n = r.at(i); n && n->note_id == id) return n;
        return std::nullopt;
    };

    check(noteCount() == 0, "no notes yet");
    check(run(j, "note.insert",
              {{"clip", 5}, {"note", 1}, {"start", 0}, {"dur", 1441440},
               {"key", 60}, {"vel", 100}}, err), "inserted C4: " + err);
    check(noteCount() == 1, "one note");
    check(findNote(1) && findNote(1)->key == 60, "it is C4");

    // Inserted out of order; the writer keeps the stream sorted because the
    // header advertises SortedByTime and a reader may believe it.
    check(run(j, "note.insert",
              {{"clip", 5}, {"note", 3}, {"start", 2882880}, {"dur", 1441440},
               {"key", 67}}, err), "inserted a later note");
    check(run(j, "note.insert",
              {{"clip", 5}, {"note", 2}, {"start", 1441440}, {"dur", 1441440},
               {"key", 64}}, err), "inserted a middle note");
    check(noteCount() == 3, "three notes");
    {
        const auto blob = s.getEventStream(5, "notes");
        StreamReader<NoteRecord> r(*blob, FourCC::Notes);
        bool sorted = true;
        for (std::uint32_t i = 1; i < r.count(); ++i)
            if (r.at(i)->start_ticks < r.at(i - 1)->start_ticks) sorted = false;
        check(sorted, "the stream is kept sorted by time despite out-of-order inserts");
    }

    // A duplicate note id must be refused, not silently reassigned -- same rule
    // as object ids (ADR-0021 §7.3).
    check(!run(j, "note.insert",
               {{"clip", 5}, {"note", 1}, {"start", 0}, {"dur", 1}, {"key", 60}}, err),
          "a duplicate note id is refused");

    check(run(j, "note.setVelocity", {{"clip", 5}, {"note", 2}, {"vel", 30}}, err),
          "velocity set: " + err);
    check(findNote(2) && findNote(2)->vel_on == 30, "velocity is 30");
    check(h.undo().ok, "undo velocity");
    check(findNote(2) && findNote(2)->vel_on == 100,
          "velocity restored to the default the insert used");

    check(!run(j, "note.setVelocity", {{"clip", 5}, {"note", 2}, {"vel", 0}}, err),
          "velocity 0 is refused (SPEC 6.3.1 says 1..127)");
    check(!run(j, "note.setVelocity", {{"clip", 5}, {"note", 2}, {"vel", 200}}, err),
          "velocity 200 is refused");

    check(run(j, "note.move", {{"clip", 5}, {"note", 2}, {"start", 4324320}, {"key", 72}}, err),
          "moved: " + err);
    check(findNote(2) && findNote(2)->key == 72, "pitch changed");
    check(h.undo().ok, "undo move");
    check(findNote(2) && findNote(2)->key == 64 && findNote(2)->start_ticks == 1441440,
          "both pitch and time restored");

    check(!run(j, "note.resize", {{"clip", 5}, {"note", 2}, {"dur", 0}}, err),
          "a zero-length note is refused (SPEC 6.3.1 says dur > 0)");

    check(run(j, "note.delete", {{"clip", 5}, {"note", 2}}, err), "deleted: " + err);
    check(noteCount() == 2, "two notes left");
    check(h.undo().ok, "undo delete");
    check(noteCount() == 3, "three again");
    const auto back = findNote(2);
    check(back && back->key == 64 && back->vel_on == 100 && back->start_ticks == 1441440,
          "the captured note came back with every field intact");

    // ADR-0009: the blob for THIS clip changed and nothing else did.
    check(scalarInt(s, "SELECT COUNT(*) FROM event_streams WHERE clip_id=?", 5) == 1,
          "one notes blob for the clip, not one per note");
}

// --- tempo, signature, routing -------------------------------------------------

void testTempoAndRouting() {
    section("tempo, signature and routing");
    Fixture f("tempo");
    if (!f.store) return;
    auto& s = *f.store;
    OpJournal j(s);
    History h(s);
    std::string err;

    check(run(j, "project.insertTempoEvent", {{"pos", 0}, {"bpm", 120.0}}, err),
          "tempo at 0: " + err);
    check(run(j, "project.insertTempoEvent", {{"pos", 46126080}, {"bpm", 140.0}}, err),
          "tempo change");
    check(countRows(s, "SELECT COUNT(*) FROM tempo_map") == 2, "two tempo events");
    check(h.undo().ok, "undo the change");
    check(countRows(s, "SELECT COUNT(*) FROM tempo_map") == 1, "one left");

    check(run(j, "project.removeTempoEvent", {{"pos", 0}}, err), "removed: " + err);
    check(countRows(s, "SELECT COUNT(*) FROM tempo_map") == 0, "none left");
    check(h.undo().ok, "undo the removal");
    check(scalarReal(s, "SELECT bpm FROM tempo_map WHERE pos_ticks=?", 0) == 120.0,
          "the captured tempo came back at the right bpm");

    check(run(j, "project.insertTimeSignature", {{"pos", 0}, {"num", 7}, {"den", 8}}, err),
          "7/8: " + err);
    check(scalarInt(s, "SELECT numerator FROM time_signature_map WHERE pos_ticks=?", 0) == 7,
          "signature stored");

    check(run(j, "track.create", {{"id", 1}, {"kind", "midi"}, {"name", "A"}}, err), "track A");
    check(run(j, "track.create", {{"id", 2}, {"kind", "group"}, {"name", "B"}}, err), "track B");
    check(run(j, "routing.connect",
              {{"id", 1}, {"srcKind", "track"}, {"src", 1}, {"dstKind", "track"},
               {"dst", 2}, {"kind", "main"}}, err), "connected: " + err);
    check(scalarInt(s, "SELECT COUNT(*) FROM routing WHERE id=?", 1) == 1, "connection exists");

    // ADR-0029 removed 'bus'; the CHECK should still refuse it.
    check(!run(j, "routing.connect",
               {{"id", 2}, {"srcKind", "bus"}, {"src", 1}, {"dstKind", "track"},
                {"dst", 2}, {"kind", "main"}}, err),
          "a 'bus' endpoint kind is refused by the schema (ADR-0029)");

    check(run(j, "routing.disconnect", {{"id", 1}}, err), "disconnected: " + err);
    check(h.undo().ok, "undo disconnect");
    check(scalarInt(s, "SELECT src_id FROM routing WHERE id=?", 1) == 1,
          "the capture restored the connection's source");
}

// --- ephemeral, for real this time ------------------------------------------------

void testEphemeralTransport() {
    section("transport ops are ephemeral for real now");
    Fixture f("transport");
    if (!f.store) return;
    auto& s = *f.store;
    OpJournal j(s);
    History h(s);
    std::string err;

    check(run(j, "track.create", {{"id", 1}, {"kind", "midi"}, {"name", "Real"}}, err),
          "a real edit");
    const auto headAfterEdit = h.head();

    check(run(j, "transport.play", Payload::object(), err), "play: " + err);
    check(run(j, "transport.seek", {{"pos", 5765760}}, err), "seek: " + err);
    check(run(j, "transport.stop", Payload::object(), err), "stop: " + err);

    check(h.head() == headAfterEdit,
          "three transport ops did not move the undo head");

    const auto step = h.nextUndo();
    check(step && step->label == "track.create",
          "Ctrl-Z still lands on the last real EDIT, not on stop");

    check(j.count() == 4, "but all four ops are in the log -- the audit trail keeps them");
    check(countRows(s, "SELECT COUNT(*) FROM ops WHERE tags='ephemeral'") == 3,
          "three of them tagged ephemeral");

    // Undoing still works, and undoes the edit rather than a transport op.
    check(h.undo().ok, "undo");
    check(countRows(s, "SELECT COUNT(*) FROM tracks") == 0,
          "the track is gone -- undo reached past the transport ops");

    // Their payloads are still validated: an empty schema is CLOSED, not absent.
    check(!run(j, "transport.play", {{"unexpected", 1}}, err),
          "transport.play rejects an unknown field -- empty means closed, not anything goes");
    check(!run(j, "transport.seek", Payload::object(), err),
          "transport.seek still requires its position");
}

}  // namespace

int main() {
    std::printf("adi_catalog_tests -- OPS.md 9\n\n");
    try {
        testRegistryGrew();
        testScalarSetters();
        testClips();
        testNotes();
        testTempoAndRouting();
        testEphemeralTransport();
    } catch (const std::exception& e) {
        std::printf("\nFAILED -- exception escaped: %s\n", e.what());
        return 1;
    }
    std::printf("\n%s -- %d checks, %d failure(s)\n", g_failures ? "FAILED" : "PASS", g_checks,
                g_failures);
    return g_failures ? 1 : 0;
}
