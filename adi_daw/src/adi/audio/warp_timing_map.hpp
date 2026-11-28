// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include "adi/audio/warp_analysis_pcm.hpp"
#include "adi/audio/warp_map.hpp"

#include <optional>

namespace adi::audio {

struct WarpTempoRegion {
    std::size_t firstBeat = 0;
    double bpm = 0.0;
};

// Detect constant clocks and snap their displayed BPM within the per-region
// phase error budget. Beats are strictly increasing and use the given rate.
// A boundary beat closes one region and starts the next. Off the callback.
[[nodiscard]] std::vector<WarpTempoRegion> warpTempoRegions(
    std::span<const std::uint64_t> beats, std::uint32_t analysisRate = kWarpAnalysisRate,
    std::stop_token = {});

struct WarpTimingMap {
    std::uint64_t trimFrames = 0; // in original source frames
    std::vector<WarpMarker> markers;
};

// Capture a full-coverage AWRP grid from analyser-independent timing. The
// input is on the 44.1 kHz source-relative clock. Downbeats are sorted beat
// indices. A nonempty region sequence must start at beat zero. The first
// measured downbeat is the exact trim and 1.1.1 when fromFirstDownbeat is true.
// Interior bar markers are ironed within each region, with the measured
// tempo-change boundaries preserved. An empty region list uses one clock.
[[nodiscard]] std::optional<WarpTimingMap> buildWarpTimingMap(
    std::span<const std::uint64_t> beats, std::span<const std::size_t> downbeats,
    std::span<const WarpTempoRegion> regions, std::uint64_t sourceFrames,
    std::uint32_t sourceRate, bool fromFirstDownbeat, std::stop_token = {});

} // namespace adi::audio
