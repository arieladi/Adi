// SPDX-License-Identifier: AGPL-3.0-or-later
#include "adi/audio/pitch_edit.hpp"

#include "adi/audio/io_audit.hpp"

#include <algorithm>
#include <cmath>
#include <iterator>
#include <limits>
#include <type_traits>
#include <utility>

namespace adi::audio {
namespace {

constexpr double kMaximumShiftCents = 2400;
constexpr double kMaximumTiltCents = 2400;
constexpr double kMaximumFormantCents = 1200;
constexpr double kMinimumGainDb = -70;
constexpr double kMaximumGainDb = 18;
constexpr std::uint64_t kMaximumTransitionMilliseconds = 200;
constexpr double kStraightenTaperFraction = 0.1;

PitchNoteEdit defaultEdit(const PitchAnalysis& analysis, std::size_t index) noexcept {
    const auto& note = analysis.notes[index];
    return {index, note.startFrame, note.endFrame};
}

bool inRange(double value, double minimum, double maximum) noexcept {
    return value >= minimum && value <= maximum;
}

PitchEditError validateState(const PitchAnalysis& analysis, const PitchEditState& state) {
    std::size_t nextIndex = 0;
    bool retimed = false;
    for (const auto& edit : state.notes) {
        if (edit.noteIndex < nextIndex || edit.noteIndex >= analysis.notes.size()) return PitchEditError::InvalidState;
        const auto& note = analysis.notes[edit.noteIndex];
        if (!validPitchNoteEdit(note, edit, analysis.sourceFrames, analysis.sampleRate)) return PitchEditError::InvalidValue;
        nextIndex = edit.noteIndex + 1;
        retimed = retimed || edit.startFrame != note.startFrame || edit.endFrame != note.endFrame;
    }
    if (!retimed) return PitchEditError::None;

    std::vector<std::pair<std::uint64_t, std::uint64_t>> ranges;
    ranges.reserve(analysis.notes.size());
    auto edit = state.notes.begin();
    for (std::size_t index = 0; index < analysis.notes.size(); ++index) {
        if (edit != state.notes.end() && edit->noteIndex == index) {
            ranges.emplace_back(edit->startFrame, edit->endFrame);
            ++edit;
        } else {
            const auto& note = analysis.notes[index];
            ranges.emplace_back(note.startFrame, note.endFrame);
        }
    }
    std::sort(ranges.begin(), ranges.end());
    const auto overlap = std::adjacent_find(ranges.begin(), ranges.end(), [](const auto& left, const auto& right) {
        return left.second > right.first;
    });
    return overlap == ranges.end() ? PitchEditError::None : PitchEditError::Overlap;
}

PitchEditError updateEdit(const PitchAnalysis& analysis, PitchNoteEdit& edit, const PitchEditCommand& command) {
    const auto& note = analysis.notes[edit.noteIndex];
    return std::visit([&](const auto& change) {
        using Change = std::decay_t<decltype(change)>;
        if constexpr (std::is_same_v<Change, PitchSetShift>) {
            edit.pitchShiftCents = change.cents;
        } else if constexpr (std::is_same_v<Change, PitchSetTiming>) {
            edit.startFrame = change.startFrame;
            edit.endFrame = change.endFrame;
        } else if constexpr (std::is_same_v<Change, PitchSetStraighten>) {
            edit.straighten = change.amount;
        } else if constexpr (std::is_same_v<Change, PitchSetTilt>) {
            edit.tiltCents = change.cents;
        } else if constexpr (std::is_same_v<Change, PitchSetFormant>) {
            edit.formantShiftCents = change.cents;
        } else if constexpr (std::is_same_v<Change, PitchSetGain>) {
            edit.gainDb = change.db;
        } else if constexpr (std::is_same_v<Change, PitchSetDrift>) {
            edit.drift = change.amount;
            edit.preserveExpression = change.amount != 1 && change.preserveExpression;
        } else if constexpr (std::is_same_v<Change, PitchSetTransition>) {
            edit.transitionFrames = change.frames;
        } else if constexpr (std::is_same_v<Change, PitchQuantize>) {
            const auto nominalPitch = note.pitchCents;
            const auto nearest = change.scale.nearest(nominalPitch + edit.pitchShiftCents, change.tonicCents, true);
            if (!nearest) return PitchEditError::InvalidScale;
            edit.pitchShiftCents = nearest->cents - nominalPitch;
        } else if constexpr (std::is_same_v<Change, PitchReset>) {
            edit = defaultEdit(analysis, edit.noteIndex);
        } else if constexpr (std::is_same_v<Change, PitchSetTiltGeometry>) {
            edit.tiltGeometry = change.geometry;
        } else if constexpr (std::is_same_v<Change, PitchSetStraightenRange>) {
            edit.straightenRange = change.range;
            if (edit.straightenRange == PitchStraightenRange{note.startFrame, note.endFrame})
                edit.straightenRange.reset();
        }
        return validPitchNoteEdit(note, edit, analysis.sourceFrames, analysis.sampleRate) ?
            PitchEditError::None : PitchEditError::InvalidValue;
    }, command.change);
}

double interpolatedPitch(const PitchNote& note, double sourceFrame) noexcept {
    const auto right = std::upper_bound(note.curve.begin(), note.curve.end(), sourceFrame,
        [](double frame, const PitchPoint& point) { return frame < static_cast<double>(point.frame); });
    if (right == note.curve.begin()) return right->cents;
    const auto left = std::prev(right);
    if (right == note.curve.end()) return left->cents;
    const auto fraction = (sourceFrame - static_cast<double>(left->frame)) /
                          static_cast<double>(right->frame - left->frame);
    return std::lerp(left->cents, right->cents, fraction);
}

double smoothStep(double position) noexcept {
    return position * position * (3 - 2 * position);
}

double straightenWeight(const PitchNote& note, const PitchNoteEdit& edit, double elapsed, double length) noexcept {
    if (edit.straighten == 0 || !edit.straightenRange) return edit.straighten;
    const auto start = static_cast<double>(edit.straightenRange->startFrame - note.startFrame);
    const auto end = static_cast<double>(edit.straightenRange->endFrame - note.startFrame);
    if (elapsed < start || elapsed >= end) return 0;
    const double taper = static_cast<double>(edit.straightenRange->endFrame - edit.straightenRange->startFrame) *
                         kStraightenTaperFraction;
    double edgeWeight = 1;
    if (start != 0) edgeWeight = std::min(edgeWeight, (elapsed - start) / taper);
    if (end != length) edgeWeight = std::min(edgeWeight, (end - elapsed) / taper);
    return edit.straighten * smoothStep(edgeWeight);
}

double tiltCorrection(const PitchNoteEdit& edit, double position) noexcept {
    const auto& shape = edit.tiltGeometry;
    const double distance = position - shape.anchor;
    double correction = edit.tiltCents * distance;
    if (distance < 0) correction -= shape.startCents * (distance / shape.anchor);
    if (distance > 0) correction += shape.endCents * (distance / (1 - shape.anchor));
    return correction;
}

} // namespace

bool validPitchNoteEdit(const PitchNote& note, const PitchNoteEdit& edit, std::uint64_t sourceFrames,
                        std::uint32_t sampleRate) noexcept {
    const auto maximumTransitionFrames = static_cast<std::uint64_t>(sampleRate) * kMaximumTransitionMilliseconds / 1000;
    const auto& tilt = edit.tiltGeometry;
    const auto& range = edit.straightenRange;
    return sampleRate != 0 && note.startFrame < note.endFrame && note.endFrame <= sourceFrames &&
           edit.startFrame < edit.endFrame && edit.endFrame <= sourceFrames &&
           inRange(edit.pitchShiftCents, -kMaximumShiftCents, kMaximumShiftCents) &&
           inRange(edit.straighten, 0, 1) && inRange(edit.tiltCents, -kMaximumTiltCents, kMaximumTiltCents) &&
           inRange(tilt.anchor, 0, 1) && (tilt.anchor > 0 || tilt.startCents == 0) &&
           (tilt.anchor < 1 || tilt.endCents == 0) &&
           inRange(tilt.startCents, -kMaximumTiltCents, kMaximumTiltCents) &&
           inRange(tilt.endCents, -kMaximumTiltCents, kMaximumTiltCents) &&
           (!range || (note.startFrame <= range->startFrame && range->startFrame < range->endFrame &&
                       range->endFrame <= note.endFrame)) &&
           inRange(edit.formantShiftCents, -kMaximumFormantCents, kMaximumFormantCents) &&
           (inRange(edit.gainDb, kMinimumGainDb, kMaximumGainDb) ||
            edit.gainDb == -std::numeric_limits<double>::infinity()) &&
           inRange(edit.drift, 0, 1) && edit.transitionFrames <= maximumTransitionFrames;
}

PitchEditError applyPitchEdits(const PitchAnalysis& analysis, PitchEditState& state,
                               std::span<const PitchEditCommand> commands) {
    const auto current = validatePitchEdits(analysis, state);
    if (current != PitchEditError::None || commands.empty()) return current;

    std::vector<PitchNoteEdit> staged;
    staged.reserve(analysis.notes.size());
    for (std::size_t index = 0; index < analysis.notes.size(); ++index) staged.push_back(defaultEdit(analysis, index));
    for (const auto& edit : state.notes) staged[edit.noteIndex] = edit;
    for (const auto& command : commands) {
        if (command.noteIndex >= staged.size()) return PitchEditError::UnknownNote;
        const auto result = updateEdit(analysis, staged[command.noteIndex], command);
        if (result != PitchEditError::None) return result;
    }

    std::erase_if(staged, [&](const auto& edit) { return edit == defaultEdit(analysis, edit.noteIndex); });
    PitchEditState candidate{std::move(staged)};
    const auto result = validateState(analysis, candidate);
    if (result == PitchEditError::None) {
        // Keep the full-clip staging allocation out of the long-lived state.
        state.notes.assign(candidate.notes.begin(), candidate.notes.end());
    }
    return result;
}

PitchEditError validatePitchEdits(const PitchAnalysis& analysis, const PitchEditState& state) {
    if (onAudioThread) return PitchEditError::Realtime;
    if (!validPitchAnalysis(analysis)) return PitchEditError::InvalidAnalysis;
    return validateState(analysis, state);
}

PitchNoteEdit resolvePitchNoteEdit(const PitchAnalysis& analysis, const PitchEditState& state,
                                   std::size_t noteIndex) noexcept {
    const auto found = std::lower_bound(state.notes.begin(), state.notes.end(), noteIndex,
        [](const auto& edit, auto index) { return edit.noteIndex < index; });
    return found != state.notes.end() && found->noteIndex == noteIndex ? *found : defaultEdit(analysis, noteIndex);
}

double pitchCorrectionCents(const PitchNote& note, const PitchNoteEdit& edit, double sourceFrame,
                            const PitchNoteEdit* preceding) noexcept {
    const auto length = static_cast<double>(note.endFrame - note.startFrame);
    const auto elapsed = std::clamp(sourceFrame - static_cast<double>(note.startFrame), 0.0, length - 1);
    const auto tiltPosition = length == 1 ? edit.tiltGeometry.anchor : elapsed / (length - 1);
    double shift = edit.pitchShiftCents;
    if (shift != 0 && preceding && preceding->endFrame == edit.startFrame) {
        const auto destinationLength = edit.endFrame - edit.startFrame;
        const auto transition = std::min(edit.transitionFrames, destinationLength / 2);
        if (transition > 0) {
            const auto destinationElapsed = elapsed / length * static_cast<double>(destinationLength);
            const auto position = std::min(1.0, destinationElapsed / static_cast<double>(transition));
            shift = std::lerp(preceding->pitchShiftCents, shift, smoothStep(position));
        }
    }

    double contourCorrection = 0;
    const auto straighten = straightenWeight(note, edit, elapsed, length);
    if (straighten != 0 || edit.drift != 1) {
        const auto variation = interpolatedPitch(note, sourceFrame) - note.pitchCents;
        const auto drift = edit.preserveExpression ? note.driftCents * (elapsed / length - 0.5) : variation;
        const auto deviationAfterDrift = variation - (1 - edit.drift) * drift;
        contourCorrection = (1 - straighten) * deviationAfterDrift - variation;
    }
    return shift + tiltCorrection(edit, tiltPosition) + contourCorrection;
}

} // namespace adi::audio
