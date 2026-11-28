// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "adi/audio/pitch_analysis.hpp"
#include "adi/media/analysis_cache.hpp"

namespace adi::media {

inline constexpr std::uint32_t kPitchAnalysisVersion = 1;
inline constexpr std::uint32_t kPitchAnalysisConfiguration = 0; // equal-weight channel mean

enum class ClipPitchProblem { None, Realtime, Cancelled, NotAudio, InvalidSource, Read, Resource };

struct ClipPitchAnalysis {
    std::int64_t clipId;
    AnalysisIdentity source;
    std::uint64_t sourceStartFrame, sourceEndFrame;
    bool reversed;
    std::uint32_t analysisVersion, analysisConfiguration;
    audio::PitchAnalysis analysis;
};

struct ClipPitchResult {
    ClipPitchProblem problem = ClipPitchProblem::None;
    std::optional<ClipPitchAnalysis> data;
    std::optional<std::string> detail;
    explicit operator bool() const noexcept { return problem == ClipPitchProblem::None && data.has_value(); }
};

// Use an owner-worker Store handle. Notes cover the entire source. Consumers
// use the clip's original-frame window, direction and Warp mapping to project
// them. Discard a result if the source identity or clip view has since changed.
// Older cached note geometry is kept while source access permits refreshing
// optional boundary evidence. An unavailable source can still supply those
// notes with unassessed boundary evidence.
[[nodiscard]] ClipPitchResult buildClipPitchAnalysis(const Store&, std::int64_t clipId,
                                                      std::stop_token = {});
// Null decode uses the process decoder; null analysis disables persistence.
[[nodiscard]] ClipPitchResult buildClipPitchAnalysis(const Store&, std::int64_t clipId,
                                                      std::stop_token, audio::DecodeCache*,
                                                      AnalysisCache*);

// Worker seam for a caller that already verified the source and downmixed it.
// mono covers the entire identified source at its original sample rate.
// Prepared playback can reuse legacy cached notes without refreshing evidence.
[[nodiscard]] audio::PitchAnalysisResult buildSourcePitchAnalysisFromMono(
    const AnalysisIdentity&, std::span<const float> mono, AnalysisCache*, std::stop_token = {});

} // namespace adi::media
