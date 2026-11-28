// SPDX-License-Identifier: AGPL-3.0-or-later
#include "temp_directory.hpp"
#include "adi/audio/io_audit.hpp"
#include "adi/history.hpp"
#include "adi/history_compaction.hpp"
#include "adi/media/clip_pitch.hpp"
#include "adi/pitch_edit_store.hpp"
#include "adi/pitch_geometry_commands.hpp"
#include "adi/row_capture.hpp"
#include "adi/store.hpp"

#include <SQLiteCpp/SQLiteCpp.h>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <stdexcept>

namespace {

using namespace adi;
using namespace adi::audio;
using namespace adi::clip_edit;

constexpr auto sourceHash = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
constexpr auto replacementHash = "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb";
constexpr std::int64_t clipId = 15;
int checks = 0;
int failures = 0;

void check(bool valid, const char* what) {
    ++checks;
    if (!valid) {
        ++failures;
        std::printf("FAIL %s\n", what);
    }
}

struct Fixture {
    test::TempDirectory directory{"pitch_geometry_commands", "journal"};
    std::unique_ptr<Store> store;
    media::ClipPitchAnalysis source{clipId, {sourceHash, 48000, 1, 2000}, 0, 2000, false,
        media::kPitchAnalysisVersion, media::kPitchAnalysisConfiguration,
        {48000, 2000, {
            {100, 500, 6100, 40, 10, {{120, 6080}, {200, 6100}, {400, 6110}}},
            {600, 900, 6400, 0, 0, {{620, 6400}, {800, 6400}}},
            {950, 1200, 6700, 0, 0, {{970, 6700}, {1100, 6700}}},
        }}};

    Fixture() {
        StoreError error{};
        store = Store::createEmpty(directory.path() / "pitch.adi", error);
        if (!store) throw std::runtime_error(toString(error));
        std::ifstream input(ADI_PITCH_SCHEMA_PATH);
        if (!input) throw std::runtime_error("could not read staged pitch edit DDL");
        const std::string sql{std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
        auto& db = store->db();
        db.exec(sql);
        db.exec("INSERT INTO tracks(id,kind,name) VALUES(1,'audio','Voice');"
                "INSERT INTO clips(id,track_id,kind,pos_ticks,length_ticks) VALUES(15,1,'audio',0,1441440)");
        SQLite::Statement media(db, "INSERT INTO media_files(id,hash_blake3,sample_rate,channels,frames) "
            "VALUES(91,?,48000,1,2000),(92,?,48000,1,2000)");
        media.bind(1, sourceHash);
        media.bind(2, replacementHash);
        media.exec();
        db.exec("INSERT INTO audio_clips(clip_id,media_id,src_start_frames,src_len_frames) VALUES(15,91,0,2000)");
    }

    void reopen() {
        store.reset();
        StoreError error{};
        store = Store::open(directory.path() / "pitch.adi", error);
        if (!store) throw std::runtime_error(toString(error));
    }

    [[nodiscard]] std::optional<Payload> row() const {
        return detail::captureRow(store->db(), "clip_pitch_edits", "clip_id", clipId);
    }

    void seed(std::span<const PitchEditCommand> commands) {
        PitchEditState state;
        if (applyPitchEdits(source.analysis, state, commands) != PitchEditError::None)
            throw std::runtime_error("could not prepare fixture pitch controls");
        const auto request = prepareClipPitchEdits(source, state);
        if (!request.request) throw std::runtime_error(request.error);
        const auto result = OpJournal(*store).commit(*request.request);
        if (!result.ok) throw std::runtime_error(result.error);
    }

    [[nodiscard]] ClipPitchEdits saved() const {
        auto stored = readClipPitchEdits(*store, clipId);
        if (!stored.edits) throw std::runtime_error(stored.error.empty() ? "pitch row absent" : stored.error);
        return std::move(*stored.edits);
    }
};

CommitResult commit(Store& store, const PitchGeometryEdit& action) {
    if (!action.edit) throw std::runtime_error(action.error);
    return commitPrepared(store, *action.edit);
}

void splitAndMergeHistory() {
    Fixture fixture;
    const PitchEditCommand initial[] = {
        {0, PitchSetTiming{1000, 1600}}, {0, PitchSetShift{100.25}}, {0, PitchSetGain{-6}},
        {0, PitchSetFormant{-125.5}}, {2, PitchSetTiming{100, 350}}, {2, PitchSetShift{-50}}
    };
    fixture.seed(initial);
    const auto before = fixture.row();
    const auto prior = fixture.saved();
    const auto split = prepareSplitPitchSegment(*fixture.store, fixture.source, {1, 300});
    check(split && split.edit->requests.size() == 1 && split.edit->requests.front().opType == "clip.splitPitchSegment" &&
          split.edit->requests.front().label == "Split pitch segment", "split prepares one independently named journal action");
    const auto cut = commit(*fixture.store, split);
    check(cut.ok && fixture.row() != before, "split journals new geometry and correction rows together");
    if (!cut.ok) throw std::runtime_error(cut.error);
    const auto cutRow = fixture.row();
    const auto afterCut = fixture.saved();
    check(afterCut.segmentation && afterCut.segmentation->segments == std::vector<PitchSegmentSpan>{
              {1, 100, 300}, {4, 300, 500}, {2, 600, 900}, {3, 950, 1200}},
          "split keeps the left and unaffected identities and creates one new right identity");
    const auto resolved = resolveClipPitchEdits(afterCut, fixture.source);
    if (!resolved.state || !resolved.adjustedAnalysis) throw std::runtime_error(resolved.error);
    const auto left = resolvePitchNoteEdit(*resolved.adjustedAnalysis, *resolved.state, 0);
    const auto right = resolvePitchNoteEdit(*resolved.adjustedAnalysis, *resolved.state, 1);
    const auto other = resolvePitchNoteEdit(*resolved.adjustedAnalysis, *resolved.state, 3);
    check(left.startFrame == 1000 && left.endFrame == 1300 && right.startFrame == 1300 && right.endFrame == 1600,
          "split partitions the 1.5x destination timing at exact source-affine frame 1300");
    check(left.pitchShiftCents == 100.25 && right.pitchShiftCents == 100.25 && left.gainDb == -6 && right.gainDb == -6 &&
          left.formantShiftCents == -125.5 && right.formantShiftCents == -125.5,
          "each child retains the active pitch, gain and formant controls");
    check(other.noteIndex == 3 && other.startFrame == 100 && other.endFrame == 350 && other.pitchShiftCents == -50,
          "unaffected retimed note keeps its destination even across split placeholder geometry");

    const auto merge = prepareMergePitchSegments(*fixture.store, fixture.source, {1, 4});
    check(merge && merge.edit->requests.size() == 1 && merge.edit->requests.front().opType == "clip.mergePitchSegments" &&
          merge.edit->requests.front().label == "Merge pitch segments",
          "merge prepares its own named journal action");
    const auto glue = commit(*fixture.store, merge);
    check(glue.ok && glue.txnId != cut.txnId, "merge is a distinct Undo group");
    if (!glue.ok) throw std::runtime_error(glue.error);
    const auto mergedRow = fixture.row();
    const auto merged = fixture.saved();
    check(merged.rows == prior.rows && merged.segmentation && merged.segmentation->nextId == 5,
          "merge recombines an exact correction mapping and retires the second identity");
    fixture.reopen();
    check(fixture.row() == mergedRow, "reopen preserves the exact merge record");
    const auto logged = OpJournal(*fixture.store).recent(3);
    check(logged.size() == 3 && logged[0].opType == "clip.mergePitchSegments" && logged[0].label == "Merge pitch segments" &&
          logged[1].opType == "clip.splitPitchSegment" && logged[1].label == "Split pitch segment" &&
          logged[2].opType == "clip.setPitchEdits" && logged[0].txnId != logged[1].txnId,
          "reopened journal retains exact operation names and distinct groups");
    History history(*fixture.store);
    check(history.undo().ok && fixture.row() == cutRow, "one Undo restores exact before-merge bytes");
    check(history.undo().ok && fixture.row() == before, "separate Undo restores exact before-split bytes");
    check(history.redo().ok && fixture.row() == cutRow, "separate Redo restores exact split bytes");
    check(history.redo().ok && fixture.row() == mergedRow, "separate Redo restores exact merge bytes");
    std::printf("Pitch split placed its destination boundary at frame %llu and kept %.2f cents\n",
        static_cast<unsigned long long>(left.endFrame), left.pitchShiftCents);
}

void registeredWireAndCheckpoint() {
    const auto& registry = OpRegistry::instance();
    Fixture source;
    const PitchEditCommand initial[] = {{0, PitchSetShift{100}}, {0, PitchSetGain{-6}}};
    source.seed(initial);
    const auto original = source.row();
    const auto install = [](Fixture& target, const Payload& checkpoint) {
        SQLite::Transaction transaction(target.store->db());
        history_detail::installCheckpoint(*target.store, checkpoint);
        transaction.commit();
    };
    Fixture restored;
    install(restored, history_detail::checkpointState(*source.store));
    check(restored.row() == original, "base checkpoint preserves the legacy pitch correction row");

    const auto split = prepareSplitPitchSegment(*source.store, source.source, {1, 300});
    if (!split || split.edit->requests.size() != 1) throw std::runtime_error("split request missing");
    const auto splitRequest = split.edit->requests.front();
    check(commit(*source.store, split).ok, "source journal applies the prepared split after-image");
    const auto splitRow = source.row();
    const auto merge = prepareMergePitchSegments(*source.store, source.source, {1, 4});
    if (!merge || merge.edit->requests.size() != 1) throw std::runtime_error("merge request missing");
    check(commit(*source.store, merge).ok, "source journal applies the prepared merge after-image");
    const auto mergeRow = source.row();
    const auto logged = OpJournal(*source.store).recent(2);
    if (logged.size() != 2) throw std::runtime_error("geometry journal entries are missing");

    for (const auto* name : {"clip.splitPitchSegment", "clip.mergePitchSegments"}) {
        const auto* descriptor = registry.find(name);
        check(descriptor && descriptor->scope == Scope::Edit && descriptor->engineImpact == EngineImpact::Snapshot &&
              descriptor->apply && descriptor->buildInverse && !descriptor->ephemeral,
              "literal geometry operation is registered for captured project edits");
        auto missingGeometry = splitRequest;
        missingGeometry.opType = name;
        missingGeometry.payload.erase("segmentation");
        check(descriptor && !validateSubmission(*descriptor, missingGeometry.payload).empty() &&
              !OpJournal(*restored.store).commit(missingGeometry).ok && restored.row() == original,
              "named geometry operation requires an explicit resolved segmentation image");
        auto emptyGeometry = splitRequest;
        emptyGeometry.opType = name;
        emptyGeometry.payload["rows"] = Payload::array();
        emptyGeometry.payload["segmentation"]["segments"] = Payload::array();
        check(!OpJournal(*restored.store).commit(emptyGeometry).ok && restored.row() == original,
              "a geometry operation cannot clear all source segments");
    }

    // Replay from recorded requests over a copied checkpoint. No analysis
    // producer or cache is needed on the replay side.
    const auto replay = [](const OpJournal::LoggedOp& recorded) {
        OpRequest request;
        request.opType = recorded.opType;
        request.payload = recorded.payload;
        request.label = recorded.label;
        return request;
    };
    check(OpJournal(*restored.store).commit(replay(logged[1])).ok && restored.row() == splitRow,
          "split wire payload replays from checkpoint to the byte-exact split result");
    Fixture splitCheckpoint;
    install(splitCheckpoint, history_detail::checkpointState(*restored.store));
    check(splitCheckpoint.row() == splitRow, "checkpoint round-trip retains split geometry and controls");
    check(OpJournal(*splitCheckpoint.store).commit(replay(logged[0])).ok && splitCheckpoint.row() == mergeRow,
          "merge wire payload replays from split checkpoint to the byte-exact merge result");
    check(History(*splitCheckpoint.store).undo().ok && splitCheckpoint.row() == splitRow &&
          History(*splitCheckpoint.store).redo().ok && splitCheckpoint.row() == mergeRow,
          "merge captured history restores exact rows after checkpoint replay");

    Fixture legacy;
    legacy.seed(initial);
    auto historicalSplit = splitRequest;
    historicalSplit.opType = "clip.setPitchEdits";
    check(OpJournal(*legacy.store).commit(historicalSplit).ok && legacy.row() == splitRow,
          "old generic pitch wire payload still accepts resolved structural geometry");
    check(History(*legacy.store).undo().ok && legacy.row() == original &&
          History(*legacy.store).redo().ok && legacy.row() == splitRow,
          "legacy structural geometry retains exact Undo and Redo");
}

void directBoundaryHistory() {
    Fixture fixture;
    const PitchEditCommand initial[] = {
        {0, PitchSetTiming{1300, 1700}}, {0, PitchSetShift{25}}, {0, PitchSetStraighten{0.75}},
        {0, PitchSetStraightenRange{PitchStraightenRange{135, 430}}}, {0, PitchSetTilt{30}}, {0, PitchSetGain{-6}}
    };
    fixture.seed(initial);
    const auto before = fixture.row();
    const auto start = prepareSetPitchSegmentBoundary(*fixture.store, fixture.source, {1, PitchBoundary::Start, 180});
    check(start && start.edit->requests.front().label == "Set pitch segment start" && commit(*fixture.store, start).ok,
          "direct start correction commits and labels one action");
    const auto afterStart = fixture.row();
    const auto end = prepareSetPitchSegmentBoundary(*fixture.store, fixture.source, {1, PitchBoundary::End, 420});
    check(end && end.edit->requests.front().label == "Set pitch segment end" && commit(*fixture.store, end).ok,
          "direct end correction commits and labels its own action");
    const auto afterEnd = fixture.row();
    const auto saved = fixture.saved();
    check(saved.segmentation && saved.segmentation->segments.front() == PitchSegmentSpan{1, 180, 420} && !saved.rows.empty(),
          "corrected source range retains the original note identity");
    if (saved.rows.empty()) throw std::runtime_error("active boundary correction is missing");
    const auto& control = saved.rows.front().edit;
    check(control.startFrame == 1300 && control.endFrame == 1700 && control.straighten == 0.75 &&
          control.straightenRange == PitchStraightenRange{180, 420} && control.pitchShiftCents == 25 &&
          control.tiltCents == 30 && control.gainDb == -6, "boundary correction clips selection and keeps explicit timing and controls");
    const auto count = OpJournal(*fixture.store).count();
    const auto unchanged = prepareSetPitchSegmentBoundary(*fixture.store, fixture.source, {1, PitchBoundary::End, 420});
    check(unchanged && unchanged.edit->requests.empty() && commit(*fixture.store, unchanged).ok &&
          OpJournal(*fixture.store).count() == count, "no-op boundary avoids a journal step");
    fixture.reopen();
    History history(*fixture.store);
    check(history.undo().ok && fixture.row() == afterStart && history.undo().ok && fixture.row() == before,
          "reopened Undo visits both boundary corrections independently");
    check(history.redo().ok && history.redo().ok && fixture.row() == afterEnd,
          "reopened Redo restores exact corrected source and correction rows");
}

void rejectsIncompatibleAndStale() {
    Fixture fixture;
    const PitchEditCommand retime[] = {{0, PitchSetTiming{1000, 1601}}, {2, PitchSetTiming{100, 350}}};
    fixture.seed(retime);
    const auto retimed = fixture.row();
    const auto count = OpJournal(*fixture.store).count();
    const auto inexact = prepareSplitPitchSegment(*fixture.store, fixture.source, {1, 300});
    check(!inexact && inexact.error.find("exact destination frame") != std::string::npos,
          "split refuses to round an incompatible sub-frame destination");
    const auto gap = prepareMergePitchSegments(*fixture.store, fixture.source, {1, 2});
    check(!gap && gap.error.find("touching") != std::string::npos && fixture.row() == retimed &&
          OpJournal(*fixture.store).count() == count, "merge across a source gap refuses without changing authored state");
    const PitchEditCommand contour[] = {{0, PitchSetStraighten{0.6}}};
    fixture.seed(contour);
    const auto incompatible = prepareSplitPitchSegment(*fixture.store, fixture.source, {1, 300});
    check(!incompatible && incompatible.error.find("contour correction") != std::string::npos,
          "split refuses an active contour that cannot be represented exactly on both children");

    auto staleAnalysis = fixture.source;
    ++staleAnalysis.analysisVersion;
    check(!prepareSetPitchSegmentBoundary(*fixture.store, staleAnalysis, {1, PitchBoundary::Start, 180}),
          "a new detector version does not reinterpret authored state from the same media");

    const PitchEditCommand level[] = {{0, PitchSetGain{-6}}};
    fixture.seed(level);
    const auto split = prepareSplitPitchSegment(*fixture.store, fixture.source, {1, 300});
    const PitchEditCommand later[] = {{0, PitchSetGain{-3}}};
    fixture.seed(later);
    const auto changed = fixture.row();
    const auto stale = commit(*fixture.store, split);
    check(!stale.ok && stale.stale && fixture.row() == changed, "intervening pitch edit makes a prepared structural action stale");
    const auto sourcePending = prepareSplitPitchSegment(*fixture.store, fixture.source, {1, 300});
    fixture.store->db().exec("UPDATE audio_clips SET media_id=92 WHERE clip_id=15");
    check(!commit(*fixture.store, sourcePending).ok && fixture.row() == changed,
          "transaction rejects media replacement even when the journal head is unchanged");
    check(!prepareSplitPitchSegment(*fixture.store, fixture.source, {1, 300}),
          "owner preparation checks the live source before using stale analysis");
    const CallbackScope callback;
    const auto refused = prepareSplitPitchSegment(*fixture.store, fixture.source, {1, 300});
    check(!refused && refused.error == "audio callback", "structural action refuses callback use before constructing a draft");
}

void mergeMismatch() {
    Fixture fixture;
    const auto split = prepareSplitPitchSegment(*fixture.store, fixture.source, {1, 300});
    if (!commit(*fixture.store, split).ok) throw std::runtime_error("could not seed touching segments");
    auto stored = fixture.saved();
    auto resolved = resolveClipPitchEdits(stored, fixture.source);
    if (!resolved.state || !resolved.adjustedAnalysis || !stored.segmentation)
        throw std::runtime_error("could not resolve touching segments");
    const PitchEditCommand independent[] = {{1, PitchSetGain{-3}}};
    if (applyPitchEdits(*resolved.adjustedAnalysis, *resolved.state, independent) != PitchEditError::None)
        throw std::runtime_error("could not vary touching segment correction");
    const auto request = prepareClipPitchEdits(fixture.source, *resolved.state, &*stored.segmentation);
    if (!request.request || !OpJournal(*fixture.store).commit(*request.request).ok)
        throw std::runtime_error("could not save independent segment correction");
    const auto before = fixture.row();
    const auto merge = prepareMergePitchSegments(*fixture.store, fixture.source, {1, 4});
    check(!merge && merge.error.find("different note corrections") != std::string::npos && fixture.row() == before,
          "merge refuses touching segments with different active control values");
    const auto reset = prepareMergePitchSegments(*fixture.store, fixture.source, {1, 4},
                                                PitchGeometryCorrections::ResetChanged);
    check(reset && commit(*fixture.store, reset).ok && fixture.saved().rows.empty(),
          "explicit reset-and-merge can combine differently corrected segments");
    const auto after = fixture.row();
    fixture.reopen();
    History history(*fixture.store);
    check(history.undo().ok && fixture.row() == before && history.redo().ok && fixture.row() == after,
          "reset-and-merge keeps exact corrections on reopened Undo and Redo");
}

void splitReset() {
    Fixture fixture;
    const PitchEditCommand initial[] = {
        {0, PitchSetTiming{1300, 1901}}, {0, PitchSetStraighten{0.6}}, {0, PitchSetGain{-6}},
        {2, PitchSetShift{25}}
    };
    fixture.seed(initial);
    const auto before = fixture.row();
    const auto changed = prepareSplitPitchSegment(*fixture.store, fixture.source, {1, 300},
                                                  PitchGeometryCorrections::ResetChanged);
    check(changed && commit(*fixture.store, changed).ok, "explicit reset-and-split handles an incompatible corrected contour");
    const auto saved = fixture.saved();
    check(saved.segmentation && saved.segmentation->segments[0] == PitchSegmentSpan{1, 100, 300} &&
          saved.segmentation->segments[1] == PitchSegmentSpan{4, 300, 500} &&
          saved.rows.size() == 1 && saved.rows.front().edit.noteIndex == 3 &&
          saved.rows.front().edit.pitchShiftCents == 25,
          "reset-and-split resets both children to source timing and leaves the unrelated correction untouched");
    const auto after = fixture.row();
    fixture.reopen();
    History history(*fixture.store);
    check(history.undo().ok && fixture.row() == before && history.redo().ok && fixture.row() == after,
          "reset-and-split round trips exact bytes, including corrected timing, across reopen");
}

} // namespace

int main() {
    try {
        splitAndMergeHistory();
        registeredWireAndCheckpoint();
        directBoundaryHistory();
        rejectsIncompatibleAndStale();
        mergeMismatch();
        splitReset();
    } catch (const std::exception& error) {
        check(false, error.what());
    }
    std::printf("%d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
