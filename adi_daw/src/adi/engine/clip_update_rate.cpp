// SPDX-License-Identifier: AGPL-3.0-or-later
#include "adi/engine/clip_update_rate.hpp"

#include "adi/engine/launcher.hpp"

#include <algorithm>
#include <array>
#include <limits>
#include <utility>

namespace adi::engine {
namespace {
constexpr std::array rates{
    std::pair{"none", LaunchQuantization::None},
    std::pair{"1/32", LaunchQuantization::ThirtySecond},
    std::pair{"1/16", LaunchQuantization::Sixteenth},
    std::pair{"1/8", LaunchQuantization::Eighth},
    std::pair{"1/4", LaunchQuantization::Quarter},
    std::pair{"1/2", LaunchQuantization::Half},
    std::pair{"1 bar", LaunchQuantization::Bar},
};
static_assert(rates.size() == static_cast<std::size_t>(ClipUpdateRate::Bar) + 1);

// Live's manual specifies the rate but no arrival ordering when a prepared
// edit and the quantized timing boundary become available together.
constexpr bool clipUpdateUsesAlreadyReachedGridBoundary = true;
} // namespace

std::optional<ClipUpdateRate> parseClipUpdateRate(std::string_view value) noexcept {
    const auto it = std::ranges::find(rates, value, [](const auto& rate) { return std::string_view(rate.first); });
    if (it == rates.end())
        return {};
    return static_cast<ClipUpdateRate>(it - rates.begin());
}

std::optional<ClipUpdateBoundary> nextClipUpdateBoundary(
    const LaunchBlock& clock, std::int64_t readySample, ClipUpdateRate rate, bool clipRunning) noexcept {
    const auto index = static_cast<std::size_t>(rate);
    if (readySample < 0 || !clock.tickAtSample || index >= rates.size())
        return {};
    const auto at = clock.tickAtSample(clock.clock, readySample);
    if (at < 0)
        return {};
    if (!clipRunning || rate == ClipUpdateRate::None)
        return ClipUpdateBoundary{readySample, at};
    if (!clock.sampleAtTick)
        return {};

    // The project grid is the first implementation. The manual does not
    // identify the grid origin for a running clip launched off the beat.
    const auto code = rates[index].second;
    // Search from the last physical frame before readiness. The tick clock
    // can round past a grid tick whose mapped output frame is already usable.
    const auto before = readySample > 0 ? clock.tickAtSample(clock.clock, readySample - 1) : at;
    if (before < 0 || before > at)
        return {};
    const auto tick = launch_quantize::nextGridTick(before, code, clock.meters);
    if (!tick)
        return {};
    const auto sample = clock.sampleAtTick(clock.clock, *tick);
    if (sample > readySample || (sample == readySample && clipUpdateUsesAlreadyReachedGridBoundary))
        return ClipUpdateBoundary{sample, *tick};

    // Integer tick projection can land on the previous sample. Wait for the
    // following grid tick; a prepared edit cannot change material already heard.
    if (*tick == std::numeric_limits<std::int64_t>::max())
        return {};
    const auto nextTick = launch_quantize::nextGridTick(*tick + 1, code, clock.meters);
    if (!nextTick)
        return {};
    const auto nextSample = clock.sampleAtTick(clock.clock, *nextTick);
    if (nextSample < readySample || (nextSample == readySample && !clipUpdateUsesAlreadyReachedGridBoundary))
        return {};
    return ClipUpdateBoundary{nextSample, *nextTick};
}

std::optional<std::int32_t> clipUpdateOffsetIn(const LaunchBlock& block,
                                             const ClipUpdateBoundary& boundary) noexcept {
    if (block.sample < 0 || boundary.sample < block.sample)
        return {};
    const auto offset = boundary.sample - block.sample;
    if (offset >= block.frames)
        return {};
    return static_cast<std::int32_t>(offset);
}
} // namespace adi::engine
