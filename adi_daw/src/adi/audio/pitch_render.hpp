// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include "adi/audio/pitch_edit.hpp"

#include <cstdint>
#include <memory>
#include <span>
#include <stop_token>
#include <vector>

namespace adi::audio {

enum class PitchRenderError { None, Realtime, InvalidSource, InvalidEdits, Unsupported, Cancelled, Resource, Process };

struct PitchRenderResult {
    PitchRenderError error = PitchRenderError::None;
    std::unique_ptr<const std::vector<float>> samples;
    [[nodiscard]] explicit operator bool() const noexcept { return error == PitchRenderError::None && samples != nullptr; }
};

/// Worker-only render on the source frame clock. The result has the original
/// clip length. A moved note replaces the audio under its destination and
/// clears its source span, with short edge fades to prevent discontinuities.
/// Samples outside affected spans pass through, with non-finite input treated
/// as zero. Cancellation and failed DSP publish no render.
/// Pitch, formant and stretch DSP support rates through 192 kHz. Applied
/// pitch and time ratios must stay within [1/16, 16].
[[nodiscard]] PitchRenderResult renderPitchEdits(std::span<const float> mono, const PitchAnalysis&,
                                                const PitchEditState&, std::stop_token = {});

/// Stereo uses one synchronized DSP instance for the left/right pair.
/// Input and output are interleaved, with one or two channels per frame.
[[nodiscard]] PitchRenderResult renderPitchEdits(std::span<const float> interleaved, std::uint16_t channels,
                                                const PitchAnalysis&, const PitchEditState&, std::stop_token = {});

} // namespace adi::audio
