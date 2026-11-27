// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include "adi/audio/pitch_edit.hpp"

namespace adi::audio {

enum class PitchSnapMode { Absolute, Relative, Free };
enum class PitchResetDimension { PitchCenter, PitchCurve, Formant, Volume, Timing, All };

struct PitchMidiAssignment {
    PitchEditError error = PitchEditError::None;
    std::optional<std::size_t> nextNote;
};

// These worker commands take source note indices from the caller's current
// selection and submit one atomic batch of existing pitch edits. The first
// selected note is the anchor. Empty selections are a no-op.
//
// Absolute snaps each displaced nominal pitch to the closest semitone. Relative
// follows the semitone of the anchor target and retains each note's current
// detuning. Free applies the exact target displacement. Semitone ties resolve
// to the lower pitch.
[[nodiscard]] PitchEditError movePitchSelection(const PitchAnalysis&, PitchEditState&,
                                                std::span<const std::size_t> selection,
                                                double anchorTargetCents, PitchSnapMode);

// Adds the same signed cents offset to the selected notes' current shifts.
// Fractional offsets are supported. Zero and empty selections are no-ops.
[[nodiscard]] PitchEditError nudgePitchSelection(const PitchAnalysis&, PitchEditState&,
                                                 std::span<const std::size_t> selection, double deltaCents);

// MIDI assignment places the anchor at the incoming note and moves the rest
// by the same interval. MIDI inputs use the documented E0–C5 range (28–84
// with C3 as middle C), pending measurement against the reference.
// Step returns the next note by current destination time, without wrapping.
[[nodiscard]] PitchMidiAssignment assignPitchSelectionFromMidi(const PitchAnalysis&, PitchEditState&,
                                                                 std::span<const std::size_t> selection,
                                                                 std::uint8_t key, bool step);

// Advances each current nominal pitch towards its nearest captured scale
// degree, with ties to the lower pitch. Amount 0 leaves pitch in place and 1
// applies full quantization. A drag preview starts from its captured state.
[[nodiscard]] PitchEditError correctPitchSelection(const PitchAnalysis&, PitchEditState&,
                                                   std::span<const std::size_t> selection,
                                                   const PitchQuantize& capture, double amount);

// Apply one half-open fractional range to the selected notes on their source
// clock. Endpoints round to the nearest frame, with half frames rounding later.
// An empty result in any selected note refuses the batch. [0,1] uses each
// entire note. Straightening amount and destination timing remain independent.
[[nodiscard]] PitchEditError setPitchStraightenRangeForSelection(const PitchAnalysis&, PitchEditState&,
                                                                std::span<const std::size_t> selection,
                                                                double startFraction, double endFraction);

// A shared destination boundary moves both contiguous notes in one batch.
// Other notes remain stationary. A crossing or empty span fails atomically.
[[nodiscard]] PitchEditError retimePitchBoundary(const PitchAnalysis&, PitchEditState&, std::size_t noteIndex,
                                                PitchBoundary, std::uint64_t destinationFrame);

// Offsets the current destination boundary by a signed number of frames.
// Shared boundaries and overlap obey the same rules as retimePitchBoundary.
[[nodiscard]] PitchEditError nudgePitchBoundary(const PitchAnalysis&, PitchEditState&, std::size_t noteIndex,
                                                PitchBoundary, std::int64_t deltaFrames);

// Resets the selected authored dimension using the captured analysis. An
// empty selection covers every note. Timing restores the original spans and
// refuses a reset that would overlap another note.
[[nodiscard]] PitchEditError resetPitchSelection(const PitchAnalysis&, PitchEditState&,
                                                 std::span<const std::size_t> selection, PitchResetDimension);

} // namespace adi::audio
