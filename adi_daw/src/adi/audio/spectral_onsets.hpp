// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include "adi/audio/warp_analysis_pcm.hpp"

namespace adi::audio {

// Maximum algorithm lookahead from an attack to its classification at 44.1
// kHz, excluding any separate input conversion. This pass runs on a worker.
inline constexpr std::uint32_t kSpectralOnsetLookaheadFrames = 1152;

struct SpectralOnset {
    std::uint64_t analysisFrame = 0; // source-window-relative at 44.1 kHz
    float strength = 0;             // compressed positive spectral flux in [0, 1]
    float peakDbfs = -200;          // local sample peak near the spectral attack
    friend bool operator==(const SpectralOnset&, const SpectralOnset&) = default;
};

struct SpectralOnsetResult {
    AnalysisError error = AnalysisError::None;
    std::vector<SpectralOnset> onsets;
    [[nodiscard]] explicit operator bool() const noexcept { return error == AnalysisError::None; }
};

// Spectral candidate for the Q-D benchmark. The incumbent energy detector is
// still the default of existing callers. Append whole decoder frames in any
// chunk sizes, then finish once. This object belongs to a single worker.
class SpectralOnsetAnalyzer {
public:
    SpectralOnsetAnalyzer(std::uint32_t sourceRate, std::uint32_t channels, int sensitivity = 50);
    ~SpectralOnsetAnalyzer();
    SpectralOnsetAnalyzer(const SpectralOnsetAnalyzer&) = delete;
    SpectralOnsetAnalyzer& operator=(const SpectralOnsetAnalyzer&) = delete;

    [[nodiscard]] bool append(std::span<const float> interleaved, std::stop_token = {});
    [[nodiscard]] SpectralOnsetResult finish(std::stop_token = {});

private:
    struct State;
    std::unique_ptr<State> state_;
    AnalysisError initialError_ = AnalysisError::InvalidSource;
};

} // namespace adi::audio
