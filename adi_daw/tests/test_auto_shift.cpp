// SPDX-License-Identifier: GPL-3.0-or-later
//
// Live 12's Auto Shift, the core (DEV-22). The reference numbers are equal
// temperament's own, apart from the code: a sine 23 cents sharp of A4 comes
// out at 440 Hz with Strength 100 %, 11.7 cents sharp at 50 %, at 880 Hz with
// Shift +12, and a sine nearer D than C# in C major comes out at D. Each
// output frequency is the peak of its spectrum over the last second, found
// to 0.001 Hz, and must hold within 1 cent. Parity with Live waits on a
// reading.
#include "adi/dsp/auto_shift.hpp"

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
using S = adi::dsp::AutoShift;
constexpr double kRate = 48000;

std::vector<float> sine(double hz, std::size_t frames, double amplitude = 0.4) {
    std::vector<float> x(frames);
    for (std::size_t i = 0; i < frames; ++i) {
        x[i] = static_cast<float>(amplitude * std::sin(2 * std::numbers::pi * hz * static_cast<double>(i) / kRate));
    }
    return x;
}
double magnitude(const std::vector<float>& y, double hz) {
    std::complex<double> sum = 0;
    const std::size_t n = 48000, from = y.size() - n;
    for (std::size_t i = 0; i < n; ++i) {
        const double w = 0.5 - 0.5 * std::cos(2 * std::numbers::pi * static_cast<double>(i) / static_cast<double>(n));
        sum += w * static_cast<double>(y[from + i]) * std::polar(1.0, -2 * std::numbers::pi * hz * static_cast<double>(i) / kRate);
    }
    return std::abs(sum);
}
/// The spectrum's peak near `guess`, by a coarse scan then golden section.
double peak(const std::vector<float>& y, double guess) {
    double best = guess, top = -1;
    for (double f = guess * 0.95; f <= guess * 1.05; f += 0.5) {
        const double m = magnitude(y, f);
        if (m > top) {
            top = m;
            best = f;
        }
    }
    double a = best - 0.5, b = best + 0.5;
    const double g = (std::sqrt(5.0) - 1) / 2;
    while (b - a > 0.001) {
        const double c = b - g * (b - a), d = a + g * (b - a);
        if (magnitude(y, c) > magnitude(y, d)) {
            b = d;
        } else {
            a = c;
        }
    }
    return 0.5 * (a + b);
}
double cents(double got, double want) { return 1200 * std::log2(got / want); }

std::vector<float> render(S& s, const std::vector<float>& x, std::size_t block = 512) {
    std::vector<float> l(x.size()), r(x.size());
    for (std::size_t at = 0; at < x.size(); at += block) {
        const auto n = std::min(block, x.size() - at);
        s.process(x.data() + at, x.data() + at, l.data() + at, r.data() + at, n);
    }
    return l;
}

void scales() {
    check(S::nearest(69.234, 0, S::Chromatic) == 69, "chromatic: A4 + 23 cents is A4");
    check(S::nearest(61.17, 0, S::Major) == 62, "C major: C# + 17 cents is D");
    check(S::nearest(61.0, 0, S::Major) == 60, "C major: C# exactly, the lower of C and D");
    check(S::nearest(63.4, 9, S::Minor) == 64, "A minor: D# + 40 cents is E");
    check(S::nearest(66.2, 2, S::Major) == 66, "D major: F# is in the scale");
}

void correction() {
    const double sharp = 446;   // A4 + 23.4 cents
    const auto x = sine(sharp, 144000);
    struct Case {
        double strength, shift;
        int root, scale;
        double input, want;
        const char* what;
    };
    const double c = cents(sharp, 440);
    const Case cases[] = {
        {100, 0, 0, S::Chromatic, sharp, 440, "Strength 100 %: 446 Hz comes out at A4"},
        {50, 0, 0, S::Chromatic, sharp, 440 * std::exp2(c / 2 / 1200), "Strength 50 %: half the distance in cents"},
        {0, 0, 0, S::Chromatic, sharp, sharp, "Strength 0: the input's own pitch"},
        {100, 0, 0, S::Major, 280, 440 * std::exp2((62 - 69) / 12.0), "C major: 280 Hz (C# + 17 cents) comes out at D4"},
    };
    for (const auto& k : cases) {
        S s;
        s.set(S::Strength, k.strength);
        s.set(S::Shift, k.shift);
        s.set(S::Root, k.root);
        s.set(S::Scale, k.scale);
        s.prepare(kRate);
        const auto y = render(s, k.input == sharp ? x : sine(k.input, 144000));
        const double got = peak(y, k.want);
        check(std::abs(cents(got, k.want)) < 1, std::string(k.what) + " (" + std::to_string(got) + " Hz against " + std::to_string(k.want) + ")");
    }
    // Corrected, then Shift +12: the Shifter is told 12 semitones less the
    // correction. (Its output for so large a shift is the Shifter's own:
    // lines on a grid of the input plus multiples of its crossfade rate,
    // under an envelope at the commanded pitch. The Shifter's tests own it.)
    S up;
    up.set(S::Shift, 12);
    up.prepare(kRate);
    (void)render(up, x);
    check(std::abs(up.shift() - (12 - c / 100)) < 1e-4, "corrected, then Shift +12: A5 commanded (" + std::to_string(up.shift()) + " semitones)");
    // Detune adds after the correction.
    S detuned;
    detuned.set(S::Detune, 10);
    detuned.prepare(kRate);
    const double tuned = peak(render(detuned, x), 440 * std::exp2(10.0 / 1200));
    check(std::abs(cents(tuned, 440) - 10) < 1, "Detune +10 cents after the correction (" + std::to_string(tuned) + " Hz)");
    // Correction off, no shift: the input's pitch.
    S off;
    off.set(S::Correction, 0);
    off.prepare(kRate);
    check(std::abs(cents(peak(render(off, x), sharp), sharp)) < 1, "Correction off: the input's pitch");
    // A plant: a correction of the wrong sign lands 47 cents from A4.
    check(std::abs(cents(440 * std::exp2(2 * c / 1200), 440)) > 40, "plant: a sign error is far outside the tolerance");
}

void smoothAndHold() {
    S slow;
    slow.set(S::Smooth, 500);
    slow.prepare(kRate);
    const auto x = sine(446, 24000);
    (void)render(slow, x);
    const double early = slow.correction(), target = -cents(446, 440);
    check(early < 0 && early > target * 0.9, "Smooth 500 ms: half a second in, the correction is still on its way (" + std::to_string(early) + ")");
    (void)render(slow, sine(446, 144000));
    check(std::abs(slow.correction() - target) < 0.05, "and it arrives (" + std::to_string(slow.correction()) + " cents)");
    // A held correction through noise.
    S hold;
    hold.prepare(kRate);
    (void)render(hold, sine(446, 48000));
    const double before = hold.correction();
    std::vector<float> noise(24000);
    std::uint32_t seed = 99;
    for (auto& v : noise) {
        seed = seed * 1664525u + 1013904223u;
        v = static_cast<float>(static_cast<double>(seed >> 8) / 16777216.0 - 0.5);
    }
    (void)render(hold, noise);
    // The windows that straddle the sine's end still read it (to within a
    // cent); none of the noise's moves it toward 0 or anywhere else.
    check(std::abs(hold.correction() - before) < 1 && std::abs(before - target) < 0.05,
          "an unclear input holds the last correction (" + std::to_string(before) + " then " + std::to_string(hold.correction()) + ")");
}

void robustness() {
    S s;
    std::vector<float> bad(4096, std::numeric_limits<float>::quiet_NaN()), o(4096), p(4096);
    const auto x = sine(300, 4096);
    counting = true;
    s.process(x.data(), x.data(), o.data(), p.data(), x.size());
    s.process(bad.data(), bad.data(), o.data(), p.data(), bad.size());
    s.set(S::Scale, S::Minor);
    s.set(S::Shift, -7);
    s.set(S::Smooth, 20);
    s.process(x.data(), x.data(), o.data(), p.data(), x.size());
    counting = false;
    check(allocations == 0, "processing and moved controls allocate nothing");
    check(std::all_of(o.begin(), o.end(), [](float v) { return std::isfinite(v); }), "a NaN input leaves the output finite");
    std::vector<float> first;
    bool identical = true;
    const auto y = sine(446, 30000);
    for (std::size_t block : {1u, 64u, 333u, 4096u}) {
        S b;
        b.prepare(kRate);
        const auto out = render(b, y, block);
        if (first.empty()) {
            first = out;
        } else {
            identical = identical && out == first;
        }
    }
    check(identical, "every block size gives the same samples");
}

}  // namespace

int main() {
    scales();
    correction();
    smoothAndHold();
    robustness();
    std::printf("%s -- %d checks, %d failure(s)\n", failures ? "FAILED" : "PASS", checks, failures);
    return failures ? 1 : 0;
}
