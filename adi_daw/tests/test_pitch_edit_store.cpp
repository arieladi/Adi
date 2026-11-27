// SPDX-License-Identifier: AGPL-3.0-or-later
#include "temp_directory.hpp"
#include "adi/audio/io_audit.hpp"
#include "adi/history.hpp"
#include "adi/media/clip_pitch.hpp"
#include "adi/pitch_edit_store.hpp"
#include "adi/row_capture.hpp"
#include "adi/store.hpp"

#include <SQLiteCpp/SQLiteCpp.h>
#include <array>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <limits>
#include <stdexcept>

namespace {

using namespace adi;
using namespace adi::audio;
using namespace adi::clip_edit;

constexpr auto originalHash = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
constexpr auto otherHash = "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb";
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
    test::TempDirectory directory{"pitch_edit_store", "journal"};
    std::unique_ptr<Store> store;

    explicit Fixture(bool installPitchSchema = true) {
        StoreError error{};
        store = Store::createEmpty(directory.path() / "pitch.adi", error);
        if (!store) throw std::runtime_error(toString(error));
        if (installPitchSchema) {
            std::ifstream input(ADI_PITCH_SCHEMA_PATH);
            if (!input) throw std::runtime_error("could not read staged pitch edit DDL");
            const std::string sql{std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
            store->db().exec(sql);
        }
        auto& db = store->db();
        db.exec("INSERT INTO tracks(id,kind,name) VALUES(1,'audio','Voice'),(2,'midi','Notes');"
                "INSERT INTO clips(id,track_id,kind,pos_ticks,length_ticks) VALUES"
                "(15,1,'audio',0,1441440),(16,1,'audio',1441440,1441440),(17,2,'midi',0,1441440)");
        SQLite::Statement media(db, "INSERT INTO media_files(id,hash_blake3,sample_rate,channels,frames) "
            "VALUES(91,?,48000,1,2000),(92,?,48000,1,2000)");
        media.bind(1, originalHash);
        media.bind(2, otherHash);
        media.exec();
        db.exec("INSERT INTO audio_clips(clip_id,media_id,src_start_frames,src_len_frames) "
                "VALUES(15,91,100,1000),(16,91,100,1000)");
    }

    [[nodiscard]] std::optional<Payload> row(std::int64_t id = 15) const {
        return detail::captureRow(store->db(), "clip_pitch_edits", "clip_id", id);
    }

    void reopen() {
        store.reset();
        StoreError error{};
        store = Store::open(directory.path() / "pitch.adi", error);
        if (!store) throw std::runtime_error(toString(error));
    }
};

media::ClipPitchAnalysis source(std::int64_t id = 15) {
    return {id, {originalHash, 48000, 1, 2000}, 100, 1100, false, 1, 0,
        {48000, 2000, {
            {100, 500, 6100, 40, 10, {{120, 6080}, {400, 6110}}},
            {600, 900, 6400, 0, 0, {{620, 6400}, {800, 6400}}}
        }}};
}

PitchEditState changed(const media::ClipPitchAnalysis& analysis, double cents = 100.25) {
    PitchEditState state;
    const PitchEditCommand commands[] = {
        {0, PitchSetShift{cents}}, {0, PitchSetFormant{-125.5}}, {0, PitchSetTiming{10, 420}},
        {0, PitchSetTilt{-20}}, {0, PitchSetTiltGeometry{{0.4, 12, -7}}}, {0, PitchSetStraighten{0.5}},
        {0, PitchSetStraightenRange{PitchStraightenRange{160, 250}}}, {0, PitchSetDrift{0.75, true}},
        {0, PitchSetTransition{200}}, {1, PitchSetGain{-std::numeric_limits<double>::infinity()}}
    };
    if (applyPitchEdits(analysis.analysis, state, commands) != PitchEditError::None)
        throw std::runtime_error("could not construct fixture controls");
    return state;
}

OpRequest request(const media::ClipPitchAnalysis& analysis, const PitchEditState& state,
                  const PitchSegmentation* geometry = nullptr) {
    auto prepared = prepareClipPitchEdits(analysis, state, geometry);
    if (!prepared.request) throw std::runtime_error(prepared.error);
    return std::move(*prepared.request);
}

void durabilityAndHistory() {
    Fixture fixture;
    const auto analysis = source();
    const auto edits = changed(analysis);
    const auto initial = request(analysis, edits);
    check(initial.payload.at("rows").at(1).at("muted") == true &&
          initial.payload.at("rows").at(1).at("gain") == 0, "muted note encodes a finite canonical payload");
    const auto journaled = OpJournal(*fixture.store).commit(initial);
    check(journaled.ok, "registered op replaces the clip's authored rows");
    if (!journaled.ok) throw std::runtime_error(journaled.error);
    const auto first = fixture.row();
    check(first.has_value() && first->at("edits").is_binary(), "complete edits occupy one durable clip row");

    fixture.reopen();
    check(fixture.row() == first, "reopen preserves exact authored bytes and source identity");
    const auto saved = readClipPitchEdits(*fixture.store, 15);
    check(saved.edits.has_value() && saved.error.empty(), "typed reader decodes persisted note state");
    if (!saved.edits) throw std::runtime_error(saved.error);
    const auto resolved = resolveClipPitchEdits(*saved.edits, analysis);
    check(resolved.state && resolved.state->notes == edits.notes,
          "resolver restores geometry, mute and other controls against verified identity");
    check(saved.edits->rows.front().sourceStartFrame == 100 && saved.edits->rows.front().edit.startFrame == 10,
          "authored identity retains original frames separate from destination");

    History history(*fixture.store);
    check(history.undo().ok && !fixture.row(), "first edit Undo removes the entire previously absent set");
    check(history.redo().ok && fixture.row() == first, "first edit Redo restores every byte");
    const auto replacement = OpJournal(*fixture.store).commit(request(analysis, changed(analysis, -200)));
    check(replacement.ok && fixture.row() != first, "new op replaces the whole clip edit set");
    const auto second = fixture.row();
    check(history.undo().ok && fixture.row() == first, "replacement Undo restores the exact previous record");
    check(history.redo().ok && fixture.row() == second, "replacement Redo restores the exact new record");
    check(OpJournal(*fixture.store).commit(request(analysis, {})).ok && !fixture.row(), "empty replacement clears the set");
    check(history.undo().ok && fixture.row() == second, "Undo clear restores authored bytes without analysis I/O");
    check(history.redo().ok && !fixture.row(), "Redo clear restores the absent set");
}

void validationAndRollback() {
    Fixture fixture;
    const auto analysis = source();
    auto first = request(analysis, changed(analysis));
    const auto editCount = OpJournal(*fixture.store).count();
    auto bad = first;
    bad.payload.at("rows").at(0)["sourceEnd"] = 99;
    check(!OpJournal(*fixture.store).commit(bad).ok && !fixture.row(), "invalid original note range refuses before insert");
    bad = first;
    bad.payload.at("rows").at(0)["unexpected"] = true;
    check(!OpJournal(*fixture.store).commit(bad).ok, "unknown note field is refused");
    bad = first;
    bad.payload.at("rows").at(0)["gain"] = std::numeric_limits<double>::infinity();
    check(!OpJournal(*fixture.store).commit(bad).ok, "nonfinite gain is refused in op data");
    bad = first;
    bad.payload.at("rows").at(1)["gain"] = 6;
    check(!OpJournal(*fixture.store).commit(bad).ok, "muted row has one canonical gain encoding");
    bad = first;
    bad.payload["id"] = 17;
    check(!OpJournal(*fixture.store).commit(bad).ok, "pitch rows require an attached audio clip");
    bad = first;
    bad.payload["hash"] = otherHash;
    check(!OpJournal(*fixture.store).commit(bad).ok, "stale source hash refuses the prepared edit");
    auto& db = fixture.store->db();
    db.exec("CREATE TRIGGER test_pitch_failure BEFORE INSERT ON clip_pitch_edits WHEN NEW.clip_id=16 "
            "BEGIN SELECT RAISE(ABORT, 'test failure in second pitch write'); END");
    const auto secondSource = source(16);
    const std::array commands{first, request(secondSource, changed(secondSource))};
    check(!OpJournal(*fixture.store).commit(commands).ok && !fixture.row() && !fixture.row(16) &&
          OpJournal(*fixture.store).count() == editCount, "later write failure rolls back prior rows and journal effects");

    check(OpJournal(*fixture.store).commit(first).ok, "a valid op commits after the failed batch");
    db.exec("UPDATE audio_clips SET media_id=92 WHERE clip_id=15");
    const auto dormant = readClipPitchEdits(*fixture.store, 15);
    check(dormant.edits && dormant.edits->sourceHash == originalHash,
          "changing source retains old authored rows for a later matching source");
    if (!dormant.edits) throw std::runtime_error(dormant.error);
    check(!resolveClipPitchEdits(*dormant.edits, analysis.analysis, otherHash, 1, 0).state,
          "captured edit refuses a different source despite equal sample rate and frame count");
    check(!resolveClipPitchEdits(*dormant.edits, analysis.analysis, originalHash, 2, 0).state &&
          !resolveClipPitchEdits(*dormant.edits, analysis.analysis, originalHash, 1, 1).state,
          "analysis version and configuration are stable identity components");
    auto different = analysis;
    ++different.analysis.notes.front().startFrame;
    check(!resolveClipPitchEdits(*dormant.edits, different).state,
          "matching hash/version cannot attach edits to different analyzed note bounds");
    different = analysis;
    different.analysis.notes.clear();
    check(!resolveClipPitchEdits(*dormant.edits, different).state, "missing original note index refuses");
    check(!OpJournal(*fixture.store).commit(first).ok, "prepared request is checked again in its write transaction");

    const auto dormantRow = fixture.row();
    auto newSource = analysis;
    newSource.source.hash = otherHash;
    check(OpJournal(*fixture.store).commit(request(newSource, changed(newSource, 75))).ok,
          "an edit prepared for the replacement media may replace dormant authored data");
    check(History(*fixture.store).undo().ok && fixture.row() == dormantRow,
          "Undo restores exact dormant rows even when current source differs");
}

void snapshotRead() {
    Fixture old(false);
    const auto absent = readClipPitchEdits(*old.store, 15);
    const auto empty = readProjectPitchEdits(*old.store);
    check(!absent.edits && absent.error.empty() && !empty.clips && empty.error.empty(),
          "pre-pitch project reads empty without a schema migration");
    const auto analysis = source();
    const auto pitch = request(analysis, changed(analysis));
    check(!OpJournal(*old.store).commit(pitch).ok && OpJournal(*old.store).count() == 0,
          "reserved operation reports staged schema unavailable before migration");

    Fixture fixture;
    const auto secondSource = source(16);
    const std::array commands{pitch, request(secondSource, changed(secondSource))};
    if (!OpJournal(*fixture.store).commit(commands).ok) throw std::runtime_error("fixture pitch writes failed");
    const auto before = readProjectPitchEdits(*fixture.store);
    check(before.error.empty() && before.clips && before.clips->size() == 2 &&
          before.clips->at(15).edits && before.clips->at(16).edits,
          "ordered project read captures each clip's independent edits");
    std::vector<std::uint8_t> nesting(50000, 0x81); // Each byte starts a one-element CBOR array.
    nesting.push_back(0);
    SQLite::Statement corrupt(fixture.store->db(), "UPDATE clip_pitch_edits SET edits=? WHERE clip_id=16");
    corrupt.bind(1, nesting.data(), static_cast<int>(nesting.size()));
    corrupt.exec();
    const auto after = readProjectPitchEdits(*fixture.store);
    check(after.error.empty() && after.clips && after.clips->size() == 2 && after.clips->at(15).edits &&
          !after.clips->at(16).edits && after.clips->at(16).error.find("nesting") != std::string::npos,
          "deeply nested stored record fails only its own clip before recursive decode");
    fixture.store->db().exec("UPDATE clip_pitch_edits SET edits=X'81A17F7F6178FFFF00' WHERE clip_id=16");
    const auto indefinite = readClipPitchEdits(*fixture.store, 16);
    check(!indefinite.edits && indefinite.error.find("nesting") != std::string::npos,
          "nested indefinite string keys refuse before recursive CBOR string processing");
    const CallbackScope callback;
    check(!readProjectPitchEdits(*fixture.store).error.empty() && !readClipPitchEdits(*fixture.store, 15).error.empty(),
          "project and single-clip authored reads refuse the audio callback");
}

void authoredGeometry() {
    Fixture fixture;
    const auto base = source();
    const auto edits = changed(base);
    OpJournal journal(*fixture.store);
    if (!journal.commit(request(base, edits)).ok) throw std::runtime_error("could not seed clip pitch edit");
    const auto original = fixture.row();

    auto draft = draftPitchSegments(base.analysis, edits);
    if (!draft) throw std::runtime_error("could not draft clip pitch segmentation");
    const PitchSegmentationCommand split[] = {PitchSplitSegment{1, 300}};
    if (applyPitchSegmentation(base.analysis, *draft.draft, split) != PitchEditError::None)
        throw std::runtime_error("could not split fixture note");
    const auto& geometry = draft.draft->geometry;
    const auto authored = request(base, draft.draft->edits, &geometry);
    auto malformed = authored;
    malformed.payload["segmentation"]["segments"][1]["id"] = 1;
    check(!journal.commit(malformed).ok && fixture.row() == original,
          "duplicate stable segment ID refuses atomically");
    malformed = authored;
    malformed.payload["rows"][0]["sourceStart"] = 601;
    check(!journal.commit(malformed).ok && fixture.row() == original,
          "authored edit bounds must match the stored segment they identify");
    const auto committed = journal.commit(authored);
    check(committed.ok && fixture.row() != original, "one operation commits split geometry with the retained note edit");
    if (!committed.ok) throw std::runtime_error(committed.error);
    const auto afterSplit = fixture.row();

    fixture.reopen();
    const auto stored = readClipPitchEdits(*fixture.store, 15);
    check(stored.edits && stored.edits->segmentation == geometry && fixture.row() == afterSplit,
          "reopening preserves split segment IDs, bounds and next ID");
    if (!stored.edits || !stored.edits->segmentation) throw std::runtime_error("authored split geometry is missing");
    const auto effective = resolveClipPitchEdits(*stored.edits, base);
    check(effective.state && effective.state->notes == draft.draft->edits.notes && effective.adjustedAnalysis &&
          effective.adjustedAnalysis->notes == draft.draft->analysis.notes,
          "immutable detector curves reconstitute authored geometry before applying sparse edits");

    History history(*fixture.store);
    check(history.undo().ok && fixture.row() == original, "undo restores exact original array-form CBOR");
    check(history.redo().ok && fixture.row() == afterSplit, "redo restores exact geometry-form CBOR");

    auto corrected = *stored.edits;
    corrected.segmentation->segments[1].endFrame = 550;
    const auto intoGap = resolveClipPitchEdits(corrected, base);
    check(intoGap.state && intoGap.adjustedAnalysis && intoGap.adjustedAnalysis->notes[1].endFrame == 550,
          "stored geometry projects over an unvoiced edge at its exact frame");
    auto impossible = corrected;
    impossible.segmentation->segments[1].startFrame = 510;
    check(!resolveClipPitchEdits(impossible, base).state,
          "stored geometry still rejects a segment with no detector observations");

    const PitchSegmentation empty{{}, geometry.nextId};
    const auto deleted = OpJournal(*fixture.store).commit(request(base, {}, &empty));
    check(deleted.ok && fixture.row(), "explicit deletion of every segment remains an authored row");
    const auto afterDelete = fixture.row();
    fixture.reopen();
    const auto storedEmpty = readClipPitchEdits(*fixture.store, 15);
    check(storedEmpty.edits && storedEmpty.edits->segmentation == empty && fixture.row() == afterDelete,
          "no-segments geometry survives reopen and keeps its next ID");
    if (!storedEmpty.edits) throw std::runtime_error(storedEmpty.error);
    const auto emptyEffective = resolveClipPitchEdits(*storedEmpty.edits, base);
    check(emptyEffective.state && emptyEffective.state->notes.empty() && emptyEffective.adjustedAnalysis &&
          emptyEffective.adjustedAnalysis->notes.empty() && emptyEffective.adjustedAnalysis->sourceFrames == base.analysis.sourceFrames,
          "explicit empty geometry resolves no notes and preserves the source duration");
    check(History(*fixture.store).undo().ok && fixture.row() == afterSplit,
          "undo restores split geometry after delete-all and reopen");
}

} // namespace

int main() {
    try {
        durabilityAndHistory();
        validationAndRollback();
        snapshotRead();
        authoredGeometry();
    } catch (const std::exception& exception) {
        check(false, exception.what());
    }
    std::printf("%d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
