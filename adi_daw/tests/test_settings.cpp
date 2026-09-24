// SPDX-License-Identifier: GPL-3.0-or-later
//
// The settings store: ADR-0125 R-01 to R-07 and d1-d3, ADR-0127 d5, ADR-0145,
// ADR-0149, ADR-0152.
//
// Every check here runs on every platform, whatever the environment: the
// check count is compared with the README's, so nothing is platform-only.

#include "temp_directory.hpp"

#include "adi/audio/decode.hpp"
#include "adi/settings/bundle.hpp"
#include "adi/settings/catalogue.hpp"
#include "adi/settings/registry.hpp"
#include "adi/settings/store.hpp"

#include <miniz.h>

#include <algorithm>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <sstream>
#include <set>
#include <string>

#ifndef ADI_DOCS_DIR
#error "ADI_DOCS_DIR must name adi_daw/docs"
#endif

namespace {

namespace fs = std::filesystem;
namespace appdata = adi::appdata;
using namespace adi::settings;

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
const ChangeContext kUser{Actor::User, "", kT0};

std::string readText(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}
void writeText(const fs::path& p, const std::string& s) {
    fs::create_directories(p.parent_path());
    std::ofstream(p, std::ios::binary | std::ios::trunc) << s;
}

// --- the registry ---------------------------------------------------------------------------

void testRegistry() {
    section("the registry: typed, unique, defaults legal, every project setting has its op");
    std::set<std::string> keys;
    bool unique = true, defaultsLegal = true, pageKnown = true;
    for (const auto& s : registry()) {
        unique = unique && keys.insert(s.key).second;
        std::string why;
        defaultsLegal = defaultsLegal && validate(s, s.defaultValue, why);
        pageKnown = pageKnown && !s.page.empty() && !s.label.empty() && !s.help.empty();
    }
    check(registry().size() >= 30, "at least thirty settings, saw " + std::to_string(registry().size()));
    check(unique, "every key is unique");
    check(defaultsLegal, "every default is a legal value of its own setting");
    check(pageKnown, "every setting has a page, a label and help text (R-01, R-02)");
    check(pages().front() == "Look & Feel" && pages().back() == "Project",
          "pages are listed in window order");

    // Project settings are rows changed by ops (R-03): each names one the
    // catalogue actually lists.
    const std::string ops = readText(fs::path(ADI_DOCS_DIR) / "OPS.md");
    bool opsNamed = true;
    int projectCount = 0;
    for (const auto& s : registry()) {
        if (s.scope != Scope::Project) continue;
        ++projectCount;
        opsNamed = opsNamed && !s.op.empty() && ops.find("| `" + s.op + "` |") != std::string::npos;
    }
    check(projectCount >= 3 && opsNamed,
          "every Project setting names an op catalogued in OPS.md 9");

    // ADR-0145's settings.
    const auto* zoom = find("lookfeel.zoomOnSelection");
    check(zoom && zoom->type == Type::Bool && zoom->defaultValue == false && zoom->scope == Scope::App &&
              zoom->page == "Look & Feel",
          "Zoom on Selection: an [App] switch under Look & Feel, off (ADR-0145 d2)");
    const auto* resample = find("audio.silentResampling");
    check(resample && resample->type == Type::Bool && resample->page == "Audio" &&
              resample->defaultValue == false,
          "silent resampling: an [App] switch under Audio, off (ADR-0145 d3)");
    const auto* buffer = find("audio.bufferSize");
    const std::vector<Value> sizes = {64, 128, 256, 512, 1024, 2048, 4096};
    check(buffer && buffer->choices == sizes,
          "buffer sizes are exactly 64 to 4096 (ADR-0145 d5)");
    std::string why;
    check(buffer && !validate(*buffer, 32, why), "and 32 is refused: " + why);
    const auto* driver = find("audio.driverType");
    bool noRawAsio = driver != nullptr;
    if (driver)
        for (const auto& c : driver->choices)
            noRawAsio = noRawAsio && (c == "ASIO" || c.get<std::string>().find("ASIO") == std::string::npos);
    check(noRawAsio && driver->help.find("JUCE") != std::string::npos,
          "ASIO is one choice, opened through JUCE; no raw bypass is offered (ADR-0145 d5)");
    const auto* backend = find("audio.linuxBackend");
    check(backend && backend->choices == std::vector<Value>{"ALSA", "PipeWire/JACK"} &&
              find("audio.jackTransportSync") && find("plugins.lv2Folders") &&
              find("plugins.clapFolders"),
          "Linux: the backend, JACK transport sync, CLAP and LV2 folders (ADR-0145 d4)");

    // R-02: the Find box searches names and help text.
    const auto hits = search("zoom selection");
    check(hits.size() == 1 && hits[0]->key == "lookfeel.zoomOnSelection", "Find: 'zoom selection'");
    const auto resampleHits = search("RESAMPLE");
    check(std::any_of(resampleHits.begin(), resampleHits.end(),
                      [](const Setting* s) { return s->key == "audio.silentResampling"; }),
          "Find is case-insensitive and reads help text");
    check(search("no such words anywhere").empty() && search("").empty(),
          "nothing matches nonsense, or an empty query");
}

// --- the file -------------------------------------------------------------------------------

void testStoreBasics() {
    section("the store: defaults, set, save, reload, refusals");
    adi::test::TempDirectory temp("settings", "basics");
    const auto file = temp.path() / "daw" / "settings.json";
    {
        AppSettings s(appdata::App::Daw, file);
        check(s.status() == LoadStatus::Defaults, "no file: defaults");
        check(s.get("audio.bufferSize") == 256, "the default buffer is 256");
        std::string why;
        check(s.set("audio.bufferSize", 128, kUser, why), "set 128");
        check(!s.set("audio.bufferSize", 32, kUser, why), "32 is refused: " + why);
        check(!s.set("project.sampleRate", 44100, kUser, why) &&
                  why.find("project.setSampleRate") != std::string::npos,
              "a Project setting is refused, naming its op: " + why);
        check(!s.set("no.such", 1, kUser, why), "an unknown key from a caller is refused");
        check(s.save(why), "saved: " + why);
    }
    AppSettings again(appdata::App::Daw, file);
    check(again.status() == LoadStatus::Loaded && again.get("audio.bufferSize") == 128,
          "reloaded: 128");
    const auto doc = Value::parse(readText(file));
    check(doc.at("schema") == kSettingsSchema && doc.at("app") == "ADI DAW",
          "the file carries its schema and its application");

    const auto d = AppSettings::forApp(appdata::App::Daw).file();
    const auto l = AppSettings::forApp(appdata::App::Live).file();
    const auto j = AppSettings::forApp(appdata::App::DJ).file();
    check((d.empty() && l.empty() && j.empty()) ||
              (d != l && l != j && d != j && d.filename() == "settings.json"),
          "each application has its own file under its own config folder (ADR-0149)");
}

void testUnknownKeysSurvive() {
    section("a key from a newer build survives a save");
    adi::test::TempDirectory temp("settings", "unknown");
    const auto file = temp.path() / "settings.json";
    writeText(file, R"({"schema": 1, "app": "ADI DAW", "values": {
        "audio.bufferSize": 512, "future.meterBallistics": {"attack": 3, "release": 300}}})");
    AppSettings s(appdata::App::Daw, file);
    check(s.get("audio.bufferSize") == 512, "the known key is read");
    check(s.get("future.meterBallistics").is_null(), "the unknown key has no meaning here");
    std::string why;
    check(s.set("lookfeel.theme", "light", kUser, why) && s.save(why), "change something else, save");
    const auto doc = Value::parse(readText(file));
    const Value kept = doc.contains("values") && doc["values"].contains("future.meterBallistics")
                           ? doc["values"]["future.meterBallistics"]
                           : Value();
    check(kept == Value({{"attack", 3}, {"release", 300}}),
          "and the newer build's key is still there, exactly");

    writeText(file, R"({"schema": 3, "app": "ADI DAW", "values": {"lookfeel.theme": "light"}})");
    AppSettings newer(appdata::App::Daw, file);
    check(newer.status() == LoadStatus::NewerSchema && newer.get("lookfeel.theme") == "light",
          "a newer schema is read");
    check(newer.save(why) && Value::parse(readText(file)).at("schema") == 3,
          "and saved without downgrading its schema number");
}

void testCorruptKeptAside() {
    section("a corrupt file is kept aside, never overwritten silently");
    adi::test::TempDirectory temp("settings", "corrupt");
    const auto file = temp.path() / "settings.json";
    const std::string broken = R"({"schema": 1, "values": {"audio.bufferSize": 128,)";
    writeText(file, broken);
    AppSettings s(appdata::App::Daw, file);
    check(s.status() == LoadStatus::CorruptKept, "reported as corrupt");
    check(s.get("audio.bufferSize") == 256, "and defaults are in use");
    check(s.keptAside() && fs::exists(*s.keptAside()) && readText(*s.keptAside()) == broken,
          "the broken file is kept, byte for byte, beside it");
    check(!fs::exists(file), "and nothing has been written in its place yet");
    std::string why;
    check(s.save(why) && fs::exists(file) && readText(*s.keptAside()) == broken,
          "a later save writes a fresh file and leaves the kept one alone");
    writeText(file, "not json");
    AppSettings second(appdata::App::Daw, file);
    check(second.keptAside() && *second.keptAside() != *s.keptAside(),
          "a second corrupt file is kept under a new name, not over the first");
}

void testApplicationsNeverShare() {
    section("ADI DAW, ADI Live and aDiJ never read each other's settings (ADR-0145 d10)");
    adi::test::TempDirectory temp("settings", "apps");
    const auto file = temp.path() / "settings.json";
    {
        AppSettings daw(appdata::App::Daw, file);
        std::string why;
        daw.set("lookfeel.theme", "light", kUser, why);
        daw.save(why);
    }
    const auto before = readText(file);
    AppSettings live(appdata::App::Live, file);
    check(live.status() == LoadStatus::ForeignApp, "ADI Live pointed at ADI DAW's file refuses it");
    check(live.get("lookfeel.theme") == "os", "and reads none of its values");
    std::string why;
    check(!live.save(why) && readText(file) == before,
          "and never overwrites it: " + why);
}

void testChangeLog() {
    section("the settings-change log (ADR-0125 d3)");
    adi::test::TempDirectory temp("settings", "log");
    AppSettings s(appdata::App::Daw, temp.path() / "settings.json");
    std::string why;
    s.set("lookfeel.theme", "light", kUser, why);
    s.set("lookfeel.theme", "light", kUser, why);   // no change: not logged
    const auto lines = [&] {
        std::vector<Value> out;
        std::ifstream in(s.changeLogFile());
        for (std::string line; std::getline(in, line);) out.push_back(Value::parse(line));
        return out;
    };
    auto l = lines();
    check(l.size() == 1 && l[0].at("actor") == "user" && l[0].at("key") == "lookfeel.theme" &&
              l[0].at("before") == "os" && l[0].at("after") == "light" && l[0].at("time") == kT0,
          "a person's change: time, key, before, after");
    s.setLogUserChanges(false);
    s.set("lookfeel.theme", "dark", kUser, why);
    check(lines().size() == 1, "a person's change can be left out of the log");
    check(agentSet(s, AgentTier::Apply, "lookfeel.theme", "light", "model-x 1.0", kT0 + 1).ok,
          "an agent change");
    l = lines();
    check(l.size() == 2 && l[1].at("actor") == "agent" && l[1].at("detail") == "model-x 1.0",
          "an agent's change is always logged, with the model");
}

// --- the agent's pipeline -----------------------------------------------------------------

void testAgentWhitelist() {
    section("the agent's pipeline: a whitelist, Apply tier, never the audio device (ADR-0125 d1-d2)");
    adi::test::TempDirectory temp("settings", "agent");
    AppSettings s(appdata::App::Daw, temp.path() / "settings.json");
    check(!agentSet(s, AgentTier::Propose, "lookfeel.theme", "light", "m", kT0).ok,
          "at Propose tier it changes nothing");
    check(agentSet(s, AgentTier::Apply, "lookfeel.zoomOnSelection", true, "m", kT0).ok &&
              s.get("lookfeel.zoomOnSelection") == true,
          "at Apply it may change Zoom on Selection");
    for (const char* key : {"audio.bufferSize", "audio.driverType", "audio.silentResampling",
                            "library.userLibrary", "plugins.vst3Folders", "privacy.crashReports",
                            "agent.tier", "lookfeel.language", "project.sampleRate", "no.such"}) {
        const Value v = key == std::string("audio.bufferSize") ? Value(128) : Value(true);
        const auto r = agentSet(s, AgentTier::Apply, key, v, "m", kT0);
        check(!r.ok, std::string("refused: ") + key);
    }
    check(s.get("audio.bufferSize") == 256 && s.get("agent.tier") == "propose",
          "and none of them changed");
    bool consistent = true;
    int whitelisted = 0;
    for (const auto& st : registry()) {
        if (!st.agentMayChange) continue;
        ++whitelisted;
        consistent = consistent && !agentForbidden(st);
    }
    check(whitelisted >= 5 && consistent,
          "no whitelisted setting is a path, audio, plug-in, privacy or AI setting");
}

// --- presets --------------------------------------------------------------------------------

void testPartialPreset() {
    section("presets, whole and partial: applying one changes its pages only (R-04)");
    adi::test::TempDirectory temp("settings", "preset");
    AppSettings s(appdata::App::Daw, temp.path() / "settings.json");
    std::string why;
    s.set("lookfeel.theme", "light", kUser, why);
    s.set("lookfeel.tooltipDelayMs", 700, kUser, why);
    const Preset look = makePreset(s, "My look", {"Look & Feel"});
    check(look.values.contains("lookfeel.theme") && !look.values.contains("audio.bufferSize"),
          "a Look & Feel preset holds that page's settings only");

    s.set("lookfeel.theme", "dark", kUser, why);
    s.set("lookfeel.tooltipDelayMs", 500, kUser, why);
    s.set("audio.bufferSize", 1024, kUser, why);
    const auto r = applyPreset(s, look, kUser);
    check(s.get("lookfeel.theme") == "light" && s.get("lookfeel.tooltipDelayMs") == 700,
          "applied: its page is back");
    check(s.get("audio.bufferSize") == 1024, "and the Audio page was left alone");

    // A preset file carrying a key from a page it does not hold.
    Preset forged = look;
    forged.values["audio.bufferSize"] = 64;
    s.set("lookfeel.theme", "dark", kUser, why);
    const auto r2 = applyPreset(s, forged, kUser);
    check(s.get("audio.bufferSize") == 1024 && s.get("lookfeel.theme") == "light",
          "a key from a page the preset does not hold is not applied");
    check(std::find(r2.skipped.begin(), r2.skipped.end(), "audio.bufferSize") != r2.skipped.end(),
          "and is reported as skipped");

    const auto back = presetFromJson(toJson(look), why);
    check(back && back->name == "My look" && back->pages == look.pages && back->values == look.values,
          "a preset round-trips through JSON");
    const Preset whole = makePreset(s, "Everything", pages());
    check(whole.values.contains("audio.bufferSize") && whole.values.contains("lookfeel.theme") &&
              !whole.values.contains("project.sampleRate"),
          "a whole preset holds every App setting, and no Project one");
    (void)r;
}

// --- bundles --------------------------------------------------------------------------------

Value readZipJson(const fs::path& zipFile, const char* name) {
    const std::string bytes = readText(zipFile);
    mz_zip_archive zip{};
    if (!mz_zip_reader_init_mem(&zip, bytes.data(), bytes.size(), 0)) return nullptr;
    std::size_t size = 0;
    void* data = mz_zip_reader_extract_file_to_heap(&zip, name, &size, 0);
    Value out = nullptr;
    if (data) {
        out = Value::parse(std::string(static_cast<const char*>(data), size), nullptr, false);
        mz_free(data);
    }
    mz_zip_reader_end(&zip);
    return out;
}

void testBundles() {
    section("settings bundles: one .zip, paths as roles, resolved again on import (R-05)");
    adi::test::TempDirectory temp("settings", "bundle");
    AppSettings from(appdata::App::Daw, temp.path() / "a" / "settings.json");
    std::string why;
    from.set("lookfeel.theme", "light", kUser, why);
    from.set("library.userLibrary", "/home/ana/Music/ADI", kUser, why);
    from.set("library.contentFolders", Value::array({"/data/samples", "/data/loops"}), kUser, why);
    from.set("library.recordFolder", "/home/ana/Music/ADI/Recordings", kUser, why);
    from.set("plugins.vst3Folders", Value::array({"/opt/vst3"}), kUser, why);
    const Preset look = makePreset(from, "Light", {"Look & Feel"});

    const auto zip = temp.path() / "settings-bundle.zip";
    const auto rep = exportBundle(from, {look}, zip);
    check(rep.ok, "exported: " + rep.why);
    const Value settingsDoc = readZipJson(zip, "settings.json");
    const Value manifest = readZipJson(zip, "manifest.json");
    // What the bundle holds, read without trusting its shape: a check below
    // that fails must fail as a check, not as an exception.
    const auto value = [&](const char* key) {
        return settingsDoc.is_object() && settingsDoc.contains("values") &&
                       settingsDoc["values"].contains(key)
                   ? settingsDoc["values"][key]
                   : Value();
    };
    check(manifest.is_object() && manifest.value("kind", "") == "adi-settings-bundle" &&
              manifest.value("app", "") == "ADI DAW",
          "a manifest names the kind and the application");
    check(!settingsDoc.is_null() && !containsAbsolutePath(settingsDoc),
          "no absolute path anywhere in the bundle's settings");
    check(value("library.recordFolder") == "role:user library/Recordings",
          "a folder under the user library travels as that role");
    check(value("library.contentFolders") ==
              Value::array({"role:content folder 1", "role:content folder 2"}),
          "content folders travel as 'content folder N'");
    check(std::find(rep.leftOut.begin(), rep.leftOut.end(), "plugins.vst3Folders: /opt/vst3") !=
              rep.leftOut.end(),
          "a path under no role is left out and reported, not shipped");
    check(!containsAbsolutePath(readZipJson(zip, "presets/1.json")), "and none in the preset");

    // Another machine, other folders.
    AppSettings to(appdata::App::Daw, temp.path() / "b" / "settings.json");
    to.set("library.userLibrary", "/mnt/work/ADI", kUser, why);
    to.set("library.contentFolders", Value::array({"/mnt/s"}), kUser, why);
    const auto in = importBundle(zip, to, kUser);
    check(in.ok, "imported: " + in.why);
    check(to.get("lookfeel.theme") == "light", "the theme came across");
    check(to.get("library.recordFolder") == "/mnt/work/ADI/Recordings",
          "the record folder resolved against THIS machine's user library");
    check(to.get("library.userLibrary") == "/mnt/work/ADI",
          "and this machine's own user library is its own");
    check(to.get("library.contentFolders") == Value::array({"/mnt/s"}) &&
              std::find(in.unresolved.begin(), in.unresolved.end(),
                        "library.contentFolders: role:content folder 2") != in.unresolved.end(),
          "a role this machine lacks is reported, not invented");
    check(in.presets.size() == 1 && in.presets[0].name == "Light", "the preset came across");

    AppSettings live(appdata::App::Live, temp.path() / "c" / "settings.json");
    check(!importBundle(zip, live, kUser).ok, "ADI Live refuses ADI DAW's bundle");

    check(containsAbsolutePath(Value("C:\\Users\\x")) && containsAbsolutePath(Value("\\\\srv\\share")) &&
              containsAbsolutePath(Value::array({"ok", "/abs"})) && !containsAbsolutePath(Value("1/16")) &&
              !containsAbsolutePath(Value("role:user library/x")),
          "the absolute-path test knows Windows, UNC and POSIX, and not roles or '1/16'");
}


// --- the catalogue (ADR-0156) ---------------------------------------------------------------

struct CatalogueRow {
    std::string name;     // "<section> / <setting up to its first ' ('>"
    std::string status;   // DECIDED, DIRECTION, REJECTED, NOTE, BACKLOG, WISH ...
};

std::string trim(std::string s) {
    while (!s.empty() && (s.back() == ' ' || s.back() == '\r' || s.back() == '|')) s.pop_back();
    std::size_t i = 0;
    while (i < s.size() && (s[i] == ' ' || s[i] == '|')) ++i;
    return s.substr(i);
}

/// docs/SETTINGS-CATALOGUE.md's rows, read as the file is generated: one
/// table per "## Part / section" heading, three cells a row.
std::vector<CatalogueRow> readCatalogue() {
    std::istringstream in(readText(fs::path(ADI_DOCS_DIR) / "SETTINGS-CATALOGUE.md"));
    std::vector<CatalogueRow> rows;
    std::string line, section;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();   // a CRLF checkout
        if (line.rfind("## ", 0) == 0) {
            const auto slash = line.rfind(" / ");
            section = line.substr(slash == std::string::npos ? 3 : slash + 3);
            continue;
        }
        if (line.rfind("|", 0) != 0 || line.rfind("|---", 0) == 0 || line.rfind("| Setting |", 0) == 0) continue;
        const auto first = line.find(" | ");
        const auto last = line.rfind(" | ");
        if (first == std::string::npos || last == first) continue;
        std::string cell = trim(line.substr(0, first));
        const auto paren = cell.find(" (");
        if (paren != std::string::npos) cell.resize(paren);
        rows.push_back({section + " / " + cell, trim(line.substr(last + 3))});
    }
    return rows;
}

void testCatalogue() {
    section("the catalogue: every row a setting, or deliberately absent with a reason (ADR-0156)");
    const auto rows = readCatalogue();
    check(rows.size() == 207, "the catalogue has the Settings Reference's 207 rows, read " + std::to_string(rows.size()));

    std::map<std::string, int> inAnswered, inAbsent;
    for (const auto& a : answeredRows()) ++inAnswered[a.row];
    for (const auto& a : absentRows()) ++inAbsent[a.row];
    std::set<std::string> names;
    for (const auto& r : rows) names.insert(r.name);
    check(names.size() == rows.size(), "row names are unique: section and setting");

    std::string strays;
    for (const auto& r : rows) {
        const int n = (inAnswered.count(r.name) ? inAnswered[r.name] : 0) + (inAbsent.count(r.name) ? inAbsent[r.name] : 0);
        if (n != 1) strays += "\n        " + std::to_string(n) + "x " + r.name;
    }
    check(strays.empty(), "every catalogue row is in exactly one list, answered or absent:" + strays);
    std::string unknown;
    for (const auto& [name, n] : inAnswered) if (!names.count(name)) unknown += "\n        " + name;
    for (const auto& [name, n] : inAbsent) if (!names.count(name)) unknown += "\n        " + name;
    check(unknown.empty(), "every listed row is a row of the catalogue:" + unknown);
    check(answeredRows().size() + absentRows().size() == rows.size(),
          "answered " + std::to_string(answeredRows().size()) + " + absent " + std::to_string(absentRows().size()) + " = 207");

    std::string missingKeys;
    bool nonEmpty = true;
    for (const auto& a : answeredRows()) {
        nonEmpty = nonEmpty && !a.keys.empty();
        for (const auto& k : a.keys) if (!find(k)) missingKeys += "\n        " + k + " (" + a.row + ")";
    }
    check(nonEmpty && missingKeys.empty(), "every answered row names registry keys, and each exists:" + missingKeys);

    // The status decides which list a row may be in.
    std::map<std::string, std::string> status;
    for (const auto& r : rows) status[r.name] = r.status;
    auto word = [](const std::string& st) { return st.substr(0, st.find(' ')); };
    std::string misplaced;
    for (const auto& a : answeredRows()) {
        const auto w = word(status[a.row]);
        if (w != "DECIDED" && w != "DIRECTION") misplaced += "\n        answered but " + status[a.row] + ": " + a.row;
    }
    for (const auto& a : absentRows()) {
        const auto w = word(status[a.row]);
        const bool ok = a.kind == Absence::Rejected ? w == "REJECTED"
                      : a.kind == Absence::Note     ? w == "NOTE"
                      : a.kind == Absence::Backlog  ? w == "BACKLOG"
                      : a.kind == Absence::Wish     ? w == "WISH"
                      : (w == "DECIDED" || w == "DIRECTION");
        if (!ok || a.reason.empty()) misplaced += "\n        " + std::string(toString(a.kind)) + " but " + status[a.row] + ": " + a.row;
    }
    check(misplaced.empty(), "REJECTED, NOTE, BACKLOG and WISH rows are absent with their kind; only decided rows are 'not a setting'; every absence has a reason:" + misplaced);
    std::string answeredButNot;
    for (const auto& r : rows) {
        const auto w = word(r.status);
        if ((w == "REJECTED" || w == "NOTE" || w == "BACKLOG" || w == "WISH") && inAnswered.count(r.name))
            answeredButNot += "\n        " + r.name;
    }
    check(answeredButNot.empty(), "no REJECTED, NOTE, BACKLOG or WISH row is answered by a setting:" + answeredButNot);

    // ADR-0157: the ladder, and nothing below 44.1 kHz.
    const std::vector<Value> ladder = {44100, 48000, 88200, 96000, 176400, 192000, 352800, 384000, 705600, 768000};
    check(sampleRateLadder() == ladder, "the sample-rate ladder is 44.1/48, 88.2/96, 176.4/192, 352.8/384, 705.6/768 kHz");
    const auto* project = find("project.sampleRate");
    const auto* device = find("audio.sampleRate");
    check(project && project->type == Type::Choice && project->choices == ladder && project->defaultValue == 48000,
          "the project's rate chooser offers the ladder, 48 kHz by default");
    check(device && device->type == Type::Choice && device->choices == ladder, "the device's rate chooser offers the ladder");
    std::string why;
    check(project && device && !validate(*project, 32000, why) && !validate(*device, 22050, why) &&
              validate(*device, 768000, why) && !validate(*device, 1536000, why),
          "nothing below 44.1 kHz and nothing above 768 kHz is offered");

    // ADR-0156: the decoder reads the Decoding Cache settings by these keys.
    const auto* maxSize = find(adi::audio::kCacheMaxSizeKey);
    const auto* minFree = find(adi::audio::kCacheMinFreeKey);
    check(maxSize && minFree && maxSize->defaultValue == adi::audio::kCacheMaxSizeMbDefault &&
              minFree->defaultValue == adi::audio::kCacheMinFreeMbDefault && maxSize->scope == Scope::App,
          "the Decoding Cache settings are the decoder's keys, with its defaults");
    Value defaults = Value::object();
    for (const auto& st : registry()) defaults[st.key] = st.defaultValue;
    const auto limits = adi::audio::limitsFromSettings(defaults);
    const adi::audio::DecodeCacheLimits builtIn;
    check(limits.maxBytes == builtIn.maxBytes && limits.minFreeBytes == builtIn.minFreeBytes,
          "the registry's defaults give the decoder its built-in limits");

    // The agent's whitelist stays what ADR-0152 marked: the catalogue adds no mark.
    int marked = 0;
    for (const auto& st : registry()) marked += st.agentMayChange ? 1 : 0;
    check(marked == 9, "the catalogue added no setting to the agent's whitelist (" + std::to_string(marked) + " marked)");
    std::printf("  %zu settings on %zu pages; %zu rows answered, %zu absent\n", registry().size(), pages().size(),
                answeredRows().size(), absentRows().size());
}

int runAll() {
    std::printf("adi_settings_tests -- ADR-0125, ADR-0127 d5, ADR-0145, ADR-0152\n\n");
    testRegistry();
    testCatalogue();
    testStoreBasics();
    testUnknownKeysSurvive();
    testCorruptKeptAside();
    testApplicationsNeverShare();
    testChangeLog();
    testAgentWhitelist();
    testPartialPreset();
    testBundles();
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
