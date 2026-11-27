// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include "adi/audio/pitch_render.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <stop_token>

namespace adi::audio {

struct PitchAuditionRange {
    std::uint64_t startFrame = 0, endFrame = 0; // half-open
    bool operator==(const PitchAuditionRange&) const = default;
};

struct PitchAuditionSpec {
    PitchAuditionRange clipRange; // trimmed interval in original-source-relative frames
    // Indices in the current analysis. An empty selection auditions clipRange.
    std::span<const std::size_t> selectedNotes;
    bool bypassPitchFormantGain = false;
    bool bypassSegmentTiming = false;
    bool loop = false;
};

struct PitchAuditionResult {
    PitchRenderResult rendered;
    PitchAuditionRange clipRange; // original-source-relative bounds, before project Warp
    std::optional<PitchAuditionRange> loop; // buffer-local frames if requested
    [[nodiscard]] explicit operator bool() const noexcept { return static_cast<bool>(rendered); }
};

/// Worker-only comparison. Segment timing bypass restores source bounds on
/// this clock; a separate project Warp stage owns project-level Warp bypass.
/// Selected notes give one continuous span, including the audio between them,
/// clipped to clipRange. Full source context is used before cropping output.
/// Both bypasses together reproduce the selected finite source samples.
/// Invalid or empty range returns InvalidSource. Unknown notes or invalid
/// authored edits return InvalidEdits, including with both bypasses active.
[[nodiscard]] PitchAuditionResult renderPitchAudition(std::span<const float> mono, const PitchAnalysis&,
                                                    const PitchEditState&, const PitchAuditionSpec&,
                                                    std::stop_token = {});

} // namespace adi::audio
