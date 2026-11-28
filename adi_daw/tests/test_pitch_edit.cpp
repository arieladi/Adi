// SPDX-License-Identifier: AGPL-3.0-or-later
#include "adi/audio/io_audit.hpp"
#include "adi/audio/pitch_edit.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>
#include <numbers>

namespace {

using namespace adi::audio;
int checks = 0;
int failures = 0;

void check(bool satisfied, const char* description) {
    ++checks;
    if (!satisfied) {
        ++failures;
        std::printf("FAIL %s\n", description);
    }
}

bool near(double actual, double expected) {
    return std::abs(actual - expected) < 1e-9;
}

PitchAnalysis fixture() {
    return {48000, 1000, {
        {100, 300, 6110, 40, 10, {{100, 6090}, {200, 6130}, {299, 6129.8}}},
        {300, 500, 6400, 0, 0, {{300, 6400}, {499, 6400}}}
    }};
}

void contourAndLevel() {
    const auto source = fixture();
    const auto& note = source.notes.front();
    PitchEditState state;
    auto resolved = resolvePitchNoteEdit(source, state, 0);
    check(resolved.startFrame == 100 && resolved.endFrame == 300 &&
          pitchCorrectionCents(note, resolved, 200) == 0, "unmodified note resolves to identity");

    const PitchEditCommand corrections[] = {
        {0, PitchSetShift{100.25}}, {0, PitchSetStraighten{1}}, {0, PitchSetTilt{30}},
        {0, PitchSetFormant{-175}}, {0, PitchSetGain{-6}}
    };
    check(applyPitchEdits(source, state, corrections) == PitchEditError::None && state.notes.size() == 1,
          "batch captures five controls in one sparse row");
    resolved = resolvePitchNoteEdit(source, state, 0);
    const auto first = note.curve.front().cents + pitchCorrectionCents(note, resolved, 100);
    const auto last = note.curve.back().cents + pitchCorrectionCents(note, resolved, 299);
    check(near((first + last) / 2, 6210.25) && near(last - first, 30),
          "straightening, fractional transposition and centered tilt combine in cents");
    check(resolved.formantShiftCents == -175 && resolved.gainDb == -6,
          "formant and level remain independent renderer controls");
    std::printf("Pitch center moved %.2f cents and the tilt spans %.2f cents\n",
                (first + last) / 2 - note.pitchCents, last - first);

    const PitchEditCommand mute[] = {{0, PitchSetGain{-std::numeric_limits<double>::infinity()}}};
    check(applyPitchEdits(source, state, mute) == PitchEditError::None &&
          resolvePitchNoteEdit(source, state, 0).gainDb == -std::numeric_limits<double>::infinity(),
          "mute is negative infinite gain");
    const PitchEditCommand reset[] = {{0, PitchReset{}}};
    check(applyPitchEdits(source, state, reset) == PitchEditError::None && state.notes.empty(),
          "reset restores an absent edit row");
}

void driftAndTransition() {
    const auto source = fixture();
    PitchEditState state;
    const PitchEditCommand preserve[] = {{0, PitchSetDrift{0, true}}};
    check(applyPitchEdits(source, state, preserve) == PitchEditError::None, "drift edit can retain expression");
    auto resolved = resolvePitchNoteEdit(source, state, 0);
    const auto& first = source.notes.front();
    check(near(pitchCorrectionCents(first, resolved, 100), 20) &&
          near(pitchCorrectionCents(first, resolved, 200), 0) &&
          near(pitchCorrectionCents(first, resolved, 299), -19.8),
          "drift edit removes fitted linear trend while retaining residual");

    const PitchEditCommand remove[] = {{0, PitchSetDrift{0, false}}};
    check(applyPitchEdits(source, state, remove) == PitchEditError::None, "drift edit can remove expression");
    resolved = resolvePitchNoteEdit(source, state, 0);
    check(near(pitchCorrectionCents(first, resolved, 200), -20) &&
          near(pitchCorrectionCents(first, resolved, 150), 0),
          "zero drift attenuates the full interpolated curve without preservation");

    const PitchEditCommand transition[] = {
        {0, PitchSetShift{-100}}, {1, PitchSetShift{100}}, {1, PitchSetTransition{6000}}
    };
    check(applyPitchEdits(source, state, transition) == PitchEditError::None,
          "incoming transition captures duration on the destination frame clock");
    const auto preceding = resolvePitchNoteEdit(source, state, 0);
    const auto current = resolvePitchNoteEdit(source, state, 1);
    const auto& second = source.notes[1];
    check(near(pitchCorrectionCents(second, current, 300, &preceding), -100) &&
          near(pitchCorrectionCents(second, current, 350, &preceding), 0) &&
          near(pitchCorrectionCents(second, current, 400, &preceding), 100),
          "transition reaches target by half the current note even for a longer requested ramp");
    check(near(pitchCorrectionCents(second, current, 300), 100), "isolated note has no incoming transition");
    auto untransposed = current;
    untransposed.pitchShiftCents = 0;
    check(near(pitchCorrectionCents(second, untransposed, 300, &preceding), 0),
          "transition has no effect on a note with zero transpose");
}

void driftPreservesVibrato() {
    constexpr std::uint32_t rate = 48000;
    std::vector<float> voice(rate);
    double phase = 0;
    for (std::size_t i = 0; i < voice.size(); ++i) {
        const auto progress = static_cast<double>(i) / rate;
        const auto deviation = 64 * (progress - 0.5) + 24 * std::cos(12 * std::numbers::pi * progress);
        const auto hz = 220 * std::exp2(deviation / 1200);
        const auto edge = std::min(i, voice.size() - 1 - i);
        const auto gain = std::min(1.0, static_cast<double>(edge) / 360);
        voice[i] = static_cast<float>(gain * (0.35 * std::sin(phase) + 0.17 * std::sin(2 * phase + 0.4)));
        phase = std::remainder(phase + 2 * std::numbers::pi * hz / rate, 2 * std::numbers::pi);
    }
    const auto measured = analyzePitch(voice, rate);
    check(measured && measured.analysis->notes.size() == 1, "the modulated voice has one detected note");
    if (!measured || measured.analysis->notes.size() != 1) return;
    const auto& analysis = *measured.analysis;
    const auto& source = analysis.notes.front();
    check(source.driftCents > 50 && source.driftCents < 80 &&
          source.vibratoCents > 12 && source.vibratoCents < 22,
          "the voice fixture has measurable drift and vibrato");

    std::vector<double> scratch;
    const auto measureCorrection = [&](const PitchEditState& state) {
        auto corrected = source;
        const auto edit = resolvePitchNoteEdit(analysis, state, 0);
        for (auto& point : corrected.curve)
            point.cents += pitchCorrectionCents(source, edit, static_cast<double>(point.frame));
        check(summarizePitchNote(corrected, scratch), "corrected observations can be measured");
        return corrected;
    };
    PitchEditState state;
    const PitchEditCommand half{0, PitchSetDrift{0.5}};
    check(applyPitchEdits(analysis, state, std::span{&half, 1}) == PitchEditError::None, "ordinary drift edits are valid");
    const auto halfway = measureCorrection(state);
    check(std::abs(halfway.driftCents - source.driftCents / 2) < 1e-7 &&
          std::abs(halfway.vibratoCents - source.vibratoCents) < 1e-7,
          "half drift leaves the full measured vibrato");

    const PitchEditCommand straightenDrift{0, PitchSetDrift{0}};
    check(applyPitchEdits(analysis, state, std::span{&straightenDrift, 1}) == PitchEditError::None,
          "ordinary drift straightening is valid");
    const auto straightened = measureCorrection(state);
    check(std::abs(straightened.driftCents) < 1e-7 &&
          std::abs(straightened.vibratoCents - source.vibratoCents) < 1e-7,
          "straightening drift removes the measured slope and retains vibrato");
    std::printf("Drift slope %.4f -> %.4f cents, vibrato residual RMS %.4f -> %.4f cents\n",
                source.driftCents, straightened.driftCents, source.vibratoCents, straightened.vibratoCents);

    const PitchEditCommand neutral{0, PitchSetDrift{1}};
    check(applyPitchEdits(analysis, state, std::span{&neutral, 1}) == PitchEditError::None && state.notes.empty() &&
          applyPitchEdits(analysis, state, std::span{&neutral, 1}) == PitchEditError::None && state.notes.empty(),
          "ordinary drift reset is neutral and leaves no sparse row on repeated application");

    const PitchEditCommand straightenCurve{0, PitchSetStraighten{1}};
    check(applyPitchEdits(analysis, state, std::span{&straightenCurve, 1}) == PitchEditError::None,
          "whole-curve straightening still has a separate command");
    const auto flattened = measureCorrection(state);
    check(std::abs(flattened.driftCents) < 1e-7 && flattened.vibratoCents < 1e-7,
          "whole-curve straightening removes both slope and vibrato");
}

void anchoredTiltAndRangedStraighten() {
    const auto source = fixture();
    const auto& note = source.notes.front();
    PitchEditState state;
    const PitchEditCommand twoSides[] = {{0, PitchSetTiltGeometry{{0.25, 60, -30}}}};
    check(applyPitchEdits(source, state, twoSides) == PitchEditError::None, "two independent tilt handles are stored");
    auto edit = resolvePitchNoteEdit(source, state, 0);
    constexpr double anchorFrame = 149.75;
    check(near(pitchCorrectionCents(note, edit, 100), 60) && near(pitchCorrectionCents(note, edit, anchorFrame), 0) &&
          near(pitchCorrectionCents(note, edit, 299), -30), "tilt handles apply their signed cents at their ends and preserve the anchor");
    const PitchEditCommand rotate[] = {{0, PitchSetTiltGeometry{{0.25, 0, 0}}}, {0, PitchSetTilt{80}}};
    check(applyPitchEdits(source, state, rotate) == PitchEditError::None, "rotation uses the chosen anchor");
    edit = resolvePitchNoteEdit(source, state, 0);
    check(near(pitchCorrectionCents(note, edit, 100), -20) && near(pitchCorrectionCents(note, edit, anchorFrame), 0) &&
          near(pitchCorrectionCents(note, edit, 299), 60), "rotate changes both sides around the selected pivot");

    const PitchEditCommand correctRange[] = {
        {0, PitchReset{}}, {0, PitchSetStraighten{1}}, {0, PitchSetStraightenRange{PitchStraightenRange{160, 240}}}
    };
    check(applyPitchEdits(source, state, correctRange) == PitchEditError::None, "straightening captures a source range");
    edit = resolvePitchNoteEdit(source, state, 0);
    check(near(pitchCorrectionCents(note, edit, 159), 0) && near(pitchCorrectionCents(note, edit, 160), 0) &&
          near(pitchCorrectionCents(note, edit, 164), -2.8) && near(pitchCorrectionCents(note, edit, 200), -20) &&
          near(pitchCorrectionCents(note, edit, 240), 0), "ranged correction preserves outside pitch, blends the edges and flattens the interior");

    const PitchEditCommand move[] = {{0, PitchSetTiming{700, 1000}}};
    check(applyPitchEdits(source, state, move) == PitchEditError::None &&
          resolvePitchNoteEdit(source, state, 0).straightenRange == edit.straightenRange,
          "warping destination timing preserves the authored source range");
    const PitchEditCommand fullRange[] = {{0, PitchSetStraightenRange{PitchStraightenRange{100, 300}}}};
    check(applyPitchEdits(source, state, fullRange) == PitchEditError::None &&
          !resolvePitchNoteEdit(source, state, 0).straightenRange.has_value(), "a full source range uses the existing whole-note identity");
    std::printf("Anchored end offsets +60/-30 cents, rotation -20/+60 cents, selected-range interior correction -20 cents\n");
}

void scaleAndTiming() {
    const auto source = fixture();
    PitchEditState state;
    adi::engine::ClipScale scale;
    std::string error;
    const auto major = adi::engine::compileScaleContext(scale, {}, error);
    check(major.has_value(), "major scale compiles");
    if (!major) return;

    const PitchEditCommand quantize[] = {{0, PitchSetShift{210}}, {0, PitchQuantize{*major, 6000}}};
    check(applyPitchEdits(source, state, quantize) == PitchEditError::None &&
          near(resolvePitchNoteEdit(source, state, 0).pitchShiftCents, 290),
          "quantization snaps the shifted nominal pitch");

    scale.tuningId = 5;
    scale.degrees = {2, 9};
    const adi::engine::TuningDefinition tuning{
        5, "Fixture", "scala", {}, 1901.955, {{0, 0, "root"}, {2, 175.5, "two"}, {9, 500.25, "nine"}}
    };
    const auto custom = adi::engine::compileScaleContext(scale, tuning, error);
    check(custom.has_value(), "non-octave scale compiles");
    if (!custom) return;
    const PitchEditCommand microtonal[] = {{0, PitchSetShift{0}}, {0, PitchQuantize{*custom, 6000}}};
    check(applyPitchEdits(source, state, microtonal) == PitchEditError::None &&
          near(resolvePitchNoteEdit(source, state, 0).pitchShiftCents, 65.5),
          "quantization respects fractional tuning degrees");

    const auto before = state.notes;
    const PitchEditCommand overlap[] = {{0, PitchSetShift{999}}, {0, PitchSetTiming{320, 520}}};
    check(applyPitchEdits(source, state, overlap) == PitchEditError::Overlap && state.notes == before,
          "overlap rolls back preceding controls in the batch");

    const PitchEditCommand retime[] = {{1, PitchSetTiming{100, 300}}, {0, PitchSetTiming{700, 1000}}};
    check(applyPitchEdits(source, state, retime) == PitchEditError::None && state.notes.size() == 2,
          "a combined move and stretch checks the final layout atomically");
    const auto first = resolvePitchNoteEdit(source, state, 0);
    const auto second = resolvePitchNoteEdit(source, state, 1);
    check(first.startFrame == 700 && first.endFrame == 1000 && second.startFrame == 100,
          "note identity is independent of destination time order");
    std::printf("Note moved %.2f ms and stretched %.2fx\n",
                (static_cast<double>(first.startFrame) - static_cast<double>(source.notes[0].startFrame)) * 1000 / source.sampleRate,
                static_cast<double>(first.endFrame - first.startFrame) /
                    static_cast<double>(source.notes[0].endFrame - source.notes[0].startFrame));
}

void rejectsMalformedInput() {
    const auto source = fixture();
    PitchEditState state;
    const auto nan = std::numeric_limits<double>::quiet_NaN();
    const PitchEditCommand invalid[] = {
        {0, PitchSetShift{nan}}, {0, PitchSetStraighten{-1}}, {0, PitchSetTilt{nan}},
        {0, PitchSetFormant{1201}}, {0, PitchSetGain{19}}, {0, PitchSetDrift{1.1, true}},
        {0, PitchSetTransition{9601}}, {0, PitchSetTiming{500, 500}}, {0, PitchSetTiming{900, 1001}},
        {0, PitchSetTiltGeometry{{nan, 0, 0}}}, {0, PitchSetTiltGeometry{{0, 10, 0}}},
        {0, PitchSetTiltGeometry{{1, 0, 10}}}, {0, PitchSetTiltGeometry{{0.5, 0, 2401}}},
        {0, PitchSetStraightenRange{PitchStraightenRange{120, 120}}},
        {0, PitchSetStraightenRange{PitchStraightenRange{99, 120}}},
        {0, PitchSetStraightenRange{PitchStraightenRange{120, 301}}}
    };
    for (const auto& command : invalid) {
        check(applyPitchEdits(source, state, std::span{&command, 1}) == PitchEditError::InvalidValue &&
              state.notes.empty(), "out-of-domain edit leaves no state");
    }
    const PitchEditCommand unknown[] = {{2, PitchReset{}}};
    check(applyPitchEdits(source, state, unknown) == PitchEditError::UnknownNote, "unknown note refuses");
    auto malformed = source;
    malformed.notes.front().curve.clear();
    check(validatePitchEdits(malformed, state) == PitchEditError::InvalidAnalysis, "missing pitch curve refuses");

    auto row = resolvePitchNoteEdit(source, state, 0);
    row.pitchShiftCents = 100;
    const PitchEditState duplicate{{row, row}};
    check(validatePitchEdits(source, duplicate) == PitchEditError::InvalidState, "duplicate authored row refuses");

    adi::engine::ClipScale scale;
    scale.mask = 0;
    std::string error;
    const auto emptyScale = adi::engine::compileScaleContext(scale, {}, error);
    if (emptyScale) {
        const PitchEditCommand empty[] = {{0, PitchQuantize{*emptyScale, 6000}}};
        check(applyPitchEdits(source, state, empty) == PitchEditError::InvalidScale && state.notes.empty(),
              "empty scale does not silently become chromatic");
    } else {
        check(false, "empty membership still compiles as a scale context");
    }

    const CallbackScope callback;
    const PitchEditCommand valid[] = {{0, PitchSetShift{100}}};
    check(applyPitchEdits(source, state, valid) == PitchEditError::Realtime && state.notes.empty(),
          "worker-only edit refuses the audio callback before mutation");
}

} // namespace

int main() {
    contourAndLevel();
    driftAndTransition();
    driftPreservesVibrato();
    anchoredTiltAndRangedStraighten();
    scaleAndTiming();
    rejectsMalformedInput();
    std::printf("%d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
