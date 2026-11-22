// SPDX-License-Identifier: GPL-3.0-or-later
//
// Live 12's Grain Delay, the core (DEV-22). The references are the header's
// own laws: with no pitch and no spray, two half-overlapping periodic Hann
// grains sum to exactly 1, so an impulse comes back after exactly the delay,
// alone, and each repeat Feedback times the last; Pitch +12 doubles a tone
// whose frequency is a multiple of the grain rate (its spectrum is then on
// the grain rate's grid), -12 halves it; Spray spreads the channels; Sync
// follows the tempo. Parity with Live waits on a reading.
#include "adi/dsp/grain_delay.hpp"

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
using G = adi::dsp::GrainDelay;
constexpr double kRate = 48000;

std::vector<float> sine(double hz, std::size_t frames, double amplitude) {
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
        sum += static_cast<double>(y[from + i]) * std::polar(1.0, -2 * std::numbers::pi * hz * static_cast<double>(from + i) / kRate);
    }
    return std::abs(sum);
}
void render(G& g, const std::vector<float>& l, const std::vector<float>& r, std::vector<float>& ol, std::vector<float>& orr,
            std::size_t block = 512) {
    ol.assign(l.size(), 0.f);
    orr.assign(l.size(), 0.f);
    for (std::size_t at = 0; at < l.size(); at += block) {
        const auto n = std::min(block, l.size() - at);
        g.process(l.data() + at, r.data() + at, ol.data() + at, orr.data() + at, n);
    }
}

void exactDelay() {
    G g;
    g.set(G::Time, 100);
    g.set(G::Feedback, 50);
    g.set(G::DryWet, 100);
    g.prepare(kRate);
    // The impulse comes after the grains are both running (the first half
    // grain has only one).
    std::vector<float> x(30000, 0.f), l, r;
    x[1000] = 1.f;
    render(g, x, x, l, r);
    bool exact = true;
    for (std::size_t i = 0; i < l.size(); ++i) {
        const bool echo = i > 1000 && (i - 1000) % 4800 == 0;
        const double want = echo ? std::pow(0.5, static_cast<double>((i - 1000) / 4800 - 1)) : 0.0;
        exact = exact && std::abs(l[i] - want) < 1e-6;
    }
    check(exact, "no pitch, no spray: an impulse returns after exactly 100 ms, alone, each repeat half the last");
    check(l == r, "no spray: both channels alike");
}

void pitched() {
    // A 440 Hz tone at a grain rate of 20 Hz (440 = 22 x 20): Pitch +12
    // gives 880 Hz, -12 gives 220 Hz, the strongest line of each.
    for (double pitch : {12.0, -12.0}) {
        G g;
        g.set(G::Frequency, 20);
        g.set(G::Pitch, pitch);
        g.set(G::DryWet, 100);
        g.prepare(kRate);
        const auto x = sine(440, 144000, 0.4);
        std::vector<float> l, r;
        render(g, x, x, l, r);
        const double want = 440 * std::exp2(pitch / 12);
        double best = 0, top = -1;
        for (double f = want * 0.8; f <= want * 1.2; f += 1) {
            const double m = magnitude(l, f);
            if (m > top) {
                top = m;
                best = f;
            }
        }
        check(std::abs(best - want) < 0.5, "Pitch " + std::to_string(pitch) + ": 440 Hz comes out at " + std::to_string(want) + " (" + std::to_string(best) + ")");
    }
}

void spray() {
    G g;
    g.set(G::Spray, 200);
    g.set(G::DryWet, 100);
    g.prepare(kRate);
    const auto x = sine(300, 48000, 0.4);
    std::vector<float> l, r;
    render(g, x, x, l, r);
    double diff = 0;
    for (std::size_t i = 0; i < l.size(); ++i) {
        diff = std::max(diff, static_cast<double>(std::abs(l[i] - r[i])));
    }
    check(diff > 0.05, "Spray scatters each channel its own way");
    check(std::all_of(l.begin(), l.end(), [](float v) { return std::isfinite(v) && std::abs(v) < 1; }), "and stays bounded");
}

void sync() {
    G g;
    g.set(G::Sync, 1);
    g.set(G::Sixteenths, 3);
    g.tempo(120);
    g.prepare(kRate);
    check(g.delay() == 18000, "3 sixteenths at 120 BPM: 375 ms");
    g.tempo(90);
    check(g.delay() == 24000, "at 90 BPM: 500 ms");
    g.set(G::Sixteenths, 16);
    g.tempo(40);
    check(g.delay() == 48000, "16 sixteenths at 40 BPM is held at the line's 1 s");
}

void routing() {
    std::vector<float> x(20000), y(20000), l, r;
    for (std::size_t i = 0; i < x.size(); ++i) {
        x[i] = static_cast<float>(0.3 * std::sin(0.013 * static_cast<double>(i)));
        y[i] = static_cast<float>(0.2 * std::sin(0.11 * static_cast<double>(i)));
    }
    G dry;
    dry.set(G::DryWet, 0);
    render(dry, x, y, l, r);
    check(l == x && r == y, "Dry/Wet 0 is the input itself");
    std::vector<float> first;
    bool identical = true;
    for (std::size_t block : {1u, 64u, 333u, 4096u}) {
        G g;
        g.set(G::Spray, 80);
        g.set(G::RandomPitch, 40);
        g.set(G::Pitch, 5);
        g.set(G::Feedback, 30);
        g.prepare(kRate);
        render(g, x, y, l, r, block);
        if (first.empty()) {
            first = l;
        } else {
            identical = identical && l == first;
        }
    }
    check(identical, "every block size gives the same samples");
    G g;
    std::vector<float> bad(512, std::numeric_limits<float>::quiet_NaN()), o(512), p(512);
    counting = true;
    g.process(bad.data(), bad.data(), o.data(), p.data(), bad.size());
    const bool silent = std::all_of(o.begin(), o.end(), [](float v) { return v == 0.f; });
    g.set(G::Pitch, -36);
    g.set(G::Frequency, 1);
    g.set(G::Spray, 500);
    g.set(G::Time, 1000);
    g.tempo(140);
    g.process(x.data(), x.data(), o.data(), p.data(), 512);
    counting = false;
    check(allocations == 0, "processing and moved controls allocate nothing");
    check(silent, "a NaN input is silence");
    // The longest reach: 1 s, 500 ms of spray, a 2 s grain at -36 and +12
    // semitones, at 192 kHz, stays inside the line and finite.
    for (double pitch : {-36.0, 12.0}) {
        G far;
        far.set(G::Time, 1000);
        far.set(G::Spray, 500);
        far.set(G::Frequency, 1);
        far.set(G::Pitch, pitch);
        far.set(G::Feedback, 95);
        far.prepare(192000);
        std::vector<float> s(192000 * 4), a, b;
        for (std::size_t i = 0; i < s.size(); ++i) {
            s[i] = static_cast<float>(0.5 * std::sin(0.003 * static_cast<double>(i)));
        }
        render(far, s, s, a, b, 4096);
        check(std::all_of(a.begin(), a.end(), [](float v) { return std::isfinite(v) && std::abs(v) < 40; }),
              "the longest reach at 192 kHz, Pitch " + std::to_string(pitch) + ": finite and bounded");
    }
    G z;
    z.set(G::DryWet, 0);
    std::vector<float> left(4800, 0.25f), shared(4800, -0.5f), rightOut(4800);
    z.process(left.data(), shared.data(), shared.data(), rightOut.data(), 4800);
    check(shared.back() == 0.25f && rightOut.back() == -0.5f, "an output plane that is the other channel's input is read first");
}

}  // namespace

/// The review of 6ec98033: fast grains read past the write head at a low
/// Frequency (and with Random Pitch), and a move of Frequency rescaled the
/// running grains' windows (clicks, up to twice the level).
void reviewed() {
    struct Reach {
        double pitch, random, frequency, time, spray, rate;
    };
    for (const Reach r : {Reach{12, 0, 1, 50, 0, 48000}, Reach{12, 0, 1, 50, 0, 192000}, Reach{12, 0, 1.05, 1000, 0, 48000},
                          Reach{12, 100, 2, 50, 0, 48000}, Reach{12, 100, 1, 1000, 500, 48000}, Reach{-36, 100, 1, 1000, 500, 48000}}) {
        G g;
        g.set(G::Pitch, r.pitch);
        g.set(G::RandomPitch, r.random);
        g.set(G::Frequency, r.frequency);
        g.set(G::Time, r.time);
        g.set(G::Spray, r.spray);
        g.prepare(r.rate);
        const auto n = static_cast<std::size_t>(r.rate * 8);
        std::vector<float> x(n), l, rr;
        for (std::size_t i = 0; i < n; ++i) {
            x[i] = static_cast<float>(0.3 * std::sin(0.01 * static_cast<double>(i)));
        }
        render(g, x, x, l, rr, 4096);
        check(g.lowestDelay() >= 1, "Pitch " + std::to_string(r.pitch) + ", Random " + std::to_string(r.random) + ", " + std::to_string(r.frequency) +
                                        " Hz at " + std::to_string(r.rate) + ": no grain reads at or past the write head (least delay " +
                                        std::to_string(g.lowestDelay()) + ")");
    }
    // A constant input stays constant through moves of Frequency: each new
    // grain rises as its predecessor falls, so the windows still sum to 1.
    G g;
    g.set(G::Frequency, 150);
    g.set(G::DryWet, 100);
    g.prepare(kRate);
    const std::vector<float> one(4800, 1.f);
    std::vector<float> l(4800), r(4800);
    g.process(one.data(), one.data(), l.data(), r.data(), one.size());   // the line and the grains fill
    double worst = 0;
    for (double f : {1.0, 150.0, 60.0, 40.0, 7.0}) {
        g.set(G::Frequency, f);
        for (int block = 0; block < 30; ++block) {
            g.process(one.data(), one.data(), l.data(), r.data(), 3333);
            for (std::size_t i = 0; i < 3333; ++i) {
                worst = std::max(worst, std::abs(static_cast<double>(l[i]) - 1));
            }
        }
    }
    check(worst < 1e-6, "a constant input stays at 1 through moves of Frequency (worst " + std::to_string(worst) + ")");
    // And pitched grains stay behind the write head through the moves.
    G up;
    up.set(G::Pitch, 12);
    up.set(G::Frequency, 150);
    up.prepare(kRate);
    for (double f : {5.0, 150.0, 1.0}) {
        up.set(G::Frequency, f);
        for (int block = 0; block < 40; ++block) {
            up.process(one.data(), one.data(), l.data(), r.data(), 3333);
        }
    }
    check(up.lowestDelay() >= 1, "Pitch +12 through moves of Frequency: no read at or past the write head");
}

int main() {
    reviewed();
    exactDelay();
    pitched();
    spray();
    sync();
    routing();
    std::printf("%s -- %d checks, %d failure(s)\n", failures ? "FAILED" : "PASS", checks, failures);
    return failures ? 1 : 0;
}
