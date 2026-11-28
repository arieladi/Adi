// SPDX-License-Identifier: AGPL-3.0-or-later
#include "temp_directory.hpp"
#include "adi/audio/decode.hpp"
#include "adi/audio/io_audit.hpp"
#include "adi/audio/wav_file.hpp"
#include "adi/history.hpp"
#include "adi/media/media_ops.hpp"
#include "adi/pitch_reanalysis.hpp"
#include "adi/row_capture.hpp"
#include "adi/store.hpp"

#include <SQLiteCpp/SQLiteCpp.h>

#include <bit>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <numbers>
#include <stdexcept>

namespace {
using namespace adi;
using namespace adi::audio;
using namespace adi::clip_edit;
using namespace adi::media;

constexpr std::uint32_t sampleRate = 48000;
constexpr std::uint32_t frames = 24000;
int checks = 0;
int failures = 0;

void check(bool passed, const char* description) {
    ++checks;
    if (!passed) {
        ++failures;
        std::printf("FAIL %s\n", description);
    }
}

void writeTone(const std::filesystem::path& path, double hz) {
    std::vector<float> mono(frames);
    for (std::size_t frame = 0; frame < mono.size(); ++frame) {
        const auto phase = 2.0 * std::numbers::pi * hz * static_cast<double>(frame) / sampleRate;
        mono[frame] = static_cast<float>(0.5 * std::sin(phase));
    }
    WavWriter writer(path, sampleRate, 1, WavFormat::Float32);
    writer.write(mono.data(), frames);
    writer.close();
}

struct Fixture {
    test::TempDirectory temporary{"pitch", "reanalysis"};
    std::filesystem::path first = temporary.path() / "first.wav";
    std::filesystem::path alternate = temporary.path() / "alternate.wav";
    DecodeCache decode{temporary.path() / "decoded"};
    AnalysisCache analysed{temporary.path() / "analysed"};
    std::unique_ptr<Store> store;

    explicit Fixture(bool installSchema = true) {
        writeTone(first, 220);
        writeTone(alternate, 440);
        StoreError error{};
        store = Store::create(temporary.path() / "voice.adi", error);
        if (!store) throw std::runtime_error(toString(error));
        auto& db = store->db();
        if (installSchema) {
            std::ifstream stream(ADI_PITCH_SCHEMA_PATH);
            if (!stream) throw std::runtime_error("Could not read pitch schema fixture");
            const std::string schema{std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
            db.exec(schema);
        }
        db.exec("INSERT INTO tracks(id,kind) VALUES(1,'audio');"
                "INSERT INTO clips(id,track_id,kind,pos_ticks,length_ticks) VALUES"
                "(11,1,'audio',0,5765760),(12,1,'audio',5765760,5765760)");
        for (const auto& source : {first, alternate}) {
            const auto imported = importMedia(*store, source, source == first ? 1 : 2, 0);
            if (!imported.ok) throw std::runtime_error(imported.error);
        }
        db.exec("INSERT INTO audio_clips(clip_id,media_id,src_start_frames,src_len_frames) "
                "VALUES(11,1,1000,20000),(12,1,0,24000)");
    }

    ClipPitchAnalysis base(std::int64_t id = 11) {
        auto prepared = buildClipPitchAnalysis(*store, id, {}, &decode, &analysed);
        if (!prepared || prepared.data->analysis.notes.size() != 1)
            throw std::runtime_error(prepared.detail.value_or("Expected one measured source note"));
        return std::move(*prepared.data);
    }

    PitchSegmentDraft author(const ClipPitchAnalysis& clip, double shift) {
        auto draft = draftPitchSegments(clip.analysis, {});
        if (!draft) throw std::runtime_error("Expected an initial pitch draft");
        const auto& note = clip.analysis.notes.front();
        const PitchSegmentationCommand split[] = {PitchSplitSegment{1, note.startFrame + (note.endFrame - note.startFrame) / 2}};
        if (applyPitchSegmentation(clip.analysis, *draft.draft, split) != PitchEditError::None)
            throw std::runtime_error("Could not split pitch fixture");
        const PitchEditCommand controls[] = {{0, PitchSetGain{-4}}, {1, PitchSetShift{shift}}};
        if (applyPitchEdits(draft.draft->analysis, draft.draft->edits, controls) != PitchEditError::None)
            throw std::runtime_error("Could not author pitch fixture");
        const auto request = prepareClipPitchEdits(clip, draft.draft->edits, &draft.draft->geometry);
        if (!request.request) throw std::runtime_error(request.error);
        const auto committed = OpJournal(*store).commit(*request.request);
        if (!committed.ok) throw std::runtime_error(committed.error);
        return std::move(*draft.draft);
    }

    [[nodiscard]] std::optional<Payload> row(std::int64_t clip = 11) const {
        return adi::detail::captureRow(store->db(), "clip_pitch_edits", "clip_id", clip);
    }
};

void freshAnalysisAndPolicies() {
    Fixture f;
    const auto base = f.base();
    const auto authored = f.author(base, 110);
    (void)f.author(f.base(12), -70);
    const auto original = f.row();
    const auto sibling = f.row(12);
    const auto oldCount = OpJournal(*f.store).count();
    std::string error;
    const AnalysisSection unrelated{3, 8, {std::byte{2}, std::byte{4}}};
    auto& ordinary = defaultAnalysisCache();
    check(ordinary.update(base.source, "FUTR", unrelated, error) == AnalysisCacheProblem::None,
          "fixture adds an unrelated ADIA section");
    const auto cached = f.analysed.read(base.source);
    if (!cached) throw std::runtime_error(cached.error);
    auto stale = cached.data->sections.at("PNOT");
    auto word = std::bit_cast<std::uint64_t>(5850.0);
    constexpr std::size_t firstNominalPitch = 8 + 2 * 8;
    for (std::size_t byte = 0; byte < 8; ++byte) {
        stale.bytes[firstNominalPitch + byte] = static_cast<std::byte>(word & 0xffu);
        word >>= 8;
    }
    check(ordinary.update(base.source, "PNOT", stale, error) == AnalysisCacheProblem::None,
          "ordinary cache accepts the deliberately distinguishable PNOT fixture");
    const auto ordinaryAnalysis = buildClipPitchAnalysis(*f.store, 11);
    check(ordinaryAnalysis && ordinaryAnalysis.data->analysis.notes.size() == 1 &&
          ordinaryAnalysis.data->analysis.notes.front().pitchCents == 5850,
          "an ordinary analysis request consumes a distinguishable cached pitch");
    const auto cacheBefore = ordinary.read(base.source);

    const auto start = std::chrono::steady_clock::now();
    const auto kept = preparePitchReanalysis(*f.store, 11, PitchReanalysisPolicy::PreserveCompatible, {}, &f.decode);
    const auto elapsed = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    check(kept && kept.rebuilt.data->analysis == base.analysis && kept.rebuilt.data->source == base.source &&
          kept.rebuilt.data->sourceStartFrame == 1000 && kept.rebuilt.data->sourceEndFrame == 21000,
          "explicit reanalysis bypasses PNOT, remeasures source PCM, and captures the clip window");
    check(kept.kept && kept.kept->state && kept.kept->state->notes == authored.edits.notes &&
          kept.kept->adjustedAnalysis == authored.analysis && !kept.clear,
          "preserve mode reprojects compatible structural edits and correction controls from the fresh source");
    const auto cacheAfter = ordinary.read(base.source);
    check(cacheBefore && cacheAfter && cacheBefore.data->sections == cacheAfter.data->sections &&
          cacheAfter.data->sections.at("FUTR") == unrelated, "preparation leaves complete ADIA section bytes alone");
    check(f.row() == original && f.row(12) == sibling && OpJournal(*f.store).count() == oldCount,
          "preserve preparation changes neither authored rows nor history");

    const auto reset = preparePitchReanalysis(*f.store, 11, PitchReanalysisPolicy::ResetAuthored, {}, &f.decode);
    check(reset && !reset.kept && reset.clear && reset.clear->requests.size() == 1 &&
          reset.clear->requests.front().opType == "clip.setPitchEdits" &&
          reset.clear->requests.front().payload.at("rows").empty() &&
          !reset.clear->requests.front().payload.contains("segmentation") && f.row() == original,
          "reset preparation stages precisely one existing clear op without mutating project state");
    if (!reset.clear) return;
    check(commitPrepared(*f.store, *reset.clear).ok && !f.row() && f.row(12) == sibling &&
          OpJournal(*f.store).count() == oldCount + 1,
          "reset clears only the selected clip's corrections and structural geometry with one op");
    History history(*f.store);
    check(history.undo().ok && f.row() == original && f.row(12) == sibling,
          "Undo restores existing authored state byte for byte");
    check(history.redo().ok && !f.row(), "Redo clears the authored record without needing another analysis");
    const auto noChange = preparePitchReanalysis(*f.store, 11, PitchReanalysisPolicy::ResetAuthored, {}, &f.decode);
    check(noChange && !noChange.clear && !noChange.kept, "an already unedited clip needs no history row");
    if (kept) {
        std::printf("Reanalyzed 500 ms source in %.2f ms; fresh pitch %.2f cents vs cached fixture 5850.00 cents\n",
                    elapsed, kept.rebuilt.data->analysis.notes.front().pitchCents);
    }
}

void staleDormantAndGuards() {
    Fixture f;
    const auto base = f.base();
    (void)f.author(base, 100);
    auto reset = preparePitchReanalysis(*f.store, 11, PitchReanalysisPolicy::ResetAuthored, {}, &f.decode);
    (void)f.author(base, 250);
    const auto updated = f.row();
    check(reset.clear.has_value(), "initial reset is capturable");
    if (reset.clear) {
        const auto refused = commitPrepared(*f.store, *reset.clear);
        check(!refused.ok && refused.stale && f.row() == updated, "a captured reset never erases a later pitch edit");
    }

    reset = preparePitchReanalysis(*f.store, 11, PitchReanalysisPolicy::ResetAuthored, {}, &f.decode);
    check(reset.clear.has_value(), "a newly prepared reset captures the current authored row");
    f.store->db().exec("UPDATE audio_clips SET media_id=2 WHERE clip_id=11");
    if (reset.clear) {
        const auto refused = commitPrepared(*f.store, *reset.clear);
        check(!refused.ok && f.row() == updated, "source identity is checked in the clear operation even with the same history head");
    }
    const auto newer = preparePitchReanalysis(*f.store, 11, PitchReanalysisPolicy::PreserveCompatible, {}, &f.decode);
    check(newer && newer.rebuilt.data->source.hash != base.source.hash && newer.kept && !newer.kept->state &&
          !newer.kept->error.empty() && !newer.clear && f.row() == updated &&
          newer.rebuilt.data->analysis.notes.size() == 1 &&
          std::abs(newer.rebuilt.data->analysis.notes.front().pitchCents - 6900) < 5,
          "source-changed edits remain dormant while preserve mode returns the new verified pitch");
    const auto resetDormant = preparePitchReanalysis(*f.store, 11, PitchReanalysisPolicy::ResetAuthored, {}, &f.decode);
    check(resetDormant && resetDormant.clear && commitPrepared(*f.store, *resetDormant.clear).ok && !f.row() &&
          History(*f.store).undo().ok && f.row() == updated, "explicit reset of dormant state is reversible byte for byte");

    writeTone(f.alternate, 330);
    const auto hashMismatch = preparePitchReanalysis(*f.store, 11, PitchReanalysisPolicy::ResetAuthored, {}, &f.decode);
    check(!hashMismatch && hashMismatch.rebuilt.problem == ClipPitchProblem::InvalidSource && !hashMismatch.clear && f.row() == updated,
          "changed PCM at the same path fails verification and leaves dormant state intact");
    std::stop_source cancellation;
    cancellation.request_stop();
    const auto cancelled = preparePitchReanalysis(*f.store, 11, PitchReanalysisPolicy::ResetAuthored, cancellation.get_token(), &f.decode);
    check(!cancelled && cancelled.rebuilt.problem == ClipPitchProblem::Cancelled && !cancelled.clear,
          "pre-cancelled reanalysis produces no partial clear request");
    const auto fileIo = callbackFileIo.load();
    {
        CallbackScope callback;
        const auto refused = preparePitchReanalysis(*f.store, 11, PitchReanalysisPolicy::ResetAuthored);
        check(!refused && refused.rebuilt.problem == ClipPitchProblem::Realtime && !refused.clear && callbackFileIo.load() == fileIo,
              "reanalysis rejects audio callback execution before accessing the file or caches");
    }

    Fixture old(false);
    const auto untouched = OpJournal(*old.store).count();
    const auto legacy = preparePitchReanalysis(*old.store, 11, PitchReanalysisPolicy::ResetAuthored, {}, &old.decode);
    check(legacy && !legacy.kept && !legacy.clear && !old.store->db().tableExists("clip_pitch_edits") &&
          OpJournal(*old.store).count() == untouched, "legacy project can reanalyze without installing schema or staging a no-op");
}

} // namespace

int main() {
    test::TempDirectory home{"pitch", "portable"};
#if defined(_WIN32)
    if (_wputenv_s(L"ADI_HOME", home.path().c_str()) != 0) throw std::runtime_error("Could not set test app data");
#else
    if (setenv("ADI_HOME", home.path().c_str(), 1) != 0) throw std::runtime_error("Could not set test app data");
#endif
    freshAnalysisAndPolicies();
    staleDormantAndGuards();
    std::printf("%d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
