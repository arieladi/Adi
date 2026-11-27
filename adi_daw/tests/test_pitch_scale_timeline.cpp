// SPDX-License-Identifier: AGPL-3.0-or-later
#include "adi/audio/io_audit.hpp"
#include "adi/engine/scale_timeline.hpp"
#include "adi/pitch_scale_timeline.hpp"

#include <cmath>
#include <cstdio>
#include <limits>
#include <stdexcept>
#include <utility>

namespace {
using namespace adi;
using namespace adi::audio;
using namespace adi::clip_edit;

int checks = 0;
int failures = 0;

void check(bool passed, const char* description) {
    ++checks;
    if (!passed) {
        ++failures;
        std::printf("FAIL %s\n", description);
    }
}

engine::ScaleTimeline compile(std::span<const engine::ScaleKeyChange> keys,
                              std::span<const engine::TuningDefinition> tuning = {}) {
    std::string error;
    const auto timeline = engine::compileScaleTimeline(keys, tuning, error);
    if (!timeline) throw std::runtime_error(error);
    return *timeline;
}

PitchAnalysis source(double cents = 6510) {
    return {48000, 600, {{0, 100, cents, 0, 0, {{30, cents}}},
                        {200, 300, cents, 0, 0, {{230, cents}}},
                        {400, 500, cents, 0, 0, {{430, cents}}}}};
}

double pitch(const PitchAnalysis& analysis, const PitchEditState& state, std::size_t index) {
    return analysis.notes[index].pitchCents + resolvePitchNoteEdit(analysis, state, index).pitchShiftCents;
}

void quantizeAtAttacks() {
    engine::ClipScale dMajor;
    dMajor.root = 2;
    const engine::ScaleKeyChange keys[] = {{17, 100, dMajor}};
    const auto timeline = compile(keys);
    const auto notes = source();
    PitchEditState state;
    const PitchEditCommand seed[] = {{0, PitchSetTiming{120, 180}}, {0, PitchSetGain{-3}},
                                     {1, PitchSetShift{-9.75}}, {2, PitchSetGain{2}}};
    if (applyPitchEdits(notes, state, seed) != PitchEditError::None) throw std::runtime_error("seed timed pitch edits");
    const auto unaffected = resolvePitchNoteEdit(notes, state, 2);
    const PitchTimelineSelection selected[] = {{1, 100}, {0, 99}};
    check(quantizePitchSelectionInTimeline(notes, state, timeline, selected) == PitchEditError::None,
          "one batch quantizes segments projected on either side of a key change");
    check(pitch(notes, state, 0) == 6500 && pitch(notes, state, 1) == 6600,
          "the new scale is inclusive at the attack tick, with the earlier segment in C major");
    auto moved = resolvePitchNoteEdit(notes, state, 0);
    check(moved.startFrame == 120 && moved.endFrame == 180 && moved.gainDb == -3 &&
              resolvePitchNoteEdit(notes, state, 2) == unaffected,
          "scale quantization preserves timing, gain and unselected notes");
    const auto resolved = state.notes;
    check(quantizePitchSelectionInTimeline(notes, state, timeline, selected) == PitchEditError::None &&
              state.notes == resolved, "repeating a full-strength pitch quantization is idempotent");

    PitchEditState shifted;
    const PitchEditCommand beforeSnap[] = {{1, PitchSetShift{-35}}};
    if (applyPitchEdits(notes, shifted, beforeSnap) != PitchEditError::None) throw std::runtime_error("seed pitch shift");
    const PitchTimelineSelection shiftedNote[] = {{1, 100}};
    check(quantizePitchSelectionInTimeline(notes, shifted, timeline, shiftedNote) == PitchEditError::None &&
              pitch(notes, shifted, 1) == 6400, "the nearest allowed pitch is chosen from the already edited nominal pitch");

    constexpr std::int64_t lateTick = 9007199254740993LL;
    const engine::ScaleKeyChange lateKeys[] = {{19, lateTick, dMajor}};
    const auto late = compile(lateKeys);
    const PitchTimelineSelection addressed[] = {{0, lateTick - 1}, {1, lateTick}, {2, -1}};
    PitchEditState wide;
    check(quantizePitchSelectionInTimeline(notes, wide, late, addressed) == PitchEditError::None &&
              pitch(notes, wide, 0) == 6500 && pitch(notes, wide, 1) == 6600 && pitch(notes, wide, 2) == 6500,
          "attack ticks retain int64 boundary precision and pre-roll uses the timeline fallback");
}

void customAndBadBatches() {
    engine::ClipScale tritave;
    tritave.root = 4;
    tritave.tuningId = 7;
    tritave.degrees = {0, 2};
    const engine::TuningDefinition tuning[] = {{7, "Tritave", "scala", {}, 1901.955,
                                                 {{0, 0, "Unison"}, {2, 175.5, "Second"}}}};
    const engine::ScaleKeyChange keys[] = {{17, 100, tritave}};
    auto timeline = compile(keys, tuning);
    const auto notes = source(6250);
    PitchEditState state;
    const PitchEditCommand seed[] = {{0, PitchSetGain{3}}};
    if (applyPitchEdits(notes, state, seed) != PitchEditError::None) throw std::runtime_error("seed gain");
    const auto original = state.notes;
    const PitchTimelineSelection missing[] = {{0, 0}, {1, 100}};
    check(quantizePitchSelectionInTimeline(notes, state, timeline, missing) == PitchEditError::InvalidScale &&
              state.notes == original, "missing custom tonic rolls back a valid earlier selection");
    const PitchTimelineSelection invalid[] = {{0, 0}, {1, 100, std::numeric_limits<double>::infinity()}};
    check(quantizePitchSelectionInTimeline(notes, state, timeline, invalid) == PitchEditError::InvalidScale &&
              state.notes == original, "the existing edit validator rejects an invalid custom tonic atomically");
    const PitchTimelineSelection repeated[] = {{0, 0}, {0, 100, 6080.25}};
    check(quantizePitchSelectionInTimeline(notes, state, timeline, repeated) == PitchEditError::InvalidValue &&
              state.notes == original, "one segment cannot select two different loop occurrences");
    const PitchTimelineSelection unknown[] = {{0, 0}, {3, 100, 6080.25}};
    check(quantizePitchSelectionInTimeline(notes, state, timeline, unknown) == PitchEditError::UnknownNote &&
              state.notes == original, "an unknown effective note rolls back the batch");
    const PitchTimelineSelection valid[] = {{0, 0}, {1, 100, 6080.25}, {2, 110, 6081}};
    check(quantizePitchSelectionInTimeline(notes, state, timeline, valid) == PitchEditError::None &&
              std::abs(pitch(notes, state, 1) - 6255.75) < 1e-9 && std::abs(pitch(notes, state, 2) - 6256.5) < 1e-9,
          "fractional absolute tonics are captured per selection for the same non-octave key");
    const auto result = state.notes;
    const auto retained = std::move(timeline);
    check(quantizePitchSelectionInTimeline(notes, state, timeline, valid) == PitchEditError::InvalidScale &&
              state.notes == result && retained.atTicks(100), "an unavailable timeline refuses before mutation");
    PitchEditError error;
    {
        const CallbackScope callback;
        error = quantizePitchSelectionInTimeline(notes, state, retained, valid);
    }
    check(error == PitchEditError::Realtime && state.notes == result,
          "the command refuses callback use before capturing anything");
    check(quantizePitchSelectionInTimeline(notes, state, retained, {}) == PitchEditError::None &&
              state.notes == result, "an empty selection leaves existing pitch edits unchanged");
}
} // namespace

int main() {
    try {
        quantizeAtAttacks();
        customAndBadBatches();
    } catch (const std::exception& error) {
        check(false, error.what());
    }
    std::printf("%d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
