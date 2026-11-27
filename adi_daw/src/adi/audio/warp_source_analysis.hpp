// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include "adi/audio/auto_warp.hpp"
#include "adi/audio/warp_timing_cache.hpp"

namespace adi::audio {
// The hash identifies the original source bytes, even when the reader is a
// decoded WAV. A complete derived grid can be reused by both Warp producers.
[[nodiscard]] std::optional<WarpTimingAnalysis> analyzeWarpTiming(WavReader&,
    const std::string& contentHash, WarpTimingSettings, std::stop_token = {},
    media::AnalysisCache* = nullptr);

[[nodiscard]] AutoWarpResult analyzeWarpSource(WavReader&, const std::string& contentHash,
    std::uint64_t start, std::uint64_t frames, const AutoWarpOptions&, std::stop_token = {},
    media::AnalysisCache* = nullptr);

// Captures measured onset positions in a prepared clip's original PCM window.
[[nodiscard]] std::vector<std::uint64_t> warpOnsetsInWindow(const WarpTimingAnalysis&,
    std::uint32_t sourceRate, std::uint64_t start, std::uint64_t frames);
}
