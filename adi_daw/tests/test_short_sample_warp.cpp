// SPDX-License-Identifier: AGPL-3.0-or-later
#include "adi/audio/io_audit.hpp"
#include "adi/audio/short_sample_warp.hpp"
#include "adi/textproj.hpp"

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
    const CallbackScope callback;
    check(planShortSampleWarp(press, "auto", loop, frames, rate).action == ShortSampleWarpAction::Invalid,
          "planning refuses callback work");
    return failures == 0 ? 0 : 1;
}
