// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include "adi/audio/auto_warp.hpp"
#include "adi/audio/warp_timing_cache.hpp"

#include <memory>
#include <string_view>

namespace adi::audio {
// Worker-owned timing analysis for callers that already stream source PCM.
// Feed complete interleaved frames in arbitrary blocks; finish consumes the
// analyzer. A failed append cannot publish a partial result. No cache or I/O.
class WarpTimingAnalyzer {
public:
    WarpTimingAnalyzer(std::uint32_t sourceRate, std::uint32_t channels, WarpTimingSettings);
    ~WarpTimingAnalyzer();
    WarpTimingAnalyzer(const WarpTimingAnalyzer&) = delete;
    WarpTimingAnalyzer& operator=(const WarpTimingAnalyzer&) = delete;

    [[nodiscard]] bool append(std::span<const float> interleaved, std::stop_token = {});
    [[nodiscard]] std::optional<WarpTimingAnalysis> finish(std::stop_token = {});

private:
    struct State;
    std::unique_ptr<State> state_;
};

// Keeps one timing computation active for a given ADIA folder, file hash and
// settings in this process. Owner workers recheck the cache after acquisition.
// Release after publication, including failures. Disabled caches use a no-op
// lease. Waiting is cancellable, and recursive acquisition on the same thread
// fails immediately. Construct and release only off the audio callback.
class WarpTimingFlight {
public:
    WarpTimingFlight(WarpTimingFlight&&) noexcept;
    WarpTimingFlight& operator=(WarpTimingFlight&&) noexcept;
    ~WarpTimingFlight();

private:
    struct State;
    explicit WarpTimingFlight(std::unique_ptr<State> state) noexcept;
    std::unique_ptr<State> state_;

    friend std::optional<WarpTimingFlight> acquireWarpTimingFlight(const media::AnalysisCache&,
        std::string_view, WarpTimingSettings, std::stop_token);
};

[[nodiscard]] std::optional<WarpTimingFlight> acquireWarpTimingFlight(const media::AnalysisCache&,
    std::string_view contentHash, WarpTimingSettings, std::stop_token = {});

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
