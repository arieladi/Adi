// SPDX-License-Identifier: GPL-3.0-or-later
#include "temp_directory.hpp"
#include "adi/render/project_jobs.hpp"
#include "adi/render/job_json.hpp"
#include "adi/audio/io_audit.hpp"
#include "adi/digest.hpp"
#include "adi/media/media_ops.hpp"
#include "adi/store.hpp"
#include <SQLiteCpp/SQLiteCpp.h>
#include <sqlite3.h>
#include <nlohmann/json.hpp>
#include <cstdio>
#include <stdexcept>

namespace {
using namespace adi;
using namespace adi::render;
using Json = nlohmann::json;
namespace fs = std::filesystem;
int checks = 0, failures = 0;
void check(bool good, const char* name) {
    ++checks;
    if (!good) { ++failures; std::printf("FAIL: %s\n", name); }
}
struct Fixture {
    test::TempDirectory temp{"project_render_jobs", "store"};
    std::unique_ptr<Store> store;
    Fixture() {
        StoreError error{};
        store = Store::createEmpty(temp.path() / media::pathFromUtf8("Saved \xD7\x90\xF0\x9F\x8E\xB5.adi"), error);
        if (!store || error != StoreError::Ok) throw std::runtime_error("create project");
        store->db().exec("INSERT INTO project(id,name) VALUES(1,'Saved jobs');"
                         "INSERT INTO tracks(id,kind,name) VALUES(7,'audio','Source'),(9,'master','Main');"
                         "INSERT INTO session_state(key,value) VALUES('unrelated','preserve me');");
    }
    Job job() const {
        Job result;
        result.project = store->path();
        result.directory = temp.path() / "not-created";
        result.nameTemplate = "mix-{counter}";
        result.beginSample = 137; result.endSample = INT64_MAX - 4096;
        result.session = {2, 48000, 257, engine::Precision::Float64};
        result.source = {7, engine::OfflineTap::PreFader};
        result.encoding.ditherSeed = UINT64_MAX;
        return result;
    }
    std::string raw() const {
        SQLite::Statement query(store->db(), "SELECT value FROM session_state WHERE key=?");
        query.bind(1, kSavedJobsKey);
        return query.executeStep() ? query.getColumn(0).getString() : std::string{};
    }
    void raw(const std::string& value) const {
        SQLite::Statement query(store->db(), "INSERT INTO session_state(key,value) VALUES(?,?) ON CONFLICT(key) DO UPDATE SET value=excluded.value");
        query.bind(1, kSavedJobsKey); query.bind(2, value); query.exec();
    }
};
std::string journal(const Store& store) {
    auto& db = const_cast<SQLite::Database&>(store.db());
    return db.execAndGet("SELECT count(*) FROM ops").getString() + ":" +
           db.execAndGet("SELECT COALESCE(head_seq,-1) FROM op_branches WHERE is_current=1").getString();
}
void lifecycle() {
    Fixture fixture; std::string error = "old";
    auto& store = *fixture.store;
    const auto absent = loadSavedJobs(store, error);
    check(absent && !absent->present && absent->jobs.empty() && error.empty(), "absent is successful and distinguishable");
    const auto digest = digestProject(store).text, history = journal(store);
    auto first = fixture.job(); auto second = first;
    second.regionId = INT64_MAX; second.beginSample = -17; second.endSample = -1;
    second.directory = media::pathFromUtf8("relative/Caf\xC3\xA9-output");
    second.source = {0, engine::OfflineTap::PostFader}; // NI Main is always post-fader.
    second.niStems = NiStems{{{{7, engine::OfflineTap::PreFader}, "one", "#A0B1C2"},
                              {{9, engine::OfflineTap::PostFader}, "two", ""}}, 192000};
    second.encoding.ditherSeed = UINT64_MAX - 1;
    const std::vector<Job> jobs{first, second};
    const bool savedJobs = saveSavedJobs(store, jobs, error);
    check(savedJobs && error.empty(), "save complete ordered supported jobs");
    if (!savedJobs) throw std::runtime_error(error);
    const auto raw = fixture.raw();
    const auto loaded = loadSavedJobs(store, error);
    check(loaded && loaded->present && loaded->jobs.size() == 2, "load complete list");
    if (loaded && loaded->jobs.size() == 2) {
        check(jobToJson(loaded->jobs[0]) == jobToJson(first) && jobToJson(loaded->jobs[1]) == jobToJson(second),
              "exact ordered round trip including int64, uint64, native paths and NI fields");
    }
    check(fixture.raw() == raw && digestProject(store).text == digest && journal(store) == history,
          "save/load leaves authored model and journal unchanged");
    check(store.db().execAndGet("SELECT value FROM session_state WHERE key='unrelated'").getString() == "preserve me",
          "unrelated Layer 3 key unchanged");
    Queue queue;
    check(queue.snapshot().empty() && !fs::exists(first.directory), "save/load has no queue or output side effect");
    const auto path = store.path();
    check(store.close(), "close real saved project"); fixture.store.reset();
    StoreError openError{}; fixture.store = Store::open(path, openError);
    check(fixture.store && openError == StoreError::Ok, "reopen real saved project");
    if (!fixture.store) throw std::runtime_error("reopen fixture");
    const auto reopened = loadSavedJobs(*fixture.store, error);
    check(reopened && reopened->jobs.size() == 2 && fixture.raw() == raw, "saved bytes persist across close/reopen");
    const auto copyPath = fixture.temp.path() / "Copied.adi";
    const auto saved = fixture.store->saveAs(copyPath);
    check(saved.ok && fixture.store->path() == fs::absolute(copyPath), "actual Save As changes owning Store path");
    const auto moved = loadSavedJobs(*fixture.store, error);
    first.project = fixture.store->path(); second.project = fixture.store->path();
    check(moved && moved->jobs.size() == 2 && jobToJson(moved->jobs[0]) == jobToJson(first) &&
          jobToJson(moved->jobs[1]) == jobToJson(second), "Save As load changes only owner path, no source/destination relocation");
    check(fixture.raw() == raw, "load does not rewrite original owner receipt");
    check(moved && saveSavedJobs(*fixture.store, moved->jobs, error), "explicit resave accepts current-owner jobs");
    check(Json::parse(fixture.raw()).at("ownerProject") == Json::parse(jobToJson(first)).at("project"),
          "resave writes current owner path");
    const auto after = fixture.raw();
    check(!saveSavedJobs(*fixture.store, jobs, error) && fixture.raw() == after, "stale pre-Save As jobs refuse without rebinding source IDs");
    check(saveSavedJobs(*fixture.store, {}, error), "save explicit empty list");
    const auto empty = loadSavedJobs(*fixture.store, error);
    check(empty && empty->present && empty->jobs.empty(), "explicit empty differs from absent");
}
void invalidRowsAndInputs() {
    Fixture fixture; std::string error;
    const std::vector<Job> jobs{fixture.job()};
    check(saveSavedJobs(*fixture.store, jobs, error), "seed row for refusal cases");
    const auto good = fixture.raw(); const auto valid = Json::parse(good);
    const auto refusedRow = [&](const std::string& bad) {
        fixture.raw(bad);
        const auto result = loadSavedJobs(*fixture.store, error);
        check(!result && !error.empty() && fixture.raw() == bad, "invalid row load refuses wholly and preserves bytes");
        check(!saveSavedJobs(*fixture.store, {}, error) && !error.empty() && fixture.raw() == bad,
              "invalid row refuses even empty overwrite");
    };
    refusedRow("not JSON");
    refusedRow(good + std::string(1, '\0') + "trailing junk");
    auto bad = valid; bad["version"] = 2; refusedRow(bad.dump());
    bad = valid; bad["version"] = 1.0; refusedRow(bad.dump());
    bad = valid; bad["version"] = UINT64_MAX; refusedRow(bad.dump());
    bad = valid; bad["format"] = "other"; refusedRow(bad.dump());
    bad = valid; bad["extra"] = true; refusedRow(bad.dump());
    bad = valid; bad.erase("ownerProject"); refusedRow(bad.dump());
    bad = valid; bad["ownerProject"] = "foreign.adi"; refusedRow(bad.dump());
    bad = valid; bad["jobs"] = Json::object(); refusedRow(bad.dump());
    bad = valid; bad["jobs"].push_back(valid.at("jobs")[0]); bad["jobs"][1]["source"]["tap"] = "bad"; refusedRow(bad.dump());
    bad = valid; bad["jobs"][0]["version"] = 2; refusedRow(bad.dump());
    bad = valid; bad["jobs"][0]["extra"] = true; refusedRow(bad.dump());
    refusedRow("{\"version\":1," + good.substr(1));
    auto duplicate = good;
    const auto at = duplicate.find("\"beginSample\":");
    duplicate.insert(at, "\"beginSample\":137,"); refusedRow(duplicate);
    bad = valid; bad["jobs"] = Json::array();
    for (std::size_t i = 0; i <= kMaximumSavedJobs; ++i) bad["jobs"].push_back(valid.at("jobs")[0]);
    refusedRow(bad.dump());
    refusedRow(good + std::string(kMaximumSavedJobsBytes, ' '));
    bad = valid; bad["jobs"][0]["directory"] = std::string(kMaximumJobJsonBytes, 'a'); refusedRow(bad.dump());
    fixture.raw(good + std::string(kMaximumSavedJobsBytes - good.size(), ' '));
    const auto atByteLimit = loadSavedJobs(*fixture.store, error);
    check(atByteLimit && atByteLimit->jobs.size() == 1, "exact envelope byte bound loads without an off-by-one refusal");
    check(saveSavedJobs(*fixture.store, jobs, error), "valid maximum-size existing row can be explicitly replaced");
    fixture.raw(good);
    fixture.store->db().exec("UPDATE session_state SET value=NULL WHERE key='export.savedJobs'");
    check(!loadSavedJobs(*fixture.store, error) && !saveSavedJobs(*fixture.store, {}, error), "NULL row refuses without treating it as absent");
    check(fixture.store->db().execAndGet("SELECT value IS NULL FROM session_state WHERE key='export.savedJobs'").getInt() == 1,
          "NULL row is preserved");
    fixture.raw(good);
    const auto refusedInput = [&](const std::vector<Job>& input) {
        check(!saveSavedJobs(*fixture.store, input, error) && !error.empty() && fixture.raw() == good,
              "invalid incoming list preserves previous complete row");
    };
    auto other = jobs; other[0].project = fixture.temp.path() / "other.adi"; refusedInput(other);
    other = jobs; other.push_back(jobs[0]); other.back().endSample = 0; refusedInput(other);
    other = jobs; other[0].session.externalTempoBpm = 120; refusedInput(other);
    other = jobs; other[0].source.tap = static_cast<engine::OfflineTap>(99); refusedInput(other);
    refusedInput(std::vector<Job>(kMaximumSavedJobs + 1, jobs[0]));
    other.assign(5, jobs[0]);
    for (auto& job : other) job.directory = std::string(900000, 'a');
    refusedInput(other);
    std::vector<Job> maximum(kMaximumSavedJobs, jobs[0]);
    check(saveSavedJobs(*fixture.store, maximum, error), "count bound itself is accepted");
    const auto loaded = loadSavedJobs(*fixture.store, error);
    check(loaded && loaded->jobs.size() == kMaximumSavedJobs, "maximum count loads entirely");
}
void writeFailures() {
    Fixture fixture; std::string error;
    const std::vector<Job> jobs{fixture.job()};
    check(saveSavedJobs(*fixture.store, jobs, error), "seed transactional tests");
    const auto before = fixture.raw(); const auto history = journal(*fixture.store);
    fixture.store->db().exec("CREATE TEMP TRIGGER refuse_saved_jobs BEFORE UPDATE ON session_state "
                            "WHEN NEW.key='export.savedJobs' BEGIN SELECT RAISE(ABORT,'injected saved-job failure'); END;");
    check(!saveSavedJobs(*fixture.store, {}, error) && error.find("injected") != std::string::npos && fixture.raw() == before,
          "actual SQLite statement failure preserves prior row");
    check(sqlite3_get_autocommit(fixture.store->db().getHandle()) != 0 && journal(*fixture.store) == history,
          "failed save rolls back its transaction and leaves journal untouched");
    fixture.store->db().exec("DROP TRIGGER refuse_saved_jobs");
    {
        SQLite::Transaction transaction(fixture.store->db());
        check(!saveSavedJobs(*fixture.store, {}, error) && error.find("enclosing") != std::string::npos,
              "nested caller transaction is not implicitly committed or altered");
        check(sqlite3_get_autocommit(fixture.store->db().getHandle()) == 0 && fixture.raw() == before,
              "caller transaction remains active with prior saved row");
    }
    error = "untouched";
    {
        audio::CallbackScope callback;
        check(!saveSavedJobs(*fixture.store, jobs, error) && !loadSavedJobs(*fixture.store, error) && error == "untouched",
              "callback refuses before error allocation and SQL");
    }
    check(fixture.raw() == before, "callback refusal leaves saved row unchanged");
    const auto path = fixture.store->path();
    check(fixture.store->close(), "close writable fixture before readonly open"); fixture.store.reset();
    StoreError openError{}; fixture.store = Store::open(path, openError, true);
    check(fixture.store && openError == StoreError::Ok && fixture.store->readOnly(), "open readonly project");
    if (!fixture.store) throw std::runtime_error("readonly fixture");
    const auto loaded = loadSavedJobs(*fixture.store, error);
    check(loaded && loaded->jobs.size() == 1, "readonly load succeeds");
    check(!saveSavedJobs(*fixture.store, {}, error) && error.find("read-only") != std::string::npos && fixture.raw() == before,
          "readonly save refuses without touching prior row");
}
void relativeOwner() {
    Fixture fixture; std::string error;
    const std::vector<Job> jobs{fixture.job()};
    check(saveSavedJobs(*fixture.store, jobs, error), "seed absolute owner before relative reopening");
    const auto path = fixture.store->path(); const auto before = fixture.raw();
    check(fixture.store->close(), "close before relative owner test"); fixture.store.reset();
    struct CurrentDirectory {
        fs::path previous = fs::current_path();
        ~CurrentDirectory() { std::error_code ignored; fs::current_path(previous, ignored); }
    } restore;
    fs::current_path(path.parent_path());
    StoreError openError{}; fixture.store = Store::open(path.filename(), openError);
    check(fixture.store && !fixture.store->path().is_absolute(), "real Store can retain a relative caller path");
    if (!fixture.store) throw std::runtime_error("relative fixture");
    check(!loadSavedJobs(*fixture.store, error) && error.find("absolute") != std::string::npos && fixture.raw() == before,
          "relative owner load refuses to guess after possible working-directory changes");
    check(!saveSavedJobs(*fixture.store, {}, error) && error.find("absolute") != std::string::npos && fixture.raw() == before,
          "relative owner save preserves prior state");
    check(fixture.store->close(), "close relative Store while original working directory is still selected");
    fixture.store.reset();
}
} // namespace
int main() {
    try { lifecycle(); invalidRowsAndInputs(); writeFailures(); relativeOwner(); }
    catch (const std::exception& error) { check(false, error.what()); }
    std::printf("project_render_jobs: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
