// SPDX-License-Identifier: GPL-3.0-or-later
#include "adi/dsp/adi_shaper_liquid.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <limits>
#include <stdexcept>
#include <vector>

namespace {
using Liquid = adi::dsp::AdiShaperLiquid;
using Mode = Liquid::Mode;

void check(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
void near(double value, double expected, double tolerance, const char* message) {
    if (!(std::abs(value - expected) <= tolerance)) {
        std::fprintf(stderr, "%s: actual %.9g, expected %.9g, tolerance %.9g\n",
                     message, value, expected, tolerance);
        throw std::runtime_error(message);
    }
}
std::vector<float> impulse(Liquid& band, Mode mode, float center, float feedback,
                           bool negative = false, std::size_t frames = 8192) {
    band.setMode(mode);
    band.setPolarity(negative);
    band.reset();
    std::vector<float> signal(frames);
    for (std::size_t i = 0; i < frames; ++i) {
        float left = i == 0 ? 0.5f : 0.0f;
        float right = 0.0f;
        band.processStereo(left, right, center, feedback);
        check(right == 0 && std::isfinite(left), "isolated left impulse");
        signal[i] = 2 * left;
    }
    return signal;
}
std::size_t peak(const std::vector<float>& signal) {
    return static_cast<std::size_t>(std::max_element(signal.begin(), signal.end(),
        [](float a, float b) { return std::abs(a) < std::abs(b); }) - signal.begin());
}

// First arrival, sign and gain measured with a half-scale impulse at 48 kHz.
// Stored amplitudes here are normalized to the input impulse.
void flange(Liquid& band) {
    struct Reading { float center; std::size_t frame; float amount; };
    constexpr std::array<Reading, 5> values{{
        {0, 2330, .7228723f}, {.25f, 412, .6985676f}, {.5f, 73, .6473790f},
        {.75f, 13, .6974112f}, {1, 2, .5813829f}}};
    for (const auto& row : values) {
        const auto signal = impulse(band, Mode::Flange, row.center, .5f);
        check(peak(signal) == row.frame, "flanger delayed peak frame");
        near(signal[0], .4917946, .0002, "flanger direct component");
        near(signal[row.frame], row.amount, .001, "flanger delayed peak amplitude");
    }
    const auto plus = impulse(band, Mode::Flange, .5f, .5f);
    const auto minus = impulse(band, Mode::Flange, .5f, .5f, true);
    near(minus[73], -.647379, .001, "negative first echo");
    near(plus[146], .35266, .006, "positive second echo");
    near(minus[146], .36077, .006, "negative second echo");
    near(plus[75] / plus[74], .986995, .00002, "flanger low-frequency rolloff");
    const auto none = impulse(band, Mode::Flange, .5f, 0.f);
    const auto full = impulse(band, Mode::Flange, .5f, 1.f);
    near(none[0], .75, .0001, "zero feedback direct");
    near(full[0], .430416, .0001, "maximum feedback direct");
    near(full[146], .50733, .01, "maximum feedback repeat");
}

void phaser(Liquid& band) {
    struct Reading { Mode mode; std::array<float, 4> first; };
    constexpr std::array<Reading, 4> levels{{
        {Mode::Phaser4, {.460162f, -.235875f, -.197464f, -.156487f}},
        {Mode::Phaser8, {.352318f, -.390877f, -.229162f, -.072993f}},
        {Mode::Phaser12, {.264703f, -.467674f, -.128614f, .131354f}},
        {Mode::Phaser16, {.195889f, -.482481f, .036233f, .304189f}}}};
    for (const auto& row : levels) {
        const auto signal = impulse(band, row.mode, .5f, .5f);
        for (std::size_t i = 0; i < row.first.size(); ++i)
            near(signal[i], row.first[i], .0012, "phaser first four coefficients");
    }
    const auto low = impulse(band, Mode::Phaser12, 0.f, .5f);
    const auto high = impulse(band, Mode::Phaser12, 1.f, .5f);
    check(peak(low) == 0 && peak(high) == 3, "phaser center shifts impulse peak");
    near(low[0], .5748670, .0012, "phaser lowest center");
    near(high[3], .4574982, .0012, "phaser highest center");
    const auto negative = impulse(band, Mode::Phaser4, .5f, .5f, true);
    near(negative[0], .818687, .0012, "alternate phaser direct");
    near(negative[1], -.184634, .0012, "alternate phaser feedback");
    const auto allpass = impulse(band, Mode::Phaser16, .5f, 0);
    double energy = 0;
    for (const auto sample : allpass) energy += double(sample) * sample;
    near(energy, 1, .00001, "zero-feedback all-pass energy");
}

void lifecycle(Liquid& band) {
    for (const double rate : {44100., 96000., 192000.}) {
        check(band.prepare(rate), "sample-rate change");
        const auto signal = impulse(band, Mode::Flange, .5f, .5f);
        check(peak(signal) == static_cast<std::size_t>(std::round(rate / 659.2551)),
              "fractional delay follows sample rate");
    }
    for (const auto mode : {Mode::Flange, Mode::Phaser16}) {
        band.setMode(mode);
        for (unsigned i = 0; i < 12000; ++i) {
            float left = static_cast<float>(std::sin(i * .1));
            float right = -left;
            const float center = static_cast<float>(i % 1024) / 1023;
            band.processStereo(left, right, center, 1 - center);
            check(std::isfinite(left) && left == -right, "stereo symmetry with moving contours");
        }
        band.reset();
        for (int i = 0; i < 10000; ++i) {
            float left = 0, right = 0;
            band.processStereo(left, right, 0, 1);
            check(left == 0 && right == 0, "reset erases previous impulse");
        }
    }
    band.setMode(Mode::Off);
    float left = -.124f, right = .83f;
    band.processStereo(left, right, 0, 1);
    check(left == -.124f && right == .83f, "off passes both channels exactly");
    check(!band.prepare(std::numeric_limits<double>::infinity()), "invalid sample rate");
    band.setMode(Mode::Phaser8);
    band.processStereo(left, right, 1, 1);
    check(left == -.124f && right == .83f, "unprepared output is dry");
}
} // namespace

int main() try {
    Liquid band;
    check(band.prepare(48000), "prepare");
    flange(band);
    phaser(band);
    lifecycle(band);
    std::puts("ADi-SHAPER Liquid impulse, response, stereo and rate checks passed.");
    return 0;
} catch (const std::exception& e) {
    std::fprintf(stderr, "%s\n", e.what());
    return 1;
}
