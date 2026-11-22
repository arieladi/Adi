// SPDX-License-Identifier: GPL-3.0-or-later
//
// Live 12's Drum Buss, the core (DEV-22). The references are the header's
// own laws, apart from the code's signal path: a quiet sine with the
// colouring off is Trim, Damp's low pass and Output (within 0.05 dB); the
// compressor pulls a steady -6 dBFS tone by its static curve (within 0.5
// dB); Boom rings at Boom Freq (within 1 %) and longer with Boom Decay;
// Transients raise or lower a hit's attack against its body; Drive and
// Crunch add harmonics, Crunch above 1 kHz only. Parity with Live waits on
// a reading.
#include "adi/dsp/biquad.hpp"
#include "adi/dsp/drum_buss.hpp"

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
using B = adi::dsp::DrumBuss;
using C = std::complex<double>;
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
std::vector<float> render(B& b, const std::vector<float>& x) {
    std::vector<float> l(x.size()), r(x.size());
    b.process(x.data(), x.data(), l.data(), r.data(), x.size());
    return l;
}
B plain() {
    B b;
    b.set(B::Drive, 0);
    b.prepare(kRate);
    return b;
}

void quiet() {
    struct Case {
        double trim, damp, output;
    };
    for (const Case c : {Case{0, 20000, 0}, Case{6, 6000, -3}, Case{-12, 4500, 6}}) {
        double worst = 0;
        for (double hz : {100.0, 1000.0, 4000.0, 8000.0}) {
            B b = plain();
            b.set(B::Trim, c.trim);
            b.set(B::Damp, c.damp);
            b.set(B::Output, c.output);
            const auto y = render(b, sine(hz, 96000, 1e-5));
            const C z1 = std::polar(1.0, -2 * std::numbers::pi * hz / kRate), z2 = z1 * z1;
            const auto lp = adi::dsp::lowPass(kRate, c.damp, 1 / std::sqrt(2.0));
            const double h = std::abs((lp.b0 + lp.b1 * z1 + lp.b2 * z2) / (1.0 + lp.a1 * z1 + lp.a2 * z2));
            const double want = c.trim + c.output + 20 * std::log10(h), got = 20 * std::log10(component(y, hz) / 1e-5);
            worst = std::max(worst, std::abs(got - want));
        }
        check(worst < 0.05, "Trim " + std::to_string(c.trim) + ", Damp " + std::to_string(c.damp) + ": a quiet sine is Trim, Damp and Output (worst " +
                                std::to_string(worst) + " dB)");
    }
}

void compressor() {
    // A square of +-0.5 (-6.02 dBFS, |x| constant, so the peak detector
    // settles on it exactly): the static curve's gain g, through the Soft
    // curve, gives tanh(0.5 g) against tanh(0.5) without it.
    std::vector<float> x(96000);
    for (std::size_t i = 0; i < x.size(); ++i) {
        x[i] = (i / 120) % 2 ? -0.5f : 0.5f;
    }
    B off = plain(), on = plain();
    on.set(B::Comp, 1);
    const double drop = 20 * std::log10(component(render(on, x), 200) / component(render(off, x), 200));
    const double g = std::pow(10.0, B::compressorGainDb(20 * std::log10(0.5)) / 20);
    const double want = 20 * std::log10(std::tanh(0.5 * g) / std::tanh(0.5));
    check(std::abs(drop - want) < 0.05, "Comp on a -6 dBFS square: its static curve's gain, " + std::to_string(want) + " dB (" + std::to_string(drop) + ")");
    check(B::compressorGainDb(-24) == 0 && std::abs(B::compressorGainDb(-10) + 6) < 1e-12, "the curve: nothing under -18 dB, 4 : 1 above");
    // On a sine the 1 ms detector settles under the peak: less pull, never more.
    const auto s = sine(200, 96000, 0.5);
    const double sineDrop = 20 * std::log10(component(render(on, s), 200) / component(render(off, s), 200));
    check(sineDrop < -6 && sineDrop > want - 0.05, "on a -6 dBFS sine it pulls between 6 dB and the square's (" + std::to_string(sineDrop) + ")");
}

void boom() {
    const auto ring = [](double decay, double& hz) {
        B b = plain();
        b.set(B::Boom, 100);
        b.set(B::BoomFreq, 60);
        b.set(B::BoomDecay, decay);
        std::vector<float> x(48000, 0.f);
        x[0] = 1.f;
        const auto y = render(b, x);
        // Past 20 ms the boom alone rings (the click is gone): its peak.
        std::vector<float> tail(y.begin() + 960, y.end());
        double best = 0, top = -1;
        for (double f = 50; f <= 70; f += 0.05) {
            std::complex<double> s = 0;
            for (std::size_t i = 0; i < tail.size(); ++i) {
                s += static_cast<double>(tail[i]) * std::polar(1.0, -2 * std::numbers::pi * f * static_cast<double>(i) / kRate);
            }
            if (std::abs(s) > top) {
                top = std::abs(s);
                best = f;
            }
        }
        hz = best;
        double late = 0;
        for (std::size_t i = 24000; i < 48000; ++i) {
            late += static_cast<double>(y[i]) * y[i];
        }
        return late;
    };
    double hz = 0, hzShort = 0;
    const double longRing = ring(100, hz), shortRing = ring(0, hzShort);
    check(std::abs(hz / 60 - 1) < 0.01, "Boom rings at Boom Freq, 60 Hz (" + std::to_string(hz) + ")");
    check(longRing > 100 * shortRing, "Boom Decay 100 % rings far longer than 0 %");
}

void transients() {
    // Hits: a 150 Hz tone that starts at full level and decays over 40 ms,
    // every 250 ms. The attack (first 5 ms) against the body (20 to 40 ms).
    std::vector<float> x(48000, 0.f);
    for (std::size_t hit = 0; hit < x.size(); hit += 12000) {
        for (std::size_t i = 0; i < 4800 && hit + i < x.size(); ++i) {
            const double t = static_cast<double>(i) / kRate;
            x[hit + i] = static_cast<float>(0.5 * std::exp(-t / 0.04) * std::sin(2 * std::numbers::pi * 150 * t));
        }
    }
    const auto ratio = [&](double amount) {
        B b = plain();
        b.set(B::Transients, amount);
        const auto y = render(b, x);
        double attack = 0, body = 0;
        for (std::size_t i = 24000; i < 24000 + 240; ++i) {
            attack = std::max(attack, static_cast<double>(std::abs(y[i])));
        }
        for (std::size_t i = 24000 + 960; i < 24000 + 1920; ++i) {
            body = std::max(body, static_cast<double>(std::abs(y[i])));
        }
        return attack / body;
    };
    const double flat = ratio(0), up = ratio(100), down = ratio(-100);
    check(up > flat * 1.2 && down < flat / 1.2, "Transients +100 sharpens a hit's attack, -100 softens it (" + std::to_string(down) + ", " +
                                                     std::to_string(flat) + ", " + std::to_string(up) + ")");
}

void colour() {
    const auto x = sine(200, 96000, 0.5);
    const auto thd = [&](double drive, int type) {
        B b;
        b.set(B::Drive, drive);
        b.set(B::Type, type);
        b.prepare(kRate);
        const auto y = render(b, x);
        double h = 0;
        for (int k = 2; k <= 9; ++k) {
            h += std::pow(component(y, 200.0 * k), 2);
        }
        return std::sqrt(h) / component(y, 200);
    };
    check(thd(100, B::Soft) > 3 * thd(10, B::Soft), "Drive 100 distorts far more than Drive 10");
    check(std::abs(thd(60, B::Hard) - thd(60, B::Soft)) > 0.01, "Hard and Soft are different curves");
    // Crunch: a 3 kHz tone of 0.5 gains its octave and keeps its level (the
    // review of 96fd37df: the old crunch lifted the band itself by up to
    // 8 dB); a 100 Hz tone keeps its level (the band is above 1 kHz).
    const auto crunch = [](double hz, double amount, double& fundamental) {
        B b = plain();
        b.set(B::Crunch, amount);
        const auto y = render(b, sine(hz, 96000, 0.5));
        fundamental = component(y, hz);
        return component(y, 2 * hz);
    };
    double f0 = 0, f1 = 0, low0 = 0, low1 = 0;
    const double without = crunch(3000, 0, f0), with = crunch(3000, 100, f1);
    (void)crunch(100, 0, low0);
    (void)crunch(100, 100, low1);
    check(with > 10 * without + 0.01, "Crunch brightens a 3 kHz tone: its octave up over 20 dB");
    check(std::abs(20 * std::log10(f1 / f0)) < 0.1, "and leaves the tone's own level (" + std::to_string(20 * std::log10(f1 / f0)) + " dB)");
    check(std::abs(20 * std::log10(low1 / low0)) < 0.1, "and a 100 Hz tone's (" + std::to_string(20 * std::log10(low1 / low0)) + " dB)");
}

void routing() {
    std::vector<float> x(20000), y(20000), l(20000), r(20000);
    for (std::size_t i = 0; i < x.size(); ++i) {
        x[i] = static_cast<float>(0.3 * std::sin(0.013 * static_cast<double>(i)));
        y[i] = static_cast<float>(0.2 * std::sin(0.11 * static_cast<double>(i)));
    }
    B dry;
    dry.set(B::DryWet, 0);
    dry.process(x.data(), y.data(), l.data(), r.data(), x.size());
    check(l == x && r == y, "Dry/Wet 0 is the input itself");
    std::vector<float> first;
    bool identical = true;
    for (std::size_t block : {1u, 64u, 333u, 4096u}) {
        B b;
        b.set(B::Comp, 1);
        b.set(B::Boom, 50);
        b.set(B::Transients, 40);
        b.set(B::Crunch, 30);
        b.prepare(kRate);
        for (std::size_t at = 0; at < x.size(); at += block) {
            const auto n = std::min(block, x.size() - at);
            b.process(x.data() + at, y.data() + at, l.data() + at, r.data() + at, n);
        }
        if (first.empty()) {
            first = l;
        } else {
            identical = identical && l == first;
        }
    }
    check(identical, "every block size gives the same samples");
    B b;
    std::vector<float> bad(512, std::numeric_limits<float>::quiet_NaN()), o(512), p(512);
    counting = true;
    b.process(bad.data(), bad.data(), o.data(), p.data(), bad.size());
    const bool silent = std::all_of(o.begin(), o.end(), [](float v) { return v == 0.f; });
    b.set(B::Comp, 1);
    b.set(B::Type, B::Hard);
    b.set(B::Boom, 100);
    b.process(x.data(), x.data(), o.data(), p.data(), 512);
    counting = false;
    check(allocations == 0, "processing and moved controls allocate nothing");
    check(silent, "a NaN input is silence");
    B z;
    z.set(B::DryWet, 0);
    std::vector<float> left(4800, 0.25f), shared(4800, -0.5f), rightOut(4800);
    z.process(left.data(), shared.data(), shared.data(), rightOut.data(), 4800);
    check(shared.back() == 0.25f && rightOut.back() == -0.5f, "an output plane that is the other channel's input is read first");
}

}  // namespace

/// The review of 96fd37df: Transients distorted held low tones, and Drive's
/// level matching lost up to 22 dB on a loud signal.
void reviewed() {
    // A held 50 or 100 Hz tone at -26 dBFS passes Transients +-100 with
    // under 2 % THD and its level within 0.3 dB.
    for (double hz : {50.0, 100.0}) {
        for (double amount : {100.0, -100.0}) {
            const auto x = sine(hz, 96000, 0.05);
            B flat = plain(), shaped = plain();
            shaped.set(B::Transients, amount);
            const auto y0 = render(flat, x), y1 = render(shaped, x);
            double h = 0;
            for (int k = 2; k <= 9; ++k) {
                h += std::pow(component(y1, hz * k), 2);
            }
            const double thd = std::sqrt(h) / component(y1, hz), moved = 20 * std::log10(component(y1, hz) / component(y0, hz));
            check(thd < 0.02 && std::abs(moved) < 0.3, "Transients " + std::to_string(amount) + " on a held " + std::to_string(hz) + " Hz tone: THD " +
                                                           std::to_string(thd) + ", level " + std::to_string(moved) + " dB");
        }
    }
    // Drive is matched by peak at -6 dBFS: a sine of 0.5 keeps its peak at
    // Drive 100 (within 0.3 dB) and its fundamental within 2 dB (a clipped
    // wave holds more fundamental at the same peak); full scale within 3 dB.
    // On the old core Drive 100 cost a full-scale sine 20 dB.
    for (double amplitude : {0.5, 1.0}) {
        const auto x = sine(100, 96000, amplitude);
        B none = plain(), full;
        full.set(B::Drive, 100);
        full.prepare(kRate);
        const auto y0 = render(none, x), y1 = render(full, x);
        const double moved = 20 * std::log10(component(y1, 100) / component(y0, 100));
        double p0 = 0, p1 = 0;
        for (std::size_t i = 48000; i < y0.size(); ++i) {
            p0 = std::max(p0, static_cast<double>(std::abs(y0[i])));
            p1 = std::max(p1, static_cast<double>(std::abs(y1[i])));
        }
        if (amplitude < 0.75) {
            check(std::abs(20 * std::log10(p1 / p0)) < 0.3 && std::abs(moved) < 2, "Drive 100 on a sine of 0.5: its peak kept (" +
                                                                                       std::to_string(20 * std::log10(p1 / p0)) + " dB), its fundamental within 2 dB (" +
                                                                                       std::to_string(moved) + ")");
        } else {
            check(std::abs(moved) < 3, "Drive 100 on a full-scale sine: its level within 3 dB (" + std::to_string(moved) + ")");
        }
    }
}

/// Trim, Drive, Boom, Output and Dry/Wet moved at a tone's peak glide: the
/// largest step while they move stays within the signal's own, before or
/// after.
void glides() {
    struct Move {
        B::Param param;
        double from, to;
        const char* what;
    };
    for (const Move m : {Move{B::Trim, -24, 12, "Trim"}, Move{B::Drive, 0, 100, "Drive"}, Move{B::Boom, 0, 100, "Boom"},
                         Move{B::Output, -36, 12, "Output"}, Move{B::DryWet, 0, 100, "Dry/Wet"}}) {
        B b = plain();
        b.set(m.param, m.from);
        b.prepare(kRate);
        const auto x = sine(110, 96000, 0.2);
        std::vector<float> l(x.size()), r(x.size());
        constexpr std::size_t at = 48109;   // the tone's peak
        b.process(x.data(), x.data(), l.data(), r.data(), at);
        b.set(m.param, m.to);
        b.process(x.data() + at, x.data() + at, l.data() + at, r.data() + at, x.size() - at);
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
    quiet();
    compressor();
    boom();
    transients();
    colour();
    routing();
    std::printf("%s -- %d checks, %d failure(s)\n", failures ? "FAILED" : "PASS", checks, failures);
    return failures ? 1 : 0;
}
