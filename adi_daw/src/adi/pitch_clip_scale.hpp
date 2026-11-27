// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include "adi/audio/pitch_edit.hpp"

#include <optional>
#include <string>

namespace adi {
class Store;

namespace clip_edit {

enum class PitchClipScaleProblem { None, Realtime, Read, TonicRequired, InvalidTonic };

struct PitchClipScaleCapture {
    PitchClipScaleProblem problem = PitchClipScaleProblem::None;
    std::optional<audio::PitchQuantize> quantize;
    std::optional<std::string> detail;

    [[nodiscard]] explicit operator bool() const noexcept {
        return problem == PitchClipScaleProblem::None && quantize.has_value();
    }
};

// Owner-side capture of a clip's local or inherited scale. The stored root
// anchors implicit 12-TET. A custom tuning requires the absolute cents value
// for degree zero, which cannot be reconstructed from its root pitch class.
[[nodiscard]] PitchClipScaleCapture capturePitchClipScale(const Store&, std::int64_t clipId,
                                                          std::optional<double> customTonicCents = {});

} // namespace clip_edit
} // namespace adi
