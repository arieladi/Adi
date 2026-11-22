// SPDX-License-Identifier: GPL-3.0-or-later
//
// Live 12's Pedal, the core (DEV-22). The reference for small signals is the
// documented chain computed apart from the code's signal path: the input
// high pass (100 Hz, or 30 with Sub), the drive's slope g f'(b) (f' taken
// numerically from the curve), the 5 Hz DC blocker, the three tone bands and
// Output, from the header's own values through biquad.hpp's designs on the
// unit circle; a -100 dBFS sine must match within 0.05 dB. Driven, the pedal
// distorts more with Gain, Fuzz most, Overdrive's bias adds even harmonics.
// Parity with Live waits on a reading.
#include "adi/dsp/biquad.hpp"
#include "adi/dsp/live_pedal.hpp"

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
using P = adi::dsp::LivePedal;
using C = std::complex<double>;
constexpr double kRate = 48000;

C at(const adi::dsp::Biquad& b, double hz) {
    const C z1 = std::polar(1.0, -2 * std::numbers::pi * hz / kRate), z2 = z1 * z1;
    return (b.b0 + b.b1 * z1 + b.b2 * z2) / (1.0 + b.a1 * z1 + b.a2 * z2);
}
struct Setting {
    int type;
    double gain;
    int sub;
    double bass, mid;
    int midFreq;
    double treble, output;
};
double expectedDb(const Setting& s, double hz) {
    using namespace adi::dsp;
    const auto& v = P::kVoices[s.type];
    const double g = std::pow(10.0, (v.baseDb + 30 * s.gain / 100) / 20), h = 1e-6;
    const double slope = (P::curve(s.type, v.bias + h) - P::curve(s.type, v.bias - h)) / (2 * h);
    C r = at(highPass(kRate, s.sub ? 30.0 : 100.0, 1 / std::sqrt(2.0)), hz) * (g * slope);
    const double pole = std::exp(-2 * std::numbers::pi * 5 / kRate);
    const C z1 = std::polar(1.0, -2 * std::numbers::pi * hz / kRate);
    r *= (1.0 - z1) / (1.0 - pole * z1);
    r *= at(lowShelf(kRate, 100, s.bass / 100 * 15), hz) * at(peaking(kRate, P::kMidHz[s.midFreq], 1, s.mid / 100 * 15), hz) *
         at(highShelf(kRate, 3500, s.treble / 100 * 15), hz);
    return 20 * std::log10(std::abs(r)) + s.output;
}
P configured(const Setting& s) {
    P p;
    p.set(P::Type, s.type);
    p.set(P::Gain, s.gain);
    p.set(P::Sub, s.sub);
    p.set(P::Bass, s.bass);
    p.set(P::Mid, s.mid);
    p.set(P::MidFreq, s.midFreq);
    p.set(P::Treble, s.treble);
    p.set(P::Output, s.output);
    p.prepare(kRate);
    return p;
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
std::vector<float> render(P& p, const std::vector<float>& x) {
    std::vector<float> l(x.size()), r(x.size());
    p.process(x.data(), x.data(), l.data(), r.data(), x.size());
    return l;
}

void quiet() {
    const Setting settings[] = {{P::Overdrive, 50, 0, 0, 0, 1, 0, 0}, {P::Distortion, 20, 1, 60, -40, 0, 30, -12},
                                {P::Fuzz, 0, 0, -100, 100, 2, -100, -24}, {P::Overdrive, 100, 1, 100, 0, 1, 100, -30}};
    for (const auto& s : settings) {
        double worst = 0;
        for (double hz : {40.0, 150.0, 600.0, 1500.0, 5000.0}) {
            P p = configured(s);
            const auto y = render(p, sine(hz, 96000, 1e-7));
            worst = std::max(worst, std::abs(20 * std::log10(component(y, hz) / 1e-7) - expectedDb(s, hz)));
        }
        check(worst < 0.05, "voice " + std::to_string(s.type) + ": a -140 dBFS sine follows the documented chain within 0.05 dB (worst " +
                                std::to_string(worst) + " dB)");
    }
    double worst = 0;
    for (int v : {P::Overdrive, P::Distortion, P::Fuzz}) {
        for (double x : {-1.3, -0.2, 0.1, 0.4, 2.0}) {
            const double h = 1e-6;
            worst = std::max(worst, std::abs((P::curve(v, x + h) - P::curve(v, x - h)) / (2 * h) - P::slope(v, x)));
        }
    }
    check(worst < 1e-6, "each curve's slope is its derivative");
}

void driven() {
    const auto x = sine(220, 96000, 0.3);
    const auto thd = [&](int type, double gain, double& second) {
        P p = configured({type, gain, 0, 0, 0, 1, 0, 0});
        const auto y = render(p, x);
        const double f = component(y, 220);
        double h = 0;
        for (int k = 2; k <= 9; ++k) {
            h += std::pow(component(y, 220.0 * k), 2);
        }
        second = component(y, 440) / f;
        return std::sqrt(h) / f;
    };
    double s0 = 0, s1 = 0, s2 = 0;
    const double low = thd(P::Overdrive, 0, s0), high = thd(P::Overdrive, 100, s1);
    check(high > 2 * low, "Gain raises the distortion (" + std::to_string(low) + " -> " + std::to_string(high) + ")");
    check(s0 > 0.005, "Overdrive's bias adds a second harmonic (" + std::to_string(s0) + ")");
    check(thd(P::Fuzz, 50, s2) > thd(P::Overdrive, 50, s1), "Fuzz distorts more than Overdrive at the same Gain");
    // Sub lets the low end through: 50 Hz with Sub on is louder than off.
    P off = configured({P::Overdrive, 0, 0, 0, 0, 1, 0, 0}), on = configured({P::Overdrive, 0, 1, 0, 0, 1, 0, 0});
    const auto low50 = sine(50, 96000, 1e-4);
    check(component(render(on, low50), 50) > 2 * component(render(off, low50), 50), "Sub keeps the low end");
}

void routing() {
    std::vector<float> x(20000), y(20000), l(20000), r(20000);
    for (std::size_t i = 0; i < x.size(); ++i) {
        x[i] = static_cast<float>(0.3 * std::sin(0.013 * static_cast<double>(i)));
        y[i] = static_cast<float>(0.2 * std::sin(0.11 * static_cast<double>(i)));
    }
    P dry;
    dry.set(P::DryWet, 0);
    dry.process(x.data(), y.data(), l.data(), r.data(), x.size());
    check(l == x && r == y, "Dry/Wet 0 is the input itself");
    std::vector<float> first;
    bool identical = true;
    for (std::size_t block : {1u, 64u, 333u, 4096u}) {
        P p;
        p.set(P::Type, P::Fuzz);
        p.set(P::Mid, 50);
        p.prepare(kRate);
        for (std::size_t a = 0; a < x.size(); a += block) {
            const auto n = std::min(block, x.size() - a);
            p.process(x.data() + a, y.data() + a, l.data() + a, r.data() + a, n);
        }
        if (first.empty()) {
            first = l;
        } else {
            identical = identical && l == first;
        }
    }
    check(identical, "every block size gives the same samples");
    P p;
    std::vector<float> bad(512, std::numeric_limits<float>::quiet_NaN()), o(512), q(512);
    counting = true;
    p.process(bad.data(), bad.data(), o.data(), q.data(), bad.size());
    const bool silent = std::all_of(o.begin(), o.end(), [](float v) { return v == 0.f; });
    p.set(P::Type, P::Fuzz);
    p.set(P::Gain, 100);
    p.set(P::MidFreq, 2);
    p.process(x.data(), x.data(), o.data(), q.data(), 512);
    counting = false;
    check(allocations == 0, "processing and moved controls allocate nothing");
    check(silent, "a NaN input is silence");
    check(std::all_of(o.begin(), o.end(), [](float v) { return std::isfinite(v) && std::abs(v) < 20; }), "full Fuzz stays bounded");
    P z;
    z.set(P::DryWet, 0);
    std::vector<float> left(4800, 0.25f), shared(4800, -0.5f), rightOut(4800);
    z.process(left.data(), shared.data(), shared.data(), rightOut.data(), 4800);
    check(shared.back() == 0.25f && rightOut.back() == -0.5f, "an output plane that is the other channel's input is read first");
}

}  // namespace

int main() {
    quiet();
    driven();
    routing();
    std::printf("%s -- %d checks, %d failure(s)\n", failures ? "FAILED" : "PASS", checks, failures);
    return failures ? 1 : 0;
}
