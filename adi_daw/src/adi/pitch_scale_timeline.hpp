// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include "adi/audio/pitch_edit.hpp"

namespace adi::engine { class ScaleTimeline; }

namespace adi::clip_edit {

struct PitchTimelineSelection {
    std::size_t noteIndex = 0;
    std::int64_t attackTicks = 0;
    std::optional<double> customTonicCents{};
};

// Worker-side. The caller maps each effective segment's edited attack to an
// absolute Arrangement tick and selects one occurrence for a looped segment.
// Inherited pitch quantization uses the key active at that tick. Custom
// tunings need an absolute tonic for that selection entry. Clip-local scales
// and chord-track quantization are separate sources. Duplicate note indices
// refuse the whole batch so no segment can receive two different keys.
[[nodiscard]] audio::PitchEditError quantizePitchSelectionInTimeline(
    const audio::PitchAnalysis&, audio::PitchEditState&, const engine::ScaleTimeline&,
    std::span<const PitchTimelineSelection>);

} // namespace adi::clip_edit
