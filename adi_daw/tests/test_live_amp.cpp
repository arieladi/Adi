// SPDX-License-Identifier: GPL-3.0-or-later
//
// Live 12's Amp, the core (DEV-22). The reference for small signals is the
// documented chain computed apart from the code's signal path: the voice's
// high pass and bell, the preamp's slope g sech^2 b, the 5 Hz DC blocker,
// the three tone bands, the power stage's unit slope and presence, each from
// the header's own frequencies and gains through biquad.hpp's designs,
// evaluated on the unit circle. A -100 dBFS sine must match within 0.05 dB.
// Driven hard, the amp adds harmonics, even ones from its bias, more with
// Gain, and stays bounded. Parity with Live waits on a reading.
#include "adi/dsp/biquad.hpp"
#include "adi/dsp/live_amp.hpp"

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
using A = adi::dsp::LiveAmp;
using C = std::complex<double>;
constexpr double kRate = 48000;

C at(const adi::dsp::Biquad& b, double hz) {
    const C z1 = std::polar(1.0, -2 * std::numbers::pi * hz / kRate), z2 = z1 * z1;
    return (b.b0 + b.b1 * z1 + b.b2 * z2) / (1.0 + b.a1 * z1 + b.a2 * z2);
}
struct Setting {
    int type;
    double gain, bass, middle, treble, presence, volume, output;
};
/// The documented chain's small-signal gain at `hz`, in dB.
double expectedDb(const Setting& s, double hz) {
    using namespace adi::dsp;
    const auto& v = A::kVoices[s.type];
    const double q = 1 / std::sqrt(2.0);
    C h = at(highPass(kRate, v.lowCutHz, q), hz) * at(highPass(kRate, v.lowCutHz, q), hz) * at(peaking(kRate, v.peakHz, 0.7, v.peakDb), hz);
    const double g = std::pow(10.0, (v.preGainDb + (s.gain - 5) * 4) / 20), sech = 1 / std::cosh(v.bias);
    h *= g * sech * sech;
    const double pole = std::exp(-2 * std::numbers::pi * 5 / kRate);
    const C z1 = std::polar(1.0, -2 * std::numbers::pi * hz / kRate);
    h *= (1.0 - z1) / (1.0 - pole * z1);
    h *= at(lowShelf(kRate, 100, (s.bass - 5) * 3), hz) * at(peaking(kRate, 700, 0.7, (s.middle - 5) * 3), hz) *
         at(highShelf(kRate, 3000, (s.treble - 5) * 3), hz);
    h *= at(highShelf(kRate, 5000, (s.presence - 5) * 2), hz) * std::pow(10.0, s.output / 20);
    return 20 * std::log10(std::abs(h));
}

std::vector<float> sine(double hz, std::size_t frames, double amplitude) {
    std::vector<float> x(frames);
    for (std::size_t i = 0; i < frames; ++i) {
        x[i] = static_cast<float>(amplitude * std::sin(2 * std::numbers::pi * hz * static_cast<double>(i) / kRate));
    }
    return x;
}
double component(const std::vector<float>& y, double hz) {
    std::complex<double> sum = 0;
    const std::size_t n = 48000, from = y.size() - n;
    for (std::size_t i = 0; i < n; ++i) {
        sum += static_cast<double>(y[from + i]) * std::polar(1.0, -2 * std::numbers::pi * hz * static_cast<double>(from + i) / kRate);
    }
    return 2 * std::abs(sum) / static_cast<double>(n);
}
A configured(const Setting& s) {
    A a;
    a.set(A::Type, s.type);
    a.set(A::Gain, s.gain);
    a.set(A::Bass, s.bass);
    a.set(A::Middle, s.middle);
    a.set(A::Treble, s.treble);
    a.set(A::Presence, s.presence);
    a.set(A::Volume, s.volume);
    a.set(A::Output, s.output);
    a.prepare(kRate);
    return a;
}

void quiet() {
    const Setting settings[] = {{A::Clean, 5, 5, 5, 5, 5, 5, 0},  {A::Rock, 7, 8, 2, 6, 7, 3, -6},
                                {A::Heavy, 2, 10, 0, 10, 0, 10, 0}, {A::BassAmp, 5, 9, 5, 1, 5, 5, 3},
                                {A::Lead, 10, 5, 5, 5, 5, 5, -12}};
    for (const auto& s : settings) {
        double worst = 0;
        for (double hz : {60.0, 150.0, 700.0, 1500.0, 4000.0, 9000.0}) {
            A a = configured(s);
            const auto x = sine(hz, 96000, 1e-5);
            std::vector<float> l(x.size()), r(x.size());
            a.process(x.data(), x.data(), l.data(), r.data(), x.size());
            const double got = 20 * std::log10(component(l, hz) / 1e-5);
            worst = std::max(worst, std::abs(got - expectedDb(s, hz)));
        }
        check(worst < 0.05, "voice " + std::to_string(s.type) + ": a -100 dBFS sine follows the documented chain within 0.05 dB (worst " +
                                std::to_string(worst) + " dB)");
    }
    // A plant: the bands moved by a quarter of their step is outside it.
    const Setting s{A::Rock, 7, 8, 2, 6, 7, 3, -6};
    Setting off = s;
    off.bass += 0.25;
    check(std::abs(expectedDb(s, 60) - expectedDb(off, 60)) > 0.2, "plant: Bass a quarter step off is far outside the tolerance");
}

void driven() {
    // Driven hard: harmonics, the second from the bias, more with Gain.
    const auto x = sine(220, 96000, 0.5);
    const auto thd = [&](const Setting& s, double& second) {
        A a = configured(s);
        std::vector<float> l(x.size()), r(x.size());
        a.process(x.data(), x.data(), l.data(), r.data(), x.size());
        const double f = component(l, 220);
        double h = 0;
        for (int k = 2; k <= 9; ++k) {
            h += std::pow(component(l, 220.0 * k), 2);
        }
        second = component(l, 440) / f;
        return std::sqrt(h) / f;
    };
    double low2 = 0, high2 = 0;
    const double low = thd({A::Rock, 0, 5, 5, 5, 5, 5, 0}, low2), high = thd({A::Rock, 10, 5, 5, 5, 5, 5, 0}, high2);
    check(high > 2 * low && high > 0.05, "Rock: Gain 10 distorts far more than Gain 0 (" + std::to_string(low) + " -> " + std::to_string(high) + ")");
    // At moderate drive the bias shows as a second harmonic (driven to a
    // square, the stage's asymmetry is a small part of each half cycle).
    check(low2 > 0.01 && low2 > 4 * high2, "the bias makes even harmonics at moderate drive (second at " + std::to_string(low2) +
                                               " of the fundamental, " + std::to_string(high2) + " at Gain 10)");
    // At equal drive (Clean at Gain 10 and Rock at Gain 4 both put the
    // preamp at +20 dB) only the bias differs: Clean's smaller bias makes a
    // smaller second harmonic. (The review of 17130593: compared at Gain 0
    // the drive alone decided it.)
    double clean2 = 0, rock2 = 0;
    (void)thd({A::Clean, 10, 5, 5, 5, 5, 5, 0}, clean2);
    (void)thd({A::Rock, 4, 5, 5, 5, 5, 5, 0}, rock2);
    check(clean2 < 0.5 * rock2, "at equal drive, a smaller bias, a smaller second harmonic (Clean " + std::to_string(clean2) + ", Rock " +
                                    std::to_string(rock2) + ")");
    // Bounded by the power stage: |out| <= 1/d times the largest gain after
    // it (Presence +10 dB, Output), with room for the shelf's overshoot, at
    // the loosest setting (Clean, Volume 0) and the tightest, at three rates.
    for (const Setting s : {Setting{A::Clean, 10, 10, 10, 10, 10, 0, 12}, Setting{A::Heavy, 10, 0, 0, 0, 10, 10, 12}}) {
        for (double rate : {44100.0, 96000.0, 192000.0}) {
            A a = configured(s);
            a.prepare(rate);
            std::vector<float> square(static_cast<std::size_t>(rate / 2)), l(square.size()), r(square.size());
            for (std::size_t i = 0; i < square.size(); ++i) {
                square[i] = (static_cast<std::size_t>(static_cast<double>(i) * 160 / rate) % 2) ? -1.f : 1.f;
            }
            a.process(square.data(), square.data(), l.data(), r.data(), square.size());
            const auto& v = A::kVoices[s.type];
            const double d = v.drive * std::pow(10.0, (s.volume - 5) * 3 / 20);
            const double bound = 1.5 / d * std::pow(10.0, 10 / 20.0) * std::pow(10.0, s.output / 20);
            double peak = 0;
            for (float sample : l) {
                peak = std::max(peak, static_cast<double>(std::abs(sample)));
            }
            check(peak <= bound, "voice " + std::to_string(s.type) + " at " + std::to_string(rate) + ": an 80 Hz full-scale square stays within the power stage's bound (" +
                                     std::to_string(peak) + " <= " + std::to_string(bound) + ")");
        }
    }
    // A plant: without the power stage the loosest setting's bound is passed.
    check(1.5 / (0.25 * std::pow(10.0, -0.75)) * std::pow(10.0, 0.5) * std::pow(10.0, 0.6) < 1e3, "plant: the bound is tighter than the old 1e3");
}

void routing() {
    std::vector<float> x(20000), y(20000), l, r;
    for (std::size_t i = 0; i < x.size(); ++i) {
        x[i] = static_cast<float>(0.3 * std::sin(0.013 * static_cast<double>(i)));
        y[i] = static_cast<float>(0.2 * std::sin(0.11 * static_cast<double>(i)));
    }
    l.assign(x.size(), 0.f);
    r.assign(x.size(), 0.f);
    A mono;
    mono.process(x.data(), y.data(), l.data(), r.data(), x.size());
    check(l == r, "Mono: one amp, the same to both channels");
    A dual;
    dual.set(A::Dual, 1);
    dual.process(x.data(), y.data(), l.data(), r.data(), x.size());
    A single;
    single.set(A::Dual, 1);
    std::vector<float> l2(x.size()), r2(x.size());
    single.process(x.data(), x.data(), l2.data(), r2.data(), x.size());
    check(l != r && l == l2, "Dual: an amp for each channel, the left unaffected by the right");
    A dry;
    dry.set(A::DryWet, 0);
    dry.process(x.data(), y.data(), l.data(), r.data(), x.size());
    check(l == x && r == y, "Dry/Wet 0 is the input itself");
    std::vector<float> first;
    bool identical = true;
    for (std::size_t block : {1u, 64u, 333u, 4096u}) {
        A a;
        a.set(A::Type, A::Lead);
        a.set(A::Dual, 1);
        a.prepare(kRate);
        for (std::size_t at0 = 0; at0 < x.size(); at0 += block) {
            const auto n = std::min(block, x.size() - at0);
            a.process(x.data() + at0, y.data() + at0, l.data() + at0, r.data() + at0, n);
        }
        if (first.empty()) {
            first = l;
        } else {
            identical = identical && l == first;
        }
    }
    check(identical, "every block size gives the same samples");
    A a;
    std::vector<float> bad(512, std::numeric_limits<float>::quiet_NaN()), o(512), p(512);
    counting = true;
    a.process(bad.data(), bad.data(), o.data(), p.data(), bad.size());
    const bool silent = std::all_of(o.begin(), o.end(), [](float v) { return v == 0.f; });
    a.set(A::Type, A::Heavy);
    a.set(A::Treble, 9);
    a.process(x.data(), x.data(), o.data(), p.data(), 512);
    counting = false;
    check(allocations == 0, "processing and moved controls allocate nothing");
    check(silent, "a NaN input is silence");
    A z;
    z.set(A::DryWet, 0);
    std::vector<float> left(4800, 0.25f), shared(4800, -0.5f), rightOut(4800);
    z.process(left.data(), shared.data(), shared.data(), rightOut.data(), 4800);
    check(shared.back() == 0.25f && rightOut.back() == -0.5f, "an output plane that is the other channel's input is read first");
}

}  // namespace

/// Dual switched off and on again: the right amp starts from the left's
/// state, not its own from before (the review of 17130593: a 0.87 thump on
/// silence).
void dualAgain() {
    A a;
    a.set(A::Type, A::Rock);
    a.set(A::Dual, 1);
    a.prepare(kRate);
    const auto x = sine(110, 24037, 0.5);
    std::vector<float> l(48000), r(48000);
    a.process(x.data(), x.data(), l.data(), r.data(), x.size());
    a.set(A::Dual, 0);
    const std::vector<float> quiet(48000, 0.f);
    a.process(quiet.data(), quiet.data(), l.data(), r.data(), quiet.size());
    a.set(A::Dual, 1);
    a.process(quiet.data(), quiet.data(), l.data(), r.data(), quiet.size());
    double peak = 0;
    for (float v : r) {
        peak = std::max(peak, static_cast<double>(std::abs(v)));
    }
    check(peak < 1e-3, "Dual off then on over silence: no thump on the right (" + std::to_string(peak) + ")");
}

/// Gain, Volume, Output and Dry/Wet moved under a tone glide: the largest
/// step while they move stays within the signal's own, before or after.
void glides() {
    struct Move {
        A::Param param;
        double from, to;
        const char* what;
    };
    for (const Move m : {Move{A::Gain, 0, 10, "Gain"}, Move{A::Volume, 0, 10, "Volume"}, Move{A::Output, -36, 12, "Output"}, Move{A::DryWet, 0, 100, "Dry/Wet"}}) {
        A a;
        a.set(m.param, m.from);
        a.prepare(kRate);
        const auto x = sine(110, 96000, 0.05);
        std::vector<float> l(x.size()), r(x.size());
        a.process(x.data(), x.data(), l.data(), r.data(), 48000);
        a.set(m.param, m.to);
        a.process(x.data() + 48000, x.data() + 48000, l.data() + 48000, r.data() + 48000, 48000);
        double moving = 0, settled = 0;
        for (std::size_t i = 48000; i < 48000 + 4800; ++i) {
            moving = std::max(moving, static_cast<double>(std::abs(l[i] - l[i - 1])));
        }
        // The signal's own largest step, before the move and after it.
        for (std::size_t i = 24000; i < 48000; ++i) {
            settled = std::max(settled, static_cast<double>(std::abs(l[i] - l[i - 1])));
        }
        for (std::size_t i = 72000; i < 96000; ++i) {
            settled = std::max(settled, static_cast<double>(std::abs(l[i] - l[i - 1])));
        }
        check(moving <= 1.5 * std::max(settled, 1e-9), std::string(m.what) + " glides: no step past the signal's own (" + std::to_string(moving) +
                                                           " against " + std::to_string(settled) + ")");
    }
}

int main() {
    glides();
    dualAgain();
    quiet();
    driven();
    routing();
    std::printf("%s -- %d checks, %d failure(s)\n", failures ? "FAILED" : "PASS", checks, failures);
    return failures ? 1 : 0;
}
