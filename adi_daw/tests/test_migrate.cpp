// SPDX-License-Identifier: GPL-3.0-or-later
//
// Upgrading an older 1.x file: ADR-0144, SPEC §11.
//
// The old files are built from the schema.sql each minor actually shipped,
// frozen under docs/format/history/, not by dropping objects from a fresh file:
// dropping would only prove the migration inverts itself. A frozen 1.0 carries
// everything 1.0 really had, comments in its DDL included, and the upgraded
// file must then match a fresh one object for object.

#include "temp_directory.hpp"

#include "adi/check.hpp"
#include "adi/history.hpp"
#include "adi/ops.hpp"
#include "adi/store.hpp"

#include <SQLiteCpp/SQLiteCpp.h>
#include <sqlite3.h>

#include <algorithm>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <set>
#include <sstream>
#include <string>
#include <tuple>
#include <vector>

#ifndef ADI_SCHEMA_HISTORY_DIR
#error "ADI_SCHEMA_HISTORY_DIR must name docs/format/history"
#endif

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

std::string readFile(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

/// The DDL with its comments removed and whitespace collapsed. SQLite stores a
/// CREATE statement verbatim, comments included, and 1.1 added a comment inside
/// media_files's CREATE TABLE. A migration must never rewrite an existing
/// table, so comments are the one difference a correct upgrade keeps.
std::string normalise(const std::string& sql) {
    std::string out;
    bool space = false;
    for (std::size_t i = 0; i < sql.size(); ++i) {
        if (sql[i] == '-' && i + 1 < sql.size() && sql[i + 1] == '-') {
            while (i < sql.size() && sql[i] != '\n') ++i;
            space = true;
            continue;
        }
        const char c = sql[i];
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') { space = true; continue; }
        if (space && !out.empty()) out += ' ';
        space = false;
        out += c;
    }
    return out;
}

using Object = std::tuple<std::string, std::string, std::string, std::string>;

/// Every row of sqlite_master as (type, name, table, normalised sql), as a set:
/// order is not part of the schema.
std::set<Object> objectsOf(SQLite::Database& db) {
    std::set<Object> out;
    SQLite::Statement st(db, "SELECT type, name, tbl_name, IFNULL(sql, '') FROM sqlite_master");
    while (st.executeStep())
        out.emplace(st.getColumn(0).getString(), st.getColumn(1).getString(),
                    st.getColumn(2).getString(), normalise(st.getColumn(3).getString()));
    return out;
}

std::string describeDiff(const std::set<Object>& a, const std::set<Object>& b) {
    std::string s;
    for (const auto& o : a)
        if (!b.count(o)) s += " -" + std::get<0>(o) + ":" + std::get<1>(o);
    for (const auto& o : b)
        if (!a.count(o)) s += " +" + std::get<0>(o) + ":" + std::get<1>(o);
    return s.empty() ? " <none>" : s;
}

int userVersion(const fs::path& p) {
    SQLite::Database db(p.string(), SQLite::OPEN_READONLY);
    return db.execAndGet("PRAGMA user_version").getInt();
}

/// A file exactly as schema 1.`minor` created it, with a little music in it.
fs::path buildOld(const fs::path& dir, int minor, bool embeddedMedia = false) {
    const auto p = dir / ("v1_" + std::to_string(minor) + ".adi");
    const auto ddl = readFile(fs::path(ADI_SCHEMA_HISTORY_DIR) /
                              ("schema-1." + std::to_string(minor) + ".sql"));
    SQLite::Database db(p.string(), SQLite::OPEN_READWRITE | SQLite::OPEN_CREATE);
    db.exec("PRAGMA page_size = 4096");
    db.exec(ddl);
    db.exec("INSERT INTO project(id, name) VALUES (1, 'Old Song')");
    db.exec("INSERT INTO tracks(id, kind, name) VALUES (1, 'midi', 'Keys')");
    db.exec("INSERT INTO mixer_strip(track_id) VALUES (1)");
    db.exec("INSERT INTO clips(id, track_id, kind, name, time_base, pos_ticks, length_ticks) "
            "VALUES (5, 1, 'midi', 'Riff', 0, 0, 23063040)");
    if (embeddedMedia) {
        db.exec("INSERT INTO media_files(id, hash_blake3, embedded) VALUES (9, 'abc', 1)");
        db.exec("INSERT INTO media_blobs(media_id, chunk_index, data) VALUES (9, 0, X'0102')");
    }
    return p;
}

std::set<Object> freshObjects(const fs::path& dir) {
    StoreError e = StoreError::Ok;
    auto s = Store::createEmpty(dir / "fresh.adi", e);
    if (!s) return {};
    return objectsOf(s->db());
}

bool addRemark(Store& s) {
    OpJournal j(s);
    OpRequest r;
    r.opType = "remark.add";
    r.payload = {{"id", 1}, {"kind", "track"}, {"target", 1}, {"author", "user"},
                 {"text", "an old project can take a remark"}, {"created", 1790000000000000}};
    return j.commit(r).ok;
}

// --- the upgrade itself ------------------------------------------------------------

void testUpgradeEachMinor() {
    for (int minor = 0; minor < kSchemaMinor; ++minor) {
        const std::string v = "1." + std::to_string(minor);
        std::printf("[a %s file opened for writing is upgraded to 1.%d]\n", v.c_str(),
                    kSchemaMinor);
        adi::test::TempDirectory temp("migrate", ("up" + std::to_string(minor)).c_str());
        const auto p = buildOld(temp.path(), minor);
        const auto fresh = freshObjects(temp.path());
        check(userVersion(p) == 1000 + minor, v + ": the fixture really is " + v);

        StoreError e = StoreError::Ok;
        {
            auto s = Store::open(p, e);
            check(s != nullptr && e == StoreError::Ok, v + ": opens for writing");
            if (!s) continue;
            check(s->upgradedFromMinor() == minor, v + ": reports what it upgraded from");
            check(s->schemaMinor() == kSchemaMinor, v + ": now reports the current minor");
            check(s->db().execAndGet("SELECT count(*) FROM clip_warp_fade_domains").getInt() == 0,
                  v + ": migration creates no retained Warp fades for existing clips");
            check(s->db().execAndGet("PRAGMA user_version").getInt() == kUserVersion,
                  v + ": user_version is current");
            check(s->db().execAndGet("SELECT value FROM adi_meta WHERE key = 'schema_minor'")
                          .getString() == std::to_string(kSchemaMinor),
                  v + ": adi_meta.schema_minor agrees");
            const auto got = objectsOf(s->db());
            check(got == fresh, v + ": sqlite_master matches a fresh file:" +
                                    describeDiff(got, fresh));
            check(s->db().execAndGet("SELECT name FROM tracks WHERE id = 1").getString() == "Keys",
                  v + ": the existing data is untouched");
            const int storedRate = minor < 9 ? 48000 : 44100;
            check(s->db().execAndGet("SELECT sample_rate FROM project").getInt() == storedRate,
                  v + ": the stored sample rate is preserved across the upgrade");

            check(addRemark(*s), v + ": remark.add works");
            History h(*s);
            const auto snap = h.takeSnapshot("", 1790000000000000, 0);
            check(snap.ok && snap.name == "Old Song 2026-09-21 14:13",
                  v + ": a history snapshot works: " + snap.name + snap.error);
            const auto rep = checkProject(*s);
            check(rep.clean() && rep.warnings == 0, v + ": check is clean");
        }
        {
            auto s = Store::open(p, e);
            check(s && !s->upgradedFromMinor().has_value(),
                  v + ": the next open has nothing to upgrade");
        }
    }
}

void testEmbeddedMediaSurvives() {
    section("a 1.0 file with embedded media is upgraded, and its media left alone");
    adi::test::TempDirectory temp("migrate", "media");
    const auto p = buildOld(temp.path(), 0, /*embeddedMedia=*/true);
    StoreError e = StoreError::Ok;
    auto s = Store::open(p, e);
    check(s != nullptr && e == StoreError::Ok, "the upgrade does not refuse it");
    if (!s) return;
    check(s->db().execAndGet("SELECT embedded FROM media_files WHERE id = 9").getInt() == 1 &&
              s->db().execAndGet("SELECT COUNT(*) FROM media_blobs").getInt() == 1,
          "the embedded row and its bytes are still there -- extracting is not this job");
    const auto rep = checkProject(*s);
    check(rep.errors == 1 && rep.findings.size() == 1 && rep.findings[0].code == "media.embedded",
          "check reports exactly the embedded media, as for any 1.0 file");
    bool refused = false;
    try {
        s->db().exec("INSERT INTO media_blobs(media_id, chunk_index, data) VALUES (9, 1, X'03')");
    } catch (const std::exception&) {
        refused = true;
    }
    check(refused, "but the 1.1 triggers now refuse any NEW embedded bytes");
}

// --- what must not be written ---------------------------------------------------------

void testReadOnlyNeverWrites() {
    section("a read-only open never writes; missing tables read as empty");
    adi::test::TempDirectory temp("migrate", "ro");
    const auto p = buildOld(temp.path(), 1);
    const auto before = readFile(p);
    StoreError e = StoreError::Ok;
    {
        auto s = Store::open(p, e, /*readOnly=*/true);
        check(s != nullptr && e == StoreError::Ok, "opens read-only");
        if (!s) return;
        check(s->schemaMinor() == 1 && !s->upgradedFromMinor(), "and says it is still 1.1");
        check(s->db().execAndGet("SELECT COUNT(*) FROM remarks").getInt() == 0,
              "a table 1.1 lacks reads as empty, not as an error");
        History h(*s);
        check(h.listSnapshots().empty(), "and so does history_snapshots");
        check(!addRemark(*s), "writing is still refused");
        check(checkProject(*s).clean(), "check runs clean on it");
    }
    check(readFile(p) == before, "the file's bytes are identical afterwards");
    check(userVersion(p) == 1001, "user_version is still 1001");
}

void testNewerMinorUntouched() {
    section("a newer minor is never touched (SPEC 11)");
    adi::test::TempDirectory temp("migrate", "newer");
    const auto p = temp.path() / "newer.adi";
    {
        StoreError e = StoreError::Ok;
        auto s = Store::createEmpty(p, e);
        if (!s) return;
        s->db().exec("CREATE TABLE future_things(id INTEGER PRIMARY KEY, x TEXT) STRICT");
        s->db().exec("INSERT INTO future_things VALUES (1, 'from 1." +
                     std::to_string(kSchemaMinor + 1) + "')");
        s->db().exec("PRAGMA user_version = " + std::to_string(kUserVersion + 1));
    }
    StoreError e = StoreError::Ok;
    {
        auto s = Store::open(p, e);
        check(s != nullptr && e == StoreError::Ok, "opens for writing");
        if (!s) return;
        check(s->schemaMinor() == kSchemaMinor + 1 && !s->upgradedFromMinor(),
              "reports the newer minor and upgraded nothing");
        check(s->db().execAndGet("SELECT COUNT(*) FROM future_things").getInt() == 1,
              "its unknown table survives");
    }
    check(userVersion(p) == kUserVersion + 1, "user_version was not rewritten");
}

void testFailureLeavesTheOldFile() {
    section("a step that fails leaves the file exactly at its old version");
    adi::test::TempDirectory temp("migrate", "fail");
    const auto p = buildOld(temp.path(), 1);
    {
        // A third-party writer already used the name 1.3 needs. The 1.2 step
        // succeeds, the 1.3 step fails, and the 1.2 step must not survive it.
        SQLite::Database db(p.string(), SQLite::OPEN_READWRITE);
        db.exec("CREATE TABLE history_snapshots(theirs TEXT) STRICT");
        db.exec("INSERT INTO history_snapshots VALUES ('keep me')");
    }
    StoreError e = StoreError::Ok;
    auto s = Store::open(p, e);
    check(s == nullptr && e == StoreError::MigrationFailed, "the open fails, and says why");
    SQLite::Database db(p.string(), SQLite::OPEN_READONLY);
    check(db.execAndGet("PRAGMA user_version").getInt() == 1001, "user_version is still 1001");
    check(db.execAndGet("SELECT COUNT(*) FROM sqlite_master WHERE name = 'remarks'").getInt() == 0,
          "the 1.2 step's table was rolled back, not left half-upgraded");
    check(db.execAndGet("SELECT value FROM adi_meta WHERE key = 'schema_minor'").getString() == "1",
          "adi_meta still says 1");
    check(db.execAndGet("SELECT theirs FROM history_snapshots").getString() == "keep me",
          "and the other writer's table is untouched");
}

void testProjectDefaultMigration() {
    section("1.9 changes the default, preserving project values and extension indexes/triggers");
    adi::test::TempDirectory temp("migrate", "project-default");
    const auto p = buildOld(temp.path(), 8);
    {
        SQLite::Database db(p.string(), SQLite::OPEN_READWRITE);
        db.exec("UPDATE project SET sample_rate=96000, ppq=123456, length_ticks=700, "
                "timecode_origin_ns=800, frame_rate_num=30000,frame_rate_den=1001,"
                "render_bit_depth=16,author='Author',notes='Notes';"
                "CREATE INDEX project_author ON project(author);"
                "CREATE TABLE project_audit(n INTEGER);"
                "CREATE TRIGGER project_insert AFTER INSERT ON project BEGIN "
                "INSERT INTO project_audit VALUES(1); END;");
    }
    StoreError error{};
    auto s = Store::open(p, error);
    check(s && error == StoreError::Ok, "the default migration opens a 1.8 project");
    if (!s) return;
    auto& db = s->db();
    check(db.execAndGet("SELECT count(*) FROM project WHERE id=1 AND name='Old Song' "
        "AND sample_rate=96000 AND ppq=123456 AND length_ticks=700 AND timecode_origin_ns=800 "
        "AND frame_rate_num=30000 AND frame_rate_den=1001 AND render_bit_depth=16 "
        "AND author='Author' AND notes='Notes'").getInt() == 1, "every stored project value is preserved");
    check(db.execAndGet("SELECT count(*) FROM sqlite_master WHERE name IN ('project_author','project_insert')").getInt() == 2 &&
          db.execAndGet("SELECT count(*) FROM project_audit").getInt() == 0,
          "extension indexes and triggers survive, without running an insert trigger during migration");
    db.exec("DELETE FROM project; INSERT INTO project(id,name) VALUES(1,'Again');");
    check(db.execAndGet("SELECT sample_rate FROM project").getInt() == 44100 &&
          db.execAndGet("SELECT count(*) FROM project_audit").getInt() == 1,
          "an omitted rate really defaults to 44100 and the restored trigger works");
}

void testProjectMigrationRefusesDataLoss() {
    for (const bool foreignKey : {false, true}) {
        adi::test::TempDirectory temp("migrate", foreignKey ? "project-fk" : "project-column");
        const auto p = buildOld(temp.path(), 8);
        {
            SQLite::Database db(p.string(), SQLite::OPEN_READWRITE);
            if (foreignKey) db.exec("CREATE TABLE project_child(id INTEGER REFERENCES PrOjEcT(id) ON DELETE CASCADE);"
                                    "INSERT INTO project_child VALUES(1)");
            else db.exec("ALTER TABLE project ADD COLUMN custom TEXT DEFAULT 'keep me'");
        }
        const auto before = readFile(p);
        StoreError error{};
        const auto s = Store::open(p, error);
        check(!s && error == StoreError::MigrationFailed && readFile(p) == before,
              "an unsafe extension causes an unchanged-file failure instead of data loss");
    }
}

// --- how it runs -------------------------------------------------------------------

std::vector<std::string> g_trace;
int traceStatement(unsigned, void*, void* stmt, void*) {
    char* sql = sqlite3_expanded_sql(static_cast<sqlite3_stmt*>(stmt));
    if (sql) {
        g_trace.emplace_back(sql);
        sqlite3_free(sql);
    }
    return 0;
}

void testOrder() {
    section("one step per minor, in order, user_version last");
    const auto& steps = migrationSteps();
    bool contiguous = !steps.empty() && steps.back().toMinor == kSchemaMinor;
    for (std::size_t i = 0; i < steps.size(); ++i)
        contiguous = contiguous && steps[i].toMinor == static_cast<int>(i) + 1;
    check(contiguous, "the step table covers 1.1 to the current minor, once each");

    adi::test::TempDirectory temp("migrate", "order");
    const auto p = buildOld(temp.path(), 0);
    SQLite::Database db(p.string(), SQLite::OPEN_READWRITE);
    g_trace.clear();
    sqlite3_trace_v2(db.getHandle(), SQLITE_TRACE_STMT, traceStatement, nullptr);
    std::string err;
    check(migrateToCurrent(db, 0, err), "migrateToCurrent runs: " + err);
    sqlite3_trace_v2(db.getHandle(), 0, nullptr, nullptr);

    const auto at = [&](const std::string& needle) {
        for (std::size_t i = 0; i < g_trace.size(); ++i)
            if (g_trace[i].find(needle) != std::string::npos) return static_cast<long>(i);
        return -1L;
    };
    long lastCreate = -1;
    for (std::size_t i = 0; i < g_trace.size(); ++i)
        if (g_trace[i].rfind("CREATE", 0) == 0) lastCreate = static_cast<long>(i);
    const long version = at("PRAGMA user_version");
    const long meta = at("UPDATE adi_meta");
    check(version > lastCreate && version > meta && lastCreate >= 0,
          "user_version is set after every CREATE and the adi_meta update (at " +
              std::to_string(version) + ", last CREATE " + std::to_string(lastCreate) + ")");
    check(at("media_never_embedded_insert") < at("TABLE remarks") &&
              at("TABLE remarks") < at("TABLE history_snapshots"),
          "the minors' objects are created in minor order");
    check(!migrateToCurrent(db, kSchemaMinor, err), "there is nothing to upgrade at the current minor");
}

int runAll() {
    std::printf("adi_migrate_tests -- ADR-0144, SPEC 11\n\n");
    testUpgradeEachMinor();
    testEmbeddedMediaSurvives();
    testReadOnlyNeverWrites();
    testNewerMinorUntouched();
    testFailureLeavesTheOldFile();
    testProjectDefaultMigration();
    testProjectMigrationRefusesDataLoss();
    testOrder();
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
