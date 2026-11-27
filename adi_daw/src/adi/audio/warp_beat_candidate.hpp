// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include "adi/audio/warp_timing_map.hpp"
#include "adi/textproj.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <stop_token>
#include <vector>

namespace adi::audio {

enum class WarpBeatCandidateError { None, Realtime, InvalidSource, NoBeat, Cancelled };

struct WarpBeatMeter {
    unsigned int numerator = 4;
    unsigned int denominator = 4;
    std::optional<double> quarterBpm; // known source tempo, not the project tempo
};

struct WarpBeatCandidateResult {
    WarpBeatCandidateError error = WarpBeatCandidateError::None;
    std::vector<std::uint64_t> beats; // detected pulse positions at 44.1 kHz
    std::vector<std::size_t> downbeats; // indices into beats
    std::vector<WarpTempoRegion> tempoRegions;
    double medianBpm = 0.0; // MIDI quarters per minute
    std::int64_t ticksPerPulse = textproj::kPPQ;
    [[nodiscard]] explicit operator bool() const noexcept { return error == WarpBeatCandidateError::None; }
};

// Worker-only 44.1 kHz mono input. PCM can arrive in arbitrarily sized spans.
// Mixxx's beat tracker and qm-dsp DownBeat produce positions in this same
// analysis-frame clock. The candidate is used by the benchmark until Q-D.
class WarpBeatCandidateAnalyzer {
public:
    explicit WarpBeatCandidateAnalyzer(unsigned int beatsPerBar = 4);
    // Meter alone uses its customary metrical pulse. A known source quarter
    // tempo can distinguish subdivisions of that pulse on the detected grid.
    explicit WarpBeatCandidateAnalyzer(WarpBeatMeter);
    ~WarpBeatCandidateAnalyzer();
    WarpBeatCandidateAnalyzer(const WarpBeatCandidateAnalyzer&) = delete;
    WarpBeatCandidateAnalyzer& operator=(const WarpBeatCandidateAnalyzer&) = delete;

    [[nodiscard]] bool append(std::span<const float> mono, std::stop_token = {});
    [[nodiscard]] WarpBeatCandidateResult finish(std::stop_token = {});

private:
    struct State;
    std::unique_ptr<State> state_;
    WarpBeatCandidateError error_ = WarpBeatCandidateError::None;
};

using WarpBeatCandidateMap = WarpTimingMap;

// Re-express the candidate grid on the original source clock. From First
// Downbeat trims the pickup and makes the first clear bar 1.1.1. The map
// captures the bar boundaries and stable tempo changes in the remaining PCM.
[[nodiscard]] std::optional<WarpBeatCandidateMap> buildWarpBeatCandidateMap(
    const WarpBeatCandidateResult&, std::uint64_t sourceFrames,
    std::uint32_t sourceRate, bool fromFirstDownbeat, std::stop_token = {});

} // namespace adi::audio
