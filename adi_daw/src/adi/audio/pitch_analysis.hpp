// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <stop_token>
#include <vector>

namespace adi::audio {

struct PitchPoint {
    std::uint64_t frame = 0;
    double cents = 0; // absolute MIDI cents, A4 = 6900
    friend bool operator==(const PitchPoint&, const PitchPoint&) = default;
};

struct PitchNote {
    std::uint64_t startFrame = 0;
    std::uint64_t endFrame = 0; // exclusive
    double pitchCents = 0; // median of the voiced curve
    double driftCents = 0; // fitted linear trend across the entire note
    double vibratoCents = 0; // RMS around that trend
    std::vector<PitchPoint> curve;
    friend bool operator==(const PitchNote&, const PitchNote&) = default;
};

struct PitchAnalysis {
    std::uint32_t sampleRate = 0;
    std::uint64_t sourceFrames = 0;
    // Source-relative bounds. Notes are ordered and disjoint, and each note
    // contains a nonempty, strictly time-ordered curve within its bounds.
    std::vector<PitchNote> notes;
    friend bool operator==(const PitchAnalysis&, const PitchAnalysis&) = default;
};

enum class PitchAnalysisError { None, Realtime, InvalidSource, Cancelled, Resource };

struct PitchAnalysisResult {
    PitchAnalysisError error = PitchAnalysisError::None;
    std::optional<PitchAnalysis> analysis;
    [[nodiscard]] explicit operator bool() const noexcept { return error == PitchAnalysisError::None && analysis.has_value(); }
};

[[nodiscard]] bool validPitchAnalysis(const PitchAnalysis&) noexcept;

/// Worker-only signal analysis of a monophonic source at 8–384 kHz. Silence
/// produces an analysis with no notes. A failed or cancelled call has no result.
[[nodiscard]] PitchAnalysisResult analyzePitch(std::span<const float> mono, std::uint32_t sourceRate,
                                               std::stop_token = {});

} // namespace adi::audio
