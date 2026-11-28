// SPDX-License-Identifier: AGPL-3.0-or-later
#include "adi/audio/io_audit.hpp"
#include "adi/audio/pitch_segments.hpp"

#include <cmath>
#include <cstdio>

namespace {
using namespace adi::audio;
int checks = 0;
int failures = 0;

void check(bool passed, const char* description) {
    ++checks;
    if (!passed) {
        ++failures;
        std::printf("FAIL %s\n", description);
    }
}

PitchAnalysis source() {
    return {48000, 1200, {
        {100, 300, 6110, 40, 10, {{100, 6090}, {200, 6130}, {299, 6129.8}}},
        {300, 500, 6400, 0, 0, {{300, 6400}, {499, 6400}}},
        {700, 900, 6900, 0, 0, {{701, 6900}, {899, 6900}}}
    }};
}

void splitGlueDelete() {
    const auto original = source();
    PitchEditState edits;
    const PitchEditCommand corrections[] = {{0, PitchSetShift{100}}, {1, PitchSetGain{-5}}, {2, PitchSetGain{-8}}};
    check(applyPitchEdits(original, edits, corrections) == PitchEditError::None, "fixture controls are valid");
    auto prepared = draftPitchSegments(original, edits);
    check(prepared && prepared.draft->analysis == original &&
          prepared.draft->geometry.segments == std::vector<PitchSegmentSpan>{{1, 100, 300}, {2, 300, 500}, {3, 700, 900}},
          "initial draft copies exact source notes and deterministically assigns stable IDs");
    if (!prepared) return;
    auto& draft = *prepared.draft;

    const PitchSegmentationCommand split[] = {PitchSplitSegment{1, 201}};
    check(applyPitchSegmentation(original, draft, split) == PitchEditError::None &&
          draft.geometry.segments == std::vector<PitchSegmentSpan>{{1, 100, 201}, {4, 201, 300}, {2, 300, 500}, {3, 700, 900}},
          "split retains left ID and inserts a fresh right ID at the exact source frame");
    if (draft.analysis.notes.size() != 4) return;
    check(std::abs(draft.analysis.notes[0].pitchCents - 6110) < 1e-9 &&
          std::abs(draft.analysis.notes[1].pitchCents - 6129.8) < 1e-9 &&
          draft.analysis.notes[0].curve.size() == 2 && draft.analysis.notes[1].curve.size() == 1,
          "split recalculates each new note from its measured pitch points");
    check(draft.edits.notes.size() == 2 && draft.edits.notes[0].noteIndex == 2 && draft.edits.notes[0].gainDb == -5 &&
          draft.edits.notes[1].noteIndex == 3 && draft.edits.notes[1].gainDb == -8,
          "touched correction resets while untouched corrections follow stable segments");
    const auto restored = draftPitchSegments(original, draft.edits, &draft.geometry);
    check(restored && restored.draft->analysis == draft.analysis && restored.draft->edits.notes == draft.edits.notes &&
          restored.draft->geometry == draft.geometry,
          "ordered spans, stable IDs and edit rows reproduce the materialized draft without saving pitch curves");

    const PitchSegmentationCommand glue[] = {PitchGlueSegments{1, 2}};
    check(applyPitchSegmentation(original, draft, glue) == PitchEditError::None &&
          draft.geometry.segments == std::vector<PitchSegmentSpan>{{1, 100, 500}, {3, 700, 900}} &&
          draft.analysis.notes[0].curve.size() == 5 && std::abs(draft.analysis.notes[0].pitchCents - 6130) < 1e-9,
          "glue merges a run of touching source segments and recalculates the combined pitch");
    check(draft.edits.notes.size() == 1 && draft.edits.notes[0].noteIndex == 1 && draft.edits.notes[0].gainDb == -8,
          "glue resets touched controls and keeps the other segment's edit");

    const PitchSegmentationCommand remove[] = {PitchDeleteSegment{1}};
    check(applyPitchSegmentation(original, draft, remove) == PitchEditError::None && draft.analysis.notes.size() == 1 &&
          draft.geometry.segments[0].id == 3 && draft.edits.notes[0].noteIndex == 0 &&
          draft.analysis.sourceFrames == original.sourceFrames,
          "deleting analysis geometry preserves the source extent and follows the unaffected edit");
    const PitchSegmentationCommand removeAll[] = {PitchDeleteSegment{3}};
    check(applyPitchSegmentation(original, draft, removeAll) == PitchEditError::None && draft.analysis.notes.empty() &&
          draft.edits.notes.empty() && draft.geometry.nextId == 5,
          "explicit empty geometry differs from automatic geometry and never reuses deleted IDs");
    const auto empty = draftPitchSegments(original, {}, &draft.geometry);
    check(empty && empty.draft->analysis.notes.empty() && original == source(), "empty draft replays while immutable analysis remains exact");
    std::printf("Split at source frame 201, child pitches 6110.00/6129.80 cents, glue 6130.00 cents\n");
}

void refusesWithoutMutating() {
    const auto original = source();
    auto baseline = draftPitchSegments(original, {});
    if (!baseline) {
        check(false, "baseline prepares");
        return;
    }
    auto& draft = *baseline.draft;
    const auto initial = draft.geometry;
    const PitchSegmentationCommand missing[] = {PitchSplitSegment{1, 201}, PitchDeleteSegment{999}};
    check(applyPitchSegmentation(original, draft, missing) == PitchEditError::UnknownNote && draft.geometry == initial &&
          draft.analysis == original, "an invalid later command rolls back the whole structural batch");
    const PitchSegmentationCommand noObservations[] = {PitchSplitSegment{3, 701}};
    check(applyPitchSegmentation(original, draft, noObservations) == PitchEditError::InvalidValue && draft.geometry == initial,
          "splitting a fragment without a measured pitch refuses");
    const PitchSegmentationCommand crossesGap[] = {PitchGlueSegments{2, 3}};
    check(applyPitchSegmentation(original, draft, crossesGap) == PitchEditError::InvalidValue && draft.geometry == initial,
          "glue does not pitch an unanalyzed source gap");

    auto malformed = initial;
    malformed.segments[1].id = malformed.segments[0].id;
    check(!validPitchSegmentation(malformed, original.sourceFrames) && !draftPitchSegments(original, {}, &malformed),
          "persisted segment IDs must be unique");
    malformed = initial;
    malformed.segments[0].startFrame = 0;
    malformed.segments[0].endFrame = 100;
    check(validPitchSegmentation(malformed, original.sourceFrames) && !draftPitchSegments(original, {}, &malformed),
          "explicit storage geometry must include at least one measured observation");
    malformed = initial;
    malformed.nextId = 2;
    check(!validPitchSegmentation(malformed, original.sourceFrames), "high-water ID must cover all live IDs");

    PitchEditState timings;
    const PitchEditCommand retime[] = {{0, PitchSetTiming{500, 700}}, {2, PitchSetTiming{100, 300}}};
    check(applyPitchEdits(original, timings, retime) == PitchEditError::None, "retimed fixture has no destination overlap");
    auto moved = draftPitchSegments(original, timings);
    check(static_cast<bool>(moved), "retimed geometry prepares");
    if (moved) {
        const PitchSegmentationCommand split[] = {PitchSplitSegment{1, 201}};
        check(applyPitchSegmentation(original, *moved.draft, split) == PitchEditError::Overlap &&
              moved.draft->geometry == initial && moved.draft->edits.notes == timings.notes,
              "a destination collision rejects the split and preserves earlier timing controls");
    }

    const CallbackScope callback;
    check(draftPitchSegments(original, {}).error == PitchEditError::Realtime &&
          applyPitchSegmentation(original, draft, missing) == PitchEditError::Realtime && draft.geometry == initial,
          "structural analysis refuses on the audio callback before mutation");
}

void correctSourceBoundaries() {
    const auto original = source();
    PitchEditState edits;
    const PitchEditCommand controls[] = {
        {0, PitchSetShift{100}}, {0, PitchSetStraighten{0.8}}, {0, PitchSetStraightenRange{PitchStraightenRange{110, 190}}}
    };
    check(applyPitchEdits(original, edits, controls) == PitchEditError::None, "boundary fixture applies range controls");
    auto prepared = draftPitchSegments(original, edits);
    check(static_cast<bool>(prepared), "boundary fixture prepares");
    if (!prepared) return;
    auto& draft = *prepared.draft;
    const PitchSegmentationCommand trim[] = {PitchSetSegmentBoundary{1, PitchBoundary::Start, 150}};
    check(applyPitchSegmentation(original, draft, trim) == PitchEditError::None &&
          draft.geometry.segments[0] == PitchSegmentSpan{1, 150, 300} &&
          draft.analysis.notes[0].curve.size() == 2 && std::abs(draft.analysis.notes[0].pitchCents - 6129.9) < 1e-9,
          "correcting source start retains ID and recomputes pitch from observations in the new range");
    auto resolved = resolvePitchNoteEdit(draft.analysis, draft.edits, 0);
    check(resolved.startFrame == 150 && resolved.endFrame == 300 && resolved.pitchShiftCents == 100 &&
          resolved.straighten == 0.8 && resolved.straightenRange == PitchStraightenRange{150, 190},
          "unretimed destination follows source correction, and straightening clips to the new bounds");

    const PitchEditCommand move[] = {{0, PitchSetTiming{0, 120}}};
    check(applyPitchEdits(draft.analysis, draft.edits, move) == PitchEditError::None, "explicitly retimed note is staged");
    const PitchSegmentationCommand extend[] = {PitchSetSegmentBoundary{1, PitchBoundary::Start, 110}};
    check(applyPitchSegmentation(original, draft, extend) == PitchEditError::None &&
          draft.geometry.segments[0] == PitchSegmentSpan{1, 110, 300}, "boundary can extend back into original measured source");
    resolved = resolvePitchNoteEdit(draft.analysis, draft.edits, 0);
    check(resolved.startFrame == 0 && resolved.endFrame == 120 && resolved.pitchShiftCents == 100,
          "source extension keeps explicitly authored destination timing and shift");

    const auto before = draft.geometry;
    const PitchSegmentationCommand overlap[] = {PitchSetSegmentBoundary{1, PitchBoundary::End, 301}};
    check(applyPitchSegmentation(original, draft, overlap) == PitchEditError::Overlap && draft.geometry == before,
          "source boundaries cannot overlap neighboring active notes");
    const PitchSegmentationCommand unmeasured[] = {PitchSetSegmentBoundary{1, PitchBoundary::End, 180}};
    check(applyPitchSegmentation(original, draft, unmeasured) == PitchEditError::InvalidValue && draft.geometry == before,
          "source boundary cannot leave the segment without observations");
    const PitchSegmentationCommand clearRange[] = {PitchSetSegmentBoundary{1, PitchBoundary::Start, 195}};
    check(applyPitchSegmentation(original, draft, clearRange) == PitchEditError::None,
          "a valid source correction may trim away a previous straighten selection");
    resolved = resolvePitchNoteEdit(draft.analysis, draft.edits, 0);
    check(resolved.straighten == 0 && !resolved.straightenRange && resolved.pitchShiftCents == 100 &&
          resolved.startFrame == 0 && resolved.endFrame == 120,
          "trimming away the selected range clears only straightening");
    const PitchSegmentationCommand trimSecondEnd[] = {PitchSetSegmentBoundary{2, PitchBoundary::End, 450}};
    check(applyPitchSegmentation(original, draft, trimSecondEnd) == PitchEditError::None &&
          draft.geometry.segments[1] == PitchSegmentSpan{2, 300, 450} && draft.analysis.notes[1].curve.size() == 1,
          "correcting an end removes observations after the exact exclusive boundary");
    check(original == source(), "correcting source geometry never changes immutable detector output");
    std::printf("Corrected source start from frame 100 to 150, nominal pitch 6110.00 to 6129.90 cents\n");
}

void includeUnvoicedSource() {
    const auto original = source();
    auto prepared = draftPitchSegments(original, {});
    if (!prepared) {
        check(false, "unvoiced edge fixture prepares");
        return;
    }
    auto& draft = *prepared.draft;
    const PitchSegmentationCommand edgeExtensions[] = {
        PitchSetSegmentBoundary{1, PitchBoundary::Start, 0},
        PitchSetSegmentBoundary{2, PitchBoundary::End, 700},
        PitchSetSegmentBoundary{3, PitchBoundary::End, original.sourceFrames}
    };
    check(applyPitchSegmentation(original, draft, edgeExtensions) == PitchEditError::None &&
          draft.geometry.segments == std::vector<PitchSegmentSpan>{{1, 0, 300}, {2, 300, 700}, {3, 700, 1200}} &&
          draft.analysis.notes[0].startFrame == 0 && draft.analysis.notes[1].endFrame == 700 &&
          draft.analysis.notes[2].endFrame == original.sourceFrames &&
          draft.analysis.notes[0].curve == original.notes[0].curve &&
          draft.analysis.notes[1].curve == original.notes[1].curve &&
          draft.analysis.notes[2].curve == original.notes[2].curve,
          "correction can include unvoiced leading or trailing audio without borrowing observations or rounding bounds");

    const PitchSegmentationCommand glueAfterCorrection[] = {PitchGlueSegments{2, 3}};
    check(applyPitchSegmentation(original, draft, glueAfterCorrection) == PitchEditError::None &&
          draft.geometry.segments == std::vector<PitchSegmentSpan>{{1, 0, 300}, {2, 300, 1200}} &&
          draft.analysis.notes[1].curve == std::vector<PitchPoint>{{300, 6400}, {499, 6400}, {701, 6900}, {899, 6900}} &&
          std::abs(draft.analysis.notes[1].pitchCents - 6650) < 1e-9,
          "explicitly covering the gap makes the pieces contiguous for Glue and preserves only actual observations");

    prepared = draftPitchSegments(original, {});
    if (!prepared) return;
    auto& interior = *prepared.draft;
    const PitchSegmentationCommand absorb[] = {
        PitchDeleteSegment{3}, PitchSetSegmentBoundary{2, PitchBoundary::End, 850}
    };
    check(applyPitchSegmentation(original, interior, absorb) == PitchEditError::None &&
          interior.geometry.segments == std::vector<PitchSegmentSpan>{{1, 100, 300}, {2, 300, 850}} &&
          interior.analysis.notes[0] == original.notes[0] &&
          interior.analysis.notes[1].curve == std::vector<PitchPoint>{{300, 6400}, {499, 6400}, {701, 6900}} &&
          interior.analysis.notes[1].pitchCents == 6400,
          "interior gap derives summary only from measured points and leaves untouched notes exact");
    const auto reloaded = draftPitchSegments(original, interior.edits, &interior.geometry);
    check(reloaded && reloaded.draft->analysis == interior.analysis && reloaded.draft->geometry == interior.geometry,
          "persisted source bounds across an interior gap reconstruct the same working analysis");

    const auto before = interior;
    const PitchSegmentationCommand pureGap[] = {
        PitchSetSegmentBoundary{1, PitchBoundary::Start, 90},
        PitchSetSegmentBoundary{2, PitchBoundary::Start, 510},
        PitchSetSegmentBoundary{2, PitchBoundary::End, 700}
    };
    check(applyPitchSegmentation(original, interior, pureGap) == PitchEditError::InvalidValue &&
          interior.geometry == before.geometry && interior.analysis == before.analysis &&
          interior.edits.notes == before.edits.notes,
          "an entirely unvoiced span refuses and rolls back every command in the batch");
}

} // namespace

int main() {
    splitGlueDelete();
    refusesWithoutMutating();
    correctSourceBoundaries();
    includeUnvoicedSource();
    std::printf("%d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
