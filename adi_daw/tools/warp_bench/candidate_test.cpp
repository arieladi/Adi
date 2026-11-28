// SPDX-License-Identifier: AGPL-3.0-or-later
#include "adi/audio/io_audit.hpp"
#include "adi/audio/warp_beat_candidate.hpp"
#include "adi/textproj.hpp"

#include <algorithm>
#include <cmath>
#include <iostream>
#include <numbers>

namespace {
using namespace adi;
using namespace adi::audio;
constexpr auto rate = kWarpAnalysisRate;
constexpr auto quarter = textproj::kPPQ;
int failures = 0;

void check(bool condition, const char* what) {
    if (condition) return;
    ++failures;
    std::cerr << what << '\n';
}

WarpBeatCandidateResult detect(std::span<const float> pcm, std::size_t chunkSize) {
    WarpBeatCandidateAnalyzer analyzer;
    while (!pcm.empty()) {
        const auto count = std::min(pcm.size(), chunkSize);
        if (!analyzer.append(pcm.first(count))) return {WarpBeatCandidateError::InvalidSource, {}, {}, {}, 0};
        pcm = pcm.subspan(count);
    }
    return analyzer.finish();
}

std::vector<float> accentedClicks(std::uint32_t sourceRate, bool accents = true) {
    std::vector<float> clicks(16 * sourceRate + sourceRate / 250, 0.0f);
    const auto length = sourceRate / 500;
    for (std::size_t index = 0; index < 32; ++index) {
        const auto start = index * (sourceRate / 2) + sourceRate / 20;
        for (std::size_t offset = 0; offset < length; ++offset) {
            const double t = static_cast<double>(offset) / static_cast<double>(length);
            clicks[start + offset] = static_cast<float>((1.0 - t) * std::sin(t * (accents && index % 4 == 0 ? 13.0 : 61.0)));
        }
    }
    return clicks;
}

void streaming() {
    const auto clicks = accentedClicks(rate);
    const auto reference = detect(clicks, clicks.size());
    check(reference && reference.beats.size() >= 28 && !reference.downbeats.empty(), "steady accented clicks yield beats and downbeats");
    check(std::abs(reference.medianBpm - 120.0) < 2.0, "candidate median tempo within 2 BPM");
    check(reference.tempoRegions.size() == 1 && std::abs(reference.tempoRegions.front().bpm - 120.0) < 0.3,
          "stable tempo region smooths the 512-frame timing steps");
    for (std::size_t chunk : {1u, 17u, 512u, 777u}) {
        const auto actual = detect(clicks, chunk);
        check(actual && actual.beats == reference.beats && actual.downbeats == reference.downbeats,
              "arbitrary PCM block partition preserves beats and downbeats exactly");
    }
    const std::vector<float> silence(rate * 3, 0.0f);
    check(detect(silence, 511).error == WarpBeatCandidateError::NoBeat, "silence leaves no beat estimate");
    std::stop_source stop;
    stop.request_stop();
    WarpBeatCandidateAnalyzer cancelled;
    check(!cancelled.append(clicks, stop.get_token()) && cancelled.finish().error == WarpBeatCandidateError::Cancelled,
          "stopping an append yields no partial result");
}

void unaccentedBeats() {
    const auto pcm = accentedClicks(rate, false);
    const auto result = detect(pcm, 777);
    check(result && result.beats.size() >= 28 && result.downbeats.empty(),
          "identical recurring clicks establish a steady pulse without a bar phase");
    check(!buildWarpBeatCandidateMap(result, pcm.size(), rate, true),
          "first-downbeat placement refuses to trim to an arbitrary click");
    const auto map = buildWarpBeatCandidateMap(result, pcm.size(), rate, false);
    check(map && map->trimFrames == 0, "an ambiguous bar phase preserves the source start");
}

void compareSourceRates() {
    const auto resampled = [](std::uint32_t sourceRate) {
        const auto pcm = accentedClicks(sourceRate);
        WarpBeatCandidateAnalyzer analyzer;
        WarpAnalysisPcm bridge(sourceRate, 1, [&](std::span<const float> samples) { return analyzer.append(samples); });
        if (!bridge.append(pcm) || !bridge.finish()) return WarpBeatCandidateResult{WarpBeatCandidateError::InvalidSource, {}, {}, {}, 0};
        return analyzer.finish();
    };
    const auto at48 = resampled(48000);
    const auto at96 = resampled(96000);
    check(at48 && at96 && at48.beats == at96.beats && at48.downbeats == at96.downbeats,
          "the same click audio at 48 and 96 kHz produces exactly the same 44.1 kHz grid");
}

void compoundMeter() {
    constexpr double tempo = 80.0;
    constexpr auto eighth = static_cast<std::size_t>(30.0 * rate / tempo);
    constexpr auto pickup = 2 * eighth;
    const std::vector<double> backupKick{0.32, 0.48, 0.52, 0.44};
    std::vector<float> audio(49 * eighth, 0.0f);
    for (std::size_t index = 0; index < 48; ++index) {
        const auto phase = (index + 4) % 6;
        const auto start = index * eighth;
        const auto kick = phase == 0 ? 0.90 : (phase == 3 ? backupKick[(index / 6) % backupKick.size()] : 0.0);
        for (std::size_t i = 0; i < rate / 16; ++i) {
            const auto seconds = static_cast<double>(i) / rate;
            const auto carrier = 2.0 * std::numbers::pi * seconds;
            const auto low = kick * std::sin(73.0 * carrier) * std::exp(-38.0 * seconds);
            const auto high = 0.40 * std::sin(2480.0 * carrier) * std::exp(-150.0 * seconds);
            audio[start + i] = static_cast<float>(low + high);
        }
    }
    WarpBeatCandidateAnalyzer analyzer(WarpBeatMeter{6, 8, tempo});
    check(analyzer.append(audio), "6/8 drum performance accepted");
    const auto result = analyzer.finish();
    check(result && result.ticksPerPulse == quarter / 2 && std::abs(result.medianBpm - tempo) < 2.0,
          "known 6/8 source tempo resolves the tracked eighth pulse on the quarter clock");
    check(result.downbeats.size() >= 7, "most accented 6/8 bar starts detected");
    if (result.downbeats.size() < 7) return;
    for (std::size_t i = 0; i < result.downbeats.size(); ++i) {
        const auto actual = static_cast<double>(result.beats[result.downbeats[i]]);
        const auto expected = static_cast<double>(pickup + i * 6 * eighth);
        check(std::abs(actual - expected) < 0.05 * rate, "6/8 accents distinguish the downbeat from the other dotted pulse");
    }
    const auto map = buildWarpBeatCandidateMap(result, audio.size(), rate, true);
    check(map && std::abs(static_cast<double>(map->trimFrames) - static_cast<double>(pickup)) < 0.05 * rate,
          "6/8 pickup ends at the first marked bar");
    if (!map) return;
    check(std::ranges::any_of(map->markers, [](const auto& marker) {
        return marker.tick == 3 * quarter && std::abs(static_cast<double>(marker.sourceFrame) - 2.25 * rate) < 0.05 * rate;
    }), "six detected eighths map to exactly three quarter notes in AWRP");

    WarpBeatCandidateAnalyzer unmatched(WarpBeatMeter{6, 8, 200.0});
    check(unmatched.append(audio) && unmatched.finish().error == WarpBeatCandidateError::NoBeat,
          "a mismatched source-tempo hint does not publish a conflicting 6/8 clock");
}

void pickupAndTempoShift() {
    WarpBeatCandidateResult result;
    result.beats = {11025, 33075, 55125, 77175, 99225, 116865, 134505, 152145, 169785};
    result.downbeats = {2, 6};
    result.tempoRegions = {{0, 120.0}, {4, 150.0}};

    for (std::uint32_t sourceRate : {44100u, 48000u, 96000u}) {
        const auto frame = [sourceRate](std::uint64_t analysisFrame) {
            return analysisFrame * sourceRate / rate;
        };
        const auto end = 4 * sourceRate;
        const auto map = buildWarpBeatCandidateMap(result, end, sourceRate, true);
        check(map && map->trimFrames == frame(55125), "first predicted downbeat sets the source trim despite pickup");
        if (!map) continue;
        check(map->markers.front() == WarpMarker{0, 0}, "first downbeat becomes musical 1.1.1");
        check(map->markers.size() == 4 && map->markers[1] ==
              WarpMarker{static_cast<std::int64_t>(frame(99225) - frame(55125)), 2 * quarter},
              "stable tempo transition gets an exact interior tick/frame anchor");
        check(map->markers.size() == 4 && map->markers[2] ==
              WarpMarker{static_cast<std::int64_t>(frame(134505) - frame(55125)), 4 * quarter},
              "subsequent bar starts at the same musical tick at each source rate");
        check(static_cast<bool>(writeWarpMap(map->markers, end - map->trimFrames)), "candidate produces canonical AWRP");
        const auto full = buildWarpBeatCandidateMap(result, end, sourceRate, false);
        check(full && full->trimFrames == 0 && full->markers.front() == WarpMarker{0, 0},
              "sample start option preserves the pickup");
    }
    result.downbeats.clear();
    check(!buildWarpBeatCandidateMap(result, rate * 4, rate, true), "missing downbeat refuses an invented bar origin");
    CallbackScope callback;
    WarpBeatCandidateAnalyzer realtime;
    check(realtime.finish().error == WarpBeatCandidateError::Realtime, "candidate is rejected on the callback");
}
} // namespace

int main() {
    streaming();
    unaccentedBeats();
    compareSourceRates();
    compoundMeter();
    pickupAndTempoShift();
    return failures == 0 ? 0 : 1;
}
