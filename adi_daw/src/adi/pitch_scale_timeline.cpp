// SPDX-License-Identifier: AGPL-3.0-or-later
#include "adi/pitch_scale_timeline.hpp"

#include "adi/engine/scale_timeline.hpp"

#include <algorithm>
#include <vector>

namespace adi::clip_edit {

audio::PitchEditError quantizePitchSelectionInTimeline(const audio::PitchAnalysis& analysis,
    audio::PitchEditState& state, const engine::ScaleTimeline& timeline,
    std::span<const PitchTimelineSelection> selection) {
    using audio::PitchEditError;
    const auto current = audio::validatePitchEdits(analysis, state);
    if (current != PitchEditError::None || selection.empty()) return current;

    std::vector<bool> seen(analysis.notes.size());
    std::vector<audio::PitchEditCommand> commands;
    commands.reserve(std::min(selection.size(), analysis.notes.size()));
    for (const auto& segment : selection) {
        if (segment.noteIndex >= analysis.notes.size()) return PitchEditError::UnknownNote;
        if (seen[segment.noteIndex]) return PitchEditError::InvalidValue;
        seen[segment.noteIndex] = true;

        const auto* context = timeline.atTicks(segment.attackTicks);
        const auto* scale = context ? context->scale() : nullptr;
        if (!scale || (scale->tuningId && !segment.customTonicCents)) return PitchEditError::InvalidScale;
        const auto tonic = scale->tuningId ? *segment.customTonicCents : static_cast<double>(scale->root) * 100;
        commands.push_back({segment.noteIndex, audio::PitchQuantize{*context, tonic}});
    }
    return audio::applyPitchEdits(analysis, state, commands);
}

} // namespace adi::clip_edit
