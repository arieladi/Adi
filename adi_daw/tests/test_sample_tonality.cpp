// SPDX-License-Identifier: AGPL-3.0-or-later
#include "adi/library/sample_tonality.hpp"


#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <numbers>

namespace {
using namespace adi::library;
int checks = 0;
int failures = 0;

void check(bool passes, const char* what) {
    ++checks;
    if (!passes) {
        ++failures;
        std::printf("FAIL %s\n", what);
    }
}

[[nodiscard]] double frequency(int midi) {
    return 440. * std::exp2(static_cast<double>(midi - 69) / 12.);
}

std::vector<float> cadence(SampleKey key, std::uint32_t rate) {
    const int tonic = 48 + key.tonic;
    const std::array<std::array<int, 4>, 4> chords{{
        {{0, key.minor ? 3 : 4, 7, 12}},
        {{5, key.minor ? 8 : 9, 12, 17}},
        {{7, 11, 14, 17}},
        {{0, key.minor ? 3 : 4, 7, 12}},
    }};
    const std::array<std::uint32_t, 4> noteFrames{rate / 2, rate * 3 / 8, rate * 3 / 8, rate * 3 / 4};
    std::vector<float> samples;
    samples.reserve(rate * 2);
    for (std::size_t c = 0; c < chords.size(); ++c) {
        for (std::uint32_t frame = 0; frame < noteFrames[c]; ++frame) {
            const auto time = static_cast<double>(frame) / rate;
            const auto release = static_cast<double>(noteFrames[c] - frame) / rate;
            const auto fade = std::min({1., time * 75., release * 25.});
            double sample = 0;
            for (std::size_t i = 0; i < chords[c].size(); ++i) {
                const auto radians = 2. * std::numbers::pi * frequency(tonic + chords[c][i]) * time;
                const auto amplitude = (i == 0 ? .22 : .13) * fade;
                sample += amplitude * (std::sin(radians) + .16 * std::sin(radians * 2.) + .04 * std::sin(radians * 3.));
            }
            samples.push_back(static_cast<float>(sample));
        }
    }
    return samples;
}

void keyFixtures() {
    const auto start = std::chrono::steady_clock::now();
    int exact = 0;
    float lowestStrength = 1;
    for (const bool minor : {false, true}) {
        for (std::uint8_t root = 0; root < 12; ++root) {
            const SampleKey expected{root, minor};
            const auto samples = cadence(expected, 48000);
            SampleKeyAnalyzer analyzer(48000, 1);
            bool appended = true;
            for (std::size_t offset = 0; offset < samples.size(); offset += 571) {
                appended = appended && analyzer.append(std::span(samples).subspan(offset, std::min<std::size_t>(571, samples.size() - offset)));
            }
            check(appended, "streaming accepts arbitrary whole-frame block boundaries");
            const auto detected = analyzer.finish();
            if (detected && detected->key == expected) {
                ++exact;
                lowestStrength = std::min(lowestStrength, detected->strength);
            } else {
                std::printf("MISSED %u %s, found %d %s strength %.4f\n", static_cast<unsigned>(root),
                            minor ? "minor" : "major", detected ? static_cast<int>(detected->key.tonic) : -1,
                            detected && detected->key.minor ? "minor" : "major",
                            detected ? static_cast<double>(detected->strength) : 0.);
            }
        }
    }
    const auto duration = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    std::printf("Cadences detected exactly: %d/24, minimum correct strength %.3f, fixture plus analysis %.1f ms\n",
                exact, static_cast<double>(lowestStrength), duration);
    check(exact >= 22, "at least 22 of 24 chromatically transposed cadences detect the exact key");

    const SampleKey expected{9, true};
    SampleKeyAnalyzer analyzer(96000, 2);
    const auto mono = cadence(expected, 96000);
    std::vector<float> stereo;
    stereo.reserve(mono.size() * 2);
    for (const auto sample : mono) {
        stereo.push_back(sample);
        stereo.push_back(sample * .71f);
    }
    check(!analyzer.append(std::span(stereo).first(3)), "a partial PCM frame is rejected");
    check(analyzer.append(stereo), "96 kHz stereo is accepted");
    const auto detected = analyzer.finish();
    check(detected && detected->key == expected, "96 kHz stereo cadence detects the same key");
    const auto again = analyzer.finish();
    check(again && again->key == expected && !analyzer.append(stereo), "completed analyzer is stable");
    for (std::size_t frame = 0; frame < mono.size(); ++frame) stereo[2 * frame + 1] = -mono[frame];
    SampleKeyAnalyzer antiPhase(96000, 2);
    check(antiPhase.append(stereo), "out-of-phase stereo appends whole frames");
    const auto sideKey = antiPhase.finish();
    check(sideKey && sideKey->key == expected, "audible stereo content survives destructive mono folding");

    std::vector<float> samples(48000);
    SampleKeyAnalyzer silent(48000, 1);
    check(silent.append(samples) && !silent.finish(), "silence has no detected key");
    const auto step = 2. * std::numbers::pi * 440. / 48000.;
    for (std::size_t i = 0; i < samples.size(); ++i) samples[i] = .4f * static_cast<float>(std::sin(static_cast<double>(i) * step));
    SampleKeyAnalyzer singleNote(48000, 1);
    check(singleNote.append(samples) && !singleNote.finish(), "one note cannot establish major or minor");
    std::uint32_t rng = 27271;
    for (auto& sample : samples) {
        rng = 1664525u * rng + 1013904223u;
        sample = static_cast<float>(rng / 2147483648. - 1.);
    }
    SampleKeyAnalyzer noise(48000, 1);
    check(noise.append(samples) && !noise.finish(), "broadband noise has no detected key");
}

} // namespace

int main() {
    keyFixtures();
    std::printf("%d checks, %d failures\n", checks, failures);
    return failures != 0;
}
