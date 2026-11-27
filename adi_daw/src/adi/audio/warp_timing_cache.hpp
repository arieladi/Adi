// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include "adi/audio/spectral_onsets.hpp"
#include "adi/audio/warp_timing_map.hpp"
#include "adi/media/analysis_cache.hpp"
#include "adi/textproj.hpp"

namespace adi::audio {
struct WarpTimingSettings {
    unsigned int numerator = 4;
    unsigned int sensitivity = 50;
    unsigned int denominator = 4;
    std::optional<double> quarterBpm={};
};

struct WarpTimingAnalysis {
    std::vector<SpectralOnset> onsets;
    std::vector<std::uint64_t> beats;
    std::vector<std::size_t> downbeats; // Indices into beats.
    std::vector<WarpTempoRegion> tempoRegions;
    std::optional<double> medianBpm;
    std::int64_t ticksPerPulse = textproj::kPPQ;
};

// Derived ADIA data on the fixed analysis clock. Callback and cancellation
// requests refuse. A miss includes incompatible analysis settings or bytes.
[[nodiscard]] std::optional<WarpTimingAnalysis> readWarpTiming(const media::AnalysisCache&,
    const media::AnalysisIdentity&, WarpTimingSettings, std::stop_token = {});
// For indexed files with a known content hash and no source dimensions yet.
// The dimensions in the ADIA envelope and timing section must agree.
[[nodiscard]] std::optional<WarpTimingAnalysis> readWarpTimingByHash(const media::AnalysisCache&,
    const std::string& contentHash, WarpTimingSettings, std::stop_token = {});
[[nodiscard]] media::AnalysisCacheProblem storeWarpTiming(media::AnalysisCache&,
    const media::AnalysisIdentity&, WarpTimingSettings, const WarpTimingAnalysis&,
    std::string& error, std::stop_token = {});
}
