// SPDX-License-Identifier: AGPL-3.0-or-later
#include "replace_operator_new.hpp"
#include "adi/audio/io_audit.hpp"
#include "adi/engine/snapshot.hpp"
#include "adi/engine/warp_clip_player.hpp"
#include "adi/store_rows.hpp"
#include "adi/textproj.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <numbers>
#include <vector>

namespace { unsigned callbackAllocations = 0; }
void adi::test::countReplacedNew() noexcept {
    if (adi::audio::onAudioThread) ++callbackAllocations;
}

namespace {
using namespace adi;
using namespace adi::engine;
constexpr auto beat = textproj::kPPQ;
constexpr int rate = 48000, sourceFrames = rate;

void require(bool pass, const char* message) {
    if (pass) return;
    std::fprintf(stderr, "%s\n", message);
    std::exit(1);
}

struct Source {
    std::array<float, sourceFrames> pcm{};

    explicit Source(double gain) {
        constexpr int radius = 40;
        for (const int landmark : {6000, 18000}) {
            for (int offset = -radius; offset <= radius; ++offset) {
                const auto frame = static_cast<std::size_t>(landmark + offset);
                pcm[frame] = static_cast<float>(gain * (0.5 + 0.5 * std::cos(offset * std::numbers::pi / radius)));
            }
        }
    }

    audio::WarpStreamInput input() noexcept {
        return {this, [](void* context, std::int64_t frame, std::uint16_t) noexcept {
            const auto& source = *static_cast<const Source*>(context);
            if (frame < 0 || frame >= sourceFrames) return 0.0f;
            return source.pcm[static_cast<std::size_t>(frame)];
        }, [](void* context, std::int64_t frame) noexcept -> audio::WarpStreamPcm {
            const auto& source = *static_cast<const Source*>(context);
            if (frame < 0 || frame >= sourceFrames) return {};
            return {source.pcm.data() + frame, static_cast<std::uint32_t>(sourceFrames - frame)};
        }};
    }
};

void sceneTempoHandover() {
    constexpr int start = rate / 2, handover = 2 * rate;
    const std::array<rows::TempoEvent, 2> events{{{0, 120, 0, 0}, {4 * beat, 140, 0, 0}}};
    const WarpArrangementClock clock(events, rate);
    audio::WarpRenderPlan plan;
    plan.outputRate = rate;
    plan.sourceFrames = sourceFrames;
    plan.mode = audio::WarpMode::RePitch;
    // Arrangement geometry is intentionally different from either launch.
    WarpClipPlayer voice(clock, std::move(plan), {{0, 0}, {sourceFrames, 2 * beat}},
                         rate, 1, 6 * beat, 2 * beat, 0, 2 * beat);
    require(voice.ready(), "launch slot media was prepared before driver use");
    Source first(0.55), second(0.9);
    const auto firstPcm = first.input(), secondPcm = second.input();
    std::vector<float> rendered(handover + 43000);
    const auto ioBefore = audio::callbackFileIo.load();
    bool valid = true;
    WarpClipPlayer::Placement firstSlot, secondSlot;
    {
        audio::CallbackScope callback;
        firstSlot = voice.launchAt(beat);
        // The second scene enters a quarter beat into the clip in Legato.
        secondSlot = voice.launchAt(4 * beat, beat / 4);
        for (std::size_t i = 0; i < rendered.size(); ++i) {
            const auto frame = static_cast<std::int64_t>(i);
            const bool next = frame >= handover;
            valid = voice.renderFrame(next ? secondSlot : firstSlot, frame,
                                      next ? secondPcm : firstPcm, {rendered.data() + i, 1}) && valid;
        }
    }
    require(valid && firstSlot.firstFrame == start && secondSlot.firstFrame == handover,
            "launch placement uses the shared performed clock");
    require(std::all_of(rendered.begin(), rendered.begin() + start, [](float v) { return v == 0; }),
            "slot is silent before its launch");
    require(rendered[handover - 1] == 0 && rendered[handover] > 0.89f,
            "scene handover with Legato is exact at the new bar");

    constexpr int secondOffset = 12000, repeatOffset = 48000;
    const auto transformed = [](int frames) { return static_cast<int>(std::lround(frames * 120.0 / 140)); };
    const std::array expected{start + 6000, start + 18000, handover + transformed(secondOffset),
                              handover + transformed(repeatOffset)};
    int maxError = 0;
    for (const auto at : expected) {
        const auto best = std::max_element(rendered.begin() + at - 12, rendered.begin() + at + 13);
        require(*best > 0.5f, "each expected scene tempo landmark remains audible");
        maxError = std::max(maxError, static_cast<int>(std::abs((best - rendered.begin()) - at)));
    }
    require(maxError <= 1, "both tempo sections and the repeated handover slot stay within one frame");
    require(callbackAllocations == 0 && audio::callbackFileIo.load() == ioBefore,
            "launch placement and streaming slot rendering allocate nothing and perform no I/O");
    std::printf("Warp launch handover at %d samples, maximum grid error %d samples, callback allocations %u\n",
                handover, maxError, callbackAllocations);
}

void sharedTempoIndex() {
    const auto rampEnd = 4 * beat;
    const std::array<rows::TempoEvent, 4> events{{
        {0, 120, 2, 0.9}, {rampEnd, 140, 1, 0},
        {6 * beat, 130, 2, -0.8, TempoRampSpan{6 * beat, 12 * beat, 130, 90}},
        {8 * beat, 180, 0, 0}
    }};
    const auto shared = TempoMap::fromRows(events);
    long double worst = 0;
    for (const auto sampleRate : {44100, 48000, 96000}) {
        const WarpArrangementClock indexed(shared, sampleRate);
        for (int point = 0; point < 995; ++point) {
            const auto tick = static_cast<double>(beat) * (point - 40) / 100.0;
            const auto expected = shared.secondsAtPosition(tick) * sampleRate;
            const auto frame = indexed.frameAtTick(tick);
            const auto roundTrip = indexed.frameAtTick(indexed.tickAtFrame(frame));
            worst = std::max({worst, std::abs(frame - expected), static_cast<long double>(std::abs(roundTrip - frame))});
        }
    }
    require(worst < 0.0001L, "indexed Warp clock matches the shared steps, ramps and interrupted curve spans");
    std::printf("Shared Warp clock maximum placement and inverse error %.9Lf samples at 44.1, 48 and 96 kHz\n", worst);
}
} // namespace

int main() {
    sharedTempoIndex();
    sceneTempoHandover();
}
