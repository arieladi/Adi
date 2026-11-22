// SPDX-License-Identifier: GPL-3.0-or-later
//
// Live 12's Tuner, the core, and the pitch detector Auto Shift will share
// (DEV-22). The reference numbers are the sines' own frequencies: a sine from
// 40 Hz to 4 kHz, at 44.1, 48 and 96 kHz, reads within 1 cent of it, from one
// window and through the Tuner's published reading. A rich tone reads its
// fundamental, not an octave; silence and noise read nothing; the signal
// passes untouched; processing allocates nothing. Parity with Live waits on
// a reading.
#include "adi/dsp/pitch_detector.hpp"
#include "adi/dsp/tuner.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <new>
#include <numbers>
#include <string>
#include <vector>

namespace {
bool counting = false, failing = false;
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
    if (failing) {
        throw std::bad_alloc();
    }
    if (void* p = std::malloc(n ? n : 1)) {
        return p;
    }
    throw std::bad_alloc();
}
void operator delete(void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }

namespace {
using adi::dsp::PitchDetector;
using adi::dsp::Tuner;

double cents(double got, double want) { return 1200 * std::log2(got / want); }

std::vector<float> sine(double hz, double rate, std::size_t frames, double amplitude = 0.5, double phase = 0.3) {
    std::vector<float> x(frames);
    for (std::size_t i = 0; i < frames; ++i) {
        x[i] = static_cast<float>(amplitude * std::sin(2 * std::numbers::pi * hz * static_cast<double>(i) / rate + phase));
    }
    return x;
}

/// Sines from 40 Hz to 4 kHz: 36 steps a decade, each detuned off any round
/// period, and the ends exactly.
std::vector<double> frequencies() {
    std::vector<double> f;
    for (int i = 0; i <= 72; ++i) {
        f.push_back(40 * std::pow(10.0, i / 36.0) * (1 + 0.0037 * std::sin(1.7 * i)));
    }
    f.push_back(40);
    f.push_back(4000);
    return f;
}

void oneWindow() {
    for (double rate : {44100.0, 48000.0, 96000.0}) {
        PitchDetector d;
        d.prepare(rate);
        double worst = 0, at = 0;
        bool all = true;
        for (double hz : frequencies()) {
            const auto x = sine(hz, rate, d.window());
            double got = 0, clarity = 0;
            if (!d.analyse(x.data(), got, clarity)) {
                all = false;
                std::printf("  no reading at %.3f Hz, %.0f kHz\n", hz, rate / 1000);
                continue;
            }
            const double e = std::abs(cents(got, hz));
            if (e > worst) {
                worst = e;
                at = hz;
            }
        }
        check(all, "every sine from 40 Hz to 4 kHz gives a reading at " + std::to_string(rate));
        check(worst < 1, "one window: within 1 cent from 40 Hz to 4 kHz at " + std::to_string(rate) + " Hz (worst " +
                             std::to_string(worst) + " cents, at " + std::to_string(at) + " Hz)");
        // The design's own claim, past the 1 cent asked: the refinement at
        // the period's highest multiple holds every sine within 0.05 cents (a
        // parabola at the period alone is off by up to 0.7 near 4 kHz).
        check(worst < 0.05, "one window: within 0.05 cents at " + std::to_string(rate) + " Hz");
        std::printf("  %.1f kHz: worst %.4f cents at %.2f Hz, window %zu\n", rate / 1000, worst, at, d.window());
    }
    // A quiet sine (-40 dBFS) and one with a DC offset read as well.
    PitchDetector d;
    for (const auto& [amp, dc, what] : {std::tuple{0.01, 0.0, "a -40 dBFS sine"}, std::tuple{0.4, 0.2, "a sine on a DC offset"}}) {
        auto x = sine(233.08, 48000, d.window(), amp);
        for (auto& v : x) {
            v += static_cast<float>(dc);
        }
        double got = 0, clarity = 0;
        const bool ok = d.analyse(x.data(), got, clarity);
        check(ok && std::abs(cents(got, 233.08)) < 1, std::string(what) + " reads within 1 cent");
    }
}

void richAndNot() {
    PitchDetector d;
    const std::size_t n = d.window();
    // A band-limited sawtooth at 110 Hz, and a tone whose fundamental is
    // half the level of its second harmonic: both read the fundamental.
    std::vector<float> saw(n, 0.f), weak(n, 0.f);
    for (std::size_t i = 0; i < n; ++i) {
        const double t = static_cast<double>(i) / 48000;
        double s = 0;
        for (int k = 1; 110.0 * k < 20000; ++k) {
            s += std::sin(2 * std::numbers::pi * 110 * k * t) / k;
        }
        saw[i] = static_cast<float>(0.3 * s);
        weak[i] = static_cast<float>(0.2 * std::sin(2 * std::numbers::pi * 196 * t) + 0.4 * std::sin(2 * std::numbers::pi * 392 * t + 1));
    }
    double hz = 0, clarity = 0;
    check(d.analyse(saw.data(), hz, clarity) && std::abs(cents(hz, 110)) < 1, "a sawtooth reads its fundamental (" + std::to_string(hz) + ")");
    check(d.analyse(weak.data(), hz, clarity) && std::abs(cents(hz, 196)) < 1,
          "a weak fundamental under its octave still reads the fundamental (" + std::to_string(hz) + ")");
    // Silence and white noise read nothing.
    std::vector<float> quiet(n, 0.f), noise(n);
    std::uint64_t s = 12345;
    for (auto& v : noise) {
        s = s * 6364136223846793005ULL + 1442695040888963407ULL;
        v = static_cast<float>(static_cast<double>(s >> 11) / 9007199254740992.0 - 0.5);
    }
    check(!d.analyse(quiet.data(), hz, clarity), "silence reads nothing");
    check(!d.analyse(noise.data(), hz, clarity) || clarity < Tuner::kClarity, "white noise reads nothing a tuner would show");
    // A plant: a sine a quarter tone off is not within 1 cent of the true one.
    const auto off = sine(440 * std::pow(2.0, 0.5 / 12), 48000, n);
    check(d.analyse(off.data(), hz, clarity) && std::abs(cents(hz, 440)) > 49, "plant: a quarter tone off reads as 50 cents off");
}

void tuner() {
    // Through the Tuner, in odd blocks: the reading, the note and the cents.
    for (double rate : {44100.0, 48000.0, 96000.0}) {
        Tuner t;
        t.prepare(rate);
        double worst = 0;
        bool all = true;
        for (double hz : {40.0, 61.735, 82.407, 146.83, 440.0, 1174.66, 2793.83, 4000.0}) {
            t.prepare(rate);
            const auto x = sine(hz, rate, static_cast<std::size_t>(rate / 2));
            std::vector<float> l(x.size()), r(x.size());
            for (std::size_t at = 0; at < x.size(); at += 333) {
                const auto n = std::min<std::size_t>(333, x.size() - at);
                t.process(x.data() + at, x.data() + at, l.data() + at, r.data() + at, n);
            }
            const auto got = t.reading();
            all = all && got.valid;
            worst = std::max(worst, got.valid ? std::abs(cents(got.hz, hz)) : 99.0);
        }
        check(all && worst < 1, "the Tuner's reading within 1 cent, 40 Hz to 4 kHz, at " + std::to_string(rate) + " (worst " +
                                    std::to_string(worst) + ")");
    }
    Tuner t;
    const auto a = sine(442, 48000, 24000);
    std::vector<float> l(a.size()), r(a.size());
    t.process(a.data(), a.data(), l.data(), r.data(), a.size());
    auto got = t.reading();
    check(got.valid && got.nearest == 69 && std::abs(got.cents - 1200 * std::log2(442.0 / 440)) < 1,
          "442 Hz against A4 = 440 is A4, 7.85 cents sharp (" + std::to_string(got.cents) + ")");
    t.set(Tuner::Reference, 442);
    got = t.reading();
    check(got.valid && got.nearest == 69 && std::abs(got.cents) < 1, "against A4 = 442 it is in tune");
    t.set(Tuner::Reference, 1000);
    check(t.value(Tuner::Reference) == 480, "Reference is clamped to its range");
    check(l == a && r == a, "the signal passes untouched");
    // A reading is held through silence.
    std::vector<float> quiet(9600, 0.f), q2(9600);
    t.process(quiet.data(), quiet.data(), q2.data(), q2.data(), quiet.size());
    check(t.reading().valid, "a reading is held after the note stops");
    // No allocation, non-finite input, aliased planes.
    std::vector<float> bad(4096, std::numeric_limits<float>::quiet_NaN()), o(4096), p(4096);
    counting = true;
    t.process(a.data(), a.data(), l.data(), r.data(), a.size());
    t.process(bad.data(), bad.data(), o.data(), p.data(), bad.size());
    t.set(Tuner::Reference, 440);
    t.set(Tuner::Display, 2);
    (void)t.reading();
    counting = false;
    check(allocations == 0, "processing, readings and moved controls allocate nothing");
    Tuner z;
    std::vector<float> left(4800, 0.25f), shared(4800, -0.5f), rightOut(4800);
    z.process(left.data(), shared.data(), shared.data(), rightOut.data(), 4800);
    check(shared.front() == 0.25f && rightOut.back() == -0.5f, "an output plane that is the other channel's input is read first");
}

}  // namespace

/// A DC offset, alone or under a low noise floor, is not a pitch (the
/// review of eb460dc3: the float autocorrelation's round-off against the
/// exact energies read as a perfect dip, and the Tuner held a false note).
void offsets() {
    int falseReadings = 0, cases = 0;
    for (double rate : {44100.0, 48000.0, 96000.0}) {
        for (double dc : {0.5, 0.2, 0.05, 0.01, -0.3, 0.001}) {
            for (double width : {0.0, 1e-6, 1e-5, 1e-4}) {
                Tuner t;
                t.prepare(rate);
                std::vector<float> x(static_cast<std::size_t>(rate));
                std::uint32_t s = 777;
                for (auto& v : x) {
                    s = s * 1664525u + 1013904223u;
                    v = static_cast<float>(dc + width * (static_cast<double>(s >> 8) / 16777216.0 - 0.5));
                }
                std::vector<float> l(x.size()), r(x.size());
                t.process(x.data(), x.data(), l.data(), r.data(), x.size());
                ++cases;
                if (t.reading().valid) {
                    ++falseReadings;
                    std::printf("  a reading of %.1f Hz from DC %.3f, noise %.0e, at %.0f\n", t.reading().hz, dc, width, rate);
                }
            }
        }
    }
    check(falseReadings == 0, "a DC offset with or without a low noise floor reads nothing (" + std::to_string(falseReadings) + " of " +
                                  std::to_string(cases) + " did)");
}

/// prepare() never throws, even when memory cannot be had (it runs inside a
/// Pd external's DSP callback); the detector is then inert, and the next
/// prepare that can allocate restores it.
void preparing() {
    PitchDetector d;
    bool threw = false;
    failing = true;
    try {
        d.prepare(96000);
    } catch (...) {
        threw = true;
    }
    failing = false;
    const auto x = sine(440, 48000, 8192);
    double hz = 0, clarity = 0;
    check(!threw && d.window() == 0 && !d.analyse(x.data(), hz, clarity), "prepare under a failed allocation does not throw; the detector is inert");
    d.push(x.data(), x.size());
    check(!d.estimate(hz, clarity), "an inert detector takes samples and reads nothing");
    d.prepare(48000);
    check(d.analyse(x.data(), hz, clarity) && std::abs(cents(hz, 440)) < 1, "the next prepare restores it");
    Tuner t;
    failing = true;
    try {
        t.prepare(44100);
    } catch (...) {
        threw = true;
    }
    failing = false;
    std::vector<float> l(4096), r(4096);
    t.process(x.data(), x.data(), l.data(), r.data(), 4096);
    check(!threw && std::equal(l.begin(), l.end(), x.begin()), "a Tuner whose prepare could not allocate still passes the signal");
}

/// The review of the Auto Shift core e387e86e: a window of silence then a
/// tone drove the fine-period walk toward the shortest lags, where it ran
/// some 37,000 passes (0.4 s at 48 kHz) on the audio thread before giving
/// up. And a rate past any host's hung prepare().
void bounded() {
    double worst = 0;
    for (const double rate : {44100.0, 48000.0, 96000.0}) {
        PitchDetector d;
        d.prepare(rate);
        const std::size_t n = d.window();
        for (const double hz : {55.0, 110.0, 220.0, 330.0}) {
            for (std::size_t onset = n / 2; onset < n; onset += n / 16) {
                std::vector<float> x(n, 0.f);
                for (std::size_t i = onset; i < n; ++i) {
                    x[i] = static_cast<float>(0.3 * std::sin(2 * std::numbers::pi * hz * static_cast<double>(i - onset) / rate));
                }
                double got = 0, clarity = 0;
                (void)d.analyse(x.data(), got, clarity);
                worst = std::max(worst, static_cast<double>(d.lastWork()));
            }
        }
    }
    const double bound = PitchDetector::kPasses * (2.0 * PitchDetector::kSteps + 3);
    check(worst <= bound, "silence then a tone: the fine search is bounded (" + std::to_string(static_cast<int>(worst)) + " differences, at most " +
                              std::to_string(static_cast<int>(bound)) + ")");
    // The bound does not cost a clean reading its precision.
    PitchDetector d;
    const auto x = sine(440.0, 48000.0, d.window(), 0.3);
    double hz = 0, clarity = 0;
    check(d.analyse(x.data(), hz, clarity) && std::abs(cents(hz, 440)) < 0.1 && d.lastWork() < 200,
          "a clean 440 Hz still reads within 0.1 cent (" + std::to_string(cents(hz, 440)) + "), in " + std::to_string(d.lastWork()) + " differences");
    for (const double rate : {1e20, 1e300}) {
        PitchDetector big;
        big.prepare(rate);
        double h = 0, c = 0;
        big.push(x.data(), x.size());
        check(big.window() == 0 && !big.estimate(h, c), "prepare(" + std::to_string(rate).substr(0, 6) + "...) returns, inert");
    }
}

int main() {
    bounded();
    offsets();
    preparing();
    oneWindow();
    richAndNot();
    tuner();
    std::printf("%s -- %d checks, %d failure(s)\n", failures ? "FAILED" : "PASS", checks, failures);
    return failures ? 1 : 0;
}
