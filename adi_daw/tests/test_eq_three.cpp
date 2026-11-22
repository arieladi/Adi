// SPDX-License-Identifier: GPL-3.0-or-later
//
// Live 12's EQ Three, the core (DEV-22). The expected response of a setting
// is computed apart from the code's signal path: each crossover section's
// transfer function on the unit circle, the bands combined as the design
// says (low through the second crossover's allpass, mid and high split from
// the first's high side), weighted by the bands' gains. A sine's measured
// level must match it within 0.01 dB; at unity the bands sum flat, a killed
// band is gone to the crossover's depth. Parity with Live waits on a reading.
#include "adi/dsp/biquad.hpp"
#include "adi/dsp/eq_three.hpp"

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <new>
#include <numbers>
#include <string>
#include <vector>

namespace {
bool counting = false;
std::size_t allocations = 0;
int checks = 0, failures = 0;
void check(bool ok, const std::string& what) {
    ++checks;
    if (!ok) {
        ++failures;
        std::printf("FAIL %s\n", what.c_str());
    }
}
}  // namespace
void* operator new(std::size_t n) {
    if (counting) {
        ++allocations;
    }
    if (void* p = std::malloc(n ? n : 1)) {
        return p;
    }
    throw std::bad_alloc();
}
void operator delete(void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }

namespace {
using E = adi::dsp::EqThree;
using C = std::complex<double>;
constexpr double kRate = 48000;

C response(const adi::dsp::Biquad& b, double hz) {
    const C z1 = std::polar(1.0, -2 * std::numbers::pi * hz / kRate), z2 = z1 * z1;
    return (b.b0 + b.b1 * z1 + b.b2 * z2) / (1.0 + b.a1 * z1 + b.a2 * z2);
}
/// A Linkwitz-Riley low or high pass's response: LR4 or LR8.
C linkwitzRiley(bool high, double f0, bool steep, double hz) {
    std::vector<double> qs;
    if (steep) {
        qs = {1 / (2 * std::cos(std::numbers::pi / 8)), 1 / (2 * std::cos(3 * std::numbers::pi / 8))};
    } else {
        qs = {1 / std::sqrt(2.0)};
    }
    C h = 1;
    for (int twice = 0; twice < 2; ++twice) {
        for (double q : qs) {
            h *= response(high ? adi::dsp::highPass(kRate, f0, q) : adi::dsp::lowPass(kRate, f0, q), hz);
        }
    }
    return h;
}
double expectedDb(double low, double high, bool steep, std::array<double, 3> gains, double hz) {
    const C lp1 = linkwitzRiley(false, low, steep, hz), hp1 = linkwitzRiley(true, low, steep, hz);
    const C lp2 = linkwitzRiley(false, high, steep, hz), hp2 = linkwitzRiley(true, high, steep, hz);
    const C h = gains[0] * lp1 * (lp2 + hp2) + gains[1] * hp1 * lp2 + gains[2] * hp1 * hp2;
    return 20 * std::log10(std::abs(h));
}

double measuredDb(E& eq, double hz) {
    const std::size_t frames = 96000;
    std::vector<float> in(frames), l(frames), r(frames);
    for (std::size_t i = 0; i < frames; ++i) {
        in[i] = static_cast<float>(0.25 * std::sin(2 * std::numbers::pi * hz * static_cast<double>(i) / kRate));
    }
    eq.prepare(kRate);
    eq.process(in.data(), in.data(), l.data(), r.data(), frames);
    double a = 0, b = 0;
    for (std::size_t i = 48000; i < frames; ++i) {
        a += static_cast<double>(in[i]) * in[i];
        b += static_cast<double>(l[i]) * l[i];
    }
    return 10 * std::log10(b / a);
}

void responses() {
    for (bool steep : {false, true}) {
        const std::string slope = steep ? "48 dB/oct" : "24 dB/oct";
        // At unity the three bands sum flat.
        E flat;
        flat.set(E::Slope, steep ? 1 : 0);
        double worst = 0;
        for (double hz : {30.0, 250.0, 790.0, 2500.0, 10000.0}) {
            worst = std::max(worst, std::abs(measuredDb(flat, hz)));
        }
        check(worst < 0.01, slope + ": at unity the bands sum flat within 0.01 dB");
        // Each band killed in turn, and a band's gain: the measured level
        // matches the model within 0.01 dB (or both are far below -100 dB).
        struct Case {
            E::Param param;
            double value, hz;
            std::array<double, 3> gains;
            const char* what;
        };
        const Case cases[] = {{E::LowOn, 0, 50, {0, 1, 1}, "Low killed, at 50 Hz"},
                              {E::MidOn, 0, 790, {1, 0, 1}, "Mid killed, at 790 Hz"},
                              {E::HighOn, 0, 10000, {1, 1, 0}, "High killed, at 10 kHz"},
                              {E::Low, 6, 30, {std::pow(10.0, 0.3), 1, 1}, "Low +6 dB, at 30 Hz"},
                              {E::High, -70, 15000, {1, 1, 0}, "High at its low end is silence, at 15 kHz"}};
        for (const auto& c : cases) {
            E eq;
            eq.set(E::Slope, steep ? 1 : 0);
            eq.set(c.param, c.value);
            const double got = measuredDb(eq, c.hz), want = expectedDb(250, 2500, steep, c.gains, c.hz);
            check(std::abs(got - want) < 0.01 || (got < -100 && want < -100),
                  slope + ", " + c.what + ": " + std::to_string(got) + " dB against " + std::to_string(want));
        }
        // The kill's depth: well over the 24 or 48 dB per octave it promises.
        E kill;
        kill.set(E::Slope, steep ? 1 : 0);
        kill.set(E::LowOn, 0);
        check(measuredDb(kill, 50) < (steep ? -100 : -50), slope + ": Low killed, 50 Hz is gone (over 2 octaves down)");
    }
    // Moved crossovers: Freq Low 1 kHz, Freq High 4 kHz, Mid killed at 2 kHz.
    E moved;
    moved.set(E::FreqLow, 1000);
    moved.set(E::FreqHigh, 4000);
    moved.set(E::MidOn, 0);
    check(std::abs(measuredDb(moved, 2000) - expectedDb(1000, 4000, false, {1, 0, 1}, 2000)) < 0.01,
          "the crossovers move with Freq Low and Freq High");
    // A plant: a killed low band is not flat.
    E plant;
    plant.set(E::LowOn, 0);
    check(measuredDb(plant, 50) < -20, "plant: a killed band is not the flat sum");
}

void blocksAndRobustness() {
    std::vector<float> x(20000), first, l(20000), r(20000);
    for (std::size_t i = 0; i < x.size(); ++i) {
        x[i] = static_cast<float>(0.3 * std::sin(0.013 * static_cast<double>(i)) + 0.1 * std::sin(0.9 * static_cast<double>(i)));
    }
    bool identical = true;
    for (std::size_t block : {1u, 64u, 333u, 4096u}) {
        E eq;
        eq.set(E::Slope, 1);
        eq.set(E::Mid, -12);
        eq.set(E::High, 4);
        eq.prepare(kRate);
        for (std::size_t at = 0; at < x.size(); at += block) {
            const auto n = std::min(block, x.size() - at);
            eq.process(x.data() + at, x.data() + at, l.data() + at, r.data() + at, n);
        }
        if (first.empty()) {
            first = l;
        } else {
            identical = identical && l == first;
        }
    }
    check(identical, "every block size gives the same samples");
    E eq;
    std::vector<float> bad(512, std::numeric_limits<float>::quiet_NaN()), o(512), p(512);
    counting = true;
    eq.process(bad.data(), bad.data(), o.data(), p.data(), bad.size());
    eq.set(E::FreqLow, 400);
    eq.set(E::Slope, 1);
    eq.process(bad.data(), bad.data(), o.data(), p.data(), bad.size());
    counting = false;
    check(allocations == 0 && std::all_of(o.begin(), o.end(), [](float v) { return v == 0.f; }),
          "a non-finite input is silence; processing and moved controls allocate nothing");
    E z;
    std::vector<float> left(4800, 0.25f), shared(4800, -0.5f), rightOut(4800);
    z.process(left.data(), shared.data(), shared.data(), rightOut.data(), 4800);
    check(std::abs(rightOut.back() + 0.5f) < 1e-4f, "an output plane that is the other channel's input is read first");
}

}  // namespace

int main() {
    responses();
    blocksAndRobustness();
    std::printf("%s -- %d checks, %d failure(s)\n", failures ? "FAILED" : "PASS", checks, failures);
    return failures ? 1 : 0;
}
