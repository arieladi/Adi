// SPDX-License-Identifier: GPL-3.0-or-later
//
// Live 12's Corpus, the core (DEV-22). The reference numbers are the
// objects' own physics and the header's laws, apart from the code's signal
// path: an impulse rings at the first mode times each model's textbook
// ratios (the free-free beam's (beta_n / beta_1)^2, the membrane's Bessel
// zeros, the plate's (m^2 + n^2) / 2, the pipe's odd harmonics), each peak
// found to 0.01 Hz within 0.1 %; the first mode falls 60 dB in Decay's T60
// within 2 %; a hit or a listening position at a mode's node silences it.
// Parity with Live waits on a reading.
#include "adi/dsp/corpus.hpp"

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
using P = adi::dsp::Corpus;
constexpr double kRate = 48000;

/// The impulse response's two channels, `frames` long.
void impulse(P& c, std::size_t frames, std::vector<float>& l, std::vector<float>& r) {
    std::vector<float> x(frames, 0.f);
    x[0] = 1.f;
    l.assign(frames, 0.f);
    r.assign(frames, 0.f);
    c.prepare(kRate);
    c.process(x.data(), x.data(), l.data(), r.data(), frames);
}
double magnitude(const std::vector<float>& y, double hz) {
    std::complex<double> sum = 0;
    for (std::size_t i = 0; i < y.size(); ++i) {
        sum += static_cast<double>(y[i]) * std::polar(1.0, -2 * std::numbers::pi * hz * static_cast<double>(i) / kRate);
    }
    return std::abs(sum);
}
double peakNear(const std::vector<float>& y, double guess) {
    double a = guess * 0.995, b = guess * 1.005;
    const double g = (std::sqrt(5.0) - 1) / 2;
    while (b - a > 0.01) {
        const double c = b - g * (b - a), d = a + g * (b - a);
        if (magnitude(y, c) > magnitude(y, d)) {
            b = d;
        } else {
            a = c;
        }
    }
    return 0.5 * (a + b);
}

void modes() {
    // The textbook ratios, written here apart from the code.
    struct Case {
        P::Model model;
        double ratios[4];
        const char* name;
    };
    const Case cases[] = {
        {P::Beam, {1, 2.7565, 5.4039, 8.9330}, "Beam"},
        {P::Marimba, {1, 4, 10, 10 * 8.9330 / 5.4039}, "Marimba"},
        {P::String, {1, 2, 3, 4}, "String"},
        {P::Membrane, {1, 1.5933, 2.1355, 2.2954}, "Membrane"},
        {P::Plate, {1, 2.5, 2.5, 4}, "Plate"},
        {P::Pipe, {1, 3, 5, 7}, "Pipe"},
        {P::Tube, {1, 2, 3, 4}, "Tube"},
    };
    for (const auto& c : cases) {
        P p;
        p.set(P::Type, c.model);
        p.set(P::Tune, 220);
        p.set(P::Decay, 100);
        p.set(P::Material, -100);   // every mode rings as long as the first
        std::vector<float> l, r;
        impulse(p, 24000, l, r);
        double worst = 0;
        for (int k = 0; k < 4; ++k) {
            if (k > 0 && c.ratios[k] == c.ratios[k - 1]) {
                continue;   // a degenerate pair of the plate rings as one
            }
            const double want = 220 * c.ratios[k], got = peakNear(l, want);
            worst = std::max(worst, std::abs(got / want - 1));
        }
        check(worst < 1e-3, std::string(c.name) + ": an impulse rings at its first four modes within 0.1 % (worst " + std::to_string(worst * 100) +
                                " %)");
    }
    // Tune, Fine, Inharmonics, and a mode past 0.45 of the rate.
    P t;
    t.set(P::Tune, 440);
    t.set(P::Fine, 10);
    check(std::abs(t.frequency(0) - 440 * std::exp2(10.0 / 1200)) < 1e-9, "Fine moves the first mode in cents");
    t.set(P::Fine, 0);
    t.set(P::Type, P::Pipe);
    t.set(P::Inharmonics, 100);
    check(std::abs(t.frequency(1) - 440 * std::pow(3.0, 1.1)) < 1e-6, "Inharmonics stretches the ratios: r^1.1");
    t.set(P::Inharmonics, 0);
    t.set(P::Tune, 2000);
    check(std::abs(t.frequency(4) - 18000) < 1e-6 && t.frequency(5) == 0, "Pipe at 2 kHz: the fifth mode (18 kHz) rings, the sixth (22 kHz, past 0.45 of the rate) is silent");
}

void decay() {
    // String, struck at its middle (odd modes only), Material +100 (mode k
    // rings T60 / k): past 100 ms the first mode dominates by 60 dB. Its
    // slope from 100 to 300 ms gives its T60.
    for (double d : {30.0, 50.0, 70.0}) {
        P p;
        p.set(P::Type, P::String);
        p.set(P::Tune, 440);
        p.set(P::Hit, 50);
        p.set(P::Material, 100);
        p.set(P::Decay, d);
        std::vector<float> l, r;
        const double want = P::t60(d);
        impulse(p, static_cast<std::size_t>(kRate * (0.3 + 0.02)), l, r);
        const auto rms = [&](double at) {
            double e = 0;
            const auto from = static_cast<std::size_t>(at * kRate);
            for (std::size_t i = from; i < from + 480; ++i) {
                e += static_cast<double>(l[i]) * l[i];
            }
            return std::sqrt(e / 480);
        };
        const double slope = 20 * std::log10(rms(0.1) / rms(0.3)) / 0.2, got = 60 / slope;
        check(std::abs(got / want - 1) < 0.02, "Decay " + std::to_string(d) + ": the first mode's T60 is " + std::to_string(want) + " s within 2 % (" +
                                                   std::to_string(got) + ")");
    }
}

void nodes() {
    // Struck at the middle, a string's even modes are not excited; heard at
    // the middle, they are not heard. Each is compared with the same mode
    // when it is excited and heard (the other modes' skirts reach its bin).
    const auto second = [](double hit, double listenR, bool right) {
        P p;
        p.set(P::Type, P::String);
        p.set(P::Tune, 440);
        p.set(P::Hit, hit);
        p.set(P::ListenR, listenR);
        p.set(P::Decay, 80);
        std::vector<float> l, r;
        impulse(p, 24000, l, r);
        return magnitude(right ? r : l, 880);
    };
    const double excited = second(17, 70, false), atNode = second(50, 70, false);
    check(atNode < 0.01 * excited, "Hit 50 %: the second mode is not excited (" + std::to_string(atNode / excited) + " of its level when it is)");
    const double heard = second(17, 70, true), unheard = second(17, 50, true);
    check(unheard < 0.01 * heard, "Listen R at 50 %: the right channel does not hear the second mode (" + std::to_string(unheard / heard) + ")");
    check(second(17, 50, false) > 0.5 * excited, "the left, elsewhere, still does");
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
    P a, b;
    b.set(P::Bleed, 100);
    std::vector<float> l2(x.size()), r2(x.size());
    a.process(x.data(), y.data(), l.data(), r.data(), x.size());
    b.process(x.data(), y.data(), l2.data(), r2.data(), x.size());
    double worst = 0;
    for (std::size_t i = 0; i < x.size(); ++i) {
        worst = std::max(worst, std::abs(static_cast<double>(l2[i]) - l[i] - 0.5 * (static_cast<double>(x[i]) + y[i])));
    }
    check(worst < 1e-6, "Bleed 100 % adds the input's mono sum to the resonance");
    std::vector<float> first;
    bool identical = true;
    for (std::size_t block : {1u, 64u, 333u, 4096u}) {
        P p;
        p.set(P::Type, P::Membrane);
        p.prepare(kRate);
        for (std::size_t at = 0; at < x.size(); at += block) {
            const auto n = std::min(block, x.size() - at);
            p.process(x.data() + at, y.data() + at, l.data() + at, r.data() + at, n);
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
    p.set(P::Type, P::Plate);
    p.set(P::Tune, 3000);
    p.set(P::Decay, 100);
    p.process(x.data(), x.data(), o.data(), q.data(), 512);
    counting = false;
    check(allocations == 0, "processing and moved controls allocate nothing");
    check(silent, "a NaN input is silence");
    P z;
    z.set(P::DryWet, 0);
    std::vector<float> left(4800, 0.25f), shared(4800, -0.5f), rightOut(4800);
    z.process(left.data(), shared.data(), shared.data(), rightOut.data(), 4800);
    check(shared.back() == 0.25f && rightOut.back() == -0.5f, "an output plane that is the other channel's input is read first");
}

}  // namespace

/// The review of eb01d991: three findings, each a check that fails on the
/// old core.
void reviewed() {
    // A 2 ms half-sine hit rings at the same level at 44.1 to 192 kHz.
    const auto hitPeak = [](double rate) {
        P p;
        p.set(P::Type, P::String);
        p.prepare(rate);
        const auto n = static_cast<std::size_t>(rate);
        std::vector<float> x(n, 0.f), l(n), r(n);
        const auto len = static_cast<std::size_t>(0.002 * rate);
        for (std::size_t i = 0; i < len; ++i) {
            x[i] = static_cast<float>(0.5 * std::sin(std::numbers::pi * static_cast<double>(i) / static_cast<double>(len)));
        }
        p.process(x.data(), x.data(), l.data(), r.data(), n);
        double peak = 0;
        for (float v : l) {
            peak = std::max(peak, static_cast<double>(std::abs(v)));
        }
        return peak;
    };
    const double at48 = hitPeak(48000);
    double worst = 0;
    for (double rate : {44100.0, 96000.0, 192000.0}) {
        worst = std::max(worst, std::abs(20 * std::log10(hitPeak(rate) / at48)));
    }
    check(worst < 0.5, "a 2 ms hit rings at the same level at every rate (worst " + std::to_string(worst) + " dB from 48 kHz)");
    // A full-scale tone held at a mode at the longest Decay stays under
    // full scale, at 48 and 192 kHz.
    for (double rate : {48000.0, 192000.0}) {
        P p;
        p.set(P::Type, P::String);
        p.set(P::Decay, 100);
        p.prepare(rate);
        const auto n = static_cast<std::size_t>(rate * 2);
        std::vector<float> x(n), l(n), r(n);
        for (std::size_t i = 0; i < n; ++i) {
            x[i] = static_cast<float>(std::sin(2 * std::numbers::pi * 440 * static_cast<double>(i) / rate));
        }
        p.process(x.data(), x.data(), l.data(), r.data(), n);
        double peak = 0;
        for (float v : l) {
            peak = std::max(peak, static_cast<double>(std::abs(v)));
        }
        check(peak <= 1.0, "a full-scale tone held at a mode at Decay 100 stays under full scale at " + std::to_string(rate) + " (" +
                               std::to_string(peak) + ")");
    }
    // The membrane's sixteen modes are the Bessel zeros j_mn in order, each
    // over j_01, found here by bisection on std::cyl_bessel_j.
    std::vector<double> zeros;
    for (int m = 0; m <= 9; ++m) {
        double prev = std::cyl_bessel_j(static_cast<double>(m), 0.5);
        for (double x = 0.51; x < 16; x += 0.01) {
            const double v = std::cyl_bessel_j(static_cast<double>(m), x);
            if (prev * v < 0) {
                double a = x - 0.01, b = x;
                for (int k = 0; k < 60; ++k) {
                    const double c = 0.5 * (a + b);
                    (std::cyl_bessel_j(static_cast<double>(m), a) * std::cyl_bessel_j(static_cast<double>(m), c) <= 0 ? b : a) = c;
                }
                zeros.push_back(0.5 * (a + b));
            }
            prev = v;
        }
    }
    std::sort(zeros.begin(), zeros.end());
    const auto table = P::ratios(P::Membrane);
    double off = 0;
    for (std::size_t k = 0; k < P::kModes; ++k) {
        off = std::max(off, std::abs(table[k] - zeros[k] / zeros[0]));
    }
    check(off < 1e-3, "Membrane: all sixteen ratios are the Bessel zeros in order (worst " + std::to_string(off) + ")");
}

/// Gain, Bleed and Dry/Wet moved under a tone glide: the largest step while
/// they move stays within the signal's own, before or after.
void glides() {
    struct Move {
        P::Param param;
        double from, to;
        const char* what;
    };
    for (const Move m : {Move{P::Gain, -36, 12, "Gain"}, Move{P::Bleed, 0, 100, "Bleed"}, Move{P::DryWet, 0, 100, "Dry/Wet"}}) {
        P p;
        p.set(P::Type, P::String);
        p.set(P::Decay, 20);
        p.set(m.param, m.from);
        p.prepare(kRate);
        std::vector<float> x(96000), l(96000), r(96000);
        for (std::size_t i = 0; i < x.size(); ++i) {
            x[i] = static_cast<float>(0.2 * std::sin(2 * std::numbers::pi * 110 * static_cast<double>(i) / kRate));
        }
        // The move lands on the tone's peak (a quarter period past a whole
        // second), where a step would show.
        constexpr std::size_t at = 48109;
        p.process(x.data(), x.data(), l.data(), r.data(), at);
        p.set(m.param, m.to);
        p.process(x.data() + at, x.data() + at, l.data() + at, r.data() + at, x.size() - at);
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
    modes();
    decay();
    nodes();
    routing();
    std::printf("%s -- %d checks, %d failure(s)\n", failures ? "FAILED" : "PASS", checks, failures);
    return failures ? 1 : 0;
}
