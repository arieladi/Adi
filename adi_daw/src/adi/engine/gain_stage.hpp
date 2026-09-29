// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "adi/ops.hpp"
#include <cstdint>
#include <vector>
namespace adi::engine {
enum class GainStageMeasure { GatedRms, IntegratedLufs };
struct GainStageOptions {
    GainStageMeasure measure = GainStageMeasure::GatedRms;
    double targetDb = -18.0;
    double ceilingDbTP = -1.0;
    // Zero end means the final unmuted clip's end. Measured in session samples.
    std::int64_t beginSample = 0, endSample = 0;
};
struct GainStageTrack {
    std::int64_t trackId = 0;
    double measuredDb = 0.0, peakDbTP = 0.0, gainDb = 0.0;
    bool silent = true, ceilingLimited = false;
};
struct GainStageResult {
    CommitResult commit;
    std::vector<GainStageTrack> tracks;
};
}
