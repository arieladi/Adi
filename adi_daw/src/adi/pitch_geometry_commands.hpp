// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include "adi/audio/pitch_segments.hpp"
#include "adi/clip_commands.hpp"

#include <optional>
#include <string>

namespace adi::media { struct ClipPitchAnalysis; }

namespace adi::clip_edit {

struct PitchGeometryEdit {
    std::optional<PreparedEdit> edit;
    std::string error;
    [[nodiscard]] explicit operator bool() const noexcept { return edit.has_value(); }
};

enum class PitchGeometryCorrections { PreserveExact, ResetChanged };

// Each prepared action has its own history label and captures one complete
// replacement. Commit separately with commitPrepared on the same live
// project generation. Boundary correction is a clip.setPitchEdits action.
// PreserveExact retains compatible corrections and an exact frame mapping.
// ResetChanged deliberately clears controls and timing on affected segments.
// Undo restores the entire preceding state under either policy.
[[nodiscard]] PitchGeometryEdit prepareSplitPitchSegment(const Store&, const media::ClipPitchAnalysis&,
    audio::PitchSplitSegment, PitchGeometryCorrections = PitchGeometryCorrections::PreserveExact);
[[nodiscard]] PitchGeometryEdit prepareMergePitchSegments(const Store&, const media::ClipPitchAnalysis&,
    audio::PitchGlueSegments, PitchGeometryCorrections = PitchGeometryCorrections::PreserveExact);
[[nodiscard]] PitchGeometryEdit prepareSetPitchSegmentBoundary(const Store&, const media::ClipPitchAnalysis&,
                                                               audio::PitchSetSegmentBoundary);

} // namespace adi::clip_edit
