// SPDX-License-Identifier: AGPL-3.0-or-later
#include "adi/audio/spectral_onsets.hpp"
#include "adi/audio/warp_analysis_pcm.hpp"
#include "adi/audio/warp_analysis_settings.hpp"
#include "adi/audio/io_audit.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <limits>
#include <numbers>
#include <vector>

namespace {
using namespace adi::audio;
int failures = 0;

void check(bool value, const char* what) {
    if (!value) {
        std::fprintf(stderr, "FAIL %s\n", what);
        ++failures;
    }
}

constexpr std::array<double, 4> attacks{0.10, 0.36, 0.62, 0.90};

std::vector<float> makeAttacks(std::uint32_t rate, double amplitude = 0.7) {
    std::vector<float> frames(rate);
    for (const double onset : attacks) {
        const auto start = static_cast<std::size_t>(std::round(onset * rate));
        for (std::size_t frame = start; frame < start + rate / 14; ++frame) {
            const double time = static_cast<double>(frame - start) / rate;
            const double envelope = std::min(1.0, time * 800) * std::exp(-110 * time);
            frames[frame] += static_cast<float>(amplitude * envelope *
                (.8 * std::sin(2 * std::numbers::pi * 420 * time) +
                 .2 * std::sin(2 * std::numbers::pi * 2700 * time)));
        }
    }
    return frames;
}

SpectralOnsetResult measure(std::span<const float> samples, std::uint32_t rate, std::uint32_t channels,
                            std::size_t blockFrames, int sensitivity = 50) {
    SpectralOnsetAnalyzer analyzer(rate, channels, sensitivity);
    while (!samples.empty()) {
        const auto count = std::min(samples.size(), blockFrames * channels);
        if (!analyzer.append(samples.first(count))) break;
        samples = samples.subspan(count);
    }
    return analyzer.finish();
}

void fixedRateAndStereo() {
    constexpr auto firstRate = 48000u;
    constexpr auto secondRate = 96000u;
    const auto original = makeAttacks(firstRate);
    auto first = measure(original, firstRate, 1, 337);
    auto second = measure(makeAttacks(secondRate), secondRate, 1, 401);
    auto unchunked = measure(original, firstRate, 1, original.size());
    check(first && second && unchunked, "read equivalent sources at both rates");
    check(first.onsets.size() == attacks.size() && second.onsets.size() == attacks.size(), "one hit per labelled attack");
    check(first.onsets == unchunked.onsets, "source block size has no effect");
    if (first.onsets.size() == attacks.size() && second.onsets.size() == attacks.size()) {
        double maxErrorMs = 0;
        for (std::size_t i = 0; i < first.onsets.size(); ++i) {
            const auto& a = first.onsets[i];
            const auto& b = second.onsets[i];
            check(a.analysisFrame == b.analysisFrame, "48 and 96 kHz yield the same canonical onset frame");
            check(a.strength > 0 && a.strength <= 1 && std::abs(a.strength - b.strength) < 0.01f, "level-independent stable hit strength range");
            check(a.peakDbfs > -16 && a.peakDbfs < 0 && std::abs(a.peakDbfs - b.peakDbfs) < 0.01f, "known attack peak range");
            maxErrorMs = std::max(maxErrorMs, 1000 * std::abs(static_cast<double>(a.analysisFrame) / kWarpAnalysisRate - attacks[i]));
        }
        check(maxErrorMs < 3, "attack timing within three milliseconds");
        std::printf("48/96 kHz onsets, maximum error %.3f ms\n", maxErrorMs);
    }

    std::vector<float> opposite(original.size() * 2);
    for (std::size_t i = 0; i < original.size(); ++i) {
        opposite[i * 2] = original[i];
        opposite[i * 2 + 1] = -original[i];
    }
    const auto stereo = measure(opposite, firstRate, 2, 251);
    check(stereo && stereo.onsets == first.onsets, "opposite stereo polarity retains onsets and level");
}

void sensitivityAndVibrato() {
    const auto source = makeAttacks(kWarpAnalysisRate, 0.002);
    const auto high = measure(source, kWarpAnalysisRate, 1, 256, 100);
    const auto low = measure(source, kWarpAnalysisRate, 1, 256, 0);
    check(high && low && high.onsets.size() == attacks.size() && low.onsets.empty(), "high sensitivity reveals quiet attacks");

    std::vector<float> vibrato(kWarpAnalysisRate);
    for (std::size_t frame = 0; frame < vibrato.size(); ++frame) {
        const auto time = static_cast<double>(frame) / kWarpAnalysisRate;
        const auto phase = 2 * std::numbers::pi * 440 * time + 2 * std::sin(2 * std::numbers::pi * 6 * time);
        vibrato[frame] = static_cast<float>(0.5 * std::sin(phase));
    }
    const auto steady = measure(vibrato, kWarpAnalysisRate, 1, 384);
    if (steady.onsets.size() > 1) {
        for (const auto& onset : steady.onsets)
            std::fprintf(stderr, "Vibrato %.3f ms (strength %.4f)\n",
                         1000 * static_cast<double>(onset.analysisFrame) / kWarpAnalysisRate,
                         static_cast<double>(onset.strength));
    }
    check(steady && steady.onsets.size() <= 1, "steady vibrato creates no repeated attacks");

    const auto values = adi::settings::Value{{transientSensitivityKey, 87}};
    check(warpTransientSensitivity(values) == 87 && warpTransientSensitivity(adi::settings::Value{}) == 50 &&
          warpTransientSensitivity(adi::settings::Value{{transientSensitivityKey, 101}}) == 50,
          "effective sensitivity reads the existing setting and validates its range");
}

void bridgeAndCancellation() {
    const std::vector<float> samples(48000, 0.25f);
    for (const auto rate : {8000u, 44100u, 48000u, 96000u, 768000u}) {
        std::size_t count = 0;
        const std::vector<float> source(rate, 0.25f);
        WarpAnalysisPcm bridge(rate, 1, [&](std::span<const float> frames) {
            count += frames.size();
            return true;
        });
        const bool okay = bridge.append(source) && bridge.finish();
        if (!okay || count != kWarpAnalysisRate)
            std::fprintf(stderr, "Bridge rate %u: %zu frames (%d)\n", rate, count, static_cast<int>(bridge.error()));
        check(okay && count == kWarpAnalysisRate,
              "44.1 kHz bridge drains exactly one second across supported rates");
        check(!bridge.append(source), "finished bridge refuses further samples");
    }

    constexpr std::array<std::uint64_t, 2> expected{0, kWarpAnalysisRate - 3};
    std::vector<float> edges(kWarpAnalysisRate);
    for (const auto frame : expected) edges[frame] = 0.5f;
    const auto impulses = measure(edges, kWarpAnalysisRate, 1, 4096);
    check(impulses && impulses.onsets.size() == expected.size(), "onsets at source edges survive padding");
    if (impulses.onsets.size() == expected.size()) {
        for (std::size_t i = 0; i < expected.size(); ++i)
            check(impulses.onsets[i].analysisFrame == expected[i], "impulse marked on its exact sample");
    }

    SpectralOnsetAnalyzer worker(48000, 1);
    {
        const CallbackScope callback;
        SpectralOnsetAnalyzer refused(48000, 1);
        check(!refused.append(samples) && refused.finish().error == AnalysisError::Realtime,
              "callback constructor refuses work");
        check(!worker.append(samples) && worker.finish().error == AnalysisError::Realtime,
              "callback cannot mutate an existing detector");
    }
    std::stop_source stop;
    stop.request_stop();
    check(!worker.append(samples, stop.get_token()) && worker.finish().error == AnalysisError::Cancelled,
          "stopped work publishes no partial result");

    WarpAnalysisPcm refusedSink(kWarpAnalysisRate, 1, [](std::span<const float>) { return false; });
    check(!refusedSink.append(samples) && !refusedSink.finish() && refusedSink.error() == AnalysisError::Io,
          "sink failure is terminal");
}
} // namespace

int main() {
    fixedRateAndStereo();
    sensitivityAndVibrato();
    bridgeAndCancellation();
    return failures == 0 ? 0 : 1;
}
