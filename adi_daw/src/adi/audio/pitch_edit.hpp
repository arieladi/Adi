// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include "adi/audio/pitch_analysis.hpp"
#include "adi/engine/scale_context.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <variant>
#include <vector>

namespace adi::audio {

// Edits address notes in one immutable source analysis. Destination bounds live
// on that source's frame clock and end on the first frame outside the note.
struct PitchNoteEdit {
    std::size_t noteIndex = 0;
    std::uint64_t startFrame = 0;
    std::uint64_t endFrame = 0;
    double pitchShiftCents = 0;
    double straighten = 0;
    double tiltCents = 0;
    double formantShiftCents = 0;
    double gainDb = 0;
    double drift = 1;
    bool preserveExpression = false;
    std::uint64_t transitionFrames = 0;

    bool operator==(const PitchNoteEdit&) const = default;
};

struct PitchEditState {
    // Only non-default edits, in source note index order.
    std::vector<PitchNoteEdit> notes;
};

struct PitchSetShift { double cents; };
struct PitchSetTiming { std::uint64_t startFrame, endFrame; };
struct PitchSetStraighten { double amount; };
struct PitchSetTilt { double cents; };
struct PitchSetFormant { double cents; };
struct PitchSetGain { double db; }; // Negative infinity mutes the selected note.
struct PitchSetDrift { double amount; bool preserveExpression; };
struct PitchSetTransition { std::uint64_t frames; };
struct PitchQuantize { engine::ScaleContext scale; double tonicCents; };
struct PitchReset {};

struct PitchEditCommand {
    std::size_t noteIndex;
    std::variant<PitchSetShift, PitchSetTiming, PitchSetStraighten, PitchSetTilt,
                 PitchSetFormant, PitchSetGain, PitchSetDrift, PitchSetTransition,
                 PitchQuantize, PitchReset> change;
};

enum class PitchEditError {
    None, Realtime, InvalidAnalysis, UnknownNote, InvalidState, InvalidValue, InvalidScale, Overlap
};

// Worker/owner-only. Quantization snaps the current nominal, shifted pitch to
// a degree of the supplied clip scale. Apply a batch before checking overlap
// so coordinated changes, such as moving two adjacent notes, are atomic.
[[nodiscard]] PitchEditError applyPitchEdits(
    const PitchAnalysis&, PitchEditState&, std::span<const PitchEditCommand>);
[[nodiscard]] PitchEditError validatePitchEdits(const PitchAnalysis&, const PitchEditState&);

// The state and note index must be valid. Unedited notes resolve to the source
// time ranges and neutral controls. This query does not allocate.
[[nodiscard]] PitchNoteEdit resolvePitchNoteEdit(
    const PitchAnalysis&, const PitchEditState&, std::size_t noteIndex) noexcept;

// Evaluates correction at a finite source frame for a validated note and edit.
// Straighten blends toward nominal pitch. Tilt adds a linear first-to-last
// cents change about the midpoint. Drift retains this fraction of the source
// pitch variation, or of the fitted linear trend with preserveExpression.
// For adjacent destination spans, an incoming transition blends the previous
// transpose offset into the current one over at most half the destination note.
[[nodiscard]] double pitchCorrectionCents(
    const PitchNote&, const PitchNoteEdit&, double sourceFrame,
    const PitchNoteEdit* preceding = nullptr) noexcept;

} // namespace adi::audio
