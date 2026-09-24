// SPDX-License-Identifier: GPL-3.0-or-later
//
// ADR-0149: where application data lives, and the plugin capabilities
// registry that remembers a user's route per plugin ID.

#include "temp_directory.hpp"

#include "adi/appdata.hpp"
#include "adi/plugin_registry.hpp"

#include <SQLiteCpp/SQLiteCpp.h>

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>

using namespace adi;
namespace fs = std::filesystem;

namespace {

int g_failures = 0;
int g_checks = 0;

void check(bool cond, const std::string& what) {
    ++g_checks;
    if (!cond) { ++g_failures; std::printf("  FAIL  %s\n", what.c_str()); }
}
void section(const char* s) { std::printf("[%s]\n", s); }

// Tests set ADI_HOME. The one place an environment variable is written.
void setHome(const fs::path& p) {
#if defined(_WIN32)
    _wputenv_s(L"ADI_HOME", p.wstring().c_str());
#else
    setenv("ADI_HOME", p.string().c_str(), 1);
#endif
}
void clearHome() {
#if defined(_WIN32)
    _wputenv_s(L"ADI_HOME", L"");
#else
    unsetenv("ADI_HOME");
#endif
}

void testThePlatformDefaults() {
    section("ADR-0149 d1 -- each application its own settings; the suite shares its data");
    clearHome();
    const appdata::Paths daw = appdata::pathsFor(appdata::App::Daw);
    const appdata::Paths dj = appdata::pathsFor(appdata::App::DJ);
    const appdata::Paths live = appdata::pathsFor(appdata::App::Live);
    check(!daw.config.empty() && daw.config.is_absolute(), "a real, absolute config folder: " + daw.config.string());
    check(daw.config != dj.config && daw.config != live.config && dj.config != live.config,
          "three applications, three settings folders: nothing bleeds (ADR-0145 d10)");
    check(daw.data == dj.data && daw.data == live.data, "one shared data folder for the suite");
    check(daw.cache == dj.cache, "one shared cache");
    check(daw.cache != daw.data && daw.config != daw.data, "config, data and cache are three places");
    check(appdata::pluginRegistryFile() == daw.data / "plugins.sqlite", "the registry lives in shared data");
    check(appdata::libraryIndexFile() == daw.data / "library.sqlite", "and so does the library index");
    // The same number of checks on every platform: tools/test_all.sh compares
    // the total with the README, and a platform-only check makes that total
    // depend on where it ran (it did: 3,847 on Windows against 3,845).
#if defined(_WIN32)
    const std::string vendor = "ADI";
    const std::string dawName = "ADI DAW", djName = "aDiJ";
#elif defined(__APPLE__)
    const std::string vendor = "ADI";
    const std::string dawName = "ADI DAW", djName = "aDiJ";
#else
    const std::string vendor = "adi";
    const std::string dawName = "adi-daw", djName = "adij";
#endif
    check(daw.config.parent_path().filename() == vendor, "settings sit under the suite's folder: " + vendor);
    check(daw.config.filename() == dawName && dj.config.filename() == djName, "named as the platform names them");
}

void testAdiHome() {
    const adi::test::TempDirectory scratch("plugin_registry", "home");
    section("ADR-0149 -- ADI_HOME puts everything under one folder (tests, a portable install)");
    setHome(scratch.path());
    const appdata::Paths p = appdata::pathsFor(appdata::App::Live);
    check(p.config == scratch.path() / "config" / "ADI Live", "config: " + p.config.string());
    check(p.data == scratch.path() / "data" / "Shared", "data: " + p.data.string());
    check(p.cache == scratch.path() / "cache", "cache: " + p.cache.string());
    check(!fs::exists(p.config), "and nothing was created by asking");
    clearHome();
}

void testTheRegistry() {
    const adi::test::TempDirectory scratch("plugin_registry", "routes");
    section("ADR-0149 d2 -- a route the user chose, remembered per plugin ID, across opens");
    const fs::path file = scratch.path() / "Shared" / "plugins.sqlite";
    std::string err;
    {
        auto reg = PluginRegistry::open(file, err);
        check(reg != nullptr, "opened, creating the file and its folder: " + err);
        if (!reg) return;
        check(fs::exists(file), "the file exists");
        check(!reg->route("vst3", "VST3-Synth-1").has_value(), "nothing chosen yet: no proposal, which is Auto");
        check(reg->remember("vst3", "VST3-Synth-1", engine::RouteChoice::MpeMidi, 1000, err),
              "the user picks MPE over MIDI for this synth: " + err);
        const auto r = reg->route("vst3", "VST3-Synth-1");
        check(r.has_value() && *r == engine::RouteChoice::MpeMidi, "remembered");
        check(!reg->route("clap", "VST3-Synth-1").has_value(), "per format as well as per ID");
        check(reg->remember("vst3", "VST3-Other", engine::RouteChoice::Plain, 1001, err), "a second plugin");
    }
    {
        auto reg = PluginRegistry::open(file, err);
        check(reg != nullptr, "reopened: " + err);
        if (!reg) return;
        const auto r = reg->route("vst3", "VST3-Synth-1");
        check(r.has_value() && *r == engine::RouteChoice::MpeMidi, "and still remembered: across sessions");
        // Two applications of the suite at once (WAL, busy timeout).
        auto other = PluginRegistry::open(file, err);
        check(other != nullptr, "a second handle, as aDiJ beside ADI DAW: " + err);
        if (other) {
            check(other->remember("vst3", "VST3-Synth-1", engine::RouteChoice::NoteExpression, 1002, err),
                  "it changes the choice: " + err);
            const auto seen = reg->route("vst3", "VST3-Synth-1");
            check(seen.has_value() && *seen == engine::RouteChoice::NoteExpression, "and the first handle sees it");
        }
        check(reg->remember("vst3", "VST3-Synth-1", engine::RouteChoice::Auto, 1003, err), "back to Auto");
        check(!reg->route("vst3", "VST3-Synth-1").has_value(), "Auto forgets: there is nothing to propose");
        const auto other2 = reg->route("vst3", "VST3-Other");
        check(other2.has_value() && *other2 == engine::RouteChoice::Plain, "the other plugin is untouched");
    }
    try {
        SQLite::Database db(file.string(), SQLite::OPEN_READONLY);
        check(db.execAndGet("PRAGMA application_id").getInt() == PluginRegistry::kApplicationId,
              "the file carries the registry's own application id, not a project's");
    } catch (const std::exception& e) {
        check(false, std::string("reading the header: ") + e.what());
    }
}

void testForeignFilesAreRefused() {
    const adi::test::TempDirectory scratch("plugin_registry", "foreign");
    section("ADR-0149 -- a file that is not ours, or is newer, is refused and left alone");
    const fs::path project = scratch.path() / "project.sqlite";
    {
        SQLite::Database db(project.string(), SQLite::OPEN_READWRITE | SQLite::OPEN_CREATE);
        db.exec("PRAGMA application_id = 1094994225");   // a .adi's
        db.exec("PRAGMA user_version = 1004");
        db.exec("CREATE TABLE tracks(id INTEGER)");
    }
    std::string err;
    check(PluginRegistry::open(project, err) == nullptr, "a project file is refused: " + err);
    {
        SQLite::Database db(project.string(), SQLite::OPEN_READONLY);
        check(db.execAndGet("PRAGMA user_version").getInt() == 1004, "and was not touched");
    }
    // Someone else's database at version 0: only the application id tells.
    // (The project above is also refused by its version, which would hide a
    // missing id check; this one is refused by the id alone.)
    const fs::path foreign = scratch.path() / "foreign.sqlite";
    {
        SQLite::Database db(foreign.string(), SQLite::OPEN_READWRITE | SQLite::OPEN_CREATE);
        db.exec("PRAGMA application_id = 305419896");   // 0x12345678, not ours
        db.exec("CREATE TABLE theirs(x INTEGER)");
    }
    err.clear();
    check(PluginRegistry::open(foreign, err) == nullptr, "another program's database is refused by its id: " + err);
    {
        SQLite::Database db(foreign.string(), SQLite::OPEN_READONLY);
        SQLite::Statement st(db, "SELECT COUNT(*) FROM sqlite_master WHERE name = 'plugin_routes'");
        check(st.executeStep() && st.getColumn(0).getInt() == 0, "and nothing of ours was written into it");
    }
    const fs::path newer = scratch.path() / "newer.sqlite";
    {
        SQLite::Database db(newer.string(), SQLite::OPEN_READWRITE | SQLite::OPEN_CREATE);
        db.exec("PRAGMA application_id = " + std::to_string(PluginRegistry::kApplicationId));
        db.exec("PRAGMA user_version = " + std::to_string(PluginRegistry::kVersion + 1));
    }
    err.clear();
    check(PluginRegistry::open(newer, err) == nullptr, "a newer registry is refused: " + err);
    check(PluginRegistry::open(fs::path(), err) == nullptr, "and no folder at all is an error, not a crash");
}

}  // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::printf("adi_plugin_registry_tests -- application data and the capabilities registry (ADR-0149)\n\n");
    testThePlatformDefaults();
    testAdiHome();
    testTheRegistry();
    testForeignFilesAreRefused();
    std::printf("\n%s -- %d checks, %d failure(s)\n", g_failures ? "FAILED" : "PASS", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
