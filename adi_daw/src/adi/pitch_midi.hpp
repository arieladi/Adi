// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include "adi/audio/pitch_edit.hpp"
#include "adi/blob.hpp"
#include "adi/clip_commands.hpp"
#include "adi/media/clip_pitch.hpp"
#include "adi/ops.hpp"

#include <optional>
#include <span>
#include <stop_token>
#include <string>
#include <vector>

namespace adi::clip_edit {

enum class PitchMidiContent { EditedPitch, DetectedMelody };

struct PitchMidiOptions {
    PitchMidiContent content = PitchMidiContent::EditedPitch;
    std::span<const std::size_t> selectedNotes; // Empty selects all segments.
    std::uint8_t velocity = 100;
    bool keepTuning = false;
    bool includePitchCurve = false;
    // When supplied, a verified full-source mono PCM buffer allows dynamic
    // velocity from each segment's RMS and authored segment gain.
    std::span<const float> velocitySource;
    // Empty prepares a new MIDI track. The owner can identify a previous
    // extraction on a selected destination for explicit whole replacement.
    // An overlap with any other existing clip refuses the operation.
    std::optional<std::int64_t> destinationTrack;
    std::optional<std::int64_t> previousExtractionClip;
    std::stop_token stop;
};

enum class PitchMidiProblem { None, Realtime, Cancelled, Source, Edits, Empty, Storage };

struct PitchMidiProjection {
    std::int64_t positionTicks = 0, lengthTicks = 0;
    std::vector<NoteRecord> notes;
    struct PitchCurve {
        std::uint64_t noteId;
        std::vector<ExpressionPoint> points;
    };
    // Native AEXP pitch (semitones relative to the output key and tuning).
    std::vector<PitchCurve> pitchCurves;
};

template<class Data>
struct PitchMidiResult {
    PitchMidiProblem problem = PitchMidiProblem::None;
    std::optional<Data> data;
    std::optional<std::string> detail;
    [[nodiscard]] explicit operator bool() const noexcept {
        return problem == PitchMidiProblem::None && data.has_value();
    }
};

struct PreparedPitchMidi;
using PitchMidiPreparation = PitchMidiResult<PreparedPitchMidi>;

struct PreparedPitchMidi {
    std::int64_t track = 0, midiClip = 0;
    PitchMidiProjection projection;
private:
    const Store* owner = nullptr;
    std::string client;
    std::optional<std::int64_t> head;
    std::vector<OpRequest> requests;
    friend PitchMidiPreparation preparePitchMidi(Store&, const media::ClipPitchAnalysis&, const PitchMidiOptions&);
    friend CommitResult commitPitchMidi(Store&, const PreparedPitchMidi&, Actor);
};

// Owner-side projection of explicit edited segments or raw detected Melody.
// Source identity, playback window and direction must still match the captured
// analysis. Notes reflect the clip range, markers, reverse and repeated
// material. The project clock remains authoritative across tempo changes.
[[nodiscard]] PitchMidiResult<PitchMidiProjection> projectPitchMidi(
    Store&, const media::ClipPitchAnalysis&, const audio::PitchEditState&,
    const PitchMidiOptions& = {});

// Loads saved pitch edits for EditedPitch. Data is captured before submission;
// replay uses the prepared MIDI stream independent of source I/O and analysis.
[[nodiscard]] PitchMidiPreparation preparePitchMidi(
    Store&, const media::ClipPitchAnalysis&, const PitchMidiOptions& = {});
// Detached clip image for the host's clipboard. A later native Paste creates
// new row IDs and applies the normal placement/Undo policy.
[[nodiscard]] PitchMidiResult<ClipCopy> copyPitchMidi(
    Store&, const media::ClipPitchAnalysis&, const PitchMidiOptions& = {});
[[nodiscard]] CommitResult commitPitchMidi(Store&, const PreparedPitchMidi&, Actor = Actor::User);

} // namespace adi::clip_edit
