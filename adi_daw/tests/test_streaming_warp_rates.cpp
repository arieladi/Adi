// SPDX-License-Identifier: AGPL-3.0-or-later
#include "adi/audio/streaming_warp.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <numbers>
#include <vector>

namespace {
using namespace adi;
using namespace adi::audio;

constexpr std::uint32_t outputRate = 48000;

struct Input {
    std::vector<float> pcm;
    WarpSourceRange requested;
    bool readOutsideRange = false;

    static float read(void* context, std::int64_t frame, std::uint16_t) noexcept {
        auto& input = *static_cast<Input*>(context);
        if (frame < input.requested.first || frame >= input.requested.last)
            input.readOutsideRange = true;
        return input.pcm[static_cast<std::size_t>(frame)];
    }
};

struct Clock {
    double stride;

    static double tickAtFrame(const void* context, double frame) noexcept {
        return frame * static_cast<const Clock*>(context)->stride;
    }

    static double frameAtTick(const void* context, double tick) noexcept {
        return tick / static_cast<const Clock*>(context)->stride;
    }

    [[nodiscard]] WarpStreamClock callbacks() const noexcept {
        return {this, tickAtFrame, frameAtTick, stride};
    }
};

bool mixedRate(WarpMode mode, std::uint32_t sourceRate, double passHz, double stopHz) {
    constexpr std::int64_t outputFrames = outputRate / 2;
    const auto sourceFrames = static_cast<std::int64_t>(sourceRate / 2);
    const std::array markers{WarpMarker{0, 0}, WarpMarker{sourceFrames, sourceFrames}};
    WarpRenderPlan plan;
    plan.mode = mode;
    plan.sourceFrames = static_cast<std::uint64_t>(sourceFrames);
    plan.outputRate = outputRate;
    plan.fluctuation = 0;

    Input input{std::vector<float>(plan.sourceFrames), {}, false};
    for (std::int64_t frame = 0; frame < sourceFrames; ++frame) {
        const double angle = 2 * std::numbers::pi * static_cast<double>(frame) / sourceRate;
        input.pcm[static_cast<std::size_t>(frame)] =
            static_cast<float>(0.5 * std::sin(passHz * angle) + 0.25 * std::sin(stopHz * angle));
    }

    const Clock clock{static_cast<double>(sourceRate) / outputRate};
    const StreamingWarp player(plan, markers, sourceRate, 1);
    constexpr std::int64_t margin = 3000, block = 257;
    double errorPower = 0;
    for (auto first = margin; first < outputFrames - margin; first += block) {
        const auto end = std::min(outputFrames - margin, first + block);
        input.requested = player.sourceRange(first, end - 1, clock.callbacks());
        for (auto frame = first; frame < end; ++frame) {
            std::array<float, 1> output{};
            if (!player.renderFrame(frame, {&input, Input::read}, clock.callbacks(), output)) return false;
            const double angle = 2 * std::numbers::pi * static_cast<double>(frame) / outputRate;
            const double error = output[0] - 0.5 * std::sin(passHz * angle);
            errorPower += error * error;
        }
    }
    const double rms = std::sqrt(errorPower / (outputFrames - 2 * margin));
    std::printf("%s at %u -> %u Hz: RMS error %.7f, reads covered %s\n", mode == WarpMode::Beats ? "Beats" : "Texture",
                sourceRate, outputRate, rms, input.readOutsideRange ? "no" : "yes");
    return rms < 0.001 && !input.readOutsideRange;
}

bool halfSampleSlice() {
    const std::array markers{WarpMarker{0, 0}, WarpMarker{100, 201}, WarpMarker{200, 400}};
    WarpRenderPlan plan;
    plan.sourceFrames = 200;
    const StreamingWarp player(plan, markers, outputRate, 1);
    Input input{std::vector<float>(plan.sourceFrames, 0.25f), {}, false};
    std::fill(input.pcm.begin() + 100, input.pcm.end(), 0.75f);
    const Clock clock{2};
    auto roundedInverse = clock.callbacks();
    // A clock inversion can be a representable tick above the exact marker.
    roundedInverse.tickAtFrame = [](const void* context, double frame) noexcept {
        return std::nextafter(Clock::tickAtFrame(context, frame), 1000.0);
    };
    std::array<float, 1> before{}, after{};
    input.requested = player.sourceRange(100, 101, roundedInverse);
    const auto beforeOk = player.renderFrame(100, {&input, Input::read}, roundedInverse, before);
    const auto afterOk = player.renderFrame(101, {&input, Input::read}, roundedInverse, after);
    const bool passed = beforeOk && afterOk && before[0] == 0.25f && after[0] == 0.75f && !input.readOutsideRange;
    std::printf("Beats half-sample slice: %.2f before, %.2f after\n", before[0], after[0]);
    return passed;
}

bool collapsedBeatsSlices() {
    const std::array markers{WarpMarker{0, 0}, WarpMarker{480, 480}};
    WarpRenderPlan plan;
    plan.sourceFrames = 480;
    plan.transients = {100, 101};
    plan.transientLoop = TransientLoop::Off;
    const StreamingWarp player(plan, markers, outputRate, 1);
    const Clock clock{4};
    Input input{std::vector<float>(plan.sourceFrames, 0.25f), player.sourceRange(25, 26, clock.callbacks()), false};
    input.pcm[100] = 0.75f;
    std::array<float, 1> attack{}, body{};
    const auto attackOk = player.renderFrame(25, {&input, Input::read}, clock.callbacks(), attack);
    const auto bodyOk = player.renderFrame(26, {&input, Input::read}, clock.callbacks(), body);
    std::printf("Beats shared boundary: attack %.2f, body %.2f\n", attack[0], body[0]);
    return attackOk && bodyOk && attack[0] == 0.75f && body[0] == 0.25f && !input.readOutsideRange;
}

bool unusedPcm() {
    const std::array markers{WarpMarker{0, 0}, WarpMarker{480, 480}};
    WarpRenderPlan plan;
    plan.sourceFrames = 480;
    plan.grainSeconds = 0.001;
    plan.fluctuation = 0;
    constexpr std::int64_t frame = 96;
    const StreamingWarp beats(plan, markers, outputRate, 1);
    plan.mode = WarpMode::Texture;
    const StreamingWarp texture(plan, markers, outputRate, 1);
    Input input{std::vector<float>(plan.sourceFrames, 0.25f), {frame, frame + 1}, false};
    std::array<float, 1> output{};
    const Clock unity{1};
    const auto beatsOk = beats.renderFrame(frame, {&input, Input::read}, unity.callbacks(), output);
    const bool unnecessaryBeatsRead = input.readOutsideRange;
    const bool beatsPassed = beatsOk && output[0] == 0.25f && !unnecessaryBeatsRead;

    // The grain centred at output frame 120 starts at this output frame.
    // With a 2x clock its first read is source frame 216, with zero Hann gain.
    input.requested = {0, 216};
    input.readOutsideRange = false;
    const Clock doubleSpeed{2};
    const auto textureOk = texture.renderFrame(frame, {&input, Input::read}, doubleSpeed.callbacks(), output);
    std::printf("Unneeded sample reads: Beats %s, Texture %s\n", unnecessaryBeatsRead ? "yes" : "no",
                input.readOutsideRange ? "yes" : "no");
    return beatsPassed && textureOk && output[0] == 0.25f && !input.readOutsideRange;
}

bool rePitchRatios() {
    const std::array markers{WarpMarker{0, 0}, WarpMarker{1200, 1200}};
    WarpRenderPlan plan;
    plan.mode = WarpMode::RePitch;
    plan.sourceFrames = 1200;
    const StreamingWarp player(plan, markers, outputRate, 1);
    Input input{std::vector<float>(plan.sourceFrames, 0.25f), {0, 1200}, false};
    for (const auto stride : {1.0 / 257, 1.0 / 256, 256.0, 257.0}) {
        const Clock clock{stride};
        const bool expected = stride >= 1.0 / 256 && stride <= 256;
        std::array<float, 1> output{1};
        const bool accepted = player.renderFrame(0, {&input, Input::read}, clock.callbacks(), output);
        if (expected != accepted || (!accepted && output[0] != 0)) return false;
    }
    return !input.readOutsideRange;
}

bool rePitchReadAhead() {
    // This remote rapid segment must not consume the bounded page slots
    // needed to render an earlier, ordinary-rate block.
    const std::array markers{WarpMarker{0, 0}, WarpMarker{65000, 65000}, WarpMarker{120000, 65001}};
    WarpRenderPlan plan;
    plan.mode = WarpMode::RePitch;
    plan.sourceFrames = 120000;
    const StreamingWarp player(plan, markers, outputRate, 1);
    const Clock clock{1};
    constexpr std::int64_t first = 40000, last = first + 511;
    const auto requested = player.sourceRange(first, last, clock.callbacks());
    Input input{std::vector<float>(plan.sourceFrames, 0.25f), requested, false};
    for (auto frame = first; frame <= last; ++frame) {
        std::array<float, 1> output{};
        if (!player.renderFrame(frame, {&input, Input::read}, clock.callbacks(), output)) return false;
    }
    const auto frames = requested.last - requested.first;
    std::printf("Re-Pitch 512-frame read-ahead: %lld source frames\n", static_cast<long long>(frames));
    return frames < 1024 && !input.readOutsideRange;
}
} // namespace

int main() {
    bool passed = true;
    for (const auto mode : {WarpMode::Beats, WarpMode::Texture}) {
        passed &= mixedRate(mode, 96000, 1000, 30000);
        passed &= mixedRate(mode, 32000, 8000, 0);
    }
    passed &= halfSampleSlice();
    passed &= collapsedBeatsSlices();
    passed &= unusedPcm();
    passed &= rePitchRatios();
    passed &= rePitchReadAhead();
    return passed ? 0 : 1;
}
