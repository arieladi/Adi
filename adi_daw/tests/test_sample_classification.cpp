// SPDX-License-Identifier: AGPL-3.0-or-later
#include "adi/audio/io_audit.hpp"
#include "adi/audio/sample_classification.hpp"
#include "adi/audio/transients.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <limits>
#include <vector>

namespace {
using adi::audio::classifySample;
using adi::audio::SampleKind;
using adi::audio::SampleKindHints;
using adi::audio::SampleKindSource;

int failures = 0;

void check(bool pass, const char* description) {
    if (!pass) {
        ++failures;
        std::printf("FAIL %s\n", description);
    }
}

void steadyLoops() {
    double maximumError = 0;
    for (const auto rate : {44100u, 48000u, 96000u}) {
        for (const auto bars : {1u, 2u, 4u, 8u, 16u}) {
            for (const auto bpm : {70.0, 90.0, 120.0, 150.0, 180.0}) {
                const double period = 60.0 * rate / bpm;
                std::vector<std::uint64_t> onsets;
                for (unsigned int beat = 0; beat < bars * 4; ++beat) {
                    const double jitter = beat % 2 == 0 ? 0.008 : -0.008;
                    onsets.push_back(static_cast<std::uint64_t>(std::llround(beat * period + jitter * rate)));
                }
                const auto frames = static_cast<std::uint64_t>(std::llround(bars * 4 * period));
                const auto result = classifySample(onsets, frames, rate);
                check(result.kind == SampleKind::Loop && result.source == SampleKindSource::Detection &&
                          result.bars == bars && result.bpm.has_value(),
                      "steady loop resolves to its exact number of 4/4 bars");
                if (result.bpm)
                    maximumError = std::max(maximumError, std::abs(*result.bpm - bpm));
            }
        }
    }
    check(maximumError < 0.001, "loop tempo error stays below 0.001 BPM");
    std::printf("75 loops with 8 ms onset jitter; maximum tempo error %.9f BPM\n", maximumError);
}

void streamedTransients() {
    constexpr std::uint32_t rate = 8000;
    constexpr std::uint32_t channels = 2;
    constexpr std::size_t frames = 16013;
    const std::vector<std::uint64_t> expected{3, 4005, 8007, 12009, 16002};
    std::vector<float> stereo(frames * channels, 0.0f);
    for (auto position : expected)
        stereo[static_cast<std::size_t>(position) * channels + 1] = 1.0f;

    for (std::size_t chunkFrames : std::array<std::size_t, 5>{1, 13, 41, 4096, 20000}) {
        adi::audio::TransientAnalyzer analyzer(rate, channels);
        check(analyzer.append({}), "empty append accepted without changing the detector clock");
        for (std::size_t position = 0; position < frames;) {
            const auto length = std::min(chunkFrames, frames - position);
            const auto input = std::span(stereo).subspan(position * channels, length * channels);
            check(analyzer.append(input), "whole interleaved frames accepted across an unaligned block boundary");
            position += length;
        }
        const auto result = analyzer.finish();
        check(result && result.frames == expected,
              "streaming detector preserves onset frames across chunk sizes and a final partial window");
    }
}

void classificationAndPrecedence() {
    constexpr std::uint32_t rate = 48000;
    constexpr std::uint64_t frames = rate * 2;
    constexpr std::array<std::uint64_t, 4> regular{0, rate / 2, rate, 3 * rate / 2};
    constexpr std::array<std::uint64_t, 8> twoBars{
        0, rate / 2, rate, 3 * rate / 2, 2 * rate, 5 * rate / 2, 3 * rate, 7 * rate / 2
    };
    constexpr std::array<std::uint64_t, 4> irregular{0, 3791, 17239, 29877};
    constexpr std::array<std::uint64_t, 1> one{17};
    constexpr std::array<std::uint64_t, 3> invalid{0, rate / 2, rate / 2};

    check(classifySample(one, frames, rate).kind == SampleKind::OneShot, "one attack is a one-shot");
    check(classifySample({}, frames, rate).kind == SampleKind::Unknown, "silence has no kind");
    check(classifySample(irregular, frames, rate).kind == SampleKind::Unknown, "irregular events have no kind");
    check(classifySample(invalid, frames, rate).kind == SampleKind::Unknown, "duplicate positions are rejected");
    check(classifySample(one, 0, rate).kind == SampleKind::Unknown, "empty source is rejected");
    check(classifySample(one, rate * 61, rate).kind == SampleKind::Unknown, "long files do not use short-sample inference");

    SampleKindHints hints;
    hints.acid = {SampleKind::Loop, 120.0};
    check(classifySample({}, 1600, 800, hints).kind == SampleKind::Loop,
          "the library honors type metadata at rates outside the Warp DSP range");
    auto result = classifySample({}, frames, rate, hints);
    check(result.kind == SampleKind::Loop && result.source == SampleKindSource::Acid &&
              result.bars == 1 && result.bpm == 120.0,
          "ACID supplies a known loop with no onset evidence");
    hints.fileTag = {SampleKind::OneShot, 130.0};
    result = classifySample(regular, frames, rate, hints);
    check(result.kind == SampleKind::OneShot && result.source == SampleKindSource::FileTag &&
              !result.bars && result.bpm == 130.0,
          "file tag overrides ACID and onset evidence");
    hints.user = {SampleKind::Loop, 240.0};
    result = classifySample(one, frames, rate, hints);
    check(result.kind == SampleKind::Loop && result.source == SampleKindSource::User &&
              result.bars == 2 && result.bpm == 240.0,
          "user kind and BPM have highest priority");
    hints.user.bpm = std::numeric_limits<double>::infinity();
    result = classifySample(one, frames, rate, hints);
    check(result.kind == SampleKind::Loop && result.source == SampleKindSource::User &&
              !result.bars && result.bpm == 130.0,
          "kind and BPM precedence are independent and conflicting tempo does not invent bars");
    hints = {};
    hints.user.bpm = 60.0;
    result = classifySample(twoBars, frames * 2, rate, hints);
    check(result.kind == SampleKind::Loop && result.source == SampleKindSource::Detection &&
              result.bars == 1 && result.bpm == 60.0,
          "known tempo selects the compatible subdivision for a rhythmic file");

    const adi::audio::CallbackScope callback;
    check(classifySample(regular, frames, rate).kind == SampleKind::Unknown,
          "classification refuses callback work");
}
} // namespace

int main() {
    steadyLoops();
    streamedTransients();
    classificationAndPrecedence();
    return failures == 0 ? 0 : 1;
}
