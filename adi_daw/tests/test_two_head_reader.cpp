// SPDX-License-Identifier: GPL-3.0-or-later
#include "adi/dsp/two_head_reader.hpp"

#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <numbers>

namespace {
using namespace adi::dsp::pitch_heads;

int failures = 0;
void check(bool ok, const char* label) {
    if (!ok && failures++ == 0)
        std::fprintf(stderr, "Two-head reader: %s\n", label);
}

template<class T>
bool identical(T first, T second) {
    using Bytes = std::array<unsigned char, sizeof(T)>;
    return std::bit_cast<Bytes>(first) == std::bit_cast<Bytes>(second);
}

void ringAddresses() {
    constexpr std::array<float, 5> history{-2, 1, 0, 8, 6};
    check(linearRead(history.data(), history.size(), 0, 0) == -2, "current sample");
    check(linearRead(history.data(), history.size(), 0, 0.25) == 0, "interpolate across wrap");
    check(linearRead(history.data(), history.size(), 1, 2) == 6, "integer age across wrap");
    check(linearRead(history.data(), history.size(), 4, 1.25) == 6, "fractional age");
}

void normalizedHeads(double rate) {
    constexpr std::array<double, 5> ratios{1, 0.5, 2, 4, 0.25};
    double phase = 0.0;
    for (unsigned i = 0; i < 60000; ++i) {
        const double window = rate * ((i % 127) + 10.25) / 1000.0;
        const double ratio = ratios[(i / 713) % ratios.size()];
        const double second = phase + 0.5 - std::floor(phase + 0.5);
        const double a = std::sin(std::numbers::pi * phase);
        const double b = std::sin(std::numbers::pi * second);
        // A ramp in the delay line makes the returned value its fractional sample age.
        double expected = 0;
        expected += a * a * (phase * window);
        expected += b * b * (second * window);
        const SineSquared heads(phase, window);
        const double actual = read(heads, [](double age) { return age; });
        check(identical(expected, actual), "sinusoidal read and weighting");
        const double moved = phase + (1.0 - ratio) / window;
        phase = SineSquared::advance(phase, ratio, window);
        check(identical(phase, moved - std::floor(moved)), "normalized advance");
    }
}

std::array<float, 2> stereoRamp(double age) {
    return {static_cast<float>(age), static_cast<float>(1.0 - 2.0 * age)};
}

void sampleAgeHeads(double rate) {
    const double window = 0.03715 * rate;
    const double inverse = 1.0 / window;
    float age = 0.0f;
    for (unsigned i = 0; i < 60000; ++i) {
        const double half = window / 2;
        auto expected = stereoRamp(age < half ? age + half : age - half);
        const auto other = stereoRamp(age);
        const float t = static_cast<float>(2.0 * std::min<double>(age, window - age) * inverse);
        const float weight = t * t * (3 - 2 * t);
        for (std::size_t c = 0; c < expected.size(); ++c)
            expected[c] += weight * (other[c] - expected[c]);
        int reads = 0;
        const auto actual = read(Smoothstep{age, window, inverse}, [&reads](double at) {
            ++reads;
            return stereoRamp(at);
        });
        check(identical(actual, expected), "sample-age stereo weighting");
        check(reads == (age == 0 ? 1 : 2), "unison reads once");
        const float step = (i / 443) % 2 == 0 ? -1.125f : 0.375f;
        float moved = age + step;
        if (moved < 0 || moved >= window)
            moved = static_cast<float>(moved - window * std::floor(moved * inverse));
        Smoothstep::advance(age, step, window, inverse);
        check(identical(age, moved), "sample-age advance across wrap");
    }
}
} // namespace

int main() {
    ringAddresses();
    for (double rate : {44100.0, 48000.0, 96000.0}) {
        normalizedHeads(rate);
        sampleAgeHeads(rate);
    }
    return failures == 0 ? 0 : 1;
}
