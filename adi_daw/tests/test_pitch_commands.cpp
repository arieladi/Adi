// SPDX-License-Identifier: AGPL-3.0-or-later
#include "adi/audio/io_audit.hpp"
#include "adi/audio/pitch_commands.hpp"

#include <array>
#include <cmath>
#include <cstdio>
#include <limits>

namespace {

using namespace adi::audio;

int checks = 0;
int failures = 0;

void check(bool condition, const char* description) {
    ++checks;
    if (!condition) {
        ++failures;
        std::printf("FAIL %s\n", description);
    }
}

bool near(double left, double right) { return std::abs(left - right) < 1e-9; }

PitchAnalysis fixture() {
    return {48000, 12000, {
        {1000, 3000, 6130, 0, 0, {{1000, 6130}, {2999, 6130}}},
        {3000, 5000, 6415, 0, 0, {{3000, 6415}, {4999, 6415}}},
        {6000, 8000, 6680, 0, 0, {{6000, 6680}, {7999, 6680}}},
        {9000, 11000, 6912, 0, 0, {{9000, 6912}, {10999, 6912}}}
    }};
}

double nominal(const PitchAnalysis& analysis, const PitchEditState& state, std::size_t index) {
    return analysis.notes[index].pitchCents + resolvePitchNoteEdit(analysis, state, index).pitchShiftCents;
}

void selectionSnap() {
    const auto analysis = fixture();
    const std::array<std::size_t, 2> selected{1, 0};
    PitchEditState absolute, relative, free;
    check(movePitchSelection(analysis, absolute, selected, 6551.5, PitchSnapMode::Absolute) == PitchEditError::None &&
          near(nominal(analysis, absolute, 1), 6600) && near(nominal(analysis, absolute, 0), 6300),
          "absolute mode snaps each displaced target to its nearest semitone");
    check(movePitchSelection(analysis, relative, selected, 6551.5, PitchSnapMode::Relative) == PitchEditError::None &&
          near(nominal(analysis, relative, 1), 6615) && near(nominal(analysis, relative, 0), 6330),
          "relative mode preserves each note's current cents deviation");
    check(movePitchSelection(analysis, free, selected, 6551.5, PitchSnapMode::Free) == PitchEditError::None &&
          near(nominal(analysis, free, 1), 6551.5) && near(nominal(analysis, free, 0), 6266.5),
          "free mode applies a fractional anchor displacement to the entire selection");
    check(free.notes.size() == 2 && near(nominal(analysis, free, 2), 6680), "pitch changes are sparse and selection-scoped");
    const std::array<std::size_t, 1> one{2};
    check(movePitchSelection(analysis, absolute, one, 6750, PitchSnapMode::Absolute) == PitchEditError::None &&
          near(nominal(analysis, absolute, 2), 6700), "semitone ties select the lower pitch");

    const auto unchanged = free.notes;
    const std::array invalidSelection{std::size_t{1}, analysis.notes.size()};
    check(movePitchSelection(analysis, free, invalidSelection, 6400, PitchSnapMode::Free) == PitchEditError::UnknownNote &&
          free.notes == unchanged, "an invalid member leaves the whole selection untouched");
    const std::array priorEdit{PitchEditCommand{0, PitchSetShift{2350}}};
    check(applyPitchEdits(analysis, free, priorEdit) == PitchEditError::None, "fixture has an earlier large edit");
    const auto nearLimit = free.notes;
    check(movePitchSelection(analysis, free, selected, 6700, PitchSnapMode::Free) == PitchEditError::InvalidValue &&
          free.notes == nearLimit, "a later member exceeding the edit bound rolls the gesture back");

    std::printf("Absolute targets %.0f and %.0f cents; relative detunes %.0f and %.0f cents\n",
                nominal(analysis, absolute, 1), nominal(analysis, absolute, 0),
                nominal(analysis, relative, 1) - 6600, nominal(analysis, relative, 0) - 6300);
}

void pitchNudge() {
    const auto analysis = fixture();
    const std::array<std::size_t, 3> selected{1, 0, 1};
    PitchEditState state;
    const std::array initial{PitchEditCommand{0, PitchSetShift{13.75}}, PitchEditCommand{0, PitchSetTilt{12}},
                             PitchEditCommand{0, PitchSetFormant{-125}}, PitchEditCommand{0, PitchSetDrift{0.6, true}}};
    check(applyPitchEdits(analysis, state, initial) == PitchEditError::None, "fixture authors independent controls");
    auto first = resolvePitchNoteEdit(analysis, state, 0);
    auto second = resolvePitchNoteEdit(analysis, state, 1);
    const auto priorShift = first.pitchShiftCents;
    first.pitchShiftCents += 0.125;
    second.pitchShiftCents += 0.125;
    check(nudgePitchSelection(analysis, state, selected, 0.125) == PitchEditError::None &&
          state.notes == std::vector{first, second}, "a fractional nudge preserves detuning and other authored controls");
    std::printf("Measured selected pitch nudge %+.3f cents\n",
                resolvePitchNoteEdit(analysis, state, 0).pitchShiftCents - priorShift);

    first.pitchShiftCents -= 0.375;
    second.pitchShiftCents -= 0.375;
    check(nudgePitchSelection(analysis, state, selected, -0.375) == PitchEditError::None &&
          state.notes == std::vector{first, second}, "a negative nudge applies once to each selected note");
    const auto previous = state.notes;
    check(nudgePitchSelection(analysis, state, selected, -0.0) == PitchEditError::None &&
          nudgePitchSelection(analysis, state, {}, 0.5) == PitchEditError::None && state.notes == previous,
          "zero and empty-selection nudges leave authored rows bit-stable");

    const std::array atBound{PitchEditCommand{1, PitchSetShift{2400}}};
    check(applyPitchEdits(analysis, state, atBound) == PitchEditError::None, "fixture authors the maximum shift");
    const auto bounded = state.notes;
    const std::array<std::size_t, 2> ordered{0, 1};
    check(nudgePitchSelection(analysis, state, ordered, 0.125) == PitchEditError::InvalidValue &&
          bounded == state.notes, "an out-of-range note rolls back the whole pitch nudge");
    check(nudgePitchSelection(analysis, state, ordered, std::numeric_limits<double>::quiet_NaN()) ==
          PitchEditError::InvalidValue && bounded == state.notes, "pitch nudges require finite cents");
}

void midiAssignment() {
    const auto analysis = fixture();
    const std::array<std::size_t, 2> selected{1, 0};
    PitchEditState state;
    const auto notes = assignPitchSelectionFromMidi(analysis, state, selected, 65, true);
    check(notes.error == PitchEditError::None && notes.nextNote == 2 &&
          near(nominal(analysis, state, 1), 6500) && near(nominal(analysis, state, 0), 6215),
          "MIDI places the first selected note exactly and applies the same interval to all others");

    const std::array swap{PitchEditCommand{0, PitchSetTiming{6000, 8000}},
                          PitchEditCommand{2, PitchSetTiming{1000, 3000}}};
    check(applyPitchEdits(analysis, state, swap) == PitchEditError::None, "fixture reorders notes on the destination timeline");
    const std::array<std::size_t, 1> anchor{1}, last{3};
    const auto next = assignPitchSelectionFromMidi(analysis, state, anchor, 65, true);
    check(next.error == PitchEditError::None && next.nextNote == 0, "step follows actual destination order after retiming");
    const auto done = assignPitchSelectionFromMidi(analysis, state, last, 69, true);
    check(done.error == PitchEditError::None && !done.nextNote, "step stops at the final note");
    const auto stepOff = assignPitchSelectionFromMidi(analysis, state, anchor, 66, false);
    check(stepOff.error == PitchEditError::None && !stepOff.nextNote, "MIDI step is optional");
    const auto upper = assignPitchSelectionFromMidi(analysis, state, last, 84, false);
    check(upper.error == PitchEditError::None && near(nominal(analysis, state, 3), 8400), "MIDI includes the documented upper bound");

    const auto previous = state.notes;
    const auto outOfRange = assignPitchSelectionFromMidi(analysis, state, anchor, 85, true);
    check(outOfRange.error == PitchEditError::InvalidValue && !outOfRange.nextNote && state.notes == previous,
          "MIDI outside the documented range fails without edit or cursor advance");
}

void partialCorrection() {
    const auto analysis = fixture();
    const std::array<std::size_t, 2> selected{0, 2};
    std::string error;
    const auto scale = adi::engine::compileScaleContext({}, {}, error);
    check(scale.has_value(), "native major scale can be captured");
    if (!scale) return;
    const PitchQuantize capture{*scale, 6000};
    PitchEditState state;
    const std::array preset{PitchEditCommand{0, PitchSetShift{20}}, PitchEditCommand{0, PitchSetTilt{30}}};
    check(applyPitchEdits(analysis, state, preset) == PitchEditError::None, "fixture has pitch and contour edits");
    const auto before = state.notes;
    check(correctPitchSelection(analysis, state, selected, capture, 0) == PitchEditError::None && state.notes == before,
          "zero correction has no effect");
    check(correctPitchSelection(analysis, state, selected, capture, 0.25) == PitchEditError::None &&
          near(nominal(analysis, state, 0), 6162.5) && near(nominal(analysis, state, 2), 6685) &&
          near(resolvePitchNoteEdit(analysis, state, 0).tiltCents, 30),
          "partial correction moves nominal pitch by one quarter of the scale distance");
    std::printf("Quarter correction moved the detuned note by %+.2f cents\n", nominal(analysis, state, 0) - 6150);
    check(correctPitchSelection(analysis, state, selected, capture, 1) == PitchEditError::None &&
          near(nominal(analysis, state, 0), 6200) && near(nominal(analysis, state, 2), 6700),
          "full correction reaches the nearest native scale tones");
    const auto corrected = state.notes;
    const PitchQuantize noTonic{*scale, std::numeric_limits<double>::quiet_NaN()};
    check(correctPitchSelection(analysis, state, selected, noTonic, 0.5) == PitchEditError::InvalidScale &&
          correctPitchSelection(analysis, state, selected, capture, 1.01) == PitchEditError::InvalidValue &&
          state.notes == corrected, "invalid scale capture and out-of-range amounts preserve the prior state");
    const std::array existing{PitchEditCommand{0, PitchSetShift{-30}}};
    const std::array<std::size_t, 1> first{0};
    check(applyPitchEdits(analysis, state, existing) == PitchEditError::None &&
          correctPitchSelection(analysis, state, first, capture, 1) == PitchEditError::None &&
          near(nominal(analysis, state, 0), 6000), "captured scale resolves an equal distance to its lower degree");
}

void normalizedStraightenRange() {
    const PitchAnalysis analysis{48000, 4000, {
        {100, 500, 6100, 0, 0, {{100, 6090}, {300, 6140}, {499, 6130}}},
        {900, 1702, 6400, 0, 0, {{900, 6380}, {1300, 6440}, {1701, 6420}}},
        {3000, 3001, 6500, 0, 0, {{3000, 6500}}},
    }};
    PitchEditState state;
    const std::array initial{PitchEditCommand{0, PitchSetStraighten{0.5}}, PitchEditCommand{1, PitchSetStraighten{1}},
                             PitchEditCommand{1, PitchSetTiming{2500, 2800}}, PitchEditCommand{2, PitchSetGain{-3}}};
    check(applyPitchEdits(analysis, state, initial) == PitchEditError::None, "fixture has distinct amounts and retimed destination geometry");
    const std::array<std::size_t, 3> selection{1, 0, 1};
    const auto before = state.notes;
    if (before.size() != 3) return;
    check(setPitchStraightenRangeForSelection(analysis, state, selection, 0.25, 0.75) == PitchEditError::None,
          "one normalized range maps atomically onto multiple selected source durations");
    auto first = before[0];
    auto second = before[1];
    first.straightenRange = PitchStraightenRange{200, 400};
    second.straightenRange = PitchStraightenRange{1101, 1502};
    check(state.notes == std::vector{first, second, before[2]},
          "nearest-frame, half-up mapping changes ranges only, even with a repeated selected index");
    const auto firstResolved = resolvePitchNoteEdit(analysis, state, 0);
    const auto secondResolved = resolvePitchNoteEdit(analysis, state, 1);
    const auto firstCorrection = pitchCorrectionCents(analysis.notes[0], firstResolved, 300);
    const auto secondCorrection = pitchCorrectionCents(analysis.notes[1], secondResolved, 1300);
    check(near(firstCorrection, -20) && near(secondCorrection, -40) &&
          near(pitchCorrectionCents(analysis.notes[0], firstResolved, 150), 0) &&
          near(pitchCorrectionCents(analysis.notes[1], secondResolved, 1502), 0),
          "both ranges shape their own interior and leave the contour outside unchanged");
    if (firstResolved.straightenRange && secondResolved.straightenRange) {
        const auto firstRange = *firstResolved.straightenRange;
        const auto secondRange = *secondResolved.straightenRange;
        std::printf("Normalized 25–75%% selected source ranges [%llu,%llu) and [%llu,%llu), interior correction %+.2f/%+.2f cents\n",
                    static_cast<unsigned long long>(firstRange.startFrame), static_cast<unsigned long long>(firstRange.endFrame),
                    static_cast<unsigned long long>(secondRange.startFrame), static_cast<unsigned long long>(secondRange.endFrame),
                    firstCorrection, secondCorrection);
    }

    const auto ranged = state.notes;
    const std::array<std::size_t, 2> collapsed{0, 2}, unknown{0, 3};
    check(setPitchStraightenRangeForSelection(analysis, state, collapsed, 0.1, 0.2) == PitchEditError::InvalidValue &&
          state.notes == ranged, "a later note whose selected range collapses rolls back the whole gesture");
    check(setPitchStraightenRangeForSelection(analysis, state, unknown, 0.25, 0.5) == PitchEditError::UnknownNote &&
          state.notes == ranged, "an unknown selected index cannot partially update mapped notes");
    auto missingCurve = analysis;
    missingCurve.notes[1].curve.clear();
    check(setPitchStraightenRangeForSelection(missingCurve, state, selection, 0.25, 0.5) == PitchEditError::InvalidAnalysis &&
          state.notes == ranged, "a selected note without an analysis curve refuses before any mutation");
    const auto nan = std::numeric_limits<double>::quiet_NaN();
    check(setPitchStraightenRangeForSelection(analysis, state, selection, nan, 0.5) == PitchEditError::InvalidValue &&
          setPitchStraightenRangeForSelection(analysis, state, selection, 0.5, 0.5) == PitchEditError::InvalidValue &&
          setPitchStraightenRangeForSelection(analysis, state, selection, 0.75, 0.25) == PitchEditError::InvalidValue &&
          setPitchStraightenRangeForSelection(analysis, state, selection, -0.1, 1) == PitchEditError::InvalidValue &&
          setPitchStraightenRangeForSelection(analysis, state, selection, 0, 1.1) == PitchEditError::InvalidValue &&
          state.notes == ranged, "invalid or reversed normalized bounds leave captured edits untouched");
    check(setPitchStraightenRangeForSelection(analysis, state, {}, 0.25, 0.5) == PitchEditError::None &&
          state.notes == ranged, "empty selection leaves authored ranges unchanged");
    check(setPitchStraightenRangeForSelection(analysis, state, selection, 0, 1) == PitchEditError::None &&
          state.notes == before, "the full normalized range restores whole-note straightening without touching amounts");
}

void boundaryRetime() {
    const auto analysis = fixture();
    PitchEditState state;
    check(retimePitchBoundary(analysis, state, 0, PitchBoundary::End, 3400) == PitchEditError::None &&
          resolvePitchNoteEdit(analysis, state, 0).endFrame == 3400 &&
          resolvePitchNoteEdit(analysis, state, 1).startFrame == 3400 &&
          resolvePitchNoteEdit(analysis, state, 1).endFrame == 5000,
          "moving a shared end stretches the first note and sets the next start atomically");
    const auto newBoundary = static_cast<double>(resolvePitchNoteEdit(analysis, state, 0).endFrame);
    const auto originalBoundary = static_cast<double>(analysis.notes[0].endFrame);
    std::printf("Shared boundary moved %.0f frames (%.3f ms)\n", newBoundary - originalBoundary,
                (newBoundary - originalBoundary) * 1000 / analysis.sampleRate);
    check(retimePitchBoundary(analysis, state, 1, PitchBoundary::Start, 3200) == PitchEditError::None &&
          resolvePitchNoteEdit(analysis, state, 0).endFrame == 3200 &&
          resolvePitchNoteEdit(analysis, state, 1).startFrame == 3200,
          "moving a shared start changes the preceding end on the authored timeline");
    check(retimePitchBoundary(analysis, state, 1, PitchBoundary::End, 5300) == PitchEditError::None &&
          resolvePitchNoteEdit(analysis, state, 2).startFrame == 6000, "a gap isolates a neighbor from a boundary gesture");
    const auto previous = state.notes;
    check(retimePitchBoundary(analysis, state, 1, PitchBoundary::End, 6400) == PitchEditError::Overlap &&
          retimePitchBoundary(analysis, state, 1, PitchBoundary::Start, 1000) == PitchEditError::InvalidValue &&
          state.notes == previous, "crossing a gap or collapsing a contiguous neighbor fails atomically");

    const std::array swap{PitchEditCommand{0, PitchSetTiming{6000, 8000}},
                          PitchEditCommand{2, PitchSetTiming{1000, 3200}}};
    check(applyPitchEdits(analysis, state, swap) == PitchEditError::None &&
          retimePitchBoundary(analysis, state, 1, PitchBoundary::Start, 3333) == PitchEditError::None &&
          resolvePitchNoteEdit(analysis, state, 2).endFrame == 3333 &&
          resolvePitchNoteEdit(analysis, state, 0).endFrame == 8000,
          "neighbor matching uses the destination span even when source notes are reordered");
}

void boundaryNudge() {
    const auto analysis = fixture();
    PitchEditState state;
    const auto original = resolvePitchNoteEdit(analysis, state, 1).startFrame;
    check(nudgePitchBoundary(analysis, state, 1, PitchBoundary::Start, 1) == PitchEditError::None &&
          resolvePitchNoteEdit(analysis, state, 0).endFrame == original + 1 &&
          resolvePitchNoteEdit(analysis, state, 1).startFrame == original + 1,
          "a one-frame nudge moves both sides of the shared destination boundary");
    const auto moved = resolvePitchNoteEdit(analysis, state, 1).startFrame;
    std::printf("Measured shared boundary nudge %.0f frame (%.6f ms)\n",
                static_cast<double>(moved - original), static_cast<double>(moved - original) * 1000 / analysis.sampleRate);
    check(nudgePitchBoundary(analysis, state, 1, PitchBoundary::Start, -1) == PitchEditError::None &&
          state.notes.empty(), "undoing the frame displacement reaches the exact original sparse state");
    check(nudgePitchBoundary(analysis, state, 1, PitchBoundary::End, 0) == PitchEditError::None &&
          state.notes.empty(), "a zero-frame nudge leaves the state unchanged");

    check(nudgePitchBoundary(analysis, state, 1, PitchBoundary::End, 1001) == PitchEditError::Overlap &&
          state.notes.empty(), "a nudge across a gap refuses to overlap the next note");
    check(nudgePitchBoundary(analysis, state, 1, PitchBoundary::Start, std::numeric_limits<std::int64_t>::min()) ==
          PitchEditError::InvalidValue && state.notes.empty(), "an extreme negative nudge cannot wrap below zero");
    check(nudgePitchBoundary(analysis, state, 3, PitchBoundary::End, std::numeric_limits<std::int64_t>::max()) ==
          PitchEditError::InvalidValue && state.notes.empty(), "an extreme positive nudge cannot wrap beyond the source");
    check(nudgePitchBoundary(analysis, state, 3, PitchBoundary::End, 1000) == PitchEditError::None &&
          resolvePitchNoteEdit(analysis, state, 3).endFrame == analysis.sourceFrames,
          "the boundary may land exactly at the source end");
    const auto edge = state.notes;
    check(nudgePitchBoundary(analysis, state, 3, PitchBoundary::End, 1) == PitchEditError::InvalidValue &&
          edge == state.notes, "a one-frame nudge past the source end preserves the prior boundary");
}

void dimensionReset() {
    const auto analysis = fixture();
    const auto originalAnalysis = analysis;
    const std::array<std::size_t, 1> first{0}, second{1};
    PitchEditState state;
    const std::array edits{
        PitchEditCommand{0, PitchSetShift{100}}, PitchEditCommand{0, PitchSetTilt{30}},
        PitchEditCommand{0, PitchSetTiltGeometry{{0.4, 10, -20}}},
        PitchEditCommand{0, PitchSetStraighten{0.5}},
        PitchEditCommand{0, PitchSetStraightenRange{PitchStraightenRange{1200, 2500}}},
        PitchEditCommand{0, PitchSetDrift{0.5, true}}, PitchEditCommand{0, PitchSetTransition{80}},
        PitchEditCommand{0, PitchSetFormant{200}},
        PitchEditCommand{0, PitchSetGain{-std::numeric_limits<double>::infinity()}},
        PitchEditCommand{0, PitchSetTiming{1100, 2900}},
        PitchEditCommand{1, PitchSetShift{200}}, PitchEditCommand{1, PitchSetFormant{-300}},
        PitchEditCommand{1, PitchSetGain{-7}}, PitchEditCommand{1, PitchSetTiming{3200, 5300}},
    };
    check(applyPitchEdits(analysis, state, edits) == PitchEditError::None, "fixture authors independent edit dimensions");
    auto expectedFirst = resolvePitchNoteEdit(analysis, state, 0);
    auto expectedSecond = resolvePitchNoteEdit(analysis, state, 1);
    expectedFirst.pitchShiftCents = 0;
    check(resetPitchSelection(analysis, state, first, PitchResetDimension::PitchCenter) == PitchEditError::None &&
          resolvePitchNoteEdit(analysis, state, 0) == expectedFirst &&
          resolvePitchNoteEdit(analysis, state, 1) == expectedSecond,
          "pitch-center reset affects only selected centers");

    expectedFirst.straighten = 0;
    expectedFirst.straightenRange.reset();
    expectedFirst.tiltCents = 0;
    expectedFirst.tiltGeometry = {};
    expectedFirst.drift = 1;
    expectedFirst.preserveExpression = false;
    expectedFirst.transitionFrames = 0;
    check(resetPitchSelection(analysis, state, first, PitchResetDimension::PitchCurve) == PitchEditError::None &&
          resolvePitchNoteEdit(analysis, state, 0) == expectedFirst &&
          resolvePitchNoteEdit(analysis, state, 1) == expectedSecond && analysis == originalAnalysis,
          "curve reset removes authored expression and retains the source analysis and other edits");

    expectedFirst.formantShiftCents = 0;
    expectedSecond.formantShiftCents = 0;
    check(resetPitchSelection(analysis, state, {}, PitchResetDimension::Formant) == PitchEditError::None &&
          resolvePitchNoteEdit(analysis, state, 0) == expectedFirst &&
          resolvePitchNoteEdit(analysis, state, 1) == expectedSecond,
          "no-selection formant reset applies to every authored note");
    expectedFirst.gainDb = 0;
    check(resetPitchSelection(analysis, state, first, PitchResetDimension::Volume) == PitchEditError::None &&
          resolvePitchNoteEdit(analysis, state, 0) == expectedFirst &&
          resolvePitchNoteEdit(analysis, state, 1) == expectedSecond,
          "volume reset restores unity from mute and keeps the other note's gain");
    check(resetPitchSelection(analysis, state, second, PitchResetDimension::All) == PitchEditError::None &&
          state.notes == std::vector{expectedFirst}, "all-dimension reset can target one note");
    check(resetPitchSelection(analysis, state, {}, PitchResetDimension::All) == PitchEditError::None &&
          state.notes.empty(), "all-dimension reset without a selection clears the clip edits");

    const std::array swap{PitchEditCommand{0, PitchSetTiming{3000, 5000}},
                          PitchEditCommand{1, PitchSetTiming{1000, 3000}},
                          PitchEditCommand{0, PitchSetShift{125}}};
    check(applyPitchEdits(analysis, state, swap) == PitchEditError::None, "fixture swaps timing spans");
    const auto before = state.notes;
    check(resetPitchSelection(analysis, state, first, PitchResetDimension::Timing) == PitchEditError::Overlap &&
          before == state.notes, "a conflicting selected timing reset leaves authored rows intact");
    check(resetPitchSelection(analysis, state, {}, PitchResetDimension::Timing) == PitchEditError::None &&
          state.notes.size() == 1 && resolvePitchNoteEdit(analysis, state, 0).startFrame == 1000 &&
          resolvePitchNoteEdit(analysis, state, 1).startFrame == 3000 && near(nominal(analysis, state, 0), 6255),
          "resetting both timing spans validates the final layout and keeps the independent pitch");
}

void workerOnly() {
    const auto analysis = fixture();
    PitchEditState state;
    const std::array<std::size_t, 1> one{0};
    const auto before = state.notes;
    check(movePitchSelection(analysis, state, {}, 6200, PitchSnapMode::Absolute) == PitchEditError::None &&
          state.notes == before, "empty selection is a no-op");
    const CallbackScope callback;
    check(movePitchSelection(analysis, state, one, 6200, PitchSnapMode::Free) == PitchEditError::Realtime &&
          assignPitchSelectionFromMidi(analysis, state, one, 62, true).error == PitchEditError::Realtime &&
          retimePitchBoundary(analysis, state, 0, PitchBoundary::End, 3200) == PitchEditError::Realtime &&
          nudgePitchSelection(analysis, state, one, 0) == PitchEditError::Realtime &&
          nudgePitchBoundary(analysis, state, 0, PitchBoundary::End, 0) == PitchEditError::Realtime &&
          setPitchStraightenRangeForSelection(analysis, state, one, 0.25, 0.75) == PitchEditError::Realtime &&
          resetPitchSelection(analysis, state, {}, PitchResetDimension::All) == PitchEditError::Realtime &&
          state.notes == before, "callback refusal occurs before staging any commands");
}

} // namespace

int main() {
    selectionSnap();
    pitchNudge();
    midiAssignment();
    partialCorrection();
    normalizedStraightenRange();
    boundaryRetime();
    boundaryNudge();
    dimensionReset();
    workerOnly();
    std::printf("%s -- %d checks\n", failures == 0 ? "PASS" : "FAILED", checks);
    return failures == 0 ? 0 : 1;
}
