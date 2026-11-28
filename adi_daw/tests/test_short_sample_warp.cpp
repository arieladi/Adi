// SPDX-License-Identifier: AGPL-3.0-or-later
#include "adi/audio/io_audit.hpp"
#include "adi/audio/decode.hpp"
#include "adi/audio/wav_file.hpp"
#include "adi/audio/short_sample_warp.hpp"
#include "adi/history.hpp"
#include "adi/library/sample_analysis.hpp"
#include "adi/material_commands.hpp"
#include "adi/media/analysis_cache.hpp"
#include "adi/media/blake3.hpp"
#include "adi/store.hpp"
#include "adi/textproj.hpp"
#include "adi/ui/project_document.hpp"
#include "adi/warp_commands.hpp"
#include "temp_directory.hpp"
#include <SQLiteCpp/SQLiteCpp.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>

namespace {
using namespace adi;
using namespace adi::audio;

int failures = 0;

void check(bool pass, const char* description) {
    if (!pass) {
        ++failures;
        std::printf("FAIL %s\n", description);
    }
}

void throughProducers() {
    constexpr std::uint32_t rate = 48000;
    test::TempDirectory temp{"short_warp", "setting"};
    const auto wave = temp.path() / "loop.wav";
    std::vector<float> samples(rate * 2);
    for (std::uint32_t i = 0; i < 4; ++i) samples[i * rate / 2] = 0.9f;
    {
        WavWriter writer(wave, rate, 1, WavFormat::Pcm24);
        writer.write(samples.data(), static_cast<std::uint32_t>(samples.size()));
        writer.close();
    }
    const auto hash = media::blake3File(wave);
    media::AnalysisCache cache(temp.path() / "analysed");
    DecodeCache decode(temp.path() / "decoded");
    library::SampleAnalysisService analysis(cache, decode);
    StoreError error{};
    auto store = Store::createEmpty(temp.path() / "fixture.adi", error);
    if (!store || !hash) throw std::runtime_error("cannot create an explicit Warp fixture");
    store->db().exec("INSERT INTO tracks(id,kind,name) VALUES(1,'audio','Clips')");
    store->db().exec("INSERT INTO tempo_map(id,pos_ticks,bpm) VALUES(1,0,127)");
    clip_edit::ImportOptions options;
    options.source = wave;
    options.track = 1;
    options.mediaId = 1;
    options.clipId = 1;
    options.shortSamplePolicy = library::sampleWarpPolicyFromSettings({{"record.shortSampleMode", "auto"}}, &analysis);
    auto drop = clip_edit::prepareImportAudio(*store, options, rate);
    check(clip_edit::commitMaterial(*store, drop).ok && !analysis.read(hash.hex) &&
          store->db().execAndGet("SELECT warp_enabled FROM audio_clips WHERE clip_id=1").getInt() == 0,
          "ordinary drop with Auto performs no analysis and leaves the clip clean");

    clip_edit::WarpOptions button;
    button.shortSamplePolicy = options.shortSamplePolicy;
    const auto pressed = clip_edit::prepareSetWarp(*store, 1, button, rate);
    check(clip_edit::commitPrepared(*store, pressed).ok && analysis.read(hash.hex).fromCache &&
          store->db().execAndGet("SELECT loop_len_ticks FROM clips WHERE id=1").getInt64() == textproj::kWhole,
          "the explicit Warp producer reads Auto, classifies on a miss and captures a one-bar loop");
    const auto count = decode.stats().passthrough;
    options.warp = true;
    options.clipId = 3;
    auto second = clip_edit::prepareImportAudio(*store, options, rate);
    check(clip_edit::commitMaterial(*store, second).ok && decode.stats().passthrough == count &&
          store->db().execAndGet("SELECT loop_len_ticks FROM clips WHERE id=3").getInt64() == textproj::kWhole,
          "the explicit import prompt reuses the same cached classification and one-bar result");

    auto off = button;
    off.enabled = false;
    const auto disabled = clip_edit::prepareSetWarp(*store, 1, off, rate);
    const auto warpAndLoop = [&] {
        return store->db().execAndGet("SELECT 2*warp_enabled+loop_enabled FROM audio_clips "
                                     "JOIN clips ON clips.id=audio_clips.clip_id WHERE clip_id=1").getInt();
    };
    History history(*store);
    check(clip_edit::commitPrepared(*store, disabled).ok && warpAndLoop() == 0,
          "turning Warp off on the generated full-source loop also turns looping off");
    check(history.undo().ok && warpAndLoop() == 3 && history.redo().ok && warpAndLoop() == 0,
          "undo and redo restore the captured Warp and loop states together");

    const auto storedMap = store->db().execAndGet("SELECT hex(warp_markers) FROM audio_clips WHERE clip_id=1").getString();
    auto reenable = button;
    reenable.shortSamplePolicy = library::sampleWarpPolicyFromSettings(
        {{"record.shortSampleMode", "unwarpedOneShot"}}, &analysis);
    const auto fromStoredMap = clip_edit::prepareSetWarp(*store, 1, reenable, rate);
    check(!storedMap.empty() && clip_edit::commitPrepared(*store, fromStoredMap).ok && warpAndLoop() == 2 &&
          store->db().execAndGet("SELECT hex(warp_markers) FROM audio_clips WHERE clip_id=1").getString() == storedMap,
          "re-enabling an existing Warp map preserves its bytes and the current loop state");

    ui::ProjectDocument::MaterialJob job;
    job.project = store->path();
    job.source = wave;
    job.track = 1;
    job.clipId = 9;
    job.mediaId = 3;
    job.rate = rate;
    job.choices.warp = true;
    job.shortSamplePolicy = library::sampleWarpPolicyFromSettings({{"record.shortSampleMode", "unwarpedOneShot"}}, &analysis);
    auto fromDocument = ui::ProjectDocument::prepareMaterial(job);
    check(clip_edit::commitMaterial(*store, fromDocument).ok &&
          store->db().execAndGet("SELECT warp_enabled FROM audio_clips WHERE clip_id=9").getInt() == 0,
          "the import worker honors the short-sample setting captured in the document's job");

    auto manual = reenable;
    manual.analysis.sourceBpm = 90;
    const auto corrected = clip_edit::prepareSetWarp(*store, 9, manual, rate);
    check(clip_edit::commitPrepared(*store, corrected).ok &&
          store->db().execAndGet("SELECT src_start_frames FROM audio_clips WHERE clip_id=9").getInt64() == 0 &&
          store->db().execAndGet("SELECT length_ticks FROM clips WHERE id=9").getInt64() == 3 * textproj::kPPQ &&
          store->db().execAndGet("SELECT loop_enabled FROM clips WHERE id=9").getInt() == 0,
          "manual source BPM overrides the automatic policy and pins the original source start");

    options.shortSamplePolicy = library::sampleWarpPolicyFromSettings({{"record.shortSampleMode", "warpedOneShot"}}, &analysis);
    options.clipId = 5;
    auto forced = clip_edit::prepareImportAudio(*store, options, rate);
    check(clip_edit::commitMaterial(*store, forced).ok &&
          store->db().execAndGet("SELECT warp_enabled FROM audio_clips WHERE clip_id=5").getInt() == 1 &&
          store->db().execAndGet("SELECT loop_enabled FROM clips WHERE id=5").getInt() == 0,
          "forced one-shot setting preserves Warp and leaves the clip loop off");
    check(store->db().execAndGet("SELECT bpm FROM tempo_map LIMIT 1").getDouble() == 127,
          "classification and short Warp never apply tempo to the song");

    const auto crash = temp.path() / "single-crash.wav";
    std::uint32_t random = 29;
    float previous = 0;
    for (std::size_t frame = 0; frame < samples.size(); ++frame) {
        random = random * 1664525u + 1013904223u;
        const auto noise = static_cast<float>(random >> 16) / 32768.0f - 1.0f;
        samples[frame] = (noise - previous) * 0.4f *
            static_cast<float>(std::exp(-5.5 * static_cast<double>(frame) / rate));
        previous = noise;
    }
    {
        WavWriter writer(crash, rate, 1, WavFormat::Pcm24);
        writer.write(samples.data(), static_cast<std::uint32_t>(samples.size()));
        writer.close();
    }
    options.source = crash;
    options.clipId = 7;
    options.mediaId = 2;
    options.warp = false;
    options.shortSamplePolicy = button.shortSamplePolicy;
    auto cleanCrash = clip_edit::prepareImportAudio(*store, options, rate);
    const auto crashHash = media::blake3File(crash);
    check(clip_edit::commitMaterial(*store, cleanCrash).ok && crashHash, "import a single decaying cymbal-like hit");
    const auto crashWarp = clip_edit::prepareSetWarp(*store, 7, button, rate);
    const auto measuredCrash = analysis.read(crashHash.hex);
    check(crashWarp.ok && crashWarp.requests.empty() && measuredCrash &&
          measuredCrash.analysis->classification.kind == SampleKind::OneShot,
          "Auto treats a decaying crash as a single one-shot and captures no Warp edit");
}
} // namespace

int main() {
    constexpr auto press = ShortSampleWarpTrigger::WarpPress;
    constexpr auto import = ShortSampleWarpTrigger::ImportWarp;
    constexpr std::uint32_t rate = 48000;
    constexpr std::uint64_t frames = rate * 24 / 10; // one 4/4 bar at 100 BPM
    const SampleClassification loop{SampleKind::Loop, SampleKindSource::Detection, 100.0, 1u};
    const SampleClassification oneShot{SampleKind::OneShot, SampleKindSource::Detection, {}, {}};
    const SampleClassification unknown;

    for (const auto mode : {"auto", "unwarpedOneShot", "warpedOneShot", "warpedLoop"}) {
        const auto drop = planShortSampleWarp(ShortSampleWarpTrigger::Drop, mode, loop, frames, rate);
        check(drop.action == ShortSampleWarpAction::KeepUnwarped && !drop.sourceBpm && !drop.loopMarkers,
              "all modes keep ordinary drop clean");
        const auto longFile = planShortSampleWarp(press, mode, loop, 61ull * rate, rate);
        check(longFile.action == ShortSampleWarpAction::UseAutoWarp && !longFile.sourceBpm && !longFile.loopMarkers,
              "long sample delegates an explicit Warp request to the ordinary producer");
    }
    const auto shortLimit = static_cast<std::uint64_t>(kShortSampleSeconds) * rate;
    check(planShortSampleWarp(press, "unwarpedOneShot", oneShot, shortLimit, rate).action ==
              ShortSampleWarpAction::KeepUnwarped &&
          planShortSampleWarp(press, "unwarpedOneShot", oneShot, shortLimit + 1, rate).action ==
              ShortSampleWarpAction::UseAutoWarp,
          "the named provisional boundary changes policy one source frame above the limit");
    for (const auto trigger : {press, import}) {
        const auto detected = planShortSampleWarp(trigger, "auto", loop, frames, rate, 127.0);
        check(detected.action == ShortSampleWarpAction::WarpedLoop && detected.sourceBpm == 100.0 &&
                  detected.loopMarkers == std::array<WarpMarker, 2>{{{0, 0}, {frames, textproj::kWhole}}},
              "Auto uses the resolved loop clock even at a different project tempo");
        if (detected.loopMarkers) {
            const auto encoded = writeWarpMap(*detected.loopMarkers, frames);
            const auto decoded = readWarpMap(encoded.bytes, frames);
            check(encoded && decoded && std::ranges::equal(*detected.loopMarkers, decoded.markers),
                  "two planned markers round-trip through the existing AWRP codec");
        }
        check(planShortSampleWarp(trigger, "auto", oneShot, frames, rate).action == ShortSampleWarpAction::KeepUnwarped &&
                  planShortSampleWarp(trigger, "auto", unknown, frames, rate).action == ShortSampleWarpAction::KeepUnwarped,
              "Auto keeps one-shots and unknown sources clean");
        check(planShortSampleWarp(trigger, "unwarpedOneShot", loop, frames, rate).action == ShortSampleWarpAction::KeepUnwarped,
              "forced Unwarped One Shot overrides a measured loop");
        const auto shot = planShortSampleWarp(trigger, "warpedOneShot", loop, frames, rate);
        check(shot.action == ShortSampleWarpAction::WarpedOneShot && shot.sourceBpm == 100.0 && !shot.loopMarkers,
              "forced Warped One Shot gives ordinary Warp the known source clock and keeps loop off");
        const auto shotWithHint = planShortSampleWarp(trigger, "warpedOneShot", oneShot, frames, rate, 127.0);
        check(shotWithHint.action == ShortSampleWarpAction::WarpedOneShot && shotWithHint.sourceBpm == 127.0,
              "forced Warped One Shot can use a source tempo hint");
        const auto forced = planShortSampleWarp(trigger, "warpedLoop", unknown, 8ull * rate, rate, 118.0);
        check(forced.action == ShortSampleWarpAction::WarpedLoop && forced.sourceBpm == 120.0 &&
                  forced.loopMarkers == std::array<WarpMarker, 2>{{{0, 0}, {8 * rate, 4 * textproj::kWhole}}},
              "forced Warped Loop picks allowed bars and derives an endpoint-consistent source clock");
        const SampleClassification taggedLoop{SampleKind::Loop, SampleKindSource::FileTag, {}, {}};
        check(planShortSampleWarp(trigger, "auto", taggedLoop, 8ull * rate, rate).loopMarkers == forced.loopMarkers,
              "an authoritative loop tag also works without measured timing");
    }
    check(planShortSampleWarp(press, "badSetting", loop, frames, rate).action == ShortSampleWarpAction::Invalid,
          "unknown mode names are rejected");
    check(planShortSampleWarp(press, "auto", loop, 0, rate).action == ShortSampleWarpAction::Invalid,
          "empty sources are rejected");
    check(planShortSampleWarp(press, "warpedOneShot", oneShot, frames, rate,
              std::numeric_limits<double>::quiet_NaN()).sourceBpm == kShortSampleWarpTempoHint,
          "non-finite tempo hints fall back to the provisional default");
    {
        const CallbackScope callback;
        check(planShortSampleWarp(press, "auto", loop, frames, rate).action == ShortSampleWarpAction::Invalid,
              "planning refuses callback work");
    }
    try { throughProducers(); }
    catch (const std::exception& error) { check(false, error.what()); }
    return failures == 0 ? 0 : 1;
}
