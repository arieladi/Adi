// SPDX-License-Identifier: GPL-3.0-or-later
//
// Live 12's Glue Compressor, the core (DEV-22, ADR-0249 d7). Each property is
// checked against a number derived apart from the code: the static curve from
// the hard-knee gain computer, Range as a cap on it, the ballistics from a
// one-pole's time constant on the reduction in dB, Auto's two stages, Makeup,
// Soft Clip's ceiling and Dry/Wet. Parity with Live's sound waits on a Live
// 12.4 reading of the Glue Compressor.
#include "adi/dsp/glue_compressor.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <new>
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
void near(double got, double want, double tolerance, const std::string& what) {
    ++checks;
    if (!(std::abs(got - want) <= tolerance)) {
        ++failures;
        std::printf("FAIL %s\n      got %.6f, want %.6f +- %g\n", what.c_str(), got, want, tolerance);
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
using G = adi::dsp::GlueCompressor;
double fromDb(double db) { return std::pow(10.0, db / 20.0); }
double db(double x) { return 20 * std::log10(std::abs(x)); }

std::vector<float> run(G& g, const std::vector<float>& in) {
    std::vector<float> l(in.size()), r(in.size());
    g.process(in.data(), in.data(), l.data(), r.data(), in.size());
    return l;
}
std::vector<float> constant(std::size_t n, double v) { return std::vector<float>(n, static_cast<float>(v)); }
/// Threshold -20 dBFS, the fastest attack, 0.1 s release.
G made(int ratio = 1) {
    G g;
    g.set(G::Threshold, -20);
    g.set(G::Ratio, ratio);
    g.set(G::Attack, 0);
    g.set(G::Release, 0);
    g.prepare(48000);
    return g;
}

void staticCurve() {
    for (int ratio = 0; ratio < 3; ++ratio) {
        const double n = G::kRatios[ratio];
        auto g = made(ratio);
        near(g.staticGainDb(-10), -10 * (1 - 1 / n), 1e-12,
             std::to_string(static_cast<int>(n)) + ":1, 10 dB over: the reduction is 10 (1 - 1/n) dB");
        near(g.staticGainDb(-25), 0, 0, "and under the threshold, none");
        // Settled on a constant -10 dBFS: the output follows the curve.
        const auto y = run(g, constant(24000, fromDb(-10)));
        near(db(y.back()), -20 + 10 / n, 1e-4, "settled at -10 dBFS in, " + std::to_string(-20 + 10 / n) + " out");
    }
    // Range caps the reduction.
    auto g = made(2);   // 10:1
    g.set(G::Range, 5);
    const auto y = run(g, constant(24000, fromDb(-10)));
    near(db(y.back()), -15, 1e-4, "Range 5 dB: 10:1 would take 9 dB, it takes 5");
}

void ballistics() {
    // Attack: the reduction grows toward its target with Attack's constant.
    auto g = made();   // 4:1: 7.5 dB at -10 dBFS
    g.set(G::Attack, 5);   // 10 ms, 480 samples
    (void)run(g, constant(480, fromDb(-10)));
    near(g.gainReductionDb(), -7.5 * (1 - std::exp(-1.0)), 0.01, "Attack 10 ms: 1 - 1/e of the way after 480 samples");
    // Release: back toward 0 with Release's constant once the level is gone.
    auto r = made();
    r.set(G::Release, 2);   // 0.4 s, 19,200 samples
    (void)run(r, constant(48000, fromDb(-10)));
    const double held = r.gainReductionDb();
    (void)run(r, constant(19200, fromDb(-60)));
    near(r.gainReductionDb(), held * std::exp(-1.0), 0.02, "Release 0.4 s: 1/e of the reduction left after 19,200 samples");
    // Auto: a short peak releases fast (0.1 s), a held level slowly (1.2 s).
    auto shortPeak = made(), held2 = made();
    shortPeak.set(G::Release, 6);
    held2.set(G::Release, 6);
    (void)run(shortPeak, constant(96, fromDb(-10)));   // 2 ms
    (void)run(held2, constant(480000, fromDb(-10)));   // 10 s
    const double a0 = shortPeak.gainReductionDb(), b0 = held2.gainReductionDb();
    (void)run(shortPeak, constant(4800, fromDb(-60)));   // 0.1 s later
    (void)run(held2, constant(4800, fromDb(-60)));
    check(shortPeak.gainReductionDb() / a0 < 0.45 && held2.gainReductionDb() / b0 > 0.85,
          "Auto: 0.1 s after a 2 ms peak " + std::to_string(shortPeak.gainReductionDb() / a0) +
              " of it is left, after 10 s held " + std::to_string(held2.gainReductionDb() / b0));
}

void makeupClipAndMix() {
    auto g = made();
    g.set(G::Makeup, 6);
    const auto y = run(g, constant(24000, fromDb(-30)));   // under the threshold
    near(db(y.back()), -24, 1e-4, "Makeup 6 dB under the threshold: -30 dBFS leaves at -24");
    auto s = made();
    s.set(G::Makeup, 20);
    s.set(G::SoftClip, 1);
    const auto loud = run(s, constant(24000, fromDb(-5)));
    check(std::abs(loud.back()) < fromDb(G::kSoftClipDb), "Soft Clip: nothing leaves at or above -0.5 dBFS (" +
                                                           std::to_string(db(loud.back())) + ")");
    auto q = made();
    q.set(G::SoftClip, 1);
    const auto quiet = run(q, constant(4800, fromDb(-40)));
    near(quiet.back(), fromDb(-40), 1e-6, "and a -40 dBFS signal passes as itself");
    // Dry/Wet 0: the input itself, bit for bit (no lookahead, so aligned).
    auto d = made();
    d.set(G::DryWet, 0);
    std::vector<float> x(4800);
    for (std::size_t i = 0; i < x.size(); ++i) {
        x[i] = static_cast<float>(0.5 * std::sin(0.01 * static_cast<double>(i)));
    }
    check(run(d, x) == x, "Dry/Wet 0: the input, bit for bit");
    // 50 %: halfway between, linearly.
    auto h = made(2), w = made(2);
    h.set(G::DryWet, 50);
    const auto half = run(h, constant(24000, fromDb(-10)));
    const auto full = run(w, constant(24000, fromDb(-10)));
    near(half.back(), 0.5 * fromDb(-10) + 0.5 * full.back(), 1e-6, "Dry/Wet 50 %: the mean of dry and wet");
}

void robustness() {
    // Linked: a loud left compresses a quiet right as well.
    auto g = made(2);
    std::vector<float> l(24000, static_cast<float>(fromDb(-5))), r(24000, static_cast<float>(fromDb(-40))), ol(24000), orr(24000);
    g.process(l.data(), r.data(), ol.data(), orr.data(), l.size());
    near(db(orr.back()), -40 + g.staticGainDb(-5), 1e-3, "linked: the quiet right takes the loud left's reduction");
    auto a = made(), b = made();
    std::vector<float> withNaN(9600), withZero(9600);
    for (std::size_t i = 0; i < withNaN.size(); ++i) {
        withNaN[i] = withZero[i] = static_cast<float>(0.5 * std::sin(0.02 * static_cast<double>(i)));
    }
    for (std::size_t i = 3000; i < 3300; ++i) {
        withNaN[i] = std::numeric_limits<float>::quiet_NaN();
        withZero[i] = 0.f;
    }
    check(run(a, withNaN) == run(b, withZero), "a NaN block reads as silence, exactly");
    auto c = made();
    std::vector<float> x(512, 0.5f), y(512);
    counting = true;
    for (int i = 0; i < 100; ++i) {
        c.set(G::Release, i % 7);
        c.process(x.data(), x.data(), y.data(), y.data(), 512);
    }
    counting = false;
    check(allocations == 0, "process and control changes allocate nothing");
}

}  // namespace

int main() {
    staticCurve();
    ballistics();
    makeupClipAndMix();
    robustness();
    std::printf("%s -- %d checks, %d failure(s)\n", failures ? "FAILED" : "PASS", checks, failures);
    return failures ? 1 : 0;
}
