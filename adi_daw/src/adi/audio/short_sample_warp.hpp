// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include "adi/audio/sample_classification.hpp"
#include "adi/audio/warp_map.hpp"

#include <string_view>
#include <string>

namespace adi::audio {

enum class ShortSampleWarpTrigger { Drop, WarpPress, ImportWarp };
enum class ShortSampleWarpAction { Invalid, KeepUnwarped, UseAutoWarp, WarpedOneShot, WarpedLoop };

struct ShortSampleWarpPlan {
    ShortSampleWarpAction action = ShortSampleWarpAction::Invalid;
    std::optional<double> sourceBpm;
    std::optional<std::array<WarpMarker, 2>> loopMarkers;
};

struct ShortSampleWarpInput {
    std::string mode;
    SampleClassification classification;
    std::string contentHash{}; // Use the measurement's source hash for a library result.
};

inline constexpr double kShortSampleWarpTempoHint = 120.0;

// Plan an explicit short-sample Warp request from the resolved library answer.
// A Drop stays clean. Long explicit requests delegate to the ordinary Auto-Warp
// path. WarpedOneShot gives that producer an explicit source BPM; WarpedLoop
// gives the caller a full-source map and a clock consistent with its end tick.
// A known loop without bar timing and a forced loop use the nearest permitted
// bar count to the source or tempo hint. The hint never changes project tempo.
[[nodiscard]] ShortSampleWarpPlan planShortSampleWarp(ShortSampleWarpTrigger,
    std::string_view mode, const SampleClassification&, std::uint64_t frames,
    std::uint32_t rate, double tempoHint = kShortSampleWarpTempoHint) noexcept;

} // namespace adi::audio
