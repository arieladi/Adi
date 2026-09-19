// SPDX-License-Identifier: GPL-3.0-or-later
//
// Store tests. The one that matters most is the WAL sidecar rule (SPEC §3.3):
// a cleanly closed .adi must be exactly one file on disk. Getting that wrong is
// silent — the project still opens, just missing everything since the last
// checkpoint — so it is asserted by counting files, not by trusting a pragma.

#include "adi/blob.hpp"
#include "adi/store.hpp"

#include <SQLiteCpp/SQLiteCpp.h>

#include <cstdio>
#include <filesystem>
#include <fstream>
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

/// A scratch directory that cleans itself up, so a failing test does not leave
/// a .adi behind that the next run then fails to create over.
struct Scratch {
    fs::path dir;
    explicit Scratch(const char* name) {
        dir = fs::temp_directory_path() / ("adi_store_test_" + std::string(name));
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

std::vector<std::string> siblings(const fs::path& p) {
    std::vector<std::string> out;
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(p.parent_path(), ec))
        out.push_back(e.path().filename().string());
    return out;
}

// --- create / open -----------------------------------------------------------

void testCreateAndOpen() {
    section("create and open");
    Scratch s("create");
    const auto p = s / "song.adi";

    StoreError err = StoreError::Ok;
    {
        auto st = Store::create(p, err);
        check(st != nullptr, "create() returns a store");
        check(err == StoreError::Ok, "create() reports Ok");
        if (!st) return;
        check(!st->readOnly(), "a created store is writable");
        check(st->schemaMajor() == kSchemaMajor, "schema major is ours");
        check(st->close(), "close() succeeds");
    }
    check(fs::exists(p), "the file exists after close");

    {
        auto st = Store::open(p, err);
        check(st != nullptr && err == StoreError::Ok, "reopen succeeds");
        if (st) {
            check(st->schemaMajor() == kSchemaMajor, "major survives the round trip");
            check(st->schemaMinor() == kSchemaMinor, "minor survives the round trip");
        }
    }

    // create() must never quietly overwrite an existing project.
    auto again = Store::create(p, err);
    check(again == nullptr && err == StoreError::AlreadyExists,
          "create() refuses to overwrite an existing file");
}

// --- SPEC 3.3: the WAL sidecar rule -----------------------------------------

void testWalSidecarRule() {
    section("SPEC 3.3 WAL sidecar rule");
    Scratch s("wal");
    const auto p = s / "song.adi";

    StoreError err = StoreError::Ok;
    auto st = Store::create(p, err);
    check(st != nullptr, "created");
    if (!st) return;

    // Force real WAL traffic so the sidecars genuinely exist mid-session.
    // Without a write there may be nothing to checkpoint and the test would
    // pass for the wrong reason.
    st->db().exec("INSERT INTO project(id, name) VALUES (1, 'WAL test')");
    st->db().exec("INSERT INTO tempo_map(pos_ticks, bpm) VALUES (0, 120.0)");
    const auto journal = st->db().execAndGet("PRAGMA journal_mode").getString();
    check(journal == "wal", "journal_mode is wal while open, got '" + journal + "'");

    const auto during = siblings(p);
    const bool sawSidecar =
        std::find(during.begin(), during.end(), "song.adi-wal") != during.end();
    check(sawSidecar, "a -wal sidecar exists while the project is open");

    check(st->close(), "close() succeeds");

    // THE assertion. Not "the pragma returned delete" -- what is on disk.
    const auto after = siblings(p);
    std::string listing;
    for (const auto& f : after) listing += f + " ";
    check(after.size() == 1, "exactly one file remains after close, saw: " + listing);
    check(!fs::exists(p.string() + "-wal"), "no -wal remains");
    check(!fs::exists(p.string() + "-shm"), "no -shm remains");

    // And the data actually landed in the main file rather than being lost
    // with the sidecar -- the failure this rule exists to prevent.
    auto re = Store::open(p, err);
    check(re != nullptr, "reopens after close");
    if (re) {
        const auto name = re->db().execAndGet("SELECT name FROM project WHERE id=1").getString();
        check(name == "WAL test", "data written before close survived the checkpoint");
    }
}

// --- SPEC 2 and 11: identity and version gates -------------------------------

void testIdentityAndVersionGates() {
    section("SPEC 2 / 11 identity and version");
    Scratch s("identity");

    // A perfectly good SQLite file that is not a .adi.
    const auto foreign = s / "notours.adi";
    {
        SQLite::Database db(foreign.string(), SQLite::OPEN_READWRITE | SQLite::OPEN_CREATE);
        db.exec("CREATE TABLE t(x)");
    }
    StoreError err = StoreError::Ok;
    auto bad = Store::open(foreign, err);
    check(bad == nullptr && err == StoreError::NotAnAdiFile,
          "a SQLite file with the wrong application_id is rejected, not parsed");

    // Not a database at all.
    const auto garbage = s / "garbage.adi";
    {
        std::ofstream f(garbage, std::ios::binary);
        f << "this is not a database, it is a text file";
    }
    auto g = Store::open(garbage, err);
    check(g == nullptr, "a non-database file is rejected");

    // SPEC 11: a newer MAJOR opens read-only rather than read-write.
    const auto future = s / "future.adi";
    {
        auto st = Store::create(future, err);
        check(st != nullptr, "created a project to age forward");
        if (st) {
            st->db().exec("PRAGMA user_version = " + std::to_string((kSchemaMajor + 1) * 1000));
            st->close();
        }
    }
    auto fut = Store::open(future, err);
    check(fut != nullptr, "a newer-major project still opens");
    check(err == StoreError::SchemaTooNew, "and reports SchemaTooNew");
    if (fut) {
        check(fut->readOnly(), "a newer-major project is READ-ONLY, never read-write");
        check(fut->schemaMajor() == kSchemaMajor + 1, "its major is reported as found");
    }

    // A newer MINOR is fine and stays writable -- unknown tables and columns
    // survive because we never rewrite the file wholesale.
    const auto minorer = s / "minor.adi";
    {
        auto st = Store::create(minorer, err);
        if (st) {
            st->db().exec("PRAGMA user_version = " + std::to_string(kSchemaMajor * 1000 + 99));
            st->close();
        }
    }
    auto mn = Store::open(minorer, err);
    check(mn != nullptr && err == StoreError::Ok, "a newer minor opens normally");
    if (mn) check(!mn->readOnly(), "a newer minor stays writable");
}

// --- ADR-0009 blob persistence ----------------------------------------------

void testBlobRoundTrip() {
    section("ADR-0009 blob round trip");
    Scratch s("blobs");
    const auto p = s / "song.adi";

    StoreError err = StoreError::Ok;
    auto st = Store::create(p, err);
    check(st != nullptr, "created");
    if (!st) return;

    st->db().exec("INSERT INTO project(id,name) VALUES (1,'Blobs')");
    st->db().exec("INSERT INTO tracks(id,kind,name) VALUES (1,'midi','Keys')");
    st->db().exec(
        "INSERT INTO clips(id,track_id,kind,name,time_base,pos_ticks,length_ticks) "
        "VALUES (1,1,'midi','Riff',0,0,23063040)");

    std::vector<NoteRecord> notes;
    for (int i = 0; i < 500; ++i) {
        NoteRecord n{};
        n.start_ticks = static_cast<std::int64_t>(i) * 1441440;
        n.dur_ticks = 1441440;
        n.note_id = static_cast<std::uint64_t>(i) + 1;
        n.key = static_cast<std::uint8_t>(36 + (i % 60));
        n.vel_on = static_cast<std::uint8_t>(1 + (i % 127));
        notes.push_back(n);
    }
    const auto blob = writeStream<NoteRecord>(FourCC::Notes, notes);

    check(st->putEventStream(1, "notes", blob), "put notes stream");
    const auto back = st->getEventStream(1, "notes");
    check(back.has_value(), "get notes stream");
    if (back) {
        check(*back == blob, "note blob is byte-identical through SQLite");
        StreamReader<NoteRecord> r(*back, FourCC::Notes);
        check(r.ok() && r.count() == 500, "and parses back to 500 notes");
        check(r.at(499)->note_id == 500, "last note intact");
    }

    // Upsert: writing the same object twice replaces, never duplicates. This is
    // the ADR-0009 granularity bound in practice -- one blob per editable object.
    std::vector<NoteRecord> fewer(notes.begin(), notes.begin() + 10);
    const auto small = writeStream<NoteRecord>(FourCC::Notes, fewer);
    check(st->putEventStream(1, "notes", small), "re-put notes stream");
    const auto rows =
        st->db().execAndGet("SELECT COUNT(*) FROM event_streams WHERE clip_id=1").getInt();
    check(rows == 1, "re-putting replaces rather than duplicating");
    const auto back2 = st->getEventStream(1, "notes");
    check(back2 && *back2 == small, "the replacement is what reads back");

    // Per-note expression, one row per (clip, note, dimension) -- ADR-0009 again.
    std::vector<ExpressionPoint> exp(64);
    for (std::size_t i = 0; i < exp.size(); ++i) {
        exp[i].time_ticks = static_cast<std::int64_t>(i) * 1000;
        exp[i].value = static_cast<float>(i) / 63.0f;
    }
    const auto eb = writeStream<ExpressionPoint>(FourCC::Expression, exp);
    check(st->putNoteExpression(1, 1, 1, eb), "put pressure curve for note 1");
    const auto eback = st->getNoteExpression(1, 1, 1);
    check(eback && *eback == eb, "expression blob round-trips byte-identically");
    check(!st->getNoteExpression(1, 2, 1).has_value(),
          "a note with no expression returns nullopt, not an empty blob");

    check(st->close(), "closed");
}

// --- the embedded DDL is the file on disk ------------------------------------

void testSchemaMatchesFile() {
    section("embedded schema == schema.sql");
    Scratch s("schema");
    const auto p = s / "song.adi";
    StoreError err = StoreError::Ok;
    auto st = Store::create(p, err);
    check(st != nullptr, "created");
    if (!st) return;

    // The generated header is built from docs/format/schema.sql, so the table
    // count here is a check that the generation ran and produced the real DDL
    // rather than something stale or empty.
    const auto tables = st->db()
        .execAndGet("SELECT COUNT(*) FROM sqlite_master WHERE type='table' "
                    "AND name NOT LIKE 'sqlite_%'").getInt();
    check(tables >= 38, "the created database has the full schema, got " +
                            std::to_string(tables) + " tables");

    const auto appId = st->db().execAndGet("PRAGMA application_id").getInt();
    check(appId == kApplicationId, "application_id is stamped on create");
    const auto pageSize = st->db().execAndGet("PRAGMA page_size").getInt();
    check(pageSize == 4096, "page_size is 4096 (SPEC 3.4), got " + std::to_string(pageSize));
}

}  // namespace

int main() {
    std::printf("adi_store_tests -- SPEC 2, 3, 11 and ADR-0009\n\n");
    testCreateAndOpen();
    testWalSidecarRule();
    testIdentityAndVersionGates();
    testBlobRoundTrip();
    testSchemaMatchesFile();
    std::printf("\n%s -- %d checks, %d failure(s)\n", g_failures ? "FAILED" : "PASS", g_checks,
                g_failures);
    return g_failures ? 1 : 0;
}
