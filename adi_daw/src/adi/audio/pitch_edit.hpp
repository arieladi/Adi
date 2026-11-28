// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include "adi/audio/pitch_analysis.hpp"
#include "adi/engine/scale_context.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <variant>
#include <vector>

namespace adi::audio {

enum class PitchBoundary { Start, End };

struct PitchTiltGeometry {
    double anchor = 0.5; // normalized source position, from first to last sample
    double startCents = 0;
    double endCents = 0;

    bool operator==(const PitchTiltGeometry&) const = default;
};

struct PitchStraightenRange {
    std::uint64_t startFrame;
    std::uint64_t endFrame; // exclusive on the source frame clock

    bool operator==(const PitchStraightenRange&) const = default;
};

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
    PitchTiltGeometry tiltGeometry{};
    std::optional<PitchStraightenRange> straightenRange{}; // absent means the entire note

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
// A new drift adjustment leaves the residual pitch expression intact. An
// explicit false retains the full-variation control supported by older edits.
struct PitchSetDrift { double amount; bool preserveExpression = true; };
struct PitchSetTransition { std::uint64_t frames; };
struct PitchQuantize { engine::ScaleContext scale; double tonicCents; };
struct PitchReset {};
struct PitchSetTiltGeometry { PitchTiltGeometry geometry; };
struct PitchSetStraightenRange { std::optional<PitchStraightenRange> range; };

struct PitchEditCommand {
    std::size_t noteIndex;
    std::variant<PitchSetShift, PitchSetTiming, PitchSetStraighten, PitchSetTilt,
                 PitchSetFormant, PitchSetGain, PitchSetDrift, PitchSetTransition,
                 PitchQuantize, PitchReset, PitchSetTiltGeometry, PitchSetStraightenRange> change;
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

// Validates one authored row from known source geometry. The source note's
// curve and summaries are not needed when validating a persisted edit.
[[nodiscard]] bool validPitchNoteEdit(const PitchNote&, const PitchNoteEdit&, std::uint64_t sourceFrames,
                                    std::uint32_t sampleRate) noexcept;

// The state and note index must be valid. Unedited notes resolve to the source
// time ranges and neutral controls. This query does not allocate.
[[nodiscard]] PitchNoteEdit resolvePitchNoteEdit(
    const PitchAnalysis&, const PitchEditState&, std::size_t noteIndex) noexcept;

// Evaluates correction at a finite source frame for a validated note and edit.
// Straighten blends toward nominal pitch within the selected source range,
// tapering at the range's interior edges. Tilt adds a first-to-last change
// around the anchor, plus independent linear offsets on either side that
// reach zero at the anchor. Drift retains a fraction of the fitted trend by
// default, leaving residual vibrato. Stored edits with preserveExpression
// disabled apply drift to the whole source pitch variation.
// For adjacent destination spans, an incoming transition blends the previous
// transpose offset into the current one over at most half the destination note.
[[nodiscard]] double pitchCorrectionCents(
    const PitchNote&, const PitchNoteEdit&, double sourceFrame,
    const PitchNoteEdit* preceding = nullptr) noexcept;

} // namespace adi::audio
