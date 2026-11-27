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
    if (snap != PitchSnapMode::Absolute && displacement == 0) return PitchEditError::None;

    std::vector<PitchEditCommand> commands;
    commands.reserve(selection.size());
    for (const auto index : selection) {
        const auto shift = resolvePitchNoteEdit(analysis, state, index).pitchShiftCents;
        const auto original = analysis.notes[index].pitchCents;
        auto moved = shift + displacement;
        if (snap == PitchSnapMode::Absolute) moved = nearestSemitone(original + moved) - original;
        commands.push_back({index, PitchSetShift{moved}});
    }
    return applyPitchEdits(analysis, state, commands);
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

PitchEditError retimePitchBoundary(const PitchAnalysis& analysis, PitchEditState& state, std::size_t noteIndex,
                                  PitchBoundary boundary, std::uint64_t destinationFrame) {
    const auto current = validatePitchEdits(analysis, state);
    if (current != PitchEditError::None) return current;
    if (noteIndex >= analysis.notes.size()) return PitchEditError::UnknownNote;
    if (boundary != PitchBoundary::Start && boundary != PitchBoundary::End) return PitchEditError::InvalidValue;

    const auto note = resolvePitchNoteEdit(analysis, state, noteIndex);
    const bool start = boundary == PitchBoundary::Start;
    const auto previousFrame = start ? note.startFrame : note.endFrame;
    if (destinationFrame == previousFrame) return PitchEditError::None;

    const auto timing = start ? PitchSetTiming{destinationFrame, note.endFrame} :
                                PitchSetTiming{note.startFrame, destinationFrame};
    std::array commands{PitchEditCommand{noteIndex, timing}, PitchEditCommand{}};
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
