// SPDX-License-Identifier: AGPL-3.0-or-later
#include "adi/audio/io_audit.hpp"
#include "adi/audio/decode.hpp"
#include "adi/audio/wav_file.hpp"
#include "adi/audio/short_sample_warp.hpp"
#include "adi/library/sample_analysis.hpp"
#include "adi/material_commands.hpp"
#include "adi/media/analysis_cache.hpp"
#include "adi/media/blake3.hpp"
#include "adi/store.hpp"
#include "adi/textproj.hpp"
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

    options.shortSamplePolicy = library::sampleWarpPolicyFromSettings({{"record.shortSampleMode", "warpedOneShot"}}, &analysis);
    options.clipId = 5;
    auto forced = clip_edit::prepareImportAudio(*store, options, rate);
    check(clip_edit::commitMaterial(*store, forced).ok &&
          store->db().execAndGet("SELECT warp_enabled FROM audio_clips WHERE clip_id=5").getInt() == 1 &&
          store->db().execAndGet("SELECT loop_enabled FROM clips WHERE id=5").getInt() == 0,
          "forced one-shot setting preserves Warp and leaves the clip loop off");
    check(store->db().execAndGet("SELECT bpm FROM tempo_map LIMIT 1").getDouble() == 127,
          "classification and short Warp never apply tempo to the song");
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
