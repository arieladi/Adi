// SPDX-License-Identifier: AGPL-3.0-or-later
#include "adi/engine/clip_update_rate.hpp"
#include "adi/engine/launch_gain_edits.hpp"
#include "adi/engine/launcher.hpp"
#include "adi/settings/registry.hpp"
#include "adi/textproj.hpp"

#include <algorithm>
#include <array>
#include <cstdio>
#include <limits>
#include <span>
#include <vector>

namespace {
using namespace adi;
using namespace adi::engine;
constexpr auto quarter = textproj::kPPQ;
using Rate = ClipUpdateRate;
int checks = 0, failures = 0;

void check(bool good, const char* label) {
    ++checks;
    if (!good) {
        ++failures;
        std::printf("FAIL %s\n", label);
    }
}

std::int64_t tickAtSample(const void* context, std::int64_t sample) noexcept {
    const auto beat = *static_cast<const std::int64_t*>(context);
    return (sample * quarter + beat / 2) / beat;
}

std::int64_t sampleAtTick(const void* context, std::int64_t tick) noexcept {
    const auto beat = *static_cast<const std::int64_t*>(context);
    return (tick * beat + quarter / 2) / quarter;
}

LaunchBlock clockAt(const std::int64_t& quarterFrames) {
    LaunchBlock clock{};
    clock.clock = &quarterFrames;
    clock.tickAtSample = tickAtSample;
    clock.sampleAtTick = sampleAtTick;
    return clock;
}

void ratesAndCoordinator() {
    constexpr std::array values{"none", "1/32", "1/16", "1/8", "1/4", "1/2", "1 bar"};
    constexpr std::array rates{Rate::None, Rate::ThirtySecond, Rate::Sixteenth, Rate::Eighth,
                              Rate::Quarter, Rate::Half, Rate::Bar};
    constexpr std::array<std::int64_t, 7> ticks{0, quarter / 8, quarter / 4, quarter / 2,
                                               quarter, 2 * quarter, 4 * quarter};
    const auto* setting = settings::find("record.clipUpdateRate");
    check(setting && setting->choices == std::vector<settings::Value>(values.begin(), values.end()) &&
          setting->defaultValue == "1/16", "engine choices match the current registry and sixteenth default");
    check(!parseClipUpdateRate("global") && !parseClipUpdateRate("1/8T"), "launch-only rates are rejected");
    std::size_t i = 0;
    constexpr std::int64_t beat = 24000;
    auto clock = clockAt(beat);
    clock.global = LaunchQuantization::EightBars;
    for (auto rate : rates) {
        check(parseClipUpdateRate(values[i]) == rate, "parse enabled Clip Update Rate setting");
        const auto projected = nextClipUpdateBoundary(clock, 1, rate, true);
        const auto expectedTick = rate == Rate::None ? tickAtSample(&beat, 1) : ticks[i];
        const auto expectedSample = rate == Rate::None ? 1 : sampleAtTick(&beat, ticks[i]);
        check(projected == ClipUpdateBoundary{expectedSample, expectedTick},
              "each running edit has its own musical grid independent of launch quantization");
        check(nextClipUpdateBoundary(clock, 1, rate, false) == ClipUpdateBoundary{1, tickAtSample(&beat, 1)},
              "stopped or inactive clip property edit applies immediately");
        ++i;
    }
    constexpr std::array meters{rows::TimeSignature{0, 4, 4}, rows::TimeSignature{4 * quarter, 3, 4}};
    clock.meters = meters;
    check(nextClipUpdateBoundary(clock, 4 * beat + 1, Rate::Bar, true) == ClipUpdateBoundary{7 * beat, 7 * quarter},
          "the 1-bar update follows the authored 3/4 grid after a 4/4 bar");
    check(!nextClipUpdateBoundary(clock, -1, Rate::Bar, true) &&
          !nextClipUpdateBoundary(clock, 100, static_cast<Rate>(255), true), "invalid rates and times refuse");
}

void exactAudioBoundary() {
    for (const std::int64_t sampleRate : {44100, 48000, 96000, 192000}) {
        const auto beat = sampleRate / 2;
        const auto updateFrame = sampleAtTick(&beat, quarter / 4);
        auto clock = clockAt(beat);
        const auto ready = nextClipUpdateBoundary(clock, 10, Rate::Sixteenth, true);
        check(ready == ClipUpdateBoundary{updateFrame, quarter / 4}, "the prepared edit has the exact device-rate boundary");
        check(nextClipUpdateBoundary(clock, updateFrame, Rate::Sixteenth, true) == ready,
              "an edit ready on the boundary can take that rounded output frame");
        check(nextClipUpdateBoundary(clock, updateFrame + 1, Rate::Sixteenth, true) ==
              ClipUpdateBoundary{sampleAtTick(&beat, quarter / 2), quarter / 2},
              "an edit ready one frame late waits for the next sixteenth");
        if (!ready)
            continue;
        for (const auto blockSize : {32, 257, 4096}) {
            float gain = 0.25f;
            auto applications = 0;
            std::vector<float> rendered(static_cast<std::size_t>(updateFrame + 1));
            for (std::int32_t start = 0; start <= updateFrame; start += blockSize) {
                clock.sample = start;
                clock.frames = std::min(blockSize, static_cast<std::int32_t>(updateFrame) + 1 - start);
                const auto offset = clipUpdateOffsetIn(clock, *ready).value_or(clock.frames);
                std::fill_n(rendered.begin() + start, offset, gain);
                if (offset < clock.frames) {
                    gain = 0.5f;
                    ++applications;
                    std::fill_n(rendered.begin() + start + offset, clock.frames - offset, gain);
                }
            }
            check(applications == 1 && rendered.back() == 0.5f &&
                  std::ranges::all_of(std::span(rendered).first(rendered.size() - 1),
                      [](auto value) { return value == 0.25f; }),
                  "the old and new prepared gain switch exactly once at the same sample for each block size");
        }
    }
    LaunchBlock invalid{};
    check(!nextClipUpdateBoundary(invalid, 0, Rate::None, true), "missing rendered clock cannot schedule an edit");
    invalid.tickAtSample = invalid.sampleAtTick = [](const void*, std::int64_t value) noexcept { return value; };
    check(!nextClipUpdateBoundary(invalid, std::numeric_limits<std::int64_t>::max(), Rate::Bar, true),
          "overflow near project end cannot wrap an edit backward");
}

void runningGainPlan() {
    constexpr std::array<std::int64_t, 1> tracks{1};
    constexpr std::int64_t beat = 24000;
    auto block = clockAt(beat);
    block.incarnation = 7;
    auto pcmRevision = std::uint64_t{1};
    LaunchSection section{};
    section.track = 1;
    section.clip = 101;
    section.run = 15;
    section.arrangement = false;
    LaunchClipGainEdits gains(tracks.size());
    const auto prepare = [&](std::int64_t sample, std::int32_t frames) {
        block.sample = sample;
        block.frames = frames;
        section.frames = frames;
        gains.begin(block, tracks, pcmRevision);
        return gains.range(block, section);
    };

    check(prepare(0, 1000).before == 1.0f, "a running clip starts with its prepared PCM gain");
    const LaunchClipGainEdit edit{1, 101, 7, 15, 0.5f, Rate::Sixteenth};
    check(gains.post(edit, pcmRevision), "the owner can publish a prepared running-clip gain edit");
    const auto delay = prepare(1000, 4096);
    check(delay.beforeFrames == 4096 && delay.before == 1.0f && delay.after == 1.0f,
          "the current gain stays audible before the quantized boundary");
    auto stale = edit;
    stale.run = 14;
    stale.multiplier = 0.25f;
    check(gains.post(stale, pcmRevision), "a stale input can reach the generation-aware driver");
    const auto split = prepare(5096, 1024);
    check(split.beforeFrames == 904 && split.before == 1.0f && split.after == 0.5f &&
          gains.currentMultiplier(1, 15) == 0.5f, "the edit splits one PCM section at exactly frame 6000");
    auto immediate = edit;
    immediate.rate = Rate::None;
    immediate.multiplier = 0.25f;
    check(gains.post(immediate, pcmRevision), "the owner can explicitly disable edit quantization");
    const auto next = prepare(6120, 64);
    check(next.beforeFrames == 0 && next.before == 0.5f && next.after == 0.25f,
          "None updates the same active run at the first ready sample");
    check(gains.post(edit, pcmRevision), "a pending update retains its original run identity");
    ++section.run;
    check(prepare(6184, 64).before == 1.0f && gains.currentMultiplier(1, 16) == 1.0f,
          "relaunch starts from prepared PCM gain and never inherits a stale edit");
    immediate.run = 16;
    check(gains.post(immediate, pcmRevision) && prepare(6248, 64).after == 0.25f,
          "a new run accepts its own literal gain edit");
    auto nextEdit = immediate;
    nextEdit.multiplier = 0.75f;
    ++pcmRevision;
    check(gains.post(nextEdit, pcmRevision) && gains.post(immediate, pcmRevision - 1),
          "owner can enqueue a gain for freshly prepared PCM before its first rendered block");
    const auto newPcm = prepare(6312, 64);
    check(newPcm.before == 1.0f && newPcm.after == 0.75f && newPcm.beforeFrames == 0,
          "a new PCM baseline clears the prior gain, admits its own command and drops older PCM stamps");
    ++block.incarnation;
    check(prepare(6376, 64).before == 1.0f, "a different incarnation retains its own authored gain");
}
} // namespace

int main() {
    ratesAndCoordinator();
    exactAudioBoundary();
    runningGainPlan();
    std::printf("%d Clip Update Rate checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
