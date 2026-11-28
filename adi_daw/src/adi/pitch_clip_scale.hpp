// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include "adi/audio/pitch_edit.hpp"

#include <optional>
#include <span>
#include <string>
#include <vector>

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

struct PitchClipScaleSuggestion {
    audio::PitchQuantize quantize;
    std::size_t matchedNotes;
    double meanCorrectionCents;
};

struct PitchClipScaleSuggestions {
    audio::PitchEditError error = audio::PitchEditError::None;
    std::size_t testedNotes = 0;
    // A refused callback constructs no container, including MSVC Debug's
    // iterator proxy. Empty results keep the storage disengaged as well.
    std::optional<std::vector<PitchClipScaleSuggestion>> candidates;

    [[nodiscard]] explicit operator bool() const noexcept { return error == audio::PitchEditError::None; }
    [[nodiscard]] std::span<const PitchClipScaleSuggestion> ranked() const noexcept {
        if (!candidates) return {};
        return *candidates;
    }
};

// Score selected notes or, when selection is empty, the displayed notes.
// Every unmuted nominal pitch carries one vote regardless of duration. Notes
// match when their nearest tuning degree belongs to the suggested scale.
// Ranks prefer coverage, mean absolute correction, a phrase ending on the
// tonic, then the captured scale. Standard tuning offers major and minor in
// all keys plus the captured scale. Custom tuning scores the captured scale
// and its absolute tonic. The ranking awaits a reference comparison.
[[nodiscard]] PitchClipScaleSuggestions suggestPitchClipScales(const audio::PitchAnalysis&, const audio::PitchEditState&,
                                                                  const audio::PitchQuantize& captured,
                                                                  std::span<const std::size_t> displayed,
                                                                  std::span<const std::size_t> selected = {});

} // namespace clip_edit
} // namespace adi
