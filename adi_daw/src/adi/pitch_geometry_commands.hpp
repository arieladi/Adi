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

// Each prepared action has its own history label and captures one complete
// replacement through clip.setPitchEdits. Commit separately with
// commitPrepared on the same live project generation.
// Split and merge preserve constant authored corrections and exact frame
// mapping. Inputs requiring incompatible contours or a fractional cut refuse.
[[nodiscard]] PitchGeometryEdit prepareSplitPitchSegment(const Store&, const media::ClipPitchAnalysis&,
                                                         audio::PitchSplitSegment);
[[nodiscard]] PitchGeometryEdit prepareMergePitchSegments(const Store&, const media::ClipPitchAnalysis&,
                                                          audio::PitchGlueSegments);
[[nodiscard]] PitchGeometryEdit prepareSetPitchSegmentBoundary(const Store&, const media::ClipPitchAnalysis&,
                                                               audio::PitchSetSegmentBoundary);

} // namespace adi::clip_edit
