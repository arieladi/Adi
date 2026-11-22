// SPDX-License-Identifier: GPL-3.0-or-later
//
// Live 12's Dynamic Tube, the core (DEV-22). The references are the
// header's own laws: a quiet sine passes at Output's gain whatever Drive,
// Tube, Bias and Tone are (the tube is normalised by its slope at the bias,
// the tone's tilt and its inverse cancel), within 0.05 dB; each curve's slope
// matches its numerical derivative; with no bias a curve is odd and adds no
// even harmonic, with one it does; the envelope moves the bias with level.
// Parity with Live waits on a reading.
#include "adi/dsp/dynamic_tube.hpp"

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
using T = adi::dsp::DynamicTube;
constexpr double kRate = 48000;

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
std::vector<float> render(T& t, const std::vector<float>& x) {
    std::vector<float> l(x.size()), r(x.size());
    t.process(x.data(), x.data(), l.data(), r.data(), x.size());
    return l;
}

void quietUnity() {
    double worst = 0;
    for (int tube : {T::A, T::B, T::C}) {
        for (double drive : {-12.0, 0.0, 24.0}) {
            for (double bias : {0.0, 50.0, 100.0}) {
                for (double tone : {-100.0, 0.0, 100.0}) {
                    for (double hz : {200.0, 2000.0, 8000.0}) {
                        T t;
                        t.set(T::Tube, tube);
                        t.set(T::Drive, drive);
                        t.set(T::Bias, bias);
                        t.set(T::Tone, tone);
                        t.set(T::Output, -3);
                        t.prepare(kRate);
                        const auto y = render(t, sine(hz, 96000, 1e-5));
                        // A quiet signal passes at the curve's slope at the
                        // bias, f'(b), and Output.
                        const double want = -3 + 20 * std::log10(T::slope(tube, 0.5 * bias / 100));
                        worst = std::max(worst, std::abs(20 * std::log10(component(y, hz) / 1e-5) - want));
                    }
                }
            }
        }
    }
    check(worst < 0.05, "a quiet sine passes at f'(b) and Output's gain for every Tube, Drive, Bias and Tone (worst " + std::to_string(worst) + " dB)");
}

void slopes() {
    double worst = 0;
    for (int tube : {T::A, T::B, T::C}) {
        for (double x : {-1.7, -0.6, 0.0, 0.3, 0.9, 2.5}) {
            const double h = 1e-5, numeric = (T::curve(tube, x + h) - T::curve(tube, x - h)) / (2 * h);
            worst = std::max(worst, std::abs(numeric - T::slope(tube, x)));
        }
    }
    check(worst < 1e-8, "each curve's slope is its derivative (worst " + std::to_string(worst) + ")");
}

void harmonics() {
    const auto x = sine(200, 96000, 0.5);
    const auto second = [&](double bias, double envelope, double drive, double& thd) {
        T t;
        t.set(T::Drive, drive);
        t.set(T::Bias, bias);
        t.set(T::Envelope, envelope);
        t.prepare(kRate);
        const auto y = render(t, x);
        const double f = component(y, 200);
        double h = 0;
        for (int k = 2; k <= 9; ++k) {
            h += std::pow(component(y, 200.0 * k), 2);
        }
        thd = std::sqrt(h) / f;
        return component(y, 400) / f;
    };
    double thd = 0, thdLow = 0;
    check(second(0, 0, 12, thd) < 1e-5, "no bias: the curve is odd, no second harmonic");
    check(second(100, 0, 12, thd) > 0.01, "Bias 100 %: a second harmonic");
    check(second(0, 100, 12, thd) > 0.01, "Envelope +100 % with no bias: the level moves the bias, a second harmonic");
    (void)second(50, 0, 0, thdLow);
    (void)second(50, 0, 24, thd);
    check(thd > 3 * thdLow, "Drive raises the distortion");
    // A plant: a curve whose slope were off would not pass at unity.
    check(std::abs(T::slope(T::B, 0.5) - T::slope(T::A, 0.5)) > 0.1, "plant: the curves' slopes differ, so a wrong one would show");
}

void routing() {
    std::vector<float> x(20000), y(20000), l(20000), r(20000);
    for (std::size_t i = 0; i < x.size(); ++i) {
        x[i] = static_cast<float>(0.3 * std::sin(0.013 * static_cast<double>(i)));
        y[i] = static_cast<float>(0.2 * std::sin(0.11 * static_cast<double>(i)));
    }
    T dry;
    dry.set(T::DryWet, 0);
    dry.process(x.data(), y.data(), l.data(), r.data(), x.size());
    check(l == x && r == y, "Dry/Wet 0 is the input itself");
    std::vector<float> first;
    bool identical = true;
    for (std::size_t block : {1u, 64u, 333u, 4096u}) {
        T t;
        t.set(T::Envelope, 60);
        t.set(T::Drive, 18);
        t.set(T::Tone, 40);
        t.prepare(kRate);
        for (std::size_t at = 0; at < x.size(); at += block) {
            const auto n = std::min(block, x.size() - at);
            t.process(x.data() + at, y.data() + at, l.data() + at, r.data() + at, n);
        }
        if (first.empty()) {
            first = l;
        } else {
            identical = identical && l == first;
        }
    }
    check(identical, "every block size gives the same samples");
    T t;
    std::vector<float> bad(512, std::numeric_limits<float>::quiet_NaN()), o(512), p(512);
    counting = true;
    t.process(bad.data(), bad.data(), o.data(), p.data(), bad.size());
    const bool silent = std::all_of(o.begin(), o.end(), [](float v) { return v == 0.f; });
    t.set(T::Tube, T::C);
    t.set(T::Drive, 36);
    t.process(x.data(), x.data(), o.data(), p.data(), 512);
    counting = false;
    check(allocations == 0, "processing and moved controls allocate nothing");
    check(silent, "a NaN input is silence");
    check(std::all_of(o.begin(), o.end(), [](float v) { return std::isfinite(v); }), "Drive 36 dB stays finite");
    T z;
    z.set(T::DryWet, 0);
    std::vector<float> left(4800, 0.25f), shared(4800, -0.5f), rightOut(4800);
    z.process(left.data(), shared.data(), shared.data(), rightOut.data(), 4800);
    check(shared.back() == 0.25f && rightOut.back() == -0.5f, "an output plane that is the other channel's input is read first");
}

}  // namespace

/// The review of 2b507061: a biased tube's DC came out, and normalising by
/// the slope at the bias made loud swings up to +18 dB louder.
void reviewed() {
    struct Case {
        int tube;
        double drive, bias, envelope, attack, release, tone, hz, amplitude;
        const char* what;
    };
    const Case cases[] = {{T::A, 0, 50, 0, 5, 200, 0, 200, 0.5, "the defaults, a -6 dBFS sine"},
                          {T::B, 0, 100, 0, 5, 200, 0, 200, 0.5, "Tube B, Bias 100, a -6 dBFS sine"},
                          {T::B, -12, 100, 100, 0.1, 1, 100, 5000, 1.0, "Tube B, Bias and Envelope full, fast, Tone +100, full scale"}};
    for (const auto& c : cases) {
        T t;
        t.set(T::Tube, c.tube);
        t.set(T::Drive, c.drive);
        t.set(T::Bias, c.bias);
        t.set(T::Envelope, c.envelope);
        t.set(T::Attack, c.attack);
        t.set(T::Release, c.release);
        t.set(T::Tone, c.tone);
        t.prepare(kRate);
        const auto y = render(t, sine(c.hz, 96000, c.amplitude));
        double mean = 0, peak = 0;
        for (std::size_t i = 48000; i < y.size(); ++i) {
            mean += y[i];
            peak = std::max(peak, static_cast<double>(std::abs(y[i])));
        }
        mean /= 48000;
        check(std::abs(mean) < 1e-3, std::string(c.what) + ": no DC (" + std::to_string(mean) + ")");
        check(peak <= 1.6 * c.amplitude, std::string(c.what) + ": no louder than its input, room for the tone's overshoot (" + std::to_string(peak) + ")");
    }
}

/// Drive, Bias, Output and Dry/Wet moved at a tone's peak glide: the
/// largest step while they move stays within the signal's own, before or
/// after.
void glides() {
    struct Move {
        T::Param param;
        double from, to;
        const char* what;
    };
    for (const Move m : {Move{T::Drive, -12, 36, "Drive"}, Move{T::Bias, 0, 100, "Bias"}, Move{T::Output, -36, 12, "Output"},
                         Move{T::DryWet, 0, 100, "Dry/Wet"}}) {
        T t;
        t.set(m.param, m.from);
        t.prepare(kRate);
        const auto x = sine(110, 96000, 0.2);
        std::vector<float> l(x.size()), r(x.size());
        constexpr std::size_t at = 48109;   // the tone's peak
        t.process(x.data(), x.data(), l.data(), r.data(), at);
        t.set(m.param, m.to);
        t.process(x.data() + at, x.data() + at, l.data() + at, r.data() + at, x.size() - at);
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
        check(moving <= 1.5 * std::max(settled, 1e-9), std::string(m.what) + " glides: no step past the signal's own (" + std::to_string(moving) +
                                                           " against " + std::to_string(settled) + ")");
    }
}

int main() {
    glides();
    reviewed();
    quietUnity();
    slopes();
    harmonics();
    routing();
    std::printf("%s -- %d checks, %d failure(s)\n", failures ? "FAILED" : "PASS", checks, failures);
    return failures ? 1 : 0;
}
