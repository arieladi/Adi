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
    const auto finite = [&] {
        return std::all_of(o.begin(), o.end(), [](float v) { return std::isfinite(v); }) &&
               std::all_of(p.begin(), p.end(), [](float v) { return std::isfinite(v); });
    };
    counting = true;
    s.process(x.data(), x.data(), o.data(), p.data(), x.size());
    s.process(bad.data(), bad.data(), o.data(), p.data(), bad.size());
    const bool nanBlock = finite();   // checked as it comes out, both channels
    s.set(S::Scale, S::Minor);
    s.set(S::Shift, -7);
    s.set(S::Smooth, 20);
    s.process(x.data(), x.data(), o.data(), p.data(), x.size());
    const bool after = finite();
    counting = false;
    check(allocations == 0, "processing and moved controls allocate nothing");
    check(nanBlock && after, "a NaN input's own block, and the one after it, come out finite");
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

/// The second review's findings, each a check that fails on the old core.
void reviewed() {
    // A corrected tone keeps its level through every crossfade: the 20 ms
    // RMS after the correction has settled stays within 1 dB of the input's,
    // at pitches whose period put the fixed 50 ms window's taps in antiphase
    // (420 and 460 Hz, C major's 300 Hz: dips of 40 dB) and at benign ones.
    struct Tone {
        double hz;
        int scale;
    };
    for (const Tone t : {Tone{460, S::Chromatic}, Tone{420, S::Chromatic}, Tone{300, S::Major}, Tone{446, S::Chromatic}, Tone{280, S::Major}}) {
        S s;
        s.set(S::Scale, t.scale);
        s.prepare(kRate);
        const auto x = sine(t.hz, 48000 * 8, 0.4);
        const auto y = render(s, x, 256);
        double lowest = 1e9;
        for (std::size_t at = 96000; at + 960 <= y.size(); at += 480) {
            double e = 0;
            for (std::size_t i = 0; i < 960; ++i) {
                e += static_cast<double>(y[at + i]) * y[at + i];
            }
            lowest = std::min(lowest, std::sqrt(e / 960));
        }
        const double dip = 20 * std::log10(lowest / (0.4 / std::sqrt(2.0)));
        check(dip > -1, std::to_string(t.hz) + " Hz corrected: its level holds through the crossfades (lowest " + std::to_string(dip) + " dB)");
    }
    double n460 = 0, n30 = 0;
    check(std::abs(S::windowFor(460, n460) - 2 * 12 * 1000 / 460.0) < 1e-9 && S::windowFor(30, n30) > 60, "the window spans whole periods");
    // Near half way between two counts the count holds: 220 Hz is 5.5
    // periods in 25 ms, and a reading on either side keeps the first count.
    double n = 0;
    (void)S::windowFor(220.01, n);
    const double first = n;
    (void)S::windowFor(219.99, n);
    (void)S::windowFor(220.02, n);
    check(n == first, "near half way the count of periods holds");
    // So a held tone there comes out without jumps: its largest step is a
    // sine's own at the shifted pitch, within half again.
    S held;
    held.set(S::Shift, 7);
    held.prepare(kRate);
    const auto steady = render(held, sine(220, 96000, 0.3));
    double step = 0;
    for (std::size_t i = 48000; i < steady.size(); ++i) {
        step = std::max(step, static_cast<double>(std::abs(steady[i] - steady[i - 1])));
    }
    const double own = 0.3 * 2 * std::numbers::pi * 220 * std::exp2(7 / 12.0) / kRate;
    check(step < 1.5 * own, "a held 220 Hz tone shifted +7 has no jumps (largest step " + std::to_string(step) + ", a sine's " + std::to_string(own) + ")");
    // Detune with a whole-tone correction is not clamped: C major, Detune
    // +50, MIDI 61.1 (278.79 Hz) goes to D (+90 cents) and 50 cents on, 1.40
    // semitones in all, 302.27 Hz.
    S d;
    d.set(S::Scale, S::Major);
    d.set(S::Detune, 50);
    d.prepare(kRate);
    const double in = 440 * std::exp2((61.1 - 69) / 12), want = 440 * std::exp2((62.5 - 69) / 12);
    const auto y = render(d, sine(in, 144000, 0.4));
    check(std::abs(d.shift() - 1.4) < 1e-3, "C major, Detune +50: a shift of 1.40 semitones, unclamped (" + std::to_string(d.shift()) + ")");
    const double got = peak(y, want);
    check(std::abs(cents(got, want)) < 1, "and it comes out at 302.27 Hz (" + std::to_string(got) + ")");
    // With the window following the tone, a large Shift comes out clean too.
    S up;
    up.set(S::Shift, 12);
    up.prepare(kRate);
    const double at880 = peak(render(up, sine(446, 144000, 0.4)), 880);
    check(std::abs(cents(at880, 880)) < 1, "446 Hz corrected, then Shift +12: 880 Hz (" + std::to_string(at880) + ")");
}

/// Dry/Wet moved at a tone's peak glides: the largest step while it moves
/// stays within the signal's own, before or after.
void glides() {
    const auto x = sine(220, 96000, 0.3);
    // The move lands where the dry and the shifted signals differ most (from
    // a fully wet run), so a step would show.
    S wetOnly;
    wetOnly.set(S::Shift, 7);
    wetOnly.prepare(kRate);
    const auto w = render(wetOnly, x);
    std::size_t at = 48000;
    for (std::size_t i = 48000; i < 48500; ++i) {
        if (std::abs(w[i] - x[i]) > std::abs(w[at] - x[at])) {
            at = i;
        }
    }
    S s;
    s.set(S::Shift, 7);
    s.set(S::DryWet, 0);
    s.prepare(kRate);
    std::vector<float> l(x.size()), r(x.size());
    s.process(x.data(), x.data(), l.data(), r.data(), at);
    s.set(S::DryWet, 100);
    s.process(x.data() + at, x.data() + at, l.data() + at, r.data() + at, x.size() - at);
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
    check(moving <= 1.5 * settled, "Dry/Wet glides: no step past the signal's own (" + std::to_string(moving) + " against " + std::to_string(settled) + ")");
}

int main() {
    glides();
    reviewed();
    scales();
    correction();
    smoothAndHold();
    robustness();
    std::printf("%s -- %d checks, %d failure(s)\n", failures ? "FAILED" : "PASS", checks, failures);
    return failures ? 1 : 0;
}
