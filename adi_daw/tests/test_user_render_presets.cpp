// SPDX-License-Identifier: GPL-3.0-or-later
#include "temp_directory.hpp"
#include "adi/render/user_presets.hpp"
#include "adi/render/job_json.hpp"
#include "adi/render/job_preset.hpp"
#include "adi/appdata.hpp"
#include "adi/audio/io_audit.hpp"
#include <nlohmann/json.hpp>
#include <cstdio>
#include <fstream>
#include <iterator>
#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace {
using namespace adi;
using namespace adi::render;
using Json = nlohmann::json;
namespace fs = std::filesystem;
int checks = 0, failures = 0;
void check(bool ok, const char* why) { ++checks; if (!ok) { ++failures; std::printf("FAIL: %s\n", why); } }
void write(const fs::path& path, const std::string& bytes) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(bytes.data(), static_cast<std::streamsize>(bytes.size())); out.close();
    if (!out) throw std::runtime_error("test write failed");
}
std::string read(const fs::path& path) {
    std::ifstream input(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}
UserPreset preset() {
    Job job; job.project = "relative/source.adi"; job.directory = "../relative/output";
    job.beginSample = 137; job.endSample = INT64_MAX - 4096;
    job.encoding.ditherSeed = UINT64_MAX;
    job.session = {2, 48000, 257, engine::Precision::Float64};
    return {"../../display name \xD7\x90 \xF0\x9F\x8E\xB5", job};
}
bool published(const media::PublishResult& result) { return result.status == media::PublishStatus::published; }
struct Fixture {
    test::TempDirectory temp{"user_render_presets", "file"};
    fs::path file = temp.path() / "settings" / "presets.json";
    UserPresets store{file};
    std::vector<UserPreset> presets{preset(), preset()};
    void seed() {
        const auto result = store.save(presets);
        if (!published(result)) throw std::runtime_error(result.error);
    }
};
std::error_code ioFailure(const fs::path&) { return std::make_error_code(std::errc::io_error); }
std::error_code renameFailure(const fs::path&, const fs::path&) { return std::make_error_code(std::errc::io_error); }
fs::path changedTarget;
std::string changedBytes;
std::error_code corruptBeforeLease(const fs::path&) { write(changedTarget, changedBytes); return {}; }
std::error_code bypassAttempt(const fs::path&) { return {}; }
void lifecycle() {
    Fixture fixture; std::string error = "old";
    const auto missing = fixture.store.load(error);
    check(missing && !missing->present && missing->presets.empty() && error.empty() && !fs::exists(fixture.file.parent_path()),
          "absent loads without creating app-data folders");
    const auto data = appdata::pathsFor(appdata::App::Daw).data;
    check(userPresetsFile() == (data.empty() ? fs::path{} : data / "ADI" / "render-presets.json"),
          "default path is fixed and ADI-specific under existing data root");
    fixture.presets[1].name = fixture.presets[0].name; // no invented uniqueness policy
    fixture.presets[1].job.directory = "another/output";
    fixture.presets[1].job.encoding.ditherSeed = UINT64_MAX - 1;
    const auto saved = fixture.store.save(fixture.presets);
    check(published(saved) && !saved.usedFallback && saved.identity && saved.durabilityConfirmed, "real exclusive first publication is durable");
    const auto loaded = UserPresets(fixture.file).load(error);
    check(loaded && loaded->present && loaded->presets.size() == 2, "new store instance loads ordered file");
    if (!loaded || loaded->presets.size() != 2) throw std::runtime_error(error);
    for (std::size_t i = 0; i < loaded->presets.size(); ++i)
        check(loaded->presets[i].name == fixture.presets[i].name && jobToJson(loaded->presets[i].job) == jobToJson(fixture.presets[i].job),
              "names and exact complete job values round trip without rebinding relative paths");
    auto target = preset().job; target.project = "different/project.adi"; target.directory = "target/output";
    target.beginSample = 789; target.endSample = 9999; target.source.trackId = 77;
    const auto before = target;
    auto savedOptions = loaded->presets[0].job; savedOptions.encoding.format = audio::WavFormat::Pcm16;
    applyJobPreset(target, savedOptions, PresetFields::OptionsAndFormat);
    check(target.directory == before.directory && target.project == before.project && target.beginSample == before.beginSample &&
          target.endSample == before.endSample && target.source.trackId == 77 && target.encoding.format == audio::WavFormat::Pcm16,
          "explicit existing mask applies options across projects without moving destination/source/bounds");
    try { applyJobPreset(target, loaded->presets[0].job, PresetFields::All); check(false, "cross-project All refuses"); }
    catch (const std::invalid_argument&) { check(true, "cross-project All refuses without inventing ID relocation"); }
    check(!fs::exists(fixture.temp.path() / "display name") && !fs::exists(fixture.temp.path() / "output"),
          "preset names and job destinations cause no extra publication/render");
    check(published(fixture.store.save({})), "real atomic replacement supports explicit empty list");
    const auto empty = fixture.store.load(error);
    check(empty && empty->present && empty->presets.empty(), "explicit empty remains distinguishable from absent");
    UserPresets noPath(fs::path{});
    check(!noPath.load(error) && !error.empty() && !published(noPath.save({})), "unavailable default/explicit path refuses instead of using cwd");
}
void invalid() {
    Fixture fixture; fixture.seed(); std::string error;
    const auto good = read(fixture.file); const auto original = Json::parse(good);
    const auto refused = [&](const std::string& bytes) {
        write(fixture.file, bytes);
        check(!fixture.store.load(error) && !error.empty() && read(fixture.file) == bytes, "invalid load is all-or-nothing and leaves file intact");
        check(!published(fixture.store.save({})) && read(fixture.file) == bytes, "invalid existing file refuses even empty replacement");
    };
    refused("not JSON"); refused(good + std::string(1, '\0') + "junk");
    auto bad = original; bad["version"] = 2; refused(bad.dump());
    bad = original; bad["version"] = 1.0; refused(bad.dump());
    bad = original; bad["version"] = UINT64_MAX; refused(bad.dump());
    bad = original; bad["app"] = "ADiJ"; refused(bad.dump());
    bad = original; bad["extra"] = true; refused(bad.dump());
    bad = original; bad.erase("format"); refused(bad.dump());
    bad = original; bad["presets"][1]["job"]["encoding"]["format"] = "bad"; refused(bad.dump());
    bad = original; bad["presets"][1]["extra"] = 1; refused(bad.dump());
    bad = original; bad["presets"][0]["name"] = std::string(kMaximumUserPresetNameBytes + 1, 'a'); refused(bad.dump());
    refused("{\"version\":1," + good.substr(1));
    auto duplicate = good; duplicate.insert(duplicate.find("\"beginSample\":"), "\"beginSample\":137,"); refused(duplicate);
    refused(good + std::string(kMaximumUserPresetsBytes, ' '));
    write(fixture.file, good + std::string(kMaximumUserPresetsBytes - good.size(), ' '));
    check(fixture.store.load(error).has_value(), "exact byte bound loads");
    check(published(fixture.store.save(fixture.presets)), "valid maximum-byte prior file can be replaced");
    const auto before = read(fixture.file);
    const auto refusedInput = [&](const std::vector<UserPreset>& input) {
        check(!published(fixture.store.save(input)) && read(fixture.file) == before, "invalid input preserves prior saved presets");
    };
    auto entries = fixture.presets; entries.back().job.endSample = 0; refusedInput(entries);
    entries = fixture.presets; entries.back().job.session.externalTempoBpm = 120; refusedInput(entries);
    entries = fixture.presets; entries.back().name = std::string(1, '\0'); refusedInput(entries);
    refusedInput(std::vector<UserPreset>(kMaximumUserPresets + 1, preset()));
    entries.assign(5, preset()); for (auto& entry : entries) entry.job.directory = std::string(900000, 'a'); refusedInput(entries);
    entries.assign(kMaximumUserPresets, preset()); entries[0].name = std::string(kMaximumUserPresetNameBytes, 'a');
    check(published(fixture.store.save(entries)), "exact name/count bounds save");
    const auto maximum = fixture.store.load(error);
    check(maximum && maximum->presets.size() == kMaximumUserPresets && maximum->presets[0].name.size() == kMaximumUserPresetNameBytes,
          "exact name/count bounds load wholly");
}
void publication() {
    Fixture fixture; fixture.seed(); const auto good = read(fixture.file);
    const auto unrelated = fixture.file.parent_path() / ".adi-render-presets-keep";
    fs::create_directory(unrelated); write(unrelated / "payload.json", "do not remove");
    media::ReplaceHooks hooks; hooks.syncFile = ioFailure;
    check(!published(fixture.store.save({}, hooks)) && read(fixture.file) == good, "pre-publication sync failure preserves old bytes");
    hooks = {}; hooks.rename = renameFailure;
    check(!published(fixture.store.save({}, hooks)) && read(fixture.file) == good, "rename failure preserves old bytes");
    check(read(unrelated / "payload.json") == "do not remove", "unrelated staging-looking directory survives failed writes");
    changedTarget = fixture.file; auto newer = Json::parse(good); newer["version"] = 99; changedBytes = newer.dump();
    hooks = {}; hooks.syncFile = corruptBeforeLease; hooks.validateTarget = bypassAttempt;
    check(!published(fixture.store.save({}, hooks)) && read(fixture.file) == changedBytes,
          "strict under-lease validation catches newly unsupported bytes and cannot be bypassed by hooks");
    write(fixture.file, good);
    std::error_code leaseError; auto lease = media::ProjectFileLease::openShared(fixture.file, leaseError);
    check(lease && !leaseError, "own real cooperating shared file lease");
    check(!published(fixture.store.save({})) && read(fixture.file) == good, "live cooperating reader prevents replacement");
    lease.reset();
    hooks = {}; hooks.syncDirectory = ioFailure;
    const auto committed = fixture.store.save({}, hooks);
    check(published(committed) && !committed.warning.empty() && !committed.durabilityConfirmed && read(fixture.file) != good,
          "post-rename durability failure reports published with warning, never rollback");
    std::string error = "untouched";
    { audio::CallbackScope callback;
      check(!fixture.store.load(error) && !published(fixture.store.save({})) && error == "untouched", "callback refuses before file I/O/error mutation"); }
#if defined(_WIN32)
    write(fixture.file, good);
    const auto handle = CreateFileW(fixture.file.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                    nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    check(handle != INVALID_HANDLE_VALUE, "open owned file for actual short-read error control");
    if (handle == INVALID_HANDLE_VALUE) throw std::runtime_error("open lock fixture");
    OVERLAPPED range{}; range.Offset = static_cast<DWORD>(good.size() - 1);
    const bool locked = LockFileEx(handle, LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY, 0, 1, 0, &range) != 0;
    check(locked, "lock last byte of existing valid file");
    if (locked) {
        check(!fixture.store.load(error) && !error.empty(), "actual tail read error is not mistaken for EOF or missing state");
        check(!published(fixture.store.save({})), "actual unreadable existing bytes refuse replacement");
        (void)UnlockFileEx(handle, 0, 1, 0, &range);
    }
    CloseHandle(handle);
    check(read(fixture.file) == good && fixture.store.load(error).has_value(), "locked-read refusal preserves exact recoverable file");
#endif
}
} // namespace
int main() {
    try { lifecycle(); invalid(); publication(); }
    catch (const std::exception& error) { check(false, error.what()); }
    std::printf("user_render_presets: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
