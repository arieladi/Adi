// SPDX-License-Identifier: GPL-3.0-or-later
//
// Live 12's Vinyl Distortion, the core (DEV-22). The reference numbers come
// from the distortions' own curves, apart from the code's signal path: a
// tone A at a band's centre passes that band unchanged in steady state, so
// the tracing distortion adds a second harmonic of Drive A^2 and nothing odd,
// and the pinch adds the harmonics of f(g A sin) / g - A sin, computed here
// by a numerical Fourier sum of the curve. Each must hold within 0.05 dB.
// Stereo pinch keeps the mid clean; the crackle comes at its density and
// within its volume. Parity with Live waits on a reading.
#include "adi/dsp/vinyl_distortion.hpp"

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
using V = adi::dsp::VinylDistortion;
constexpr double kRate = 48000;

std::vector<float> sine(double hz, std::size_t frames, double amplitude) {
    std::vector<float> x(frames);
    for (std::size_t i = 0; i < frames; ++i) {
        x[i] = static_cast<float>(amplitude * std::sin(2 * std::numbers::pi * hz * static_cast<double>(i) / kRate));
    }
    return x;
}
/// The amplitude of the component at `hz` over the last whole second.
double amplitude(const std::vector<float>& y, double hz) {
    std::complex<double> sum = 0;
    const std::size_t n = 48000, from = y.size() - n;
    for (std::size_t i = 0; i < n; ++i) {
        sum += static_cast<double>(y[from + i]) * std::polar(1.0, -2 * std::numbers::pi * hz * static_cast<double>(from + i) / kRate);
    }
    return 2 * std::abs(sum) / static_cast<double>(n);
}
void run(V& v, const std::vector<float>& l, const std::vector<float>& r, std::vector<float>& outL, std::vector<float>& outR,
         std::size_t block = 512) {
    outL.assign(l.size(), 0.f);
    outR.assign(l.size(), 0.f);
    for (std::size_t at = 0; at < l.size(); at += block) {
        const auto n = std::min(block, l.size() - at);
        v.process(l.data() + at, r.data() + at, outL.data() + at, outR.data() + at, n);
    }
}
double db(double a, double b) { return std::abs(20 * std::log10(a / b)); }

/// The k-th harmonic's amplitude of the pinch curve on a sine A, by a
/// numerical Fourier sum of the curve itself (not the code's function).
double pinchHarmonic(double a, double drive, bool soft, int k) {
    const double g = 1 + 20 * drive;
    const int n = 1 << 16;
    double s = 0;
    for (int i = 0; i < n; ++i) {
        const double theta = 2 * std::numbers::pi * (i + 0.5) / n, b = a * std::sin(theta), x = g * b;
        const double f = soft ? std::tanh(x) : (x > 1 ? 1 : x < -1 ? -1 : x);
        s += (f / g - b) * std::sin(k * theta);
    }
    return std::abs(2 * s / n);
}

void tracing() {
    for (double drive : {0.2, 0.6, 1.0}) {
        V v;
        v.set(V::TracingDrive, drive);
        v.set(V::TracingFreq, 1000);
        v.set(V::TracingWidth, 1);
        v.prepare(kRate);
        const auto x = sine(1000, 96000, 0.4);
        std::vector<float> l, r;
        run(v, x, x, l, r);
        const double second = amplitude(l, 2000), third = amplitude(l, 3000), first = amplitude(l, 1000);
        check(db(second, drive * 0.16) < 0.05, "tracing, Drive " + std::to_string(drive) + ": the second harmonic is Drive A^2 within 0.05 dB (" +
                                                   std::to_string(second) + " against " + std::to_string(drive * 0.16) + ")");
        check(third < 1e-5 * first && db(first, 0.4) < 0.01, "tracing adds nothing odd and leaves the tone");
    }
}

void pinch() {
    for (bool soft : {true, false}) {
        for (double drive : {0.1, 0.5}) {
            V v;
            v.set(V::TracingOn, 0);
            v.set(V::PinchOn, 1);
            v.set(V::PinchDrive, drive);
            v.set(V::PinchFreq, 1100);
            v.set(V::Soft, soft ? 1 : 0);
            v.set(V::Stereo, 1);
            v.prepare(kRate);
            // 1.1 kHz, not a divisor of the rate: the hard curve's harmonics
            // above Nyquist fold between its harmonics, not onto them.
            const auto x = sine(1100, 96000, 0.4);
            std::vector<float> l, r;
            run(v, x, x, l, r);
            const std::string what = std::string(soft ? "Soft" : "Hard") + " pinch, Drive " + std::to_string(drive);
            for (int k : {3, 5}) {
                const double want = pinchHarmonic(0.4, drive, soft, k), got = amplitude(l, 1100.0 * k);
                check(db(got, want) < 0.05, what + ": harmonic " + std::to_string(k) + " within 0.05 dB of the curve's (" + std::to_string(got) +
                                                " against " + std::to_string(want) + ")");
            }
            double mid = 0;
            for (std::size_t i = 0; i < x.size(); ++i) {
                mid = std::max(mid, std::abs(0.5 * (static_cast<double>(l[i]) + r[i]) - x[i]));
            }
            check(mid < 1e-6, what + ", Stereo: the mid is the clean input");
            check(amplitude(l, 2200) < 1e-5, what + ": the pinch adds nothing even");
        }
    }
    V mono;
    mono.set(V::TracingOn, 0);
    mono.set(V::PinchOn, 1);
    mono.set(V::Stereo, 0);
    const auto x = sine(3000, 9600, 0.5);
    std::vector<float> l, r;
    run(mono, x, x, l, r);
    check(l == r && l != x, "Mono pinch distorts both channels alike");
    // Below its clip (g A = 0.8) the hard curve adds nothing and the soft one
    // a third harmonic: a swapped switch fails one of these.
    for (bool soft : {false, true}) {
        V v;
        v.set(V::TracingOn, 0);
        v.set(V::PinchOn, 1);
        v.set(V::PinchDrive, 0.05);
        v.set(V::PinchFreq, 1100);
        v.set(V::Soft, soft ? 1 : 0);
        v.prepare(kRate);
        const auto s1 = sine(1100, 96000, 0.4);
        run(v, s1, s1, l, r);
        const double third = amplitude(l, 3300);
        check(soft ? db(third, pinchHarmonic(0.4, 0.05, true, 3)) < 0.05 : third < 1e-6,
              soft ? "Soft pinch below the clip: its third harmonic" : "Hard pinch below the clip adds nothing");
    }
}

void crackle() {
    V v;
    v.set(V::TracingOn, 0);
    v.set(V::CrackleVolume, 0);
    v.set(V::CrackleDensity, 20);
    v.prepare(kRate);
    const std::vector<float> quiet(480000, 0.f);
    std::vector<float> l, r;
    run(v, quiet, quiet, l, r, 1000);
    std::size_t clicks = 0;
    float peak = 0;
    for (std::size_t i = 1; i < l.size(); ++i) {
        clicks += std::abs(l[i]) > std::abs(l[i - 1]) + 0.1f ? std::size_t{1} : std::size_t{0};
        peak = std::max(peak, std::abs(l[i]));
    }
    check(clicks > 140 && clicks < 260, "Density 20: about 200 crackles in 10 s (" + std::to_string(clicks) + ")");
    check(peak <= 1.f && peak > 0.5f && l == r, "Volume 0 dB: crackles up to full scale, alike in both channels");
    V quietOne;
    quietOne.set(V::CrackleVolume, -20);
    quietOne.prepare(kRate);
    run(quietOne, quiet, quiet, l, r, 1000);
    check(*std::max_element(l.begin(), l.end()) <= 0.1f + 1e-6f, "Volume -20 dB: no crackle above 0.1");
    V off;
    off.prepare(kRate);
    run(off, quiet, quiet, l, r, 1000);
    check(std::all_of(l.begin(), l.end(), [](float s) { return s == 0.f; }), "Volume at -70 dB is off");
}

void identityAndRobustness() {
    std::vector<float> x(20000), y(20000);
    for (std::size_t i = 0; i < x.size(); ++i) {
        x[i] = static_cast<float>(0.3 * std::sin(0.013 * static_cast<double>(i)) + 0.2 * std::sin(1.9 * static_cast<double>(i)));
        y[i] = static_cast<float>(0.1 * std::sin(0.7 * static_cast<double>(i)));
    }
    V clean;
    clean.set(V::TracingDrive, 0);
    clean.set(V::PinchOn, 1);
    clean.set(V::PinchDrive, 0);
    std::vector<float> l, r;
    run(clean, x, y, l, r);
    check(l == x && r == y, "both Drives at 0 and Crackle off: the input itself");
    std::vector<float> first;
    bool identical = true;
    for (std::size_t block : {1u, 64u, 333u, 4096u}) {
        V v;
        v.set(V::PinchOn, 1);
        v.set(V::CrackleVolume, -10);
        v.set(V::CrackleDensity, 40);
        v.prepare(kRate);
        run(v, x, y, l, r, block);
        if (first.empty()) {
            first = l;
        } else {
            identical = identical && l == first;
        }
    }
    check(identical, "every block size gives the same samples");
    V v;
    v.set(V::PinchOn, 1);
    std::vector<float> bad(512, std::numeric_limits<float>::quiet_NaN()), o(512), p(512);
    counting = true;
    v.process(bad.data(), bad.data(), o.data(), p.data(), bad.size());
    v.set(V::TracingFreq, 17000);
    v.set(V::Soft, 1);
    v.set(V::CrackleVolume, -6);
    v.process(x.data(), x.data(), o.data(), p.data(), 512);
    counting = false;
    check(allocations == 0, "processing and moved controls allocate nothing");
    check(std::all_of(o.begin(), o.end(), [](float s) { return std::isfinite(s); }), "a NaN input leaves the output finite");
    V z;
    z.set(V::TracingDrive, 0);
    std::vector<float> left(4800, 0.25f), shared(4800, -0.5f), rightOut(4800);
    z.process(left.data(), shared.data(), shared.data(), rightOut.data(), 4800);
    check(rightOut.back() == -0.5f && shared.back() == 0.25f, "an output plane that is the other channel's input is read first");
}

}  // namespace

/// The review of 6f75c80a: three findings, each a check that fails on the
/// old core.
void reviewed() {
    // Stereo pinch keeps the mid clean for any input, not only L == R: one
    // channel alone, and a pure side signal (whose mid is silence).
    {
        V v;
        v.set(V::TracingOn, 0);
        v.set(V::PinchOn, 1);
        v.set(V::PinchDrive, 0.5);
        v.set(V::PinchFreq, 1100);
        v.set(V::Soft, 1);
        v.set(V::Stereo, 1);
        v.prepare(kRate);
        const auto s = sine(1100, 48000, 0.4);
        const std::vector<float> zero(s.size(), 0.f);
        std::vector<float> l, r;
        run(v, s, zero, l, r);
        double mid = 0, side = 0;
        for (std::size_t i = 0; i < s.size(); ++i) {
            mid = std::max(mid, std::abs(0.5 * (static_cast<double>(l[i]) + r[i]) - 0.5 * s[i]));
            side = std::max(side, std::abs(0.5 * (static_cast<double>(l[i]) - r[i]) - 0.5 * s[i]));
        }
        check(mid < 1e-6 && side > 0.01, "Stereo pinch, the left channel alone: the mid is clean, the side carries the pinch");
        V w;
        w.set(V::TracingOn, 0);
        w.set(V::PinchOn, 1);
        w.set(V::PinchDrive, 0.5);
        w.set(V::Stereo, 1);
        w.prepare(kRate);
        std::vector<float> minus(s.size());
        for (std::size_t i = 0; i < s.size(); ++i) {
            minus[i] = -s[i];
        }
        run(w, s, minus, l, r);
        check(l == s && r == minus, "Stereo pinch on a pure side signal: its mid is silence, and nothing is added");
    }
    // Moving Tracing Drive under a steady tone makes no thump: the output
    // less its input, averaged over 20 ms (whole cycles of the tone and of
    // its second harmonic, long beside Drive's 10 ms glide), stays near 0
    // after the move; the old thump was 0.48 decaying over 30 ms.
    {
        V v;
        v.set(V::TracingDrive, 0);
        v.set(V::TracingFreq, 1000);
        v.prepare(kRate);
        const auto s = sine(1000, 57600, 0.7);
        std::vector<float> l(s.size()), r(s.size());
        v.process(s.data(), s.data(), l.data(), r.data(), 48000);
        v.set(V::TracingDrive, 1);
        v.process(s.data() + 48000, s.data() + 48000, l.data() + 48000, r.data() + 48000, 9600);
        double worst = 0;
        for (std::size_t i = 48000; i + 960 <= s.size(); i += 12) {
            double sum = 0;
            for (std::size_t k = 0; k < 960; ++k) {
                sum += static_cast<double>(l[i + k]) - s[i + k];
            }
            worst = std::max(worst, std::abs(sum / 960));
        }
        check(worst < 0.01, "Tracing Drive moved from 0 to 1 under a tone: no low thump (" + std::to_string(worst) + ")");
    }
    // A huge finite input never comes out infinite.
    {
        V v;
        v.set(V::TracingDrive, 1);
        v.set(V::PinchOn, 1);
        v.prepare(kRate);
        std::vector<float> big(4800), l, r;
        for (std::size_t i = 0; i < big.size(); ++i) {
            big[i] = static_cast<float>(1.5e19 * std::sin(2 * std::numbers::pi * 1000 * static_cast<double>(i) / kRate));
        }
        run(v, big, big, l, r);
        check(std::all_of(l.begin(), l.end(), [](float s) { return std::isfinite(s); }) &&
                  std::all_of(r.begin(), r.end(), [](float s) { return std::isfinite(s); }),
              "an input of 1.5e19 comes out finite");
    }
}

/// Tracing Drive, and the pinch switched on, under a tone glide: the
/// largest step while they move stays within the signal's own, before or
/// after.
void glides() {
    struct Move {
        V::Param param;
        double from, to;
        const char* what;
    };
    for (const Move m : {Move{V::TracingDrive, 0, 1, "Tracing Drive"}, Move{V::PinchOn, 0, 1, "Pinch switched on"}}) {
        V v;
        v.set(V::PinchDrive, 0.6);
        v.set(V::TracingFreq, 1100);
        v.set(V::PinchFreq, 1100);
        v.set(m.param, m.from);
        v.prepare(kRate);
        const auto x = sine(1100, 96000, 0.5);
        std::vector<float> l(x.size()), r(x.size());
        constexpr std::size_t at = 48011;   // the tone's peak
        v.process(x.data(), x.data(), l.data(), r.data(), at);
        v.set(m.param, m.to);
        v.process(x.data() + at, x.data() + at, l.data() + at, r.data() + at, x.size() - at);
        double moving = 0, settled = 0;
        for (std::size_t i = at; i < at + 4800; ++i) {
            moving = std::max(moving, static_cast<double>(std::abs(l[i] - l[i - 1])));
        }
        for (std::size_t i = 24000; i < 48000; ++i) {
            settled = std::max(settled, static_cast<double>(std::abs(l[i] - l[i - 1])));
        }
        for (std::size_t i = 72000; i < 96000; ++i) {
            settled = std::max(settled, static_cast<double>(std::abs(l[i] - l[i - 1])));
        }
        check(moving <= 1.5 * settled, std::string(m.what) + " glides: no step past the signal's own (" + std::to_string(moving) + " against " +
                                           std::to_string(settled) + ")");
    }
}

int main() {
    glides();
    reviewed();
    tracing();
    pinch();
    crackle();
    identityAndRobustness();
    std::printf("%s -- %d checks, %d failure(s)\n", failures ? "FAILED" : "PASS", checks, failures);
    return failures ? 1 : 0;
}
