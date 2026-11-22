// SPDX-License-Identifier: GPL-3.0-or-later
//
// Live 12's Limiter, the core (DEV-22, ADR-0249 d7), on ADI's lookahead
// brickwall. Each property is checked against a number derived apart from the
// code: the latency from the lookahead in whole samples (plus the brickwall's
// true-peak delay of 10 at 48 kHz), the ceiling on every sample (and between
// them, on the Annex's reading, in True Peak mode), unity below the ceiling,
// and each link's independence. Parity with Live's sound waits on a Live 12.4
// reading of the Limiter.
#include "adi/dsp/live_limiter.hpp"
#include "adi/dsp/true_peak.hpp"

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
using L = adi::dsp::LiveLimiter;
constexpr double kPi = 3.14159265358979323846;
double fromDb(double db) { return std::pow(10.0, db / 20.0); }

struct Stereo {
    std::vector<float> l, r;
};
Stereo run(L& lim, const Stereo& in, std::size_t block = 97) {
    Stereo out{std::vector<float>(in.l.size()), std::vector<float>(in.r.size())};
    for (std::size_t at = 0; at < in.l.size(); at += block) {
        const auto n = std::min(block, in.l.size() - at);
        lim.process(in.l.data() + at, in.r.data() + at, out.l.data() + at, out.r.data() + at, n);
    }
    return out;
}
L made(double ceilingDb, int mode = L::Standard, int link = L::Stereo, int lookahead = 1) {
    L lim;
    lim.set(L::Ceiling, ceilingDb);
    lim.set(L::Mode, mode);
    lim.set(L::Link, link);
    lim.set(L::Lookahead, lookahead);
    lim.prepare(48000);
    return lim;
}
Stereo noise(std::size_t n, double amplitude, std::uint32_t seed = 2026u) {
    Stereo x{std::vector<float>(n), std::vector<float>(n)};
    for (std::size_t i = 0; i < n; ++i) {
        seed = seed * 1664525u + 1013904223u;
        x.l[i] = static_cast<float>(amplitude * 2 * ((seed >> 8) / 16777216.0 - 0.5));
        seed = seed * 1664525u + 1013904223u;
        x.r[i] = static_cast<float>(amplitude * 2 * ((seed >> 8) / 16777216.0 - 0.5));
    }
    return x;
}
double peakOf(const std::vector<float>& v) {
    double p = 0;
    for (float s : v) {
        p = std::max(p, static_cast<double>(std::abs(s)));
    }
    return p;
}

void latency() {
    const int lookahead[3] = {72, 144, 288};   // 1.5, 3 and 6 ms at 48 kHz
    for (int choice = 0; choice < 3; ++choice) {
        auto lim = made(-1, L::Standard, L::Stereo, choice);
        check(lim.latency() == lookahead[choice], "Lookahead " + std::to_string(L::kLookaheadMs[choice]) + " ms: " +
                                                      std::to_string(lim.latency()) + " samples");
        auto tp = made(-1, L::TruePeak, L::Stereo, choice);
        check(tp.latency() == lookahead[choice] + 10, "and in True Peak mode, 10 more (the detector's delay): " +
                                                          std::to_string(tp.latency()));
    }
}

void belowTheCeiling() {
    // Under the ceiling every mode is a delay, bit for bit, at Gain 0 dB.
    Stereo x{std::vector<float>(9600), std::vector<float>(9600)};
    for (std::size_t i = 0; i < x.l.size(); ++i) {
        x.l[i] = static_cast<float>(0.25 * std::sin(2 * kPi * 440 * static_cast<double>(i) / 48000));
        x.r[i] = static_cast<float>(0.2 * std::sin(2 * kPi * 997 * static_cast<double>(i) / 48000));
    }
    for (int mode : {L::Standard, L::TruePeak}) {
        for (int link : {L::Stereo, L::LeftRight, L::MidSide}) {
            auto lim = made(-1, mode, link);
            const auto y = run(lim, x);
            const auto d = static_cast<std::size_t>(lim.latency());
            bool same = true;
            for (std::size_t i = d; i < x.l.size() && same; ++i) {
                same = std::abs(y.l[i] - x.l[i - d]) <= 1e-7f && std::abs(y.r[i] - x.r[i - d]) <= 1e-7f;
            }
            check(same, "mode " + std::to_string(mode) + ", link " + std::to_string(link) +
                            ": under the ceiling the output is the input, " + std::to_string(d) + " samples late");
        }
    }
    // Gain: +6 dB before the limiter, still under the ceiling.
    auto lim = made(0);
    lim.set(L::Gain, 6);
    Stereo quiet{std::vector<float>(4800, 0.1f), std::vector<float>(4800, -0.1f)};
    const auto y = run(lim, quiet);
    check(std::abs(y.l.back() - 0.1f * static_cast<float>(fromDb(6))) < 1e-6f,
          "Gain +6 dB: a 0.1 signal leaves at " + std::to_string(y.l.back()));
}

void theCeiling() {
    const double ceiling = fromDb(-1);
    const auto loud = noise(48000, 4.0);   // +12 dB over full scale
    for (int link : {L::Stereo, L::LeftRight}) {
        auto lim = made(-1, L::Standard, link);
        const auto y = run(lim, loud);
        check(std::max(peakOf(y.l), peakOf(y.r)) <= ceiling * (1 + 1e-6),
              "Standard, link " + std::to_string(link) + ": every sample at or under the -1 dB ceiling");
    }
    // True Peak: under the ceiling between the samples too, on the Annex's reading.
    auto tp = made(-1, L::TruePeak, L::Stereo, 2);
    const auto y = run(tp, loud);
    adi::dsp::TruePeakMeter meter;
    meter.prepare(48000, 2);
    const float* p[2] = {y.l.data(), y.r.data()};
    meter.process(p, 2, static_cast<int>(y.l.size()));
    check(meter.peakDb() <= -1 + 0.001, "True Peak: the output's true peak " + std::to_string(meter.peakDb()) +
                                            " dBTP against a -1 dB ceiling");
    // Soft Clip: never at the ceiling, a quiet signal as itself.
    auto soft = made(-1, L::SoftClip);
    const auto s = run(soft, loud);
    // The brickwall holds the soft clip's input to the ceiling, so its output
    // tops out at ceiling * tanh(1), about 2.4 dB under it, and gets there.
    const double softPeak = std::max(peakOf(s.l), peakOf(s.r));
    check(softPeak <= ceiling * std::tanh(1.0) * (1 + 1e-6) && softPeak > 0.99 * ceiling * std::tanh(1.0),
          "Soft Clip: the peak is the ceiling's tanh(1) (" + std::to_string(softPeak / ceiling) + " of it)");
    auto quietSoft = made(-1, L::SoftClip);
    Stereo quiet{std::vector<float>(4800, 0.01f), std::vector<float>(4800, 0.01f)};
    const auto q = run(quietSoft, quiet);
    check(std::abs(q.l.back() - 0.01f) < 1e-6f, "and a -40 dBFS signal passes as itself (" +
                                                    std::to_string(q.l.back()) + ")");
}

void links() {
    // A loud left and a quiet right.
    Stereo x{std::vector<float>(24000), std::vector<float>(24000)};
    for (std::size_t i = 0; i < x.l.size(); ++i) {
        x.l[i] = static_cast<float>(3.0 * std::sin(2 * kPi * 200 * static_cast<double>(i) / 48000));
        x.r[i] = static_cast<float>(0.1 * std::sin(2 * kPi * 300 * static_cast<double>(i) / 48000));
    }
    auto stereo = made(-1, L::Standard, L::Stereo);
    auto separate = made(-1, L::Standard, L::LeftRight);
    const auto ys = run(stereo, x), yl = run(separate, x);
    const auto d = static_cast<std::size_t>(stereo.latency());
    double rs = 0, rl = 0;
    for (std::size_t i = 12000; i < x.r.size(); ++i) {
        rs = std::max(rs, static_cast<double>(std::abs(ys.r[i])));
        rl = std::max(rl, static_cast<double>(std::abs(yl.r[i] - x.r[i - d])));
    }
    check(rs < 0.05, "Stereo: the quiet right ducks with the loud left (peak " + std::to_string(rs) + ")");
    check(rl < 1e-7, "L/R: the quiet right passes as itself (largest change " + std::to_string(rl) + ")");
    // M/S: the same signal in both channels has no side; mid is limited.
    Stereo mono{x.l, x.l};
    auto ms = made(-1, L::Standard, L::MidSide);
    const auto ym = run(ms, mono);
    check(peakOf(ym.l) <= fromDb(-1) * (1 + 1e-6) && ym.l == ym.r,
          "M/S: a mono signal is limited as its mid, left and right equal");
}

void robustness() {
    auto a = made(-1), b = made(-1);
    auto withNaN = noise(9600, 2.0), withZero = withNaN;
    for (std::size_t i = 3000; i < 3300; ++i) {
        withNaN.l[i] = std::numeric_limits<float>::quiet_NaN();
        withNaN.r[i] = std::numeric_limits<float>::infinity();
        withZero.l[i] = withZero.r[i] = 0.f;
    }
    const auto ya = run(a, withNaN), yb = run(b, withZero);
    check(ya.l == yb.l && ya.r == yb.r, "a NaN or infinite block reads as silence, exactly");
    // Changing every control while running allocates nothing.
    auto lim = made(-1);
    const auto x = noise(512, 2.0);
    std::vector<float> ol(512), orr(512);
    counting = true;
    for (int i = 0; i < 60; ++i) {
        lim.set(L::Lookahead, i % 3);
        lim.set(L::Mode, (i / 3) % 3);
        lim.set(L::Link, (i / 9) % 3);
        lim.set(L::Release, 10.0 + i);
        lim.process(x.l.data(), x.r.data(), ol.data(), orr.data(), 512);
    }
    counting = false;
    check(allocations == 0, "process and every control change allocate nothing");
}

}  // namespace

int main() {
    latency();
    belowTheCeiling();
    theCeiling();
    links();
    robustness();
    std::printf("%s -- %d checks, %d failure(s)\n", failures ? "FAILED" : "PASS", checks, failures);
    return failures ? 1 : 0;
}
