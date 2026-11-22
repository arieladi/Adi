// SPDX-License-Identifier: GPL-3.0-or-later
//
// Live 12's Channel EQ, the core (DEV-22). Each band's measured response (a
// sine's steady-state level in and out) is checked against the design's
// analytic magnitude within a stated 0.01 dB, and against the shelf's and
// the bell's own anchors: a shelf's gain far from its corner, a bell's gain
// at its centre. Parity with Live's Channel EQ waits on a Live 12.4 reading.
#include "adi/dsp/channel_eq.hpp"

#include <algorithm>
#include <cmath>
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
using E = adi::dsp::ChannelEq;
constexpr double kRate = 48000;

/// The steady-state gain in dB of a sine at `hz` (a second to settle, the
/// next 0.5 s measured by RMS against the input's).
double measuredDb(E& eq, double hz) {
    const std::size_t frames = 72000;
    std::vector<float> in(frames), l(frames), r(frames);
    for (std::size_t i = 0; i < frames; ++i) {
        in[i] = static_cast<float>(0.25 * std::sin(2 * std::numbers::pi * hz * static_cast<double>(i) / kRate));
    }
    eq.prepare(kRate);
    eq.process(in.data(), in.data(), l.data(), r.data(), frames);
    double a = 0, b = 0;
    for (std::size_t i = 48000; i < frames; ++i) {
        a += static_cast<double>(in[i]) * in[i];
        b += static_cast<double>(l[i]) * l[i];
    }
    return 10 * std::log10(b / a);
}

void bands() {
    struct Case {
        E::Param param;
        double value, hz, anchor;
        const char* what;
    };
    const Case cases[] = {
        {E::Low, 9, 20, 9, "Low +9 dB: a shelf, 9 dB well below its 100 Hz corner"},
        {E::Low, -12, 25, -12, "Low -12 dB: the shelf cuts below its corner"},
        {E::Mid, 6, 1500, 6, "Mid +6 dB: the bell's gain at its centre"},
        {E::High, 10, 20000, 10, "High +10 dB: a shelf, 10 dB well above its 8 kHz corner"},
    };
    for (const auto& c : cases) {
        E eq;
        eq.set(c.param, c.value);
        eq.prepare(kRate);
        const double got = measuredDb(eq, c.hz);
        const double design = eq.responseDb(c.hz);
        check(std::abs(got - design) < 0.01, std::string(c.what) + ": measured within 0.01 dB of the design");
        check(std::abs(design - c.anchor) < 0.5, std::string(c.what) + ": the design meets its anchor");
    }
    // The bell moves with Mid Freq: at 400 Hz it peaks there.
    E moved;
    moved.set(E::Mid, -9);
    moved.set(E::MidFreq, 400);
    moved.prepare(kRate);
    check(std::abs(measuredDb(moved, 400) + 9) < 0.02, "Mid Freq moves the bell: -9 dB at 400 Hz");
    // Low Cut: an 80 Hz Butterworth, -3 dB at 80 Hz and some -24 dB at 20 Hz.
    E cut;
    cut.set(E::LowCut, 1);
    cut.prepare(kRate);
    const double at80 = measuredDb(cut, 80), at20 = measuredDb(cut, 20);
    check(std::abs(at80 + 3.0103) < 0.05 && std::abs(at20 - cut.responseDb(20)) < 0.01 && at20 < -20,
          "Low Cut: -3 dB at 80 Hz, a 12 dB/octave slope below");
    // Output: +6 dB on everything.
    E out;
    out.set(E::Output, 6);
    out.prepare(kRate);
    check(std::abs(measuredDb(out, 1000) - 6) < 0.01, "Output +6 dB");
    // Flat at its defaults, within 1e-7 of the input.
    E flat;
    std::vector<float> x(4800), l(4800), r(4800);
    for (std::size_t i = 0; i < x.size(); ++i) {
        x[i] = static_cast<float>(std::sin(0.05 * static_cast<double>(i)));
    }
    flat.process(x.data(), x.data(), l.data(), r.data(), x.size());
    double worst = 0;
    for (std::size_t i = 0; i < x.size(); ++i) {
        worst = std::max(worst, static_cast<double>(std::abs(l[i] - x[i])));
    }
    check(worst < 1e-7, "at its defaults it passes the input");
    // A plant: +9 dB is not measured at the wrong frequency's design.
    E plant;
    plant.set(E::Low, 9);
    plant.prepare(kRate);
    check(std::abs(measuredDb(plant, 20) - plant.responseDb(400)) > 1, "plant: the low shelf at 400 Hz is not its 20 Hz gain");
}

void blocksAndRobustness() {
    std::vector<float> x(20000), first, l(20000), r(20000);
    for (std::size_t i = 0; i < x.size(); ++i) {
        x[i] = static_cast<float>(0.3 * std::sin(0.031 * static_cast<double>(i)) + 0.1 * std::sin(0.7 * static_cast<double>(i)));
    }
    bool identical = true;
    for (std::size_t block : {1u, 64u, 333u, 4096u}) {
        E eq;
        eq.set(E::Low, 5);
        eq.set(E::Mid, -4);
        eq.set(E::High, 7);
        eq.set(E::LowCut, 1);
        eq.prepare(kRate);
        for (std::size_t at = 0; at < x.size(); at += block) {
            const auto n = std::min(block, x.size() - at);
            eq.process(x.data() + at, x.data() + at, l.data() + at, r.data() + at, n);
        }
        if (first.empty()) {
            first = l;
        } else {
            identical = identical && l == first;
        }
    }
    check(identical, "every block size gives the same samples");
    E eq;
    eq.set(E::High, 8);
    std::vector<float> bad(512, std::numeric_limits<float>::quiet_NaN()), o(512), p(512);
    counting = true;
    eq.process(bad.data(), bad.data(), o.data(), p.data(), bad.size());
    eq.set(E::MidFreq, 300);
    eq.process(bad.data(), bad.data(), o.data(), p.data(), bad.size());
    counting = false;
    check(allocations == 0 && std::all_of(o.begin(), o.end(), [](float v) { return v == 0.f; }),
          "a non-finite input is silence; processing and a moved control allocate nothing");
    // Aliased planes.
    E z;
    std::vector<float> left(64, 0.25f), shared(64, -0.5f), rightOut(64);
    z.process(left.data(), shared.data(), shared.data(), rightOut.data(), 64);
    check(std::abs(rightOut[63] + 0.5f) < 1e-6f, "an output plane that is the other channel's input is read first");
}

}  // namespace

/// Output moved at a tone's peak glides: the largest step while it moves
/// stays within the signal's own, before or after.
void glides() {
    E e;
    e.set(E::Output, -15);
    e.prepare(48000);
    std::vector<float> x(96000), l(96000), r(96000);
    for (std::size_t i = 0; i < x.size(); ++i) {
        x[i] = static_cast<float>(0.4 * std::sin(2 * std::numbers::pi * 110 * static_cast<double>(i) / 48000));
    }
    constexpr std::size_t at = 48109;   // the tone's peak
    e.process(x.data(), x.data(), l.data(), r.data(), at);
    e.set(E::Output, 6);
    e.process(x.data() + at, x.data() + at, l.data() + at, r.data() + at, x.size() - at);
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
    check(moving <= 1.5 * settled, "Output glides: no step past the signal's own (" + std::to_string(moving) + " against " + std::to_string(settled) + ")");
}

int main() {
    glides();
    bands();
    blocksAndRobustness();
    std::printf("%s -- %d checks, %d failure(s)\n", failures ? "FAILED" : "PASS", checks, failures);
    return failures ? 1 : 0;
}
