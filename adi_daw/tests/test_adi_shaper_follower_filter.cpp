// SPDX-License-Identifier: GPL-3.0-or-later
#include "adi/dsp/adi_shaper_follower_filter.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <limits>

namespace {
using Filter = adi::dsp::AdiShaperFollowerFilter;
int failures = 0;

void check(bool passed, const char* message) {
    if (!passed) {
        std::printf("FAIL %s\n", message);
        ++failures;
    }
}

void measuredGain(double rate, float hp, float lp, double ratio, double target, double tolerance) {
    Filter filter;
    filter.prepare(rate);
    filter.setHighPass(hp);
    filter.setLowPass(lp);
    const int frames = std::abs(ratio) < 1.01 ? 28000 : 1200;
    float left = 0.f, right = 0.f;
    for (int frame = 0; frame < frames; ++frame) {
        const float signal = static_cast<float>(.5 * std::pow(std::abs(ratio), frame - (frames - 1)));
        left = ratio < 0. && (frames - 1 - frame) % 2 ? -signal : signal;
        right = -.25f * left;
        filter.process(left, right);
    }
    check(std::abs(2. * left - target) < tolerance && std::abs(-8. * right - target) < tolerance,
          "signed exponential gain matches the independent stereo reading");
}

void stationaryGains() {
    // Independent normalized gain ratios on rising geometric sequences.
    measuredGain(44100., 0.f, 25.f, 1.02, .20801680, 3.e-6);
    measuredGain(48000., 50.f, 100.f, 1.22, .48516355, 3.e-6);
    measuredGain(96000., 25.f, 75.f, 1.02, .44621881, 3.e-6);
    measuredGain(32000., 50.f, 50.f, 1.02, .01430804, 3.e-6);
    measuredGain(32000., 75.f, 25.f, 1.02, .00061222, 3.e-6);
    // Alternating polarity resolves the active cutoff immediately below Nyquist.
    for (double rate : {8000., 16000., 22050., 32000.})
        measuredGain(rate, 0.f, 99.998f, -1.0014, .4762573, .00015);
    measuredGain(8000., 95.f, 100.f, -1.0014, .09603882, .00015);
    measuredGain(32000., 95.f, 100.f, -1.0014, .98701664, .00015);
}

void automation() {
    struct Cutoffs { float hp, lp; };
    constexpr std::array<Cutoffs, 6> controls{{
        {15,74}, {55,40}, {17,51}, {0,100}, {17,51}, {46,88}
    }};
    // The rising-slope detector readings recover the filter output magnitude.
    // The signed marker uses opposite polarity and a different gain on the right.
    struct Marker { int frame; float output; };
    constexpr std::array<Marker, 12> markers{{
        {3,.07748672f}, {87,-.20721720f}, {512,.32406883f}, {1001,.06479780f},
        {1086,-.10417846f}, {1087,-.11736592f}, {1538,.30761718f}, {1577,-.36132811f},
        {2432,-.17084117f}, {2515,.17510662f}, {2575,-.33085791f}, {2824,.36753607f}
    }};
    Filter filter, reverse;
    std::size_t marker = 0;
    for (int n = 0; n <= markers.back().frame; ++n) {
        if (n % 512 == 0) {
            const auto pair = controls[static_cast<std::size_t>(n / 512)];
            filter.setHighPass(pair.hp);
            filter.setLowPass(pair.lp);
            reverse.setLowPass(pair.lp);
            reverse.setHighPass(pair.hp);
        }
        const float signal = static_cast<float>((n * 73 + 29) % 257 - 128) / 1024.f
                           + ((n / 83) % 2 == 0 ? .1875f : -.25f);
        float l = signal, r = -.375f * signal, l2 = l, r2 = r;
        filter.process(l, r);
        reverse.process(l2, r2);
        if (l != l2 || r != r2) {
            check(false, "both setters before a sample give the same result in either order");
            return;
        }
        if (n == markers[marker].frame) {
            const float expected = markers[marker++].output;
            check(std::abs(l - expected) < 3.e-6f && std::abs(r + .375f * expected) < 3.e-6f,
                  "active jumps, cutoff collisions and reactivation preserve signed history");
        }
    }
}

void neutralAndReset() {
    Filter a, b;
    a.setLowPass(23.f);
    float l = .625f, r = -.25f;
    for (int i = 0; i < 128; ++i) {
        l = .625f; r = -.25f; a.process(l, r);
    }
    b = a;
    a.setLowPass(100.f);
    for (int i = 0; i < 257; ++i) {
        const float input = static_cast<float>(i - 100) / 256.f;
        l = input; r = -.5f * input;
        a.process(l, r);
        if (l != input || r != -.5f * input) {
            check(false, "neutral passes each signed channel exactly");
            return;
        }
    }
    a.setLowPass(23.f);
    float x = -.125f, y = .25f;
    l = x; r = y; a.process(l, r); b.process(x, y);
    check(l == x && r == y, "bypassed frames do not update the integrators");
    a.reset();
    b.prepare(96000.);
    b.prepare(48000.);
    x = .25f; y = 0.f; l = x; r = y; a.process(l, r); b.process(x, y);
    check(l == x && r == y && r == 0.f && l > 0.f && l < .25f,
          "reset and reprepare clear both ears while preserving the requested controls");
    a.setHighPass(std::numeric_limits<float>::quiet_NaN());
    a.setLowPass(std::numeric_limits<float>::infinity());
    a.prepare(std::numeric_limits<double>::quiet_NaN());
    a.process(l, r);
    check(std::isfinite(l) && std::isfinite(r), "invalid values leave a finite filter");
}
} // namespace

int main() {
    stationaryGains();
    automation();
    neutralAndReset();
    return failures ? 1 : 0;
}
