// SPDX-License-Identifier: GPL-3.0-or-later
#include "temp_directory.hpp"
#include "adi/render/job_preset.hpp"
#include "adi/render/job_json.hpp"
#include "adi/media/media_ops.hpp"
#include <cstdio>
#include <stdexcept>

namespace {
using namespace adi;
using namespace adi::render;
int checks = 0, failures = 0;
void check(bool condition, const char* text) {
    ++checks;
    if (!condition) { ++failures; std::printf("FAIL: %s\n", text); }
}
std::string persistent(const Job& job) {
    auto value = job;
    value.session.progressiveDecoder = nullptr;
    value.session.externalTempoBpm.reset();
    return jobToJson(value);
}
Job target() {
    Job job;
    job.project = media::pathFromUtf8("projects/\xD7\x90 session.adi");
    job.directory = "output/current"; job.nameTemplate = "current-{track}";
    job.source = {7, engine::OfflineTap::PreFader};
    job.beginSample = 137; job.endSample = 192137;
    job.session = {2, 48000, 257, engine::Precision::Float32};
    job.session.performanceState = false; job.session.exporting = true; job.session.offline = true;
    job.encoding = {audio::WavFormat::Pcm16, audio::WavDither::None, 17};
    return job;
}
Job preset() {
    auto job = target();
    job.directory = media::pathFromUtf8("output/Caf\xC3\xA9"); job.nameTemplate = "preset-{counter}";
    job.beginSample = 300; job.endSample = 240300; job.source = {9, engine::OfflineTap::PostFader};
    job.session = {4, 48000, 1024, engine::Precision::Float64};
    job.session.performanceState = true; job.session.exporting = false; job.session.offline = false;
    job.session.delayCompensation = false;
    job.encoding = {audio::WavFormat::Float32, audio::WavDither::Rectangular, UINT64_MAX};
    job.normalization = {NormalizeMode::Loudness, -18.5, -2, {1, 0, 1.41, 2}};
    return job;
}
bool sourceSame(const Source& a, const Source& b) { return a.trackId == b.trackId && a.tap == b.tap; }
bool bindingsSame(const Job& a, const Job& b) {
    return a.project.native() == b.project.native() && a.session.performanceState == b.session.performanceState &&
        a.session.exporting == b.session.exporting && a.session.offline == b.session.offline &&
        a.session.progressiveDecoder == b.session.progressiveDecoder && a.session.externalTempoBpm == b.session.externalTempoBpm;
}
void acceptedGroups() {
    auto current = target(); const auto saved = preset();
    const auto savedBytes = jobToJson(saved); const auto before = current;
    applyJobPreset(current, saved, PresetFields::OptionsAndFormat);
    check(current.directory.native() == before.directory.native() && current.nameTemplate == before.nameTemplate &&
          sourceSame(current.source, before.source) && current.beginSample == before.beginSample &&
          current.endSample == before.endSample && current.regionId == before.regionId,
          "Options/Format preserves destination, naming, source and both bounds exactly");
    check(current.encoding.format == saved.encoding.format && current.encoding.dither == saved.encoding.dither &&
          current.encoding.ditherSeed == UINT64_MAX && current.session.channels == 4 && current.session.maxFrames == 1024 &&
          current.session.precision == engine::Precision::Float64 && !current.session.delayCompensation &&
          current.normalization.channelWeights == saved.normalization.channelWeights && current.normalization.targetLufs == -18.5,
          "Options/Format copies complete encoding, processing format and normalization");
    check(bindingsSame(current, before) && jobToJson(saved) == savedBytes, "target bindings and preset remain untouched");
    check(jobToJson(jobFromJson(jobToJson(current))) == jobToJson(current), "result remains a valid persistent complete job");
    current = before;
    applyJobPreset(current, saved, PresetFields::BoundsAndOutput);
    auto expected = before;
    expected.directory = saved.directory; expected.nameTemplate = saved.nameTemplate;
    expected.beginSample = saved.beginSample; expected.endSample = saved.endSample;
    expected.regionId = saved.regionId; expected.source = saved.source;
    check(jobToJson(current) == jobToJson(expected), "Bounds/Output changes only its exact field group");
    current = before;
    auto otherClock = saved; otherClock.session.sampleRate = 96000;
    applyJobPreset(current, otherClock, PresetFields::All);
    expected = otherClock; expected.project = before.project;
    expected.session.performanceState = before.session.performanceState;
    expected.session.exporting = before.session.exporting; expected.session.offline = before.session.offline;
    check(jobToJson(current) == jobToJson(expected), "All transfers explicit bounds with their sample clock and retains target execution flags");
    const auto identical = persistent(current);
    applyJobPreset(current, current, PresetFields::All);
    check(persistent(current) == identical, "self-aliasing application is safe");
    auto foreign = saved; foreign.project = "other/project.adi";
    current = before;
    applyJobPreset(current, foreign, PresetFields::OptionsAndFormat);
    check(current.project == before.project && sourceSame(current.source, before.source),
          "format-only reuse across projects never transfers project-bound source IDs");
    // The full candidate is validated, not the unselected half of a preset.
    foreign.beginSample = -1; foreign.endSample = -2; foreign.source.tap = static_cast<engine::OfflineTap>(99);
    current = before;
    applyJobPreset(current, foreign, PresetFields::OptionsAndFormat);
    check(current.beginSample == before.beginSample && sourceSame(current.source, before.source),
          "invalid ignored source/bounds cannot leak into an options-only candidate");
    auto runtimePreset = saved;
    char owner = 0, otherOwner = 0;
    current = before; current.session.progressiveDecoder = reinterpret_cast<audio::ProgressiveDecoder*>(&owner);
    runtimePreset.session.progressiveDecoder = reinterpret_cast<audio::ProgressiveDecoder*>(&otherOwner);
    runtimePreset.session.externalTempoBpm = 120;
    const auto runtimeBefore = current;
    applyJobPreset(current, runtimePreset, PresetFields::All);
    check(bindingsSame(current, runtimeBefore), "All preserves owner decoder and does not import a preset's live tempo or flags");
    Queue queue; queue.add(current);
    check(bindingsSame(queue.snapshot()[0].job, runtimeBefore), "masked result reaches actual Queue admission with owner bindings retained");
    test::TempDirectory folder{"render_job_preset", "pure"};
    current = target(); current.project = folder.path()/"missing.adi"; current.directory = folder.path()/"out";
    auto local = preset(); local.project = current.project;
    applyJobPreset(current, local, PresetFields::All);
    check(std::filesystem::is_empty(folder.path()), "application performs no project reads, storage or rendering");
}
template<class Action>
void refusal(Job& current, Action action, const char* reason) {
    const auto before = persistent(current); const auto bindings = current;
    try { action(); check(false, reason); }
    catch (const std::invalid_argument& error) {
        check(std::string(error.what()).find(reason) != std::string::npos, error.what());
        check(persistent(current) == before && bindingsSame(current, bindings), "refusal preserves the complete target and runtime bindings");
    }
}
void refusalsAndRegions() {
    auto current = target(); auto saved = preset();
    for (const auto group : {static_cast<PresetFields>(0), static_cast<PresetFields>(4), static_cast<PresetFields>(-1)})
        refusal(current, [&] {applyJobPreset(current, saved, group);}, "unknown export preset field group");
    saved.project = "different/project.adi";
    for (const auto group : {PresetFields::BoundsAndOutput, PresetFields::All})
        refusal(current, [&] {applyJobPreset(current, saved, group);}, "same project spelling");
    saved = preset(); saved.session.sampleRate = 96000;
    for (const auto group : {PresetFields::BoundsAndOutput, PresetFields::OptionsAndFormat})
        refusal(current, [&] {applyJobPreset(current, saved, group);}, "clock of explicit sample bounds");
    saved.regionId = 23;
    applyJobPreset(current, saved, PresetFields::BoundsAndOutput);
    check(current.regionId == 23 && current.session.sampleRate == 48000,
          "region bounds stay deferred without copying the preset's sample rate");
    const auto begin = current.beginSample, end = current.endSample;
    applyJobPreset(current, saved, PresetFields::OptionsAndFormat);
    check(current.regionId == 23 && current.session.sampleRate == 96000 && current.beginSample == begin && current.endSample == end,
          "options can change a region render's clock without altering its stored unused bounds");
    current = target(); saved = preset(); saved.normalization.channelWeights = {1, 1};
    refusal(current, [&] {applyJobPreset(current, saved, PresetFields::OptionsAndFormat);}, "one weight per channel");
    saved = preset(); saved.nameTemplate = "{bad-token}";
    refusal(current, [&] {applyJobPreset(current, saved, PresetFields::BoundsAndOutput);}, "unknown export name token");
    saved = preset(); saved.source.trackId = -1;
    refusal(current, [&] {applyJobPreset(current, saved, PresetFields::All);}, "invalid direct stem source");
    saved = preset(); saved.encoding.dither = static_cast<audio::WavDither>(99);
    refusal(current, [&] {applyJobPreset(current, saved, PresetFields::OptionsAndFormat);}, "unsupported export dither");
    current.session.externalTempoBpm = 120; saved = preset();
    refusal(current, [&] {applyJobPreset(current, saved, PresetFields::All);}, "live external tempo");
}
Job niJob(bool alternative) {
    auto job = target(); job.source = {};
    job.niStems = NiStems{{{{alternative ? 22 : 11, engine::OfflineTap::PostFader},
                           alternative ? "new" : "old", alternative ? "#abcdef" : "#112233"}},
                          alternative ? 320000u : 256000u};
    job.niStems->stems.push_back({{alternative ? 44 : 33, engine::OfflineTap::PreFader},
                                alternative ? "second-new" : "second-old", ""});
    return job;
}
void niGroups() {
    auto current = niJob(false); auto saved = niJob(true);
    const auto oldSource = current.niStems->stems[0];
    applyJobPreset(current, saved, PresetFields::OptionsAndFormat);
    check(current.niStems->bitRate == 320000 && sourceSame(current.niStems->stems[0].source, oldSource.source) &&
          current.niStems->stems[0].name == oldSource.name && current.niStems->stems[0].color == oldSource.color &&
          current.niStems->stems.size() == 2 && current.niStems->stems[1].source.trackId == 33,
          "NI format options change bitrate without replacing source IDs, stem order or metadata");
    current = niJob(false);
    applyJobPreset(current, saved, PresetFields::BoundsAndOutput);
    check(current.niStems->bitRate == 256000 && current.niStems->stems[0].source.trackId == 22 &&
          current.niStems->stems[0].name == "new" && current.niStems->stems[0].color == "#abcdef" &&
          current.niStems->stems.size() == 2 && current.niStems->stems[1].source.trackId == 44,
          "NI bounds/output copies stem metadata and IDs while preserving target bitrate");
    const auto wav = target();
    for (const auto group : {PresetFields::BoundsAndOutput, PresetFields::OptionsAndFormat})
        refusal(current, [&] {applyJobPreset(current, wav, group);}, "changing WAV/NI layout");
    applyJobPreset(current, wav, PresetFields::All);
    check(!current.niStems && sourceSame(current.source, wav.source), "All can change NI to WAV together with its source");
    applyJobPreset(current, saved, PresetFields::All);
    check(current.niStems && current.niStems->stems[0].source.trackId == 22, "All can change WAV to NI with a complete valid layout");
    saved.normalization.mode = NormalizeMode::Peak;
    refusal(current, [&] {applyJobPreset(current, saved, PresetFields::OptionsAndFormat);}, "NI Stems normalization is unavailable");
}
} // namespace
int main() {
    try { acceptedGroups(); refusalsAndRegions(); niGroups(); }
    catch (const std::exception& error) { check(false, error.what()); }
    std::printf("render preset groups: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
