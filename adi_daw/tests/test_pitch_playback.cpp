// SPDX-License-Identifier: AGPL-3.0-or-later
#include "replace_operator_new.hpp"
#include "temp_directory.hpp"
#include "adi/audio/io_audit.hpp"
#include "adi/audio/pitch_compare.hpp"
#include "adi/audio/pitch_segments.hpp"
#include "adi/audio/warp_map.hpp"
#include "adi/audio/wav_file.hpp"
#include "adi/engine/session.hpp"
#include "adi/history.hpp"
#include "adi/media/clip_pitch.hpp"
#include "adi/media/media_ops.hpp"
#include "adi/pitch_edit_store.hpp"
#include "adi/store.hpp"
#include "adi/textproj.hpp"

#include <SQLiteCpp/SQLiteCpp.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <numbers>

namespace {
std::atomic<unsigned> callbackAllocations{0};
}
void adi::test::countReplacedNew() noexcept {
    if (adi::audio::onAudioThread) ++callbackAllocations;
}

namespace {
using namespace adi;
constexpr std::uint32_t rate = 48000, sourceFrames = 24000;
constexpr std::int32_t blockFrames = 1024;
int checks = 0, failures = 0;
void check(bool passed, const char* label) {
    ++checks;
    if (!passed) {
        ++failures;
        std::printf("FAIL: %s\n", label);
    }
}

void writeTone(const std::filesystem::path& file, double hz) {
    std::vector<float> pcm(sourceFrames * 2);
    for (std::size_t i = 0; i < sourceFrames; ++i) {
        pcm[i * 2] = static_cast<float>(0.25 * std::sin(2 * std::numbers::pi * hz * static_cast<double>(i) / rate));
        pcm[i * 2 + 1] = pcm[i * 2] * 0.5f;
    }
    audio::WavWriter output(file, rate, 2, audio::WavFormat::Float32);
    output.write(pcm.data(), sourceFrames);
    output.close();
}

void load(engine::Session& session, const Store& store, bool exporting = false) {
    session.graph().setFadeFrames(0);
    engine::SessionSpec spec{2, rate, blockFrames};
    spec.exporting = exporting;
    if (!session.load(store, {}, spec)) throw std::runtime_error(session.error());
}

std::array<float, blockFrames> renderBlock(engine::Session& session) {
    if (!session.clips()->prime(std::chrono::seconds(10))) throw std::runtime_error("prime clip");
    std::array<float, blockFrames> left{}, right{};
    float* out[]{left.data(), right.data()};
    engine::AudioIo io;
    io.frames = blockFrames;
    io.out = out;
    io.numOut = 2;
    {
        audio::CallbackScope callback;
        session.process(io);
    }
    return left;
}

std::vector<float> render(engine::Session& session, std::int64_t start) {
    session.transport().locate(start);
    session.transport().play();
    std::vector<float> result;
    for (int block = 0; block < 8; ++block) {
        const auto left = renderBlock(session);
        result.insert(result.end(), left.begin(), left.end());
    }
    return result;
}

void checkComparison(engine::Session& session, const Store& store, std::span<const float> original,
                     std::span<const float> shifted) {
    using audio::PitchCompareSide;
    auto& transport = session.transport();
    auto& clips = *session.clips();
    std::string error;
    if (!clips.preparePitchComparison(7, PitchCompareSide::Original, error)) throw std::runtime_error(error);
    transport.locate(6000);
    transport.play();
    const auto first = renderBlock(session);
    check(std::equal(first.begin(), first.end(), original.begin()), "comparison plays the original at the running playhead");
    const auto before = transport.position();
    const auto revision = transport.revision();
    check(!clips.selectPitchComparison(8, PitchCompareSide::Corrected) &&
          clips.selectPitchComparison(7, PitchCompareSide::Corrected) &&
          transport.position() == before && transport.revision() == revision && before == 6000 + blockFrames,
          "selection keeps the existing clip and transport clock");
    const auto second = renderBlock(session);
    check(std::equal(second.begin(), second.end(), shifted.begin() + blockFrames),
          "the corrected side starts at the next running sample, with exact PCM");
    transport.loop(6000, 8000);
    transport.locate(7700);
    check(clips.selectPitchComparison(7, PitchCompareSide::Original), "switch back before a loop wrap");
    const auto wrapped = renderBlock(session);
    check(std::equal(wrapped.begin(), wrapped.begin() + 300, original.begin() + 1700) &&
          std::equal(wrapped.begin() + 300, wrapped.end(), original.begin()) && transport.position() == 6724,
          "A/B shares the transport's half-open loop down to the wrapping sample");
    transport.play(false);
    check(renderBlock(session) == std::array<float, blockFrames>{}, "the comparison is silent while transport is stopped");
    transport.loop(0, 0, false);
    clips.endPitchComparison();
    const auto resumed = render(session, 6000);
    check(!clips.selectPitchComparison(7, PitchCompareSide::Original) &&
          std::equal(resumed.begin(), resumed.end(), shifted.begin(), shifted.end()),
          "ending comparison returns to the ordinary corrected clip");

    engine::Session exportSession;
    load(exportSession, store, true);
    if (!exportSession.clips()->preparePitchComparison(7, PitchCompareSide::Original, error))
        throw std::runtime_error(error);
    const auto exported = render(exportSession, 6000);
    check(std::equal(exported.begin(), exported.end(), shifted.begin(), shifted.end()),
          "export keeps the committed correction during comparison");
}

double frequency(std::span<const float> pcm) {
    double first = 0, last = 0;
    unsigned count = 0;
    for (std::size_t i = 1; i < pcm.size(); ++i) {
        if (pcm[i - 1] <= 0 && pcm[i] > 0) {
            const auto frame = static_cast<double>(i - 1) -
                static_cast<double>(pcm[i - 1]) / (static_cast<double>(pcm[i]) - pcm[i - 1]);
            if (count == 0) first = frame;
            last = frame;
            ++count;
        }
    }
    return count > 1 ? rate * static_cast<double>(count - 1) / (last - first) : 0;
}

void run() {
    test::TempDirectory temp{"pitch_playback", "project"};
    const auto file = temp.path() / "tone.wav";
    writeTone(file, 220);
    StoreError error{};
    auto store = Store::create(temp.path() / "song.adi", error);
    if (!store) throw std::runtime_error(toString(error));
    std::ifstream schema(ADI_PITCH_STAGED_SCHEMA);
    if (!schema) throw std::runtime_error("read reserved pitch DDL");
    store->db().exec(std::string(std::istreambuf_iterator<char>(schema), std::istreambuf_iterator<char>()));
    auto& database = store->db();
    database.exec("INSERT INTO tracks(id,kind,name) VALUES(1,'audio','Tone'),(9,'master','Main');"
        "INSERT INTO clips(id,track_id,kind,time_base,pos_ns,length_ns) VALUES(7,1,'audio',1,0,500000000);");
    const auto imported = media::importMedia(*store, file, 1, 0);
    if (!imported.ok) throw std::runtime_error(imported.error);
    database.exec("INSERT INTO audio_clips(clip_id,media_id,src_len_frames) VALUES(7,1,24000)");
    const auto analysis = media::buildClipPitchAnalysis(*store, 7);
    if (!analysis || analysis.data->analysis.notes.size() != 1) throw std::runtime_error("analyze single note");

    engine::Session session;
    load(session, *store);
    callbackAllocations = 0;
    const auto original = render(session, 6000);
    const auto originalHz = frequency(original);
    check(std::abs(originalHz - 220) < 0.01, "the original physical clip plays through Session");

    audio::PitchEditState state;
    const std::array commands{audio::PitchEditCommand{0, audio::PitchSetShift{100}}};
    check(audio::applyPitchEdits(analysis.data->analysis, state, commands) == audio::PitchEditError::None,
          "prepare a +100-cent segment edit from the real analysis");
    const auto edit = clip_edit::prepareClipPitchEdits(*analysis.data, state);
    if (!edit.request) throw std::runtime_error(edit.error);
    const auto update = OpJournal(*store).commit(*edit.request);
    if (!update.ok || !session.refresh(*store)) throw std::runtime_error("commit and refresh pitch edits");
    const auto shifted = render(session, 6000);
    const auto shiftedHz = frequency(shifted);
    const auto shift = 1200 * std::log2(shiftedHz / originalHz);
    check(std::abs(shift - 100) < 1.0, "refresh renders the committed note edit through the ordinary paged source");
    const auto sources = session.clips()->externalSources();
    check(sources.size() == 1 && std::filesystem::equivalent(sources.front().path, file) &&
          sources.front().storedBlake3 == analysis.data->source.hash,
          "Session still verifies and reports the original pooled file");
    History history(*store);
    check(history.undo().ok && session.refresh(*store) && render(session, 6000) == original,
          "undo restores bit-identical ordinary clip playback");
    check(history.redo().ok && session.refresh(*store) && render(session, 6000) == shifted,
          "redo restores bit-identical edited playback");
    checkComparison(session, *store, original, shifted);

    database.exec("UPDATE audio_clips SET reverse=1,transpose_semis=12 WHERE clip_id=7");
    engine::Session transposed;
    load(transposed, *store);
    const double nativeShift = frequency(render(transposed, 2200));
    std::string comparisonError;
    if (!transposed.clips()->preparePitchComparison(7, audio::PitchCompareSide::Original, comparisonError))
        throw std::runtime_error(comparisonError);
    const double dryShift = frequency(render(transposed, 2200));
    check(transposed.clips()->selectPitchComparison(7, audio::PitchCompareSide::Corrected),
          "select corrected on a reversed and transposed clip");
    const double wetShift = frequency(render(transposed, 2200));
    check(std::abs(nativeShift - 2 * shiftedHz) < 0.5 && std::abs(dryShift - 440) < 0.5 &&
          std::abs(wetShift - nativeShift) < 0.5, "unwarped clip transpose and reverse apply to playback and both comparison lanes");
    check(render(transposed, sourceFrames / 2) == std::vector<float>(8 * blockFrames),
          "the transposed comparison ends at the same halved source duration as playback");
    std::printf("Reversed +12 st clip: %.3f Hz original, %.3f Hz corrected, %.3f Hz paged\n", dryShift, wetShift, nativeShift);
    database.exec("UPDATE audio_clips SET reverse=0 WHERE clip_id=7");

    const auto markers = audio::writeWarpMap(
        std::vector<WarpMarker>{{0, 0}, {sourceFrames, 2 * textproj::kPPQ}}, sourceFrames);
    if (!markers) throw std::runtime_error("build Warp fixture");
    database.exec("UPDATE clips SET time_base=0,pos_ticks=0,length_ticks=11531520,pos_ns=NULL,length_ns=NULL WHERE id=7");
    SQLite::Statement warp(database, "UPDATE audio_clips SET warp_enabled=1,warp_mode='re-pitch',"
                                     "warp_markers=? WHERE clip_id=7");
    warp.bind(1, markers.bytes.data(), static_cast<int>(markers.bytes.size()));
    warp.exec();
    engine::Session warped;
    load(warped, *store);
    const auto warpedHz = frequency(render(warped, 12000));
    check(std::abs(warpedHz * 2 - shiftedHz) < 0.5,
          "Re-Pitch stretches the edited source and ignores the retained clip transpose");
    database.exec("UPDATE audio_clips SET reverse=1 WHERE clip_id=7");
    engine::Session reversedWarp;
    load(reversedWarp, *store);
    const auto reversedWarpPcm = render(reversedWarp, 12000);
    if (!reversedWarp.clips()->preparePitchComparison(7, audio::PitchCompareSide::Corrected, comparisonError))
        throw std::runtime_error(comparisonError);
    check(reversedWarpPcm == render(reversedWarp, 12000) && frequency(reversedWarpPcm) > 100,
          "reversed Warp comparison uses the exact output samples of ordinary corrected playback");
    if (!reversedWarp.clips()->preparePitchComparison(7, audio::PitchCompareSide::Original, comparisonError, 4))
        throw std::runtime_error(comparisonError);
    const auto reversedDry = render(reversedWarp, 12000);
    check(std::abs(frequency(reversedDry) - 110) < 0.5 &&
          reversedWarp.clips()->selectPitchComparison(7, audio::PitchCompareSide::Corrected),
          "the reversed Re-Pitch comparison uses the same streaming clock for the original");
    const auto crossfade = render(reversedWarp, 12000);
    check(crossfade[0] == reversedDry[0] &&
          crossfade[1] == std::lerp(reversedDry[1], reversedWarpPcm[1], 0.25f) &&
          crossfade[2] == std::lerp(reversedDry[2], reversedWarpPcm[2], 0.5f) &&
          crossfade[3] == std::lerp(reversedDry[3], reversedWarpPcm[3], 0.75f) &&
          std::equal(crossfade.begin() + 4, crossfade.end(), reversedWarpPcm.begin() + 4),
          "the four-frame comparison ramp mixes both sides on the transport sample clock");
    check(callbackAllocations.load() == 0 && audio::callbackFileIo.load() == 0,
          "Session's edited source uses no callback allocations or file I/O");
    std::printf("Session pitch edit %.3f Hz to %.3f Hz (%+.3f cents), then Re-Pitch %.3f Hz\n",
                originalHz, shiftedHz, shift, warpedHz);

    database.exec("UPDATE audio_clips SET warp_enabled=0,warp_mode='none',warp_markers=NULL,reverse=0,transpose_semis=0 WHERE clip_id=7;"
                  "UPDATE clips SET time_base=1,pos_ticks=NULL,length_ticks=NULL,pos_ns=0,length_ns=500000000 WHERE id=7;");
    auto draft = audio::draftPitchSegments(analysis.data->analysis, {});
    if (!draft) throw std::runtime_error("draft source notes");
    const std::array split{audio::PitchSegmentationCommand{audio::PitchSplitSegment{
        draft.draft->geometry.segments.front().id, sourceFrames / 2}}};
    const std::array pitchRight{audio::PitchEditCommand{1, audio::PitchSetShift{100}}};
    if (audio::applyPitchSegmentation(analysis.data->analysis, *draft.draft, split) != audio::PitchEditError::None ||
        audio::applyPitchEdits(draft.draft->analysis, draft.draft->edits, pitchRight) != audio::PitchEditError::None)
        throw std::runtime_error("split source note and edit one child");
    const auto structural = clip_edit::prepareClipPitchEdits(*analysis.data, draft.draft->edits, &draft.draft->geometry);
    if (!structural.request || !OpJournal(*store).commit(*structural.request).ok)
        throw std::runtime_error("commit split pitch note");
    engine::Session splitSession;
    load(splitSession, *store);
    const auto leftHz = frequency(render(splitSession, 2500));
    const auto rightHz = frequency(render(splitSession, 13800));
    check(std::abs(leftHz - 220) < 0.01 && std::abs(rightHz - shiftedHz) < 0.05,
          "Session uses durable split geometry and shifts only the edited child");
    std::printf("Durable split at frame %u, left %.3f Hz, right %.3f Hz\n", sourceFrames / 2, leftHz, rightHz);

    const std::array deletion{
        audio::PitchSegmentationCommand{audio::PitchDeleteSegment{draft.draft->geometry.segments.front().id}},
        audio::PitchSegmentationCommand{audio::PitchDeleteSegment{draft.draft->geometry.segments.back().id}}};
    if (audio::applyPitchSegmentation(analysis.data->analysis, *draft.draft, deletion) != audio::PitchEditError::None)
        throw std::runtime_error("delete pitch segments");
    const auto onlyGeometry = clip_edit::prepareClipPitchEdits(*analysis.data, draft.draft->edits, &draft.draft->geometry);
    if (!onlyGeometry.request || !OpJournal(*store).commit(*onlyGeometry.request).ok)
        throw std::runtime_error("commit empty pitch geometry");
    engine::Session deleteSession;
    load(deleteSession, *store);
    check(deleteSession.clips()->waitForWarp() && render(deleteSession, 6000) == original,
          "deleting every segment restores the source PCM without silencing the clip");

    database.exec("UPDATE clip_pitch_edits SET analysis_version=99 WHERE clip_id=7");
    engine::Session geometryOnly;
    load(geometryOnly, *store);
    check(geometryOnly.clips()->waitForWarp() && render(geometryOnly, 6000) == original,
          "geometry alone stays audible even after its analysis version becomes unavailable");
    database.exec("UPDATE clip_pitch_edits SET analysis_version=" +
                  std::to_string(analysis.data->analysisVersion) + " WHERE clip_id=7");
    if (!history.undo().ok) throw std::runtime_error("restore active edits before source replacement");
    writeTone(file, 300);
    engine::Session changed;
    load(changed, *store);
    check(!changed.clips()->waitForWarp(), "offline preparation refuses source bytes that changed under authored edits");
    const auto problems = changed.clips()->decodeProblems();
    check(!problems.empty() && problems.front().find("clips#7") != std::string::npos,
          "the preparation failure names the edited clip");

    database.exec("UPDATE clip_pitch_edits SET edits=X'FF' WHERE clip_id=7");
    engine::Session malformed;
    load(malformed, *store);
    check(!malformed.clips()->problems().empty() && malformed.clips()->externalSources().empty(),
          "a malformed edit for the active source is reported");
    const auto replacement = temp.path() / "replacement.wav";
    writeTone(replacement, 300);
    if (!media::importMedia(*store, replacement, 2, 0).ok)
        throw std::runtime_error("import replacement source");
    database.exec("UPDATE audio_clips SET media_id=2 WHERE clip_id=7");
    engine::Session newSource;
    load(newSource, *store);
    check(newSource.clips()->problems().empty() && std::abs(frequency(render(newSource, 6000)) - 300) < 0.01,
          "a malformed dormant edit cannot silence a replacement source");
}
} // namespace

int main() {
    try { run(); }
    catch (const std::exception& error) { check(false, error.what()); }
    std::printf("%d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
