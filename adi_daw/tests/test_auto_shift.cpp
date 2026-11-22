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
    // Corrected, then Shift +12: 12 semitones less the correction.
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
    // A held tone half way between two counts of periods in 25 ms (which
    // once flipped a delay-line shifter's window) comes out without jumps:
    // its largest step is a sine's own at the shifted pitch, within half
    // again.
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
    // A large Shift comes out clean too, and at the input's level (the
    // review of e387e86e: its 880 Hz was 40 dB down and still the peak).
    S up;
    up.set(S::Shift, 12);
    up.prepare(kRate);
    const auto y880 = render(up, sine(446, 144000, 0.4));
    const double at880 = peak(y880, 880), level880 = 20 * std::log10(magnitude(y880, at880) * 4 / 48000 / 0.4);
    check(std::abs(cents(at880, 880)) < 1 && std::abs(level880) < 0.5,
          "446 Hz corrected, then Shift +12: 880 Hz (" + std::to_string(at880) + ") at its level (" + std::to_string(level880) + " dB)");
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

/// The review of 53080074: a delay-line shifter driven by a moving pitch
/// jumped its read taps whenever the pitch moved, and dropped up to 23 dB
/// when the correction was let go at unison. The review of e387e86e: its
/// overlap-add jumped too once a Shift was set, which the checks at Shift 0
/// could not see. So: at Shifts 0, +3, -3 and +7, a sine with vibrato, a
/// phase-continuous step up and one down, and a two-semitone glide come out
/// without a step past the input's own times the ratio, within half again.
void moving() {
    struct Motion {
        const char* what;
        double (*hz)(double t);
    };
    const Motion motions[] = {
        {"vibrato of +-50 cents at 5.5 Hz", [](double t) { return 220 * std::exp2(0.5 / 12 * std::sin(2 * std::numbers::pi * 5.5 * t)); }},
        {"a step from 220 to 330 Hz", [](double t) { return t < 1.0 ? 220.0 : 330.0; }},
        {"a step from 330 to 220 Hz", [](double t) { return t < 1.0 ? 330.0 : 220.0; }},
        {"a glide of two semitones over 300 ms", [](double t) { return 220 * std::exp2(std::clamp((t - 1.0) / 0.3, 0.0, 1.0) * 2 / 12); }},
    };
    for (const auto& m : motions) {
        for (const double semitones : {0.0, 3.0, -3.0, 7.0}) {
        for (double rate : {44100.0, 48000.0, 192000.0}) {
            const auto n = static_cast<std::size_t>(rate * 2);
            std::vector<float> x(n);
            double phase = 0, own = 0;
            for (std::size_t i = 0; i < n; ++i) {
                x[i] = static_cast<float>(0.4 * std::sin(phase));
                phase += 2 * std::numbers::pi * m.hz(static_cast<double>(i) / rate) / rate;
                if (i > 0) {
                    own = std::max(own, static_cast<double>(std::abs(x[i] - x[i - 1])));
                }
            }
            S s;
            s.set(S::Correction, 0);
            s.set(S::Shift, semitones);
            s.prepare(rate);
            std::vector<float> l(n), r(n);
            for (std::size_t at = 0; at < n; at += 256) {
                const auto k = std::min<std::size_t>(256, n - at);
                s.process(x.data() + at, x.data() + at, l.data() + at, r.data() + at, k);
            }
            double step = 0;
            for (std::size_t i = static_cast<std::size_t>(rate / 2); i < n; ++i) {
                step = std::max(step, static_cast<double>(std::abs(l[i] - l[i - 1])));
            }
            const double allowed = 1.5 * own * std::exp2(semitones / 12);
            check(step <= allowed, std::string(m.what) + ", Shift " + std::to_string(static_cast<int>(semitones)) + ", at " + std::to_string(rate) +
                                       ": no jump (largest step " + std::to_string(step) + ", allowed " + std::to_string(allowed) + ")");
        }
        }
    }
    // A corrected 446 Hz tone with the correction let go at 48 points: the
    // one-period level in the next 60 ms never falls 1.5 dB.
    double worst = 0;
    for (int k = 0; k < 48; ++k) {
        S s;
        s.prepare(kRate);
        const std::size_t at = 96000 + static_cast<std::size_t>(k) * 1000 / 48 * 48;
        const auto x = sine(446, at + 4800, 0.4);
        std::vector<float> l(x.size()), r(x.size());
        s.process(x.data(), x.data(), l.data(), r.data(), at);
        s.set(S::Correction, 0);
        s.process(x.data() + at, x.data() + at, l.data() + at, r.data() + at, x.size() - at);
        const auto period = static_cast<std::size_t>(kRate / 446) + 1;
        for (std::size_t i = at; i + period < at + 2880; i += 16) {
            double e = 0;
            for (std::size_t j = i; j < i + period; ++j) {
                e += static_cast<double>(l[j]) * l[j];
            }
            worst = std::min(worst, 20 * std::log10(std::sqrt(e / static_cast<double>(period)) / (0.4 / std::sqrt(2.0)) + 1e-300));
        }
    }
    check(worst > -1.5, "the correction let go at 48 points: the level never falls 1.5 dB (" + std::to_string(worst) + " dB)");
    // Dry/Wet 0: the input, exactly one reported latency late.
    {
        S s;
        s.set(S::DryWet, 0);
        s.prepare(kRate);
        const auto x = sine(446, 24000, 0.4);
        std::vector<float> l(x.size()), r(x.size());
        s.process(x.data(), x.data(), l.data(), r.data(), x.size());
        const auto d = static_cast<std::size_t>(s.latency());
        double off = 0;
        for (std::size_t i = d; i < x.size(); ++i) {
            off = std::max(off, static_cast<double>(std::abs(l[i] - x[i - d])));
        }
        check(d > 0 && off == 0, "Dry/Wet 0: the input, one reported latency late (" + std::to_string(off) + ")");
    }
    S l;
    check(l.latency() == 5371 && static_cast<std::size_t>(l.latency()) == S::latencyFor(kRate, 4096),
          "the latency: half a window and half a hop, a chunk, 1.625 of the longest period read and 8 (" + std::to_string(l.latency()) + ")");
}

/// An amplitude from magnitude(): a Hann-weighted second of 48,000 samples.
static double amplitudeAt(const std::vector<float>& y, double hz) { return magnitude(y, hz) * 4 / 48000; }

/// The review of e387e86e: its overlap-add cancelled a pure tone shifted up
/// (-6 dB at +7, silence at +12) and returned it unshifted an octave or more
/// down; its wet signal sat up to half a period off the dry one; and each
/// reading was applied 25 to 50 ms before the audio it was read from.
/// The review of b50591ad (three skeptics each): every period the detector
/// reads must splice whole (40 Hz came out at 70 Hz, a noisy 220 Hz read at
/// its seventh subharmonic lost its shift); a return to Shift 0 left the tap
/// off the centre (Dry/Wet at -131 dB); shifting up aliased; the taps strayed
/// past the stated budget; and the detector's cost grew with the rate.
void reviewedAgain() {
    for (const double hz : {32.0, 40.0, 50.0}) {
        for (const double semitones : {7.0, -7.0}) {
            S s;
            s.set(S::Correction, 0);
            s.set(S::Shift, semitones);
            s.prepare(kRate);
            const auto y = render(s, sine(hz, 192000, 0.4));
            const double want = hz * std::exp2(semitones / 12), level = 20 * std::log10(amplitudeAt(y, want) / 0.4);
            check(std::abs(level) < 0.5, std::to_string(hz) + " Hz at Shift " + std::to_string(static_cast<int>(semitones)) + ": " +
                                             std::to_string(want) + " Hz at " + std::to_string(level) + " dB");
        }
    }
    {
        std::vector<float> x = sine(220, 144000, 0.4);
        std::uint32_t seed = 7;
        for (auto& v : x) {
            double g = 0;
            for (int k = 0; k < 12; ++k) {   // near-Gaussian, sd 0.15
                seed = seed * 1664525u + 1013904223u;
                g += static_cast<double>(seed >> 8) / 16777216.0;
            }
            v += static_cast<float>((g - 6) * 0.15);
        }
        S s;
        s.set(S::Correction, 0);
        s.set(S::Shift, 12);
        s.prepare(kRate);
        const double level = 20 * std::log10(amplitudeAt(render(s, x), 440) / 0.4);
        check(std::abs(level) < 1, "220 Hz in noise at Shift +12: 440 Hz at " + std::to_string(level) + " dB");
    }
    // Shift +7 for a second, then 0: the tap is back at the centre, so at
    // Dry/Wet 100 % the wet signal is the input one latency late, exactly.
    for (const double hz : {220.0, 300.0, 440.0}) {
        S s;
        s.set(S::Correction, 0);
        s.set(S::Shift, 7);
        s.prepare(kRate);
        const auto x = sine(hz, 48000 * 5, 0.4);
        std::vector<float> l(x.size()), r(x.size());
        s.process(x.data(), x.data(), l.data(), r.data(), 48000);
        s.set(S::Shift, 0);
        s.process(x.data() + 48000, x.data() + 48000, l.data() + 48000, r.data() + 48000, x.size() - 48000);
        const auto d = static_cast<std::size_t>(s.latency());
        double off = 0;
        for (std::size_t i = x.size() - 24000; i < x.size(); ++i) {
            off = std::max(off, static_cast<double>(std::abs(l[i] - x[i - d])));
        }
        check(off == 0, std::to_string(hz) + " Hz, Shift +7 then 0: four seconds on, the wet signal is the input one latency late (" + std::to_string(off) + ")");
    }
    // A band-limited sawtooth to 20 kHz at Shift +24: what folds back into the
    // audible band stays 40 dB under the fundamental (19,840 Hz from the 32nd
    // harmonic, 4 kHz from the 50th).
    {
        std::vector<float> saw(144000);
        for (std::size_t i = 0; i < saw.size(); ++i) {
            double v = 0;
            for (int k = 1; 220 * k < 20000; ++k) {
                v += 0.25 / k * std::sin(2 * std::numbers::pi * 220 * k * static_cast<double>(i) / kRate);
            }
            saw[i] = static_cast<float>(v);
        }
        S s;
        s.set(S::Correction, 0);
        s.set(S::Shift, 24);
        s.prepare(kRate);
        const auto y = render(s, saw);
        const double fundamental = amplitudeAt(y, 880), a1 = amplitudeAt(y, 19840), a2 = amplitudeAt(y, 4000);
        check(std::abs(20 * std::log10(fundamental / 0.25)) < 0.5 && a1 < fundamental / 100 && a2 < fundamental / 100,
              "a sawtooth at Shift +24: 880 Hz at " + std::to_string(20 * std::log10(fundamental / 0.25)) + " dB, aliases at " +
                  std::to_string(20 * std::log10(a1 / fundamental + 1e-12)) + " and " + std::to_string(20 * std::log10(a2 / fundamental + 1e-12)) + " dB");
    }
    // The taps stay within the stated stray, whatever the Shift does: 31 Hz
    // and a flat A#1 corrected, the Shift jumping across its range every 700
    // to 2,900 samples for 20 s.
    for (const double hz : {31.0, 58.27 * std::exp2(-0.49 / 12)}) {
        S s;
        s.set(S::Correction, hz > 40 ? 1 : 0);
        s.prepare(kRate);
        const auto x = sine(hz, 48000 * 20, 0.4);
        std::vector<float> l(64), r(64);
        std::uint32_t seed = 3;
        std::size_t next = 0;
        for (std::size_t at = 0; at < x.size(); at += 64) {
            if (at >= next) {
                seed = seed * 1664525u + 1013904223u;
                const double pick = static_cast<double>(seed >> 8) / 16777216.0;
                s.set(S::Shift, pick < 0.3 ? -24 : pick < 0.6 ? 24 : std::round(pick * 48 - 24));
                s.set(S::Detune, pick < 0.5 ? 50 : -50);
                next = at + 700 + static_cast<std::size_t>(pick * 2200);
            }
            s.process(x.data() + at, x.data() + at, l.data(), r.data(), 64);
        }
        const double stray = std::ceil(1.625 * std::ceil(kRate / 28.5)) + 8, centre = s.latency();
        check(s.shortestDelay() >= centre - stray && s.longestDelay() <= centre + stray && s.shortestDelay() >= static_cast<double>(s.window()) * 5 / 8,
              std::to_string(hz) + " Hz, the Shift jumping: the taps read from " + std::to_string(s.shortestDelay()) + " to " +
                  std::to_string(s.longestDelay()) + " samples back (allowed " + std::to_string(centre - stray) + " to " + std::to_string(centre + stray) + ")");
    }
    // Above 48 kHz the detector reads the input averaged down: at 192 kHz it
    // runs at 48 kHz, its window 16,384 input samples.
    {
        S s;
        s.prepare(192000);
        check(s.detectorRate() == 48000 && s.window() == 16384, "at 192 kHz the detector runs at " + std::to_string(s.detectorRate()) + " Hz");
    }
}

/// A Hann-windowed amplitude over `n` samples from `from`.
static double shortAmplitude(const std::vector<float>& y, std::size_t from, std::size_t n, double hz) {
    std::complex<double> sum = 0;
    for (std::size_t i = 0; i < n; ++i) {
        const double w = 0.5 - 0.5 * std::cos(2 * std::numbers::pi * static_cast<double>(i) / static_cast<double>(n));
        sum += w * static_cast<double>(y[from + i]) * std::polar(1.0, -2 * std::numbers::pi * hz * static_cast<double>(i) / kRate);
    }
    return std::abs(sum) * 4 / static_cast<double>(n);
}

/// The review of 9aaf8b10 (three skeptics each): a low-pass retuned while
/// it ran burst up to +15 dB when the ratio fell; the copy was filtered for
/// the ratio when written, so a shift up aliased fully for one latency; an
/// in-tune note under Correction never gave a ratio of exactly 1, so the tap
/// stayed off the centre (and the copies flickered); 88.2 kHz was not
/// decimated.
void reviewedThird() {
    // Shift changes that fall: the output never rises past the input's peak.
    struct Change {
        double from, to, then;
    };
    for (const Change c : {Change{24, 2, 2}, Change{19, 0, 12}, Change{24, -5, 12}, Change{12, 2, 2}}) {
        S s;
        s.set(S::Correction, 0);
        s.set(S::Shift, c.from);
        s.prepare(kRate);
        const auto x = sine(200, 48000 * 3, 0.5);
        std::vector<float> l(x.size()), r(x.size());
        double peak = 0;
        for (std::size_t at = 0; at < x.size(); at += 64) {
            if (at == 48000) {
                s.set(S::Shift, c.to);
            }
            if (at == 48000 + 960) {
                s.set(S::Shift, c.then);
            }
            s.process(x.data() + at, x.data() + at, l.data() + at, r.data() + at, 64);
        }
        for (std::size_t i = 48000; i < x.size(); ++i) {
            peak = std::max(peak, static_cast<double>(std::abs(l[i])));
        }
        check(peak <= 0.5 * 1.06, "Shift " + std::to_string(static_cast<int>(c.from)) + " to " + std::to_string(static_cast<int>(c.to)) + " then " +
                                      std::to_string(static_cast<int>(c.then)) + ": no burst (peak " + std::to_string(peak) + ", the input's 0.5)");
    }
    // 200 Hz and 15 kHz, Shift 0 then +12: the 15 kHz partial's alias (18 kHz)
    // is as low just after the change as it is later.
    {
        std::vector<float> x(48000 * 4);
        for (std::size_t i = 0; i < x.size(); ++i) {
            const double t = static_cast<double>(i) / kRate;
            x[i] = static_cast<float>(0.5 * std::sin(2 * std::numbers::pi * 200 * t) + 0.3 * std::sin(2 * std::numbers::pi * 15000 * t));
        }
        S s;
        s.set(S::Correction, 0);
        s.prepare(kRate);
        std::vector<float> l(x.size()), r(x.size());
        s.process(x.data(), x.data(), l.data(), r.data(), 96000);
        s.set(S::Shift, 12);
        s.process(x.data() + 96000, x.data() + 96000, l.data() + 96000, r.data() + 96000, x.size() - 96000);
        const double soon = shortAmplitude(l, 96000 + 256, 4096, 18000), later = shortAmplitude(l, x.size() - 8192, 4096, 18000);
        check(soon < 0.3 / 100 && later < 0.3 / 100, "Shift 0 then +12 on a 15 kHz partial: its alias at 18 kHz is " +
                                                        std::to_string(20 * std::log10(soon / 0.3 + 1e-12)) + " dB just after, " +
                                                        std::to_string(20 * std::log10(later / 0.3 + 1e-12)) + " dB later");
    }
    // A3 30 cents flat for a second, then A3 in tune for four, corrected, at
    // Dry/Wet 50 %: the last second is the input's level. (The copies are
    // not reached at these ratios; reviewedFourth() drives them.)
    for (const std::size_t extra : {0u, 3313u, 6626u}) {
        S s;
        s.set(S::DryWet, 50);
        s.prepare(kRate);
        const std::size_t first = 48000 + extra, n = first + 48000 * 4;
        std::vector<float> x(n);
        double phase = 0;
        for (std::size_t i = 0; i < n; ++i) {
            x[i] = static_cast<float>(0.4 * std::sin(phase));
            phase += 2 * std::numbers::pi * (i < first ? 220 * std::exp2(-0.3 / 12) : 220.0) / kRate;
        }
        std::vector<float> l(n), r(n);
        s.process(x.data(), x.data(), l.data(), r.data(), first + 48000);
        s.process(x.data() + first + 48000, x.data() + first + 48000, l.data() + first + 48000, r.data() + first + 48000, n - first - 48000);
        double e = 0;
        for (std::size_t i = n - 48000; i < n; ++i) {
            e += static_cast<double>(l[i]) * l[i];
        }
        const double level = 20 * std::log10(std::sqrt(e / 48000) / (0.4 / std::sqrt(2.0)));
        check(std::abs(level) < 0.05,
              "A3 flat, then in tune, corrected, Dry/Wet 50 % (" + std::to_string(extra) + "): the input's level (" + std::to_string(level) + " dB)");
    }
    {
        S s;
        s.prepare(88200);
        check(s.detectorRate() == 44100, "at 88.2 kHz the detector runs at " + std::to_string(s.detectorRate()) + " Hz");
    }
}

/// The review of c95041a9 (three skeptics each): the glide back to the
/// centre at 0.5 % played an in-tune note after a corrected one up to 8.6
/// cents off; and no check reached a ratio where the copies change, so the
/// band hysteresis was untested.
void reviewedFourth() {
    // A note 30 cents off for a second, then in tune, corrected, at Dry/Wet
    // 100 %: from 100 ms after the in-tune audio comes out, no 50 ms block
    // (100 ms at 82.4 Hz) is 2 cents off.
    struct Note {
        double hz, off;
    };
    for (const Note n : {Note{220, -30}, Note{220, 30}, Note{82.41, -30}}) {
        S s;
        s.prepare(kRate);
        const std::size_t total = 48000 * 5;
        std::vector<float> x(total);
        double phase = 0;
        for (std::size_t i = 0; i < total; ++i) {
            x[i] = static_cast<float>(0.4 * std::sin(phase));
            phase += 2 * std::numbers::pi * (i < 48000 ? n.hz * std::exp2(n.off / 1200) : n.hz) / kRate;
        }
        const auto y = render(s, x, 256);
        const std::size_t block = n.hz < 100 ? 4800 : 2400;
        double worst = 0;
        for (std::size_t from = 48000 + static_cast<std::size_t>(s.latency()) + 4800; from + block < total; from += block) {
            int crossings = 0;
            double first = -1, last = -1;
            for (std::size_t i = from + 1; i < from + block; ++i) {
                if (y[i - 1] < 0 && y[i] >= 0) {
                    const double t = static_cast<double>(i - 1) + static_cast<double>(y[i - 1]) / static_cast<double>(y[i - 1] - y[i]);
                    if (first < 0) {
                        first = t;
                    }
                    last = t;
                    ++crossings;
                }
            }
            if (crossings > 2) {
                worst = std::max(worst, std::abs(cents((crossings - 1) * kRate / (last - first), n.hz)));
            }
        }
        check(worst < 2, std::to_string(n.hz) + " Hz " + std::to_string(static_cast<int>(n.off)) +
                             " cents, then in tune, corrected: no block more than 2 cents off (worst " + std::to_string(worst) + ")");
    }
    // Shift +4 (a ratio exactly at the first band's top) with Detune moving
    // between +1 and -1 cent every 64 samples for 3 s: the tap moves between
    // copies once, not on every move.
    {
        S s;
        s.set(S::Correction, 0);
        s.set(S::Shift, 4);
        s.prepare(kRate);
        const auto x = sine(220, 48000 * 3, 0.4);
        std::vector<float> l(64), r(64);
        for (std::size_t at = 0; at < x.size(); at += 64) {
            s.set(S::Detune, (at / 64) % 2 == 0 ? 1 : -1);
            s.process(x.data() + at, x.data() + at, l.data(), r.data(), 64);
        }
        check(s.bandChanges() <= 2, "a ratio hovering on a band's top: " + std::to_string(s.bandChanges()) + " moves between copies in 3 s");
    }
}

void shifted() {
    // A 220 Hz sine at every Shift comes out at 220 x the ratio, at its own
    // level within 0.5 dB, and nothing is left at 220 Hz.
    for (const double semitones : {-24.0, -19.0, -12.0, -7.0, -5.0, 3.0, 5.0, 7.0, 12.0, 19.0, 24.0}) {
        S s;
        s.set(S::Correction, 0);
        s.set(S::Shift, semitones);
        s.prepare(kRate);
        const auto y = render(s, sine(220, 144000, 0.4));
        const double want = 220 * std::exp2(semitones / 12);
        const double level = 20 * std::log10(amplitudeAt(y, want) / 0.4), left = amplitudeAt(y, 220);
        check(std::abs(level) < 0.5 && left < 0.004, "220 Hz at Shift " + std::to_string(static_cast<int>(semitones)) + ": " + std::to_string(want) + " Hz at " +
                                                         std::to_string(level) + " dB, " + std::to_string(left) + " left at 220 Hz");
    }
    // A sawtooth an octave down: every harmonic moves, each at its level.
    {
        std::vector<float> saw(144000);
        for (std::size_t i = 0; i < saw.size(); ++i) {
            double v = 0;
            for (int k = 1; k <= 20; ++k) {
                v += 0.4 / k * std::sin(2 * std::numbers::pi * 220 * k * static_cast<double>(i) / kRate);
            }
            saw[i] = static_cast<float>(v);
        }
        S s;
        s.set(S::Correction, 0);
        s.set(S::Shift, -12);
        s.prepare(kRate);
        const auto y = render(s, saw);
        const double a1 = amplitudeAt(y, 110), a2 = amplitudeAt(y, 220), a3 = amplitudeAt(y, 330);
        check(std::abs(20 * std::log10(a1 / 0.4)) < 0.5 && std::abs(20 * std::log10(a2 / 0.2)) < 0.5 && std::abs(20 * std::log10(a3 / (0.4 / 3))) < 0.5,
              "a 220 Hz sawtooth at Shift -12: 0.4 at 110, 0.2 at 220, 0.133 at 330 Hz (" + std::to_string(a1) + ", " + std::to_string(a2) + ", " +
                  std::to_string(a3) + ")");
    }
    // Shift 0, Correction off, Dry/Wet 50 %: wet and dry are the same signal,
    // at every pitch (the overlap-add's left 100 Hz at -125 dB).
    double worstMix = 0;
    for (const double hz : {100.0, 160.0, 250.0, 290.0, 330.0, 440.0}) {
        S s;
        s.set(S::Correction, 0);
        s.set(S::DryWet, 50);
        s.prepare(kRate);
        const auto y = render(s, sine(hz, 96000, 0.4));
        double e = 0;
        for (std::size_t i = 48000; i < y.size(); ++i) {
            e += static_cast<double>(y[i]) * y[i];
        }
        worstMix = std::max(worstMix, std::abs(20 * std::log10(std::sqrt(e / 48000) / (0.4 / std::sqrt(2.0)))));
    }
    check(worstMix < 0.05, "Shift 0, Dry/Wet 50 %: the input's level at 100 to 440 Hz (worst " + std::to_string(worstMix) + " dB off)");
    {
        S s;
        s.set(S::Correction, 0);
        s.prepare(kRate);
        const auto x = sine(290, 48000, 0.4);
        const auto y = render(s, x);
        const auto d = static_cast<std::size_t>(s.latency());
        double off = 0;
        for (std::size_t i = d; i < x.size(); ++i) {
            off = std::max(off, static_cast<double>(std::abs(y[i] - x[i - d])));
        }
        check(off == 0, "Shift 0, Dry/Wet 100 %: the wet signal is the input one reported latency late, exactly (" + std::to_string(off) + ")");
    }
    // A step from 446 to 330 Hz at 1 s, corrected (A4, then E4 less 2
    // cents): the 446 Hz part comes out at 440 Hz over the 800 samples ending
    // 700 before the step arrives at the output, the 330 Hz part at 329.63
    // over 800 from 2,600 after. A reading applied as soon as it is read puts
    // E4's correction on the 446 Hz audio's last 30 ms (445 Hz, measured).
    {
        std::vector<float> x(144000);
        double phase = 0;
        for (std::size_t i = 0; i < x.size(); ++i) {
            x[i] = static_cast<float>(0.4 * std::sin(phase));
            phase += 2 * std::numbers::pi * (i < 48000 ? 446.0 : 330.0) / kRate;
        }
        S s;
        s.prepare(kRate);
        const auto y = render(s, x);
        const std::size_t arrives = 48000 + static_cast<std::size_t>(s.latency());
        const auto local = [&](std::size_t from, double guess) {
            const auto mag = [&](double f) {
                std::complex<double> sum = 0;
                for (std::size_t i = 0; i < 800; ++i) {
                    const double w = 0.5 - 0.5 * std::cos(2 * std::numbers::pi * static_cast<double>(i) / 800.0);
                    sum += w * static_cast<double>(y[from + i]) * std::polar(1.0, -2 * std::numbers::pi * f * static_cast<double>(i) / kRate);
                }
                return std::abs(sum);
            };
            double a = guess - 20, b = guess + 20;
            const double g = (std::sqrt(5.0) - 1) / 2;
            while (b - a > 0.01) {
                const double c = b - g * (b - a), d = a + g * (b - a);
                if (mag(c) > mag(d)) {
                    b = d;
                } else {
                    a = c;
                }
            }
            return 0.5 * (a + b);
        };
        const double before = local(arrives - 1500, 440), after = local(arrives + 2600, 329.63);
        check(std::abs(before - 440) < 1 && std::abs(after - 329.63) < 1,
              "446 then 330 Hz, corrected: 440 Hz until the step arrives, 329.63 after (" + std::to_string(before) + ", " + std::to_string(after) + ")");
    }
    // A rate past any host's: prepare() returns, and the device is inert.
    for (const double rate : {1e7, 1e20, 1e300}) {
        S s;
        s.prepare(rate);
        const auto x = sine(300, 512, 0.4);
        std::vector<float> l(512), r(512);
        s.process(x.data(), x.data(), l.data(), r.data(), x.size());
        check(s.latency() == 0 && l == x, "prepare(" + std::to_string(rate).substr(0, 4) + "...) returns; inert, its input out");
    }
}

int main() {
    reviewedFourth();
    reviewedThird();
    reviewedAgain();
    shifted();
    moving();
    glides();
    reviewed();
    scales();
    correction();
    smoothAndHold();
    robustness();
    std::printf("%s -- %d checks, %d failure(s)\n", failures ? "FAILED" : "PASS", checks, failures);
    return failures ? 1 : 0;
}
