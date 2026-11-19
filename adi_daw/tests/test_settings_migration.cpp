// SPDX-License-Identifier: GPL-3.0-or-later
#include "adi/settings/migration.hpp"
#include "adi/settings/store.hpp"
#include "temp_directory.hpp"

#include <array>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <limits>
#include <stdexcept>

namespace {
using namespace adi::settings;
constexpr auto app = adi::appdata::App::Daw;
int checks = 0, failures = 0, calls = 0;
void check(bool condition, const char* text) {
    ++checks;
    if (!condition) { ++failures; std::printf("FAIL: %s\n", text); }
}
Value document(int schema = 1) {
    return {{"schema", schema}, {"app", "ADI"}, {"values", {{"old.volume", .375},
        {"future.key", Value::array({"keep", 42})}}}, {"writer-note", "preserve"}};
}
bool renameVolume(Value& values, std::string& why) {
    ++calls;
    if (values.contains("old.volume")) {
        if (values.contains("volume")) { why = "both volume keys exist"; return false; }
        values["volume"] = values.at("old.volume"); values.erase("old.volume");
    }
    return true;
}
bool addMissingDefault(Value& values, std::string&) {
    ++calls;
    if (!values.contains("volume")) values["volume"] = 1.;
    values["another.default"] = true; return true;
}
bool rejectedStep(Value& values, std::string& why) {
    values["partial"] = "must not escape"; why = "planted refusal"; return false;
}
bool throwingStep(Value& values, std::string&) {
    values.clear(); throw std::runtime_error("planted exception");
}
bool malformedStep(Value& values, std::string&) { values = Value::array(); return true; }
bool unknownThrow(Value&, std::string&) { throw 9; }
std::string read(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}
void write(const std::filesystem::path& path, const std::string& text) {
    std::ofstream output(path, std::ios::binary|std::ios::trunc); output << text;
    output.close(); if (!output) throw std::runtime_error("test fixture write failed");
}
void runner() {
    const std::array<MigrationStep, 2> steps{{{2, 3, addMissingDefault}, {1, 2, renameVolume}}};
    const auto source = document(); const auto before = source.dump(); calls = 0;
    const auto migrated = migrateSettingsDocument(source, app, 3, steps);
    check(migrated.status == MigrationStatus::Migrated && calls == 2, "unordered registrations run in schema order");
    check(migrated.document.at("schema") == 3 && migrated.document.at("values").at("volume") == .375,
          "two-step migration renames and retains the user's value instead of resetting it");
    check(!migrated.document.at("values").contains("old.volume") &&
          migrated.document.at("values").at("future.key") == source.at("values").at("future.key") &&
          migrated.document.at("writer-note") == "preserve" && migrated.document.at("app") == "ADI",
          "unknown values, root metadata, and application identity survive");
    check(source.dump() == before, "successful runner leaves its input untouched");
    calls = 0;
    const auto again = migrateSettingsDocument(migrated.document, app, 3, steps);
    check(again.status == MigrationStatus::Current && again.document == migrated.document && calls == 0,
          "current document is not migrated again");
    auto legacy = source; legacy.erase("schema");
    const auto legacyResult = migrateSettingsDocument(legacy, app, 3, steps);
    check(legacyResult.ok() && legacyResult.document.at("values").at("volume") == .375,
          "schema-less legacy documents start at published schema 1");

    calls = 0;
    const std::array<MigrationStep, 1> missing{{{1, 2, renameVolume}}};
    const auto gap = migrateSettingsDocument(source, app, 3, missing);
    check(gap.status == MigrationStatus::MissingStep && gap.document.is_null() && calls == 0,
          "complete route is checked before any step runs");
    const std::array<MigrationStep, 2> duplicate{{{1, 2, renameVolume}, {1, 2, addMissingDefault}}};
    check(migrateSettingsDocument(source, app, 2, duplicate).status == MigrationStatus::InvalidPlan,
          "ambiguous source schema is refused");
    for (const auto invalid : {MigrationStep{1, 3, renameVolume}, MigrationStep{1, 1, renameVolume},
            MigrationStep{2, 1, renameVolume}, MigrationStep{-1, 0, renameVolume}, MigrationStep{1, 2, nullptr},
            MigrationStep{std::numeric_limits<int>::max(), 0, renameVolume}}) {
        check(migrateSettingsDocument(source, app, 2, std::span(&invalid, 1)).status == MigrationStatus::InvalidPlan,
              "jump, cycle, backward, negative, null, or overflowing step refused");
    }
    check(migrateSettingsDocument(source, app, 0, {}).status == MigrationStatus::InvalidPlan, "invalid target refused");
    const std::array<MigrationStep, 257> tooMany{};
    check(migrateSettingsDocument(source, app, 2, tooMany).status == MigrationStatus::InvalidPlan, "registration bound enforced");
    for (const auto apply : {rejectedStep, throwingStep, malformedStep, unknownThrow}) {
        const std::array<MigrationStep, 2> failing{{{1, 2, renameVolume}, {2, 3, apply}}};
        const auto failed = migrateSettingsDocument(source, app, 3, failing);
        check(failed.status == MigrationStatus::StepFailed && failed.document.is_null() && !failed.why.empty() &&
              source.dump() == before, "late refusal, throw, or malformed output returns no partial migration");
    }
    auto collision = source; collision["values"]["volume"] = .9;
    check(migrateSettingsDocument(collision, app, 3, steps).status == MigrationStatus::StepFailed &&
          collision.at("values").at("volume") == .9, "step can refuse rename collision without losing either user value");
    auto bad = source; bad["values"] = Value::array();
    check(migrateSettingsDocument(bad, app, 3, steps).status == MigrationStatus::InvalidDocument, "malformed values refused");
    for (const auto& version : {Value(-1), Value(1.5), Value("1"), Value(nullptr)}) {
        bad = source; bad["schema"] = version;
        check(migrateSettingsDocument(bad, app, 3, steps).status == MigrationStatus::InvalidDocument, "malformed schema refused");
    }
    bad = source; bad["schema"] = std::numeric_limits<std::uint64_t>::max(); bad.erase("values");
    calls = 0;
    const auto newer = migrateSettingsDocument(bad, app, 3, steps);
    check(newer.status == MigrationStatus::NewerSchema && newer.document == bad && calls == 0,
          "huge newer schema with unfamiliar shape is preserved without running steps");
    bad["app"] = "ADiJ";
    check(migrateSettingsDocument(bad, app, 3, steps).status == MigrationStatus::ForeignApp, "foreign identity takes precedence");
    check(settingsMigrationSteps().empty() && kSettingsSchema == 1, "production schema stays 1 with no invented historical steps");
}
void storeBoundary() {
    adi::test::TempDirectory scratch("settings_migrations", "files");
    const auto file = scratch.path()/"settings.json";
    const auto backup = scratch.path()/"settings.json.bak";
    const auto old = document(0).dump(); write(file, old);
    AppSettings unavailable(app, file);
    check(unavailable.status() == LoadStatus::MigrationFailed && !unavailable.migrationError().empty() &&
          read(file) == old && !unavailable.keptAside(), "missing historical step keeps original in place and exposes diagnosis");
    std::string why;
    check(!unavailable.set("lookfeel.theme", "light", {}, why) && !unavailable.save(why) && read(file) == old,
          "failed migration cannot edit or overwrite original");
    std::filesystem::remove(file);
    check(!unavailable.save(why) && !std::filesystem::exists(file), "failed-load protection remains if destination later disappears");

    auto valid = document(); valid["values"]["lookfeel.theme"] = "dark";
    const auto original = valid.dump(); write(file, original);
    AppSettings current(app, file);
    check(current.status() == LoadStatus::Loaded && current.set("lookfeel.theme", "light", {}, why) && current.save(why),
          "current production schema remains writable through existing publisher");
    check(read(backup) == original, "backup keeps original bytes, not normalized/migrated bytes");
    const auto existingBackup = read(backup);
    write(file, old);
    check(current.set("lookfeel.theme", "dark", {}, why) && !current.save(why) &&
          read(file) == old && read(backup) == existingBackup, "save-reread refuses missing route without touching destination or backup");
    valid["values"]["hand-edit"] = "adopt"; write(file, valid.dump());
    check(current.save(why) && current.get("lookfeel.theme") == "dark" &&
          current.stored().at("hand-edit") == "adopt", "failed reread retains pending user edit for a safe later retry");

    auto future = valid; future["schema"] = 2; write(file, future.dump());
    AppSettings newer(app, file);
    check(newer.status() == LoadStatus::NewerSchema && newer.get("lookfeel.theme") == "dark" &&
          !newer.set("lookfeel.theme", "light", {}, why) && !newer.save(why) && read(file) == future.dump(),
          "newer settings remain readable and never written over");
    future["app"] = "ADiJ"; write(file, future.dump());
    AppSettings foreign(app, file);
    check(foreign.status() == LoadStatus::ForeignApp && !foreign.save(why) && read(file) == future.dump(),
          "foreign newer document stays untouched");
    auto unversioned = valid; unversioned.erase("schema"); write(file, unversioned.dump());
    AppSettings legacy(app, file);
    check(legacy.status() == LoadStatus::Loaded && legacy.save(why) &&
          Value::parse(read(file)).at("schema") == 1 && read(backup) == unversioned.dump(),
          "schema-less production document uses current schema and keeps exact backup");
}
} // namespace

int main() {
    try { runner(); storeBoundary(); }
    catch (const std::exception& error) { ++failures; std::printf("EXCEPTION: %s\n", error.what()); }
    std::printf("%s -- settings migrations: %d checks, %d failure(s)\n", failures ? "FAILED" : "PASS", checks, failures);
    return failures ? 1 : 0;
}
