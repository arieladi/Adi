// SPDX-License-Identifier: AGPL-3.0-or-later
#include "adi/audio/wav_file.hpp"
#include "adi/clip_commands.hpp"
#include "adi/digest.hpp"
#include "adi/history.hpp"
#include "adi/media/blake3.hpp"
#include "adi/media/clip_pitch.hpp"
#include "adi/media/media_ops.hpp"
#include "adi/pitch_edit_store.hpp"
#include "adi/store.hpp"
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

void lifecycle() {
    std::ifstream sql(ADI_PITCH_STAGED_SCHEMA);
    require(sql.is_open(), "cannot open staged pitch schema");
    const std::string schema{std::istreambuf_iterator<char>(sql), std::istreambuf_iterator<char>()};
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
} // namespace

int main() {
    try { lifecycle(); }
    catch (const std::exception& error) { check(false, error.what()); }
    std::printf("%d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
