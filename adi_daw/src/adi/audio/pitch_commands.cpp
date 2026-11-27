// SPDX-License-Identifier: AGPL-3.0-or-later
#include "adi/audio/pitch_commands.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <vector>

namespace adi::audio {
namespace {

constexpr double kCentsPerSemitone = 100;
constexpr std::uint8_t kLowestMidiAssignmentNote = 28; // E0
constexpr std::uint8_t kHighestMidiAssignmentNote = 84; // C5

double nearestSemitone(double cents) noexcept {
    const auto semitones = cents / kCentsPerSemitone;
    const auto lower = std::floor(semitones);
    return (lower + (semitones - lower > 0.5 ? 1 : 0)) * kCentsPerSemitone;
}

PitchEditError validateSelection(const PitchAnalysis& analysis, const PitchEditState& state,
                                 std::span<const std::size_t> selection) {
    const auto current = validatePitchEdits(analysis, state);
    if (current != PitchEditError::None) return current;
    if (std::any_of(selection.begin(), selection.end(), [&](auto index) { return index >= analysis.notes.size(); }))
        return PitchEditError::UnknownNote;
    return PitchEditError::None;
}

double currentPitch(const PitchAnalysis& analysis, const PitchEditState& state, std::size_t index) noexcept {
    return analysis.notes[index].pitchCents + resolvePitchNoteEdit(analysis, state, index).pitchShiftCents;
}

std::uint64_t frameAtFraction(const PitchNote& note, double fraction) noexcept {
    const auto length = static_cast<long double>(note.endFrame - note.startFrame);
    const auto offset = std::round(length * static_cast<long double>(fraction));
    // Clamp before integer conversion, even on platforms where long double
    // can round a very large source extent up to the first invalid integer.
    return offset >= length ? note.endFrame : note.startFrame + static_cast<std::uint64_t>(offset);
}

PitchEditError applyDisplacement(const PitchAnalysis& analysis, PitchEditState& state,
                                 std::span<const std::size_t> selection, double displacement, bool snap) {
    if (!snap && displacement == 0) return PitchEditError::None;

    std::vector<PitchEditCommand> commands;
    commands.reserve(selection.size());
    for (const auto index : selection) {
        const auto shift = resolvePitchNoteEdit(analysis, state, index).pitchShiftCents;
        const auto original = analysis.notes[index].pitchCents;
        auto moved = shift + displacement;
        if (snap) moved = nearestSemitone(original + moved) - original;
        commands.push_back({index, PitchSetShift{moved}});
    }
    return applyPitchEdits(analysis, state, commands);
}

std::optional<std::size_t> nextByDestination(const PitchAnalysis& analysis, const PitchEditState& state,
                                              std::size_t anchor) noexcept {
    const auto end = resolvePitchNoteEdit(analysis, state, anchor).endFrame;
    auto nearestStart = analysis.sourceFrames;
    std::optional<std::size_t> next;
    for (std::size_t index = 0; index < analysis.notes.size(); ++index) {
        const auto start = resolvePitchNoteEdit(analysis, state, index).startFrame;
        if (start >= end && start < nearestStart) {
            nearestStart = start;
            next = index;
        }
    }
    return next;
}

PitchEditError validateBoundaryTarget(const PitchAnalysis& analysis, const PitchEditState& state,
                                      std::size_t noteIndex, PitchBoundary boundary) {
    const auto current = validatePitchEdits(analysis, state);
    if (current != PitchEditError::None) return current;
    if (noteIndex >= analysis.notes.size()) return PitchEditError::UnknownNote;
    if (boundary != PitchBoundary::Start && boundary != PitchBoundary::End) return PitchEditError::InvalidValue;
    return PitchEditError::None;
}

PitchEditError applyBoundaryRetime(const PitchAnalysis& analysis, PitchEditState& state, const PitchNoteEdit& note,
                                   PitchBoundary boundary, std::uint64_t destinationFrame) {
    const bool start = boundary == PitchBoundary::Start;
    const auto previousFrame = start ? note.startFrame : note.endFrame;
    if (destinationFrame == previousFrame) return PitchEditError::None;

    const auto timing = start ? PitchSetTiming{destinationFrame, note.endFrame} :
                                PitchSetTiming{note.startFrame, destinationFrame};
    std::array commands{PitchEditCommand{note.noteIndex, timing}, PitchEditCommand{}};
    for (std::size_t index = 0; index < analysis.notes.size(); ++index) {
        const auto adjacent = resolvePitchNoteEdit(analysis, state, index);
        if (start && adjacent.endFrame == previousFrame) {
            commands[1] = {index, PitchSetTiming{adjacent.startFrame, destinationFrame}};
            return applyPitchEdits(analysis, state, commands);
        }
        if (!start && adjacent.startFrame == previousFrame) {
            commands[1] = {index, PitchSetTiming{destinationFrame, adjacent.endFrame}};
            return applyPitchEdits(analysis, state, commands);
        }
    }
    return applyPitchEdits(analysis, state, std::span(commands).first(1));
}

} // namespace

PitchEditError movePitchSelection(const PitchAnalysis& analysis, PitchEditState& state,
                                  std::span<const std::size_t> selection, double anchorTargetCents, PitchSnapMode snap) {
    const auto current = validateSelection(analysis, state, selection);
    if (current != PitchEditError::None || selection.empty()) return current;
    if (!std::isfinite(anchorTargetCents)) return PitchEditError::InvalidValue;

    const auto anchorPitch = currentPitch(analysis, state, selection.front());
    auto displacement = anchorTargetCents - anchorPitch;
    switch (snap) {
        case PitchSnapMode::Absolute:
        case PitchSnapMode::Free:
            break;
        case PitchSnapMode::Relative:
            displacement = nearestSemitone(anchorTargetCents) - nearestSemitone(anchorPitch);
            break;
        default:
            return PitchEditError::InvalidValue;
    }
    return applyDisplacement(analysis, state, selection, displacement, snap == PitchSnapMode::Absolute);
}

PitchEditError nudgePitchSelection(const PitchAnalysis& analysis, PitchEditState& state,
                                   std::span<const std::size_t> selection, double deltaCents) {
    const auto current = validateSelection(analysis, state, selection);
    if (current != PitchEditError::None || selection.empty()) return current;
    if (!std::isfinite(deltaCents)) return PitchEditError::InvalidValue;
    return applyDisplacement(analysis, state, selection, deltaCents, false);
}

PitchMidiAssignment assignPitchSelectionFromMidi(const PitchAnalysis& analysis, PitchEditState& state,
                                                 std::span<const std::size_t> selection, std::uint8_t key, bool step) {
    // An invalid MIDI key still observes the worker and selection preconditions.
    if (key < kLowestMidiAssignmentNote || key > kHighestMidiAssignmentNote) {
        const auto current = validateSelection(analysis, state, selection);
        return {current == PitchEditError::None && !selection.empty() ? PitchEditError::InvalidValue : current, {}};
    }
    const auto applied = movePitchSelection(analysis, state, selection, key * kCentsPerSemitone, PitchSnapMode::Free);
    if (applied != PitchEditError::None || selection.empty() || !step) return {applied, {}};
    return {applied, nextByDestination(analysis, state, selection.front())};
}

PitchEditError correctPitchSelection(const PitchAnalysis& analysis, PitchEditState& state,
                                     std::span<const std::size_t> selection, const PitchQuantize& capture,
                                     double amount) {
    const auto current = validateSelection(analysis, state, selection);
    if (current != PitchEditError::None || selection.empty()) return current;
    if (!(amount >= 0 && amount <= 1)) return PitchEditError::InvalidValue;
    if (amount == 0) return PitchEditError::None;

    std::vector<PitchEditCommand> commands;
    commands.reserve(selection.size());
    for (const auto index : selection) {
        const auto shift = resolvePitchNoteEdit(analysis, state, index).pitchShiftCents;
        const auto original = analysis.notes[index].pitchCents;
        const auto target = capture.scale.nearest(original + shift, capture.tonicCents, true);
        if (!target) return PitchEditError::InvalidScale;
        commands.push_back({index, PitchSetShift{std::lerp(shift, target->cents - original, amount)}});
    }
    return applyPitchEdits(analysis, state, commands);
}

PitchEditError setPitchStraightenRangeForSelection(const PitchAnalysis& analysis, PitchEditState& state,
                                                   std::span<const std::size_t> selection,
                                                   double startFraction, double endFraction) {
    const auto current = validateSelection(analysis, state, selection);
    if (current != PitchEditError::None || selection.empty()) return current;
    if (!(0 <= startFraction && startFraction < endFraction && endFraction <= 1)) return PitchEditError::InvalidValue;

    std::vector<PitchEditCommand> commands;
    commands.reserve(selection.size());
    for (const auto index : selection) {
        const auto& note = analysis.notes[index];
        const PitchStraightenRange range{frameAtFraction(note, startFraction), frameAtFraction(note, endFraction)};
        if (range.startFrame >= range.endFrame) return PitchEditError::InvalidValue;
        commands.push_back({index, PitchSetStraightenRange{range}});
    }
    return applyPitchEdits(analysis, state, commands);
}

PitchEditError retimePitchBoundary(const PitchAnalysis& analysis, PitchEditState& state, std::size_t noteIndex,
                                  PitchBoundary boundary, std::uint64_t destinationFrame) {
    const auto current = validateBoundaryTarget(analysis, state, noteIndex, boundary);
    if (current != PitchEditError::None) return current;
    return applyBoundaryRetime(analysis, state, resolvePitchNoteEdit(analysis, state, noteIndex), boundary, destinationFrame);
}

PitchEditError nudgePitchBoundary(const PitchAnalysis& analysis, PitchEditState& state, std::size_t noteIndex,
                                 PitchBoundary boundary, std::int64_t deltaFrames) {
    const auto current = validateBoundaryTarget(analysis, state, noteIndex, boundary);
    if (current != PitchEditError::None || deltaFrames == 0) return current;
    const auto note = resolvePitchNoteEdit(analysis, state, noteIndex);
    const auto frame = boundary == PitchBoundary::Start ? note.startFrame : note.endFrame;
    if (deltaFrames < 0) {
        // Add one before negating to handle the minimum signed displacement.
        const auto distance = static_cast<std::uint64_t>(-(deltaFrames + 1)) + 1;
        if (distance > frame) return PitchEditError::InvalidValue;
        return applyBoundaryRetime(analysis, state, note, boundary, frame - distance);
    }
    const auto distance = static_cast<std::uint64_t>(deltaFrames);
    if (distance > analysis.sourceFrames - frame) return PitchEditError::InvalidValue;
    return applyBoundaryRetime(analysis, state, note, boundary, frame + distance);
}

PitchEditError resetPitchSelection(const PitchAnalysis& analysis, PitchEditState& state,
                                   std::span<const std::size_t> selection, PitchResetDimension dimension) {
    const auto current = validateSelection(analysis, state, selection);
    if (current != PitchEditError::None) return current;

    const bool allNotes = selection.empty();
    const auto count = allNotes ? state.notes.size() : selection.size();
    std::vector<PitchEditCommand> commands;
    commands.reserve(count);
    for (std::size_t position = 0; position < count; ++position) {
        const auto index = allNotes ? state.notes[position].noteIndex : selection[position];
        switch (dimension) {
            case PitchResetDimension::PitchCenter:
                commands.push_back({index, PitchSetShift{0}});
                break;
            case PitchResetDimension::PitchCurve:
                commands.push_back({index, PitchSetStraighten{0}});
                commands.push_back({index, PitchSetStraightenRange{std::nullopt}});
                commands.push_back({index, PitchSetTilt{0}});
                commands.push_back({index, PitchSetTiltGeometry{}});
                commands.push_back({index, PitchSetDrift{1, false}});
                commands.push_back({index, PitchSetTransition{0}});
                break;
            case PitchResetDimension::Formant:
                commands.push_back({index, PitchSetFormant{0}});
                break;
            case PitchResetDimension::Volume:
                commands.push_back({index, PitchSetGain{0}});
                break;
            case PitchResetDimension::Timing: {
                const auto& note = analysis.notes[index];
                commands.push_back({index, PitchSetTiming{note.startFrame, note.endFrame}});
                break;
            }
            case PitchResetDimension::All:
                commands.push_back({index, PitchReset{}});
                break;
            default:
                return PitchEditError::InvalidValue;
        }
    }
    return applyPitchEdits(analysis, state, commands);
}

} // namespace adi::audio
