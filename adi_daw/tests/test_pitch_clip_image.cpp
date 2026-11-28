// SPDX-License-Identifier: AGPL-3.0-or-later
#include "adi/audio/wav_file.hpp"
#include "adi/clip_commands.hpp"
#include "adi/clip_image.hpp"
#include "adi/digest.hpp"
#include "adi/edit_plan.hpp"
#include "adi/history.hpp"
#include "adi/media/blake3.hpp"
#include "adi/media/clip_pitch.hpp"
#include "adi/media/media_ops.hpp"
#include "adi/pitch_edit_store.hpp"
#include "adi/store.hpp"
#include "adi/track_image.hpp"
#include "temp_directory.hpp"

#include <SQLiteCpp/SQLiteCpp.h>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <stdexcept>

namespace {
using namespace adi;
using namespace adi::clip_edit;

constexpr std::uint32_t rate = 48000;
constexpr std::int64_t beat = 5765760;
int checks = 0, failures = 0;

void check(bool good, const char* label) {
    ++checks;
    if (!good) {
        ++failures;
        std::printf("FAIL: %s\n", label);
    }
}

void require(bool good, const std::string& error) {
    if (!good) throw std::runtime_error(error);
}

void commit(Store& store, std::string opType, Payload payload) {
    OpRequest op;
    op.opType = std::move(opType);
    op.payload = std::move(payload);
    const auto result = OpJournal(store).commit(op);
    require(result.ok, result.error);
}

std::unique_ptr<Store> project(const std::filesystem::path& path, const std::string& schema) {
    StoreError error{};
    auto store = Store::create(path, error);
    require(static_cast<bool>(store), toString(error));
    store->db().exec(schema);
    return store;
}

std::string importSource(Store& store, const std::filesystem::path& path, std::int64_t id,
                         float firstSample) {
    std::vector<float> samples(rate);
    samples.front() = firstSample;
    {
        audio::WavWriter writer(path, rate, 1, audio::WavFormat::Float32);
        writer.write(samples.data(), rate);
        writer.close();
    }
    const auto imported = media::importMedia(store, path, id, 0);
    require(imported.ok, imported.error);
    return media::blake3File(path).hex;
}

std::string pitchBytes(const Store& store, std::int64_t clip) {
    SQLite::Statement query(store.db(), "SELECT hex(edits) FROM clip_pitch_edits WHERE clip_id=?");
    query.bind(1, clip);
    return query.executeStep() ? query.getColumn(0).getString() : std::string{};
}

std::string stagedSchema() {
    std::ifstream sql(ADI_PITCH_STAGED_SCHEMA);
    require(sql.is_open(), "cannot open staged pitch schema");
    return {std::istreambuf_iterator<char>(sql), std::istreambuf_iterator<char>()};
}

void lifecycle() {
    const auto schema = stagedSchema();
    test::TempDirectory temp{"pitch_clip_image", "lifecycle"};
    auto store = project(temp.path() / "source.adi", schema);
    auto& db = store->db();
    db.exec("INSERT INTO tracks(id,kind,name) VALUES(21,'audio','Voice');"
            "INSERT INTO clips(id,track_id,kind,pos_ticks,length_ticks) VALUES(7,21,'audio',0,11531520)");
    const auto hash = importSource(*store, temp.path() / "voice.wav", 1, 0.25f);
    const auto otherHash = importSource(*store, temp.path() / "other.wav", 2, -0.5f);
    commit(*store, "clip.attachAudio", {{"id", 7}, {"media", 1}, {"frames", rate}});

    const media::ClipPitchAnalysis analysis{
        7, {hash, rate, 1, rate}, 0, rate, false,
        media::kPitchAnalysisVersion, media::kPitchAnalysisConfiguration,
        {rate, rate, {{1000, 20000, 5700, 0, 0, {{1000, 5700}, {19999, 5700}}},
                      {25000, 45000, 6000, 0, 0, {{25000, 6000}, {44999, 6000}}}}}
    };
    audio::PitchEditState edits;
    const audio::PitchEditCommand changes[] = {
        {0, audio::PitchSetShift{100.25}}, {0, audio::PitchSetFormant{-150}},
        {1, audio::PitchSetTiming{24000, 46000}}, {1, audio::PitchSetGain{-2.5}}
    };
    require(audio::applyPitchEdits(analysis.analysis, edits, changes) == audio::PitchEditError::None,
            "cannot build authored pitch fixture");
    const auto before = digestProject(*store).fingerprint;
    const auto request = prepareClipPitchEdits(analysis, edits);
    require(request.request.has_value(), request.error);
    const auto written = OpJournal(*store).commit(*request.request);
    require(written.ok, written.error);
    const auto original = pitchBytes(*store, 7);
    const auto saved = readClipPitchEdits(*store, 7);
    require(saved.edits.has_value() && !original.empty(), saved.error);
    check(digestProject(*store).fingerprint != before, "authored pitch contributes to the project digest");

    auto restored = project(temp.path() / "checkpoint.adi", schema);
    {
        SQLite::Transaction transaction(restored->db());
        history_detail::installCheckpoint(*restored, history_detail::checkpointState(*store));
        transaction.commit();
    }
    check(pitchBytes(*restored, 7) == original &&
          digestProject(*restored).fingerprint == digestProject(*store).fingerprint,
          "history checkpoints include exact source identity and pitch bytes");

    History history(*store);
    const auto copy = copyClip(*store, 7, rate);
    require(copy.ok, copy.error);
    const auto paste = preparePaste(*store, copy, 8, 21, {0, 3 * beat}, rate);
    require(paste.ok, paste.error);
    const auto pasted = commitPrepared(*store, paste);
    require(pasted.ok, pasted.error);
    check(pitchBytes(*store, 8) == original, "clipboard copy retains authored source-clock edits");
    check(history.undo().ok && pitchBytes(*store, 8).empty() &&
          history.redo().ok && pitchBytes(*store, 8) == original,
          "clipboard Undo and Redo remove and restore the copied edit set");

    const auto split = prepareSplit(*store, 7, 9, {0, beat}, rate);
    require(split.ok, split.error);
    const auto splitResult = commitPrepared(*store, split);
    require(splitResult.ok, splitResult.error);
    check(pitchBytes(*store, 7) == original && pitchBytes(*store, 9) == original &&
          db.execAndGet("SELECT content_offset_ticks FROM clips WHERE id=9").getInt64() == beat,
          "split pieces retain one immutable source binding and trim through playback offsets");
    check(history.undo().ok && pitchBytes(*store, 7) == original && pitchBytes(*store, 9).empty() &&
          history.redo().ok && pitchBytes(*store, 9) == original,
          "split Undo and Redo restore clip and source edit ownership");

    commit(*store, "clip.delete", {{"id", 8}});
    check(pitchBytes(*store, 8).empty() && pitchBytes(*store, 7) == original,
          "delete removes only the edited clip's own row");
    check(history.undo().ok && pitchBytes(*store, 8) == original &&
          history.redo().ok && pitchBytes(*store, 8).empty(),
          "delete Undo and Redo retain the exact independent clip pitch data");

    commit(*store, "clip.detachAudio", {{"id", 7}});
    check(pitchBytes(*store, 7) == original, "detach leaves the clip's authored state dormant");
    commit(*store, "clip.attachAudio", {{"id", 7}, {"media", 2}, {"frames", rate}});
    auto replacement = analysis;
    replacement.source.hash = otherHash;
    check(pitchBytes(*store, 7) == original && !resolveClipPitchEdits(*saved.edits, replacement).state,
          "source replacement gives no pitch edits to different content");
    check(history.undo().ok && history.undo().ok &&
          pitchBytes(*store, 7) == original && resolveClipPitchEdits(*saved.edits, analysis).state,
          "undoing source replacement reactivates the matching authored source");
}

std::vector<std::int64_t> extractionIds(Store& store) {
    SQLite::Statement read(store.db(), "SELECT clip_id FROM clip_pitch_extractions ORDER BY clip_id");
    std::vector<std::int64_t> ids;
    while (read.executeStep()) ids.push_back(read.getColumn(0).getInt64());
    return ids;
}

void commitEdit(Store& store, const PreparedEdit& edit) {
    require(edit.ok, edit.error);
    const auto committed = commitPrepared(store, edit);
    require(committed.ok, committed.error);
}

void extractionLifecycle() {
    constexpr char marker[] = "clip_pitch_extractions";
    const auto schema = stagedSchema();
    test::TempDirectory temp{"pitch_clip_image", "extractions"};
    const auto path = temp.path() / "extraction.adi";
    auto store = project(path, schema);
    store->db().exec("INSERT INTO tracks(id,kind,name) VALUES(21,'midi','Extraction'),(22,'audio','Source');"
                     "INSERT INTO clips(id,track_id,kind,pos_ticks,length_ticks) VALUES"
                     "(7,21,'midi',0,23063040),(8,22,'audio',0,23063040)");
    const auto withoutMarker = digestProject(*store).fingerprint;
    store->db().exec("INSERT INTO clip_pitch_extractions(clip_id) VALUES(7)");
    check(digestProject(*store).fingerprint != withoutMarker, "the output marker contributes to the project digest");
    const std::vector<std::int64_t> original{7};
    {
        OpContext context{*store, store->db()};
        const auto image = captureImage(context, 7);
        const Payload expected = Payload::array({{{"clip_id", 7}}});
        check(image.at("children").value(marker, Payload{}) == expected,
              "clip capture includes the exact extraction row");

        auto checkpoint = project(temp.path() / "extraction-checkpoint.adi", schema);
        {
            SQLite::Transaction transaction(checkpoint->db());
            history_detail::installCheckpoint(*checkpoint, history_detail::checkpointState(*store));
            transaction.commit();
        }
        check(extractionIds(*checkpoint) == original &&
              digestProject(*checkpoint).fingerprint == digestProject(*store).fingerprint,
              "checkpoints replay and fingerprint the extraction row");

        History history(*store);
        {
            CopyIds ids(store->db());
            auto audioCopy = cloneImage(captureImage(context, 8), 9, ids);
            audioCopy["children"][marker] = Payload::array({{{"clip_id", 9}}});
            edit::RowEdits rows(store->db());
            createImage(rows, audioCopy);
            OpRequest request;
            request.opType = "clip.duplicate";
            request.payload = {{"id", 9}, {"changes", rows.changes()}};
            check(!OpJournal(*store).commit(request).ok && extractionIds(*store) == original,
                  "captured clip creation refuses ownership on an audio clip");
        }
        commit(*store, "track.delete", {{"id", 22}});
        check(extractionIds(*store) == original && history.undo().ok && extractionIds(*store) == original,
              "deleting or restoring the original source track leaves the placed output marker");
        commit(*store, "clip.delete", {{"id", 7}});
        check(extractionIds(*store).empty() && history.undo().ok && extractionIds(*store) == original &&
              history.redo().ok && extractionIds(*store).empty() && history.undo().ok &&
              extractionIds(*store) == original, "direct delete Undo and Redo replay the marker");

        const auto beforeSplit = digestProject(*store).fingerprint;
        commitEdit(*store, prepareSplit(*store, 7, 9, 10, {0, beat}, {0, 3 * beat}, rate));
        const std::vector<std::int64_t> pieces{7, 9, 10};
        const auto afterSplit = digestProject(*store).fingerprint;
        check(extractionIds(*store) == pieces && history.undo().ok && extractionIds(*store) == original &&
              digestProject(*store).fingerprint == beforeSplit && history.redo().ok &&
              extractionIds(*store) == pieces && digestProject(*store).fingerprint == afterSplit &&
              history.undo().ok, "an exact split and its inverse keep every descendant marked");

        const auto beforeRange = digestProject(*store).fingerprint;
        const RangeSelection inside{{21}, {0, beat}, {0, 3 * beat}};
        commitEdit(*store, prepareDeleteRange(*store, inside, rate));
        const auto rangePieces = extractionIds(*store);
        check(rangePieces.size() == 2 && rangePieces.front() == 7 && history.undo().ok &&
              extractionIds(*store) == original && digestProject(*store).fingerprint == beforeRange &&
              history.redo().ok && extractionIds(*store) == rangePieces && history.undo().ok,
              "replacement placement split remnants stay marked through Undo and Redo");
        commitEdit(*store, prepareDeleteRange(*store, {{21}, {0, 0}, {0, 4 * beat}}, rate));
        check(extractionIds(*store).empty() && history.undo().ok && extractionIds(*store) == original &&
              digestProject(*store).fingerprint == beforeRange,
              "placement deletion captures the whole marked clip and restores it exactly");

        const edit_plan::Move move{7, {21, {}}, {0, beat}};
        commitEdit(*store, edit_plan::prepareMove(*store, std::span(&move, 1), {rate, {}}));
        check(extractionIds(*store) == original && history.undo().ok && extractionIds(*store) == original,
              "moving a clip retains its marker and identity");

        const auto beforeMoveRange = digestProject(*store).fingerprint;
        commitEdit(*store, prepareMoveRange(*store, inside, {0, 6 * beat}, {rate, {}}));
        const auto movedPieces = extractionIds(*store);
        SQLite::Statement moved(store->db(),
            "SELECT count(*) FROM clip_pitch_extractions e JOIN clips c ON c.id=e.clip_id WHERE c.pos_ticks=?");
        moved.bind(1, 6 * beat);
        check(movedPieces.size() == 3 && moved.executeStep() && moved.getColumn(0).getInt() == 1,
              "moving part of the output marks both retained pieces and the relocated piece");
        moved.reset();
        check(history.undo().ok && extractionIds(*store) == original &&
              digestProject(*store).fingerprint == beforeMoveRange && history.redo().ok &&
              extractionIds(*store) == movedPieces && history.undo().ok,
              "one Undo restores the original extraction after a range move and Redo restores the pieces");

        auto copy = copyClip(*store, 7, rate);
        require(copy.ok, copy.error);
        const auto rangeCopy = copyRange(*store, inside, rate);
        check(rangeCopy.ok && rangeCopy.images.size() == 1 &&
              !copy.image.at("children").contains(marker) &&
              !rangeCopy.images.front().at("children").contains(marker),
              "clip and trimmed range clipboard captures detach replacement ownership");
        copy.image["children"][marker] = expected;
        copy.image["children"]["future_clip_detail"] = Payload::array({{{"clip_id", 7}}});
        commitEdit(*store, preparePaste(*store, copy, 11, 21, {0, 6 * beat}, rate));
        commitEdit(*store, prepareDuplicate(*store, 7, 12, 21, {0, 11 * beat}, rate));
        check(extractionIds(*store) == original &&
              store->db().execAndGet("SELECT count(*) FROM clips WHERE id IN (11,12)").getInt() == 2,
              "paste drops a legacy marker, ignores unknown children, and normal duplicate stays independent");

        auto trackCopy = detail::captureTrackForCopy(store->db(), 21);
        bool copiedMarker = false;
        for (const auto& row : trackCopy.at("rows")) copiedMarker |= row.at("table") == marker;
        check(!copiedMarker, "track clipboard capture detaches source extraction markers");
        const auto exactTrack = detail::captureTrack(store->db(), 21);
        for (const auto& row : exactTrack.at("rows"))
            if (row.at("table") == marker) trackCopy["rows"].push_back(row);
        const auto contents = detail::copyTrackContents(store->db(), trackCopy, 0, 4);
        commit(*store, "track.create", {{"id", contents.at("root")}, {"kind", "midi"}, {"contents", contents}});
        check(extractionIds(*store) == original, "track duplicate drops a marker in an older clipboard packet");

        const auto beforeTrackDelete = digestProject(*store).fingerprint;
        commit(*store, "track.delete", {{"id", 21}});
        check(extractionIds(*store).empty() && history.undo().ok && extractionIds(*store) == original &&
              digestProject(*store).fingerprint == beforeTrackDelete,
              "deleting the output's track restores exact marker ownership on Undo");
    }
    const auto beforeReopen = digestProject(*store).fingerprint;
    store.reset();
    StoreError error{};
    store = Store::open(path, error);
    require(static_cast<bool>(store), toString(error));
    check(extractionIds(*store) == original && digestProject(*store).fingerprint == beforeReopen,
          "reopening a saved staged project preserves the replacement marker");

    auto legacy = project(temp.path() / "legacy.adi", "");
    legacy->db().exec("INSERT INTO tracks(id,kind,name) VALUES(21,'midi','Legacy');"
                      "INSERT INTO clips(id,track_id,kind,pos_ticks,length_ticks) VALUES(7,21,'midi',0,5765760)");
    OpContext legacyContext{*legacy, legacy->db()};
    check(!captureImage(legacyContext, 7).at("children").contains(marker),
          "capture on legacy format keeps its original image shape");
    const auto legacyCopy = copyClip(*legacy, 7, rate);
    commitEdit(*legacy, preparePaste(*legacy, legacyCopy, 9, 21, {0, 2 * beat}, rate));
    check(legacy->db().execAndGet("SELECT count(*) FROM clips WHERE id=9").getInt() == 1,
          "legacy unmarked clip clipboard still pastes without the staged table");
}
} // namespace

int main() {
    try { lifecycle(); extractionLifecycle(); }
    catch (const std::exception& error) { check(false, error.what()); }
    std::printf("%d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
