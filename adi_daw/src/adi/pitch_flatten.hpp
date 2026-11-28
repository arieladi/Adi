// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include "adi/material_commands.hpp"

namespace adi::audio { class DecodeCache; }
namespace adi::media { class AnalysisCache; }

namespace adi::clip_edit {

enum class PitchFlattenScope { Unspecified, WholeEvent, SelectedSegments };
enum class PitchFlattenProblem { None, Realtime, Cancelled, UnsupportedScope, Capture, Stale, Render, Material };

struct PitchFlattenOptions {
    PitchFlattenScope scope = PitchFlattenScope::Unspecified;
    std::int64_t clipId = 0, mediaId = 0, importedUtc = 0;
    std::stop_token stop;
};

struct PitchFlattenResult {
    PitchFlattenProblem problem = PitchFlattenProblem::None;
    std::optional<MaterialEdit> material;
    std::optional<std::string> detail;
    [[nodiscard]] explicit operator bool() const noexcept { return problem == PitchFlattenProblem::None && material.has_value(); }
};

/// Prepare a native-rate, whole-event pitch render on the Store owner or an
/// owner-worker connection. Commits use commitMaterial on the same project
/// generation. Clip placement and project Warp remain editable; the authored
/// pitch rows and original source are recoverable by Undo. A selected-segment
/// or unspecified scope refuses before reading or staging media.
[[nodiscard]] PitchFlattenResult preparePitchFlatten(Store&, const PitchFlattenOptions&);
[[nodiscard]] PitchFlattenResult preparePitchFlatten(Store&, const PitchFlattenOptions&,
                                                     audio::DecodeCache*, media::AnalysisCache*);

} // namespace adi::clip_edit
