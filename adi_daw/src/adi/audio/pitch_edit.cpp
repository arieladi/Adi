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

PitchNoteEdit defaultEdit(const PitchAnalysis& analysis, std::size_t index) noexcept {
    const auto& note = analysis.notes[index];
    return {index, note.startFrame, note.endFrame};
}

bool inRange(double value, double minimum, double maximum) noexcept {
    return value >= minimum && value <= maximum;
}

bool validValues(const PitchAnalysis& analysis, const PitchNoteEdit& edit) noexcept {
    const auto maximumTransitionFrames =
        static_cast<std::uint64_t>(analysis.sampleRate) * kMaximumTransitionMilliseconds / 1000;
    return edit.startFrame < edit.endFrame && edit.endFrame <= analysis.sourceFrames &&
           inRange(edit.pitchShiftCents, -kMaximumShiftCents, kMaximumShiftCents) &&
           inRange(edit.straighten, 0, 1) && inRange(edit.tiltCents, -kMaximumTiltCents, kMaximumTiltCents) &&
           inRange(edit.formantShiftCents, -kMaximumFormantCents, kMaximumFormantCents) &&
           (inRange(edit.gainDb, kMinimumGainDb, kMaximumGainDb) ||
            edit.gainDb == -std::numeric_limits<double>::infinity()) &&
           inRange(edit.drift, 0, 1) && edit.transitionFrames <= maximumTransitionFrames;
}

PitchEditError validateState(const PitchAnalysis& analysis, const PitchEditState& state) {
    std::size_t nextIndex = 0;
    bool retimed = false;
    for (const auto& edit : state.notes) {
        if (edit.noteIndex < nextIndex || edit.noteIndex >= analysis.notes.size()) return PitchEditError::InvalidState;
        if (!validValues(analysis, edit)) return PitchEditError::InvalidValue;
        nextIndex = edit.noteIndex + 1;
        const auto& note = analysis.notes[edit.noteIndex];
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
            edit.preserveExpression = change.preserveExpression;
        } else if constexpr (std::is_same_v<Change, PitchSetTransition>) {
            edit.transitionFrames = change.frames;
        } else if constexpr (std::is_same_v<Change, PitchQuantize>) {
            const auto nominalPitch = analysis.notes[edit.noteIndex].pitchCents;
            const auto nearest = change.scale.nearest(nominalPitch + edit.pitchShiftCents, change.tonicCents, true);
            if (!nearest) return PitchEditError::InvalidScale;
            edit.pitchShiftCents = nearest->cents - nominalPitch;
        } else if constexpr (std::is_same_v<Change, PitchReset>) {
            edit = defaultEdit(analysis, edit.noteIndex);
        }
        return validValues(analysis, edit) ? PitchEditError::None : PitchEditError::InvalidValue;
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

} // namespace

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
    const auto tiltPosition = length == 1 ? 0.0 : elapsed / (length - 1) - 0.5;
    double shift = edit.pitchShiftCents;
    if (shift != 0 && preceding && preceding->endFrame == edit.startFrame) {
        const auto destinationLength = edit.endFrame - edit.startFrame;
        const auto transition = std::min(edit.transitionFrames, destinationLength / 2);
        if (transition > 0) {
            const auto destinationElapsed = elapsed / length * static_cast<double>(destinationLength);
            const auto position = std::min(1.0, destinationElapsed / static_cast<double>(transition));
            const auto smooth = position * position * (3 - 2 * position);
            shift = std::lerp(preceding->pitchShiftCents, shift, smooth);
        }
    }

    double contourCorrection = 0;
    if (edit.straighten != 0 || edit.drift != 1) {
        const auto variation = interpolatedPitch(note, sourceFrame) - note.pitchCents;
        const auto drift = edit.preserveExpression ? note.driftCents * (elapsed / length - 0.5) : variation;
        const auto deviationAfterDrift = variation - (1 - edit.drift) * drift;
        contourCorrection = (1 - edit.straighten) * deviationAfterDrift - variation;
    }
    return shift + edit.tiltCents * tiltPosition + contourCorrection;
}

} // namespace adi::audio
