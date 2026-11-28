// SPDX-License-Identifier: AGPL-3.0-or-later
#include "adi/audio/io_audit.hpp"
#include "adi/audio/warp_timing_map.hpp"
#include "adi/textproj.hpp"

#include <array>
#include <cmath>
#include <cstdio>
#include <tuple>

namespace {
using namespace adi;
using namespace adi::audio;
constexpr auto ppq = textproj::kPPQ;
int failures = 0;

void check(bool result, const char* what) {
    if (!result) {
        std::fprintf(stderr, "FAIL %s\n", what);
        ++failures;
    }
}

std::uint64_t inSource(std::uint64_t analysisFrame, std::uint32_t rate) {
    return (analysisFrame * rate + kWarpAnalysisRate / 2) / kWarpAnalysisRate;
}

void pickupAndChange() {
    constexpr std::size_t change = 7;
    constexpr std::size_t pickupBeats = 1;
    constexpr std::uint64_t period120 = kWarpAnalysisRate / 2;
    constexpr std::uint64_t period150 = kWarpAnalysisRate * 2 / 5;
    constexpr std::uint64_t fractionalOffset = 3;
    std::array<std::uint64_t, 16> beats{};
    beats.front() = kWarpAnalysisRate / 4 + fractionalOffset;
    for (std::size_t i = 1; i < beats.size(); ++i)
        beats[i] = beats[i - 1] + (i <= change ? period120 : period150);
    constexpr std::array<std::size_t, 4> downbeats{pickupBeats, 5, 9, 13};
    const auto regions = warpTempoRegions(beats);
    check(regions.size() == 2, "two sustained clocks give two constant regions");
    if (regions.size() != 2) return;
    check(regions[0].firstBeat == 0 && regions[0].bpm == 120.0 &&
          regions[1].firstBeat == change && regions[1].bpm == 150.0,
          "tempo change and snapped values recovered exactly");

    constexpr auto endTime = 7u;
    const auto first = buildWarpTimingMap(beats, downbeats, regions, 48000 * endTime, 48000, true);
    const auto doubled = buildWarpTimingMap(beats, downbeats, regions, 96000 * endTime, 96000, true);
    check(first && doubled, "build both maps from the shared canonical timing");
    if (!first || !doubled) return;
    check(first->trimFrames == inSource(beats[pickupBeats], 48000) &&
          doubled->trimFrames == inSource(beats[pickupBeats], 96000) &&
          first->markers.front() == WarpMarker{0, 0}, "the first clear downbeat is precisely 1.1.1 after pickup");
    constexpr std::array<std::size_t, 4> inner{5, change, 9, 13};
    check(first->markers.size() == inner.size() + 2 && first->markers.size() == doubled->markers.size(),
          "emit each bar and the interior tempo change once");
    if (first->markers.size() != inner.size() + 2 || first->markers.size() != doubled->markers.size()) return;
    for (std::size_t i = 0; i < inner.size(); ++i) {
        const auto& a = first->markers[i + 1];
        const auto& b = doubled->markers[i + 1];
        check(a.tick == static_cast<std::int64_t>(inner[i] - pickupBeats) * ppq && a.tick == b.tick,
              "bar/change ticks identical at 48 and 96 kHz");
        for (const auto& [marker, rate, trim] : {
                 std::tuple{a, 48000u, first->trimFrames}, std::tuple{b, 96000u, doubled->trimFrames}}) {
            const auto absolute = static_cast<std::uint64_t>(marker.sourceFrame) + trim;
            check((absolute * kWarpAnalysisRate + rate / 2) / rate == beats[inner[i]],
                  "source-rate anchor roundtrips to the canonical frame");
        }
    }
    check(first->markers.back().tick == doubled->markers.back().tick &&
          first->markers.back().sourceFrame == 48000 * endTime - static_cast<std::int64_t>(first->trimFrames),
          "both source endpoints cover the exact physical extent at one tick");
    const auto encoded = writeWarpMap(first->markers, 48000 * endTime - first->trimFrames);
    const auto decoded = readWarpMap(encoded.bytes, 48000 * endTime - first->trimFrames);
    check(encoded && decoded && decoded.markers == first->markers, "literal AWRP v1 roundtrip");

    const auto kept = buildWarpTimingMap(beats, downbeats, regions, 48000 * endTime, 48000, false);
    constexpr auto leadTicks = ppq / 2 + static_cast<std::int64_t>((fractionalOffset * ppq + period120 / 2) / period120);
    check(kept && kept->trimFrames == 0 && kept->markers.size() > 2 && kept->markers[1].tick == leadTicks,
          "Sample Start keeps and times the pickup before the downbeat");
    std::printf("Pickup %.3f ms, tempos %.2f to %.2f BPM, 48/96 kHz map ticks match\n",
                1000 * static_cast<double>(first->trimFrames) / 48000, regions[0].bpm, regions[1].bpm);
}

void ironJitterAndSnap() {
    std::array<std::uint64_t, 17> jitter{};
    for (std::size_t i = 0; i < jitter.size(); ++i) {
        const auto error = i == 0 || i + 1 == jitter.size() ? 0 : (i % 2 == 0 ? 512 : -512);
        jitter[i] = static_cast<std::uint64_t>(static_cast<std::int64_t>(i * 22050) + error);
    }
    constexpr std::array<std::size_t, 4> downbeats{0, 4, 8, 12};
    const auto regions = warpTempoRegions(jitter);
    check(regions.size() == 1 && regions.front().bpm == 120.0, "eleven-millisecond tracker jitter irons to one 120 BPM clock");
    const auto map = buildWarpTimingMap(jitter, downbeats, regions, 9u * 44100u, 44100, true);
    check(map && map->markers.size() == 5 && map->markers[1] == WarpMarker{88200, 4 * ppq},
          "interior bar anchor sits on the ironed grid");

    auto singleOutlier = jitter;
    singleOutlier[8] += 1300;
    const auto outlierRegions = warpTempoRegions(singleOutlier);
    check(outlierRegions.size() == 1 && outlierRegions.front().bpm == 120.0,
          "one isolated outlier does not split a stable tempo");

    std::array<std::uint64_t, 33> shifted{};
    for (std::size_t i = 0; i < shifted.size(); ++i)
        shifted[i] = i * 22050 + (i >= 16 ? 2205 : 0);
    const auto shiftedRegions = warpTempoRegions(shifted);
    check(shiftedRegions.size() >= 2 && shiftedRegions.front().bpm == 120.0 && shiftedRegions.back().bpm == 120.0,
          "steady clocks around a 50 ms phase shift retain the boundary");

    std::array<std::uint64_t, 33> fractional{};
    for (std::size_t i = 0; i < fractional.size(); ++i)
        fractional[i] = static_cast<std::uint64_t>(std::llround(static_cast<double>(i) * 60 * kWarpAnalysisRate / 83.5));
    const auto fractionalRegions = warpTempoRegions(fractional);
    check(fractionalRegions.size() == 1 && fractionalRegions.front().bpm == 83.5,
          "half-BPM snapping stays inside the accumulated phase budget");
}

void invalidInputs() {
    constexpr std::array<std::uint64_t, 3> beats{11025, 33075, 55125};
    constexpr std::array<std::size_t, 2> outOfOrder{1, 0};
    constexpr std::array<std::size_t, 1> pastEnd{3};
    constexpr std::array<WarpTempoRegion, 1> missingFirst{{{1, 120.0}}};
    constexpr std::array<std::uint64_t, 3> duplicates{11025, 33075, 33075};
    check(!buildWarpTimingMap(beats, {}, {}, 88200, 44100, true), "requires a measured downbeat when trimming");
    check(!buildWarpTimingMap(beats, outOfOrder, {}, 88200, 44100, false) &&
          !buildWarpTimingMap(beats, pastEnd, {}, 88200, 44100, false), "rejects malformed downbeat indices");
    check(!buildWarpTimingMap(beats, {}, missingFirst, 88200, 44100, false), "rejects malformed region boundaries");
    check(warpTempoRegions(duplicates).empty() && !buildWarpTimingMap(beats, {}, {}, 40000, 44100, false),
          "rejects duplicate beats and beats outside the source clock");
    const CallbackScope callback;
    check(warpTempoRegions(beats).empty() && !buildWarpTimingMap(beats, {}, {}, 88200, 44100, false),
          "both timing functions refuse the audio callback");
}

} // namespace

int main() {
    pickupAndChange();
    ironJitterAndSnap();
    invalidInputs();
    return failures != 0;
}
