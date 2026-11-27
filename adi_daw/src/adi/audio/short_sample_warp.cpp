// SPDX-License-Identifier: AGPL-3.0-or-later
#include "adi/audio/short_sample_warp.hpp"

#include "adi/audio/io_audit.hpp"
#include "adi/textproj.hpp"

#include <algorithm>
#include <cmath>

namespace adi::audio {
namespace {

bool validTempo(double bpm) {
    return std::isfinite(bpm) && bpm > 0.0 && bpm < 1000.0;
}

unsigned int chooseBars(double barBpm, double referenceBpm) {
    const auto error = [&](unsigned int bars) {
        return std::abs(std::log2(barBpm * bars / referenceBpm));
    };
    return *std::min_element(kShortLoopBarCounts.begin(), kShortLoopBarCounts.end(),
        [&](auto left, auto right) { return error(left) < error(right); });
}

} // namespace

ShortSampleWarpPlan planShortSampleWarp(ShortSampleWarpTrigger trigger, std::string_view mode,
    const SampleClassification& sample, std::uint64_t frames, std::uint32_t rate, double tempoHint) noexcept {
    using Action = ShortSampleWarpAction;
    if (onAudioThread)
        return {};
    if (trigger == ShortSampleWarpTrigger::Drop)
        return {Action::KeepUnwarped, {}, {}};
    if ((trigger != ShortSampleWarpTrigger::WarpPress && trigger != ShortSampleWarpTrigger::ImportWarp) ||
        frames == 0 || rate < 8000 || rate > 768000)
        return {};
    if (mode != "unwarpedOneShot" && mode != "warpedOneShot" && mode != "warpedLoop" && mode != "auto")
        return {};
    if (frames > static_cast<std::uint64_t>(kShortSampleSeconds) * rate)
        return {Action::UseAutoWarp, {}, {}};
    if (mode == "unwarpedOneShot" || (mode == "auto" && sample.kind != SampleKind::Loop))
        return {Action::KeepUnwarped, {}, {}};

    const auto fallbackBpm = validTempo(tempoHint) ? tempoHint : kShortSampleWarpTempoHint;
    const auto referenceBpm = sample.bpm && validTempo(*sample.bpm) ? *sample.bpm : fallbackBpm;
    if (mode == "warpedOneShot")
        return {Action::WarpedOneShot, referenceBpm, {}};

    const auto barBpm = 60.0 * kShortLoopBeatsPerBar * rate / static_cast<double>(frames);
    const auto bars = sample.bars && std::ranges::find(kShortLoopBarCounts, *sample.bars) != kShortLoopBarCounts.end()
        ? *sample.bars : chooseBars(barBpm, referenceBpm);
    const auto endTick = static_cast<std::int64_t>(bars) * kShortLoopBeatsPerBar * textproj::kPPQ;
    const std::array<WarpMarker, 2> markers{{{0, 0}, {static_cast<std::int64_t>(frames), endTick}}};
    return {Action::WarpedLoop, barBpm * bars, markers};
}

} // namespace adi::audio
