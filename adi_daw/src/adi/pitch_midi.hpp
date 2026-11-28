// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include "adi/audio/pitch_edit.hpp"
#include "adi/blob.hpp"
#include "adi/clip_commands.hpp"
#include "adi/media/clip_pitch.hpp"
#include "adi/ops.hpp"

#include <cstdint>
#include <optional>
#include <span>
#include <stop_token>
#include <string>
#include <vector>

namespace adi::clip_edit {

enum class PitchMidiContent { EditedPitch, DetectedMelody };
enum class PitchMidiVolumeMode { Velocity, NoteGain, ChannelController };
enum class PitchMidiBendMode { Static, Continuous };

struct PitchMidiOptions {
    PitchMidiContent content = PitchMidiContent::EditedPitch;
    std::span<const std::size_t> selectedNotes; // Empty selects all segments.
    std::uint8_t velocity = 100;
    bool keepTuning = false;
    bool includePitchCurve = false;
    // A verified full-source mono PCM buffer supplies RMS and authored gain to
    // dynamic velocity or the selected continuous volume mode.
    std::span<const float> velocitySource;
    // Empty prepares a new MIDI track. EditedPitch replaces prior marked
    // main-lane extractions on a selected track where lineage is available.
    // An explicit previous ID also supports an unmarked main-lane predecessor.
    // An overlap with any other existing clip refuses the operation.
    std::optional<std::int64_t> destinationTrack;
    std::optional<std::int64_t> previousExtractionClip;
    std::stop_token stop;
    // Both continuous modes keep the configured fixed attack velocity. An
    // ordinary channel controller uses channel zero and a CC from 0 to 119.
    PitchMidiVolumeMode volumeMode = PitchMidiVolumeMode::Velocity;
    std::uint8_t volumeController = 7;
    // Native channel wheel at MIDI's default ±2 semitones. The wheel carries
    // pitch in saved clips. Volume mode and attack velocity are independent.
    std::optional<PitchMidiBendMode> channelBend{};
};

enum class PitchMidiProblem { None, Realtime, Cancelled, Source, Edits, Empty, Storage };

struct PitchMidiProjection {
    std::int64_t positionTicks = 0, lengthTicks = 0;
    std::vector<NoteRecord> notes;
    struct NoteCurve {
        std::uint64_t noteId;
        std::vector<ExpressionPoint> points;
    };
    using PitchCurve = NoteCurve;
    // Native AEXP pitch (semitones relative to the output key and tuning).
    std::vector<PitchCurve> pitchCurves;
    // Native AEXP Gain (dB relative to a full-scale sine).
    std::vector<NoteCurve> gainCurves;
    struct ControllerCurve {
        std::uint8_t controller;
        std::vector<AutomationPoint> points;
    };
    // Native ACTL (channel zero, clip-relative ticks and normalized values).
    std::optional<ControllerCurve> volumeCurve;
    struct ChannelBendCurve {
        std::uint8_t channel;
        std::vector<AutomationPoint> points;
    };
    // Native ACTL pitchbend. The source tuning and pitch curves above are
    // available to a detached exporter and omitted from a native clip image.
    std::optional<ChannelBendCurve> channelBendCurve{};
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

struct PitchMidiBendEvent {
    enum class Kind { InitialCenter, Attack, Movement, ReleaseCenter };

    std::int64_t tick; // Clip-relative. A release can occur at the clip end.
    std::uint64_t noteId; // Zero for InitialCenter.
    std::uint16_t word; // 0..16383 with center at 8192.
    Kind kind;
    bool operator==(const PitchMidiBendEvent&) const = default;
};

struct PitchMidiChannelBend {
    std::uint8_t channel;
    int rangeSemitones;
    std::vector<PitchMidiBendEvent> events;
};

// Owner-side conversion of a monophonic pitch projection with keepTuning and,
// for continuous mode, includePitchCurve. The explicit sensitivity is 1..24
// semitones in both directions. Notes keep their projected keys, with tuning
// carried by these words. A MIDI consumer uses the same channel sensitivity and
// orders same-tick events as note-off, release center, attack bend, note-on.
// A continuous contour samples linear segments at their projected knots and at
// intervals of at most 1/192 quarter note, coalescing identical movement words.
[[nodiscard]] PitchMidiResult<PitchMidiChannelBend> projectPitchMidiChannelBend(
    const PitchMidiProjection&, PitchMidiBendMode, int rangeSemitones, std::stop_token = {});

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
