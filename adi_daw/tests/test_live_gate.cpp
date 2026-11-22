// SPDX-License-Identifier: GPL-3.0-or-later
//
// Live 12's Gate, the core (DEV-22, ADR-0249 d7). Each property is checked
// against a number derived apart from the code: the levels from Threshold and
// Return in dB, the ballistics from a one-pole's time constant, Hold in whole
// samples, the lookahead from the 480 samples Live 12.4 reports at 10 ms for
// the Compressor's lookahead (the same three choices). Parity with Live's
// sound waits on a Live 12.4 reading of the Gate.
#include "adi/dsp/live_gate.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <new>
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
void near(double got, double want, double tolerance, const std::string& what) {
    ++checks;
    if (!(std::abs(got - want) <= tolerance)) {
        ++failures;
        std::printf("FAIL %s\n      got %.6f, want %.6f +- %g\n", what.c_str(), got, want, tolerance);
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
using G = adi::dsp::LiveGate;
double fromDb(double db) { return std::pow(10.0, db / 20.0); }

/// The same signal on both channels; returns the left output.
std::vector<float> run(G& g, const std::vector<float>& in) {
    std::vector<float> l(in.size()), r(in.size());
    g.process(in.data(), in.data(), l.data(), r.data(), in.size());
    return l;
}
/// A gate at 48 kHz with no lookahead, Floor at silence, and fast ballistics
/// unless a test sets its own.
G made(double threshold = -40, double ret = 3, double floor = -80) {
    G g;
    g.set(G::Threshold, threshold);
    g.set(G::Return, ret);
    g.set(G::Floor, floor);
    g.set(G::Attack, 0.02);
    g.set(G::Release, 0.01);
    g.set(G::Hold, 0.02);
    g.prepare(48000);
    return g;
}
std::vector<float> constant(std::size_t n, double v) { return std::vector<float>(n, static_cast<float>(v)); }

void ranges() {
    G g;
    for (int p = 0; p < G::Count; ++p) {
        const auto& r = G::ranges[p];
        g.set(static_cast<G::Param>(p), r.high + 1000);
        near(g.value(static_cast<G::Param>(p)), r.high, 0, "parameter " + std::to_string(p) + " clamps at its top");
        g.set(static_cast<G::Param>(p), r.low - 1000);
        near(g.value(static_cast<G::Param>(p)), r.low, 0, "and at its bottom");
        g.set(static_cast<G::Param>(p), std::numeric_limits<double>::quiet_NaN());
        near(g.value(static_cast<G::Param>(p)), r.low, 0, "a NaN leaves it as it was");
    }
}

void opensAndCloses() {
    // Below the threshold: closed, Floor at silence, nothing out.
    auto g = made(-40);
    const auto quiet = run(g, constant(4800, fromDb(-50)));
    check(!g.open() && quiet.back() == 0.0f, "a level of -50 dBFS under a -40 dBFS threshold: closed, silence out");
    // Above it: open, and once Attack has run the signal passes as itself.
    const auto loud = run(g, constant(4800, fromDb(-30)));
    check(g.open(), "a level of -30 dBFS opens it");
    near(loud.back(), fromDb(-30), 1e-6, "and the signal passes at unity");
    // Floor: closed, the signal passes Floor dB down.
    auto f = made(-40, 3, -20);
    const auto floored = run(f, constant(48000, fromDb(-50)));
    near(floored.back(), fromDb(-50) * fromDb(-20), 1e-7, "Floor -20 dB: a closed gate passes the signal 20 dB down");
}

void hysteresisAndHold() {
    // Opened at -30 dBFS, then held between the close level (-46) and the
    // threshold (-40): it stays open, however long.
    auto g = made(-40, 6);
    (void)run(g, constant(480, fromDb(-30)));
    (void)run(g, constant(48000, fromDb(-43)));
    check(g.open(), "Return 6 dB: at -43 dBFS, above the close level of -46, it stays open");
    (void)run(g, constant(4800, fromDb(-47)));
    check(!g.open(), "at -47 dBFS, under the close level, it closes");

    // Hold: after the level drops away the gate stays open for exactly Hold.
    auto h = made(-40, 3);
    h.set(G::Hold, 10);   // 480 samples at 48 kHz
    (void)run(h, constant(480, fromDb(-30)));
    int openFor = 0;
    for (int i = 0; i < 2000; ++i) {
        float x = 0, y = 0;
        h.process(&x, &x, &y, &y, 1);
        if (h.open()) {
            ++openFor;
        }
    }
    check(openFor == 480, "Hold 10 ms: open for 480 more samples once the level is gone (" + std::to_string(openFor) +
                              ")");
}

void ballistics() {
    // Attack: from closed (gain 0) toward 1 with a time constant of Attack.
    auto g = made(-40);
    g.set(G::Attack, 10);   // 480 samples
    const auto rise = run(g, constant(480, 0.5));
    near(rise[479] / 0.5, 1 - std::exp(-480.0 / 480.0), 0.002, "Attack 10 ms: after 480 samples the gain is 1 - 1/e");
    // Release: from open toward Floor (0) once closed, with Release.
    auto r = made(-40);
    r.set(G::Release, 20);   // 960 samples
    (void)run(r, constant(4800, 0.5));
    std::vector<float> after(960, 0.5f);
    std::fill(after.begin(), after.end(), static_cast<float>(fromDb(-60)));   // under the close level
    // Hold is 0.02 ms (one sample): the gate closes on the second sample.
    const auto fall = run(r, after);
    near(fall[959] / after[959], std::exp(-959.0 / 960.0), 0.003, "Release 20 ms: 1/e of the way down after 960 samples");
}

void flipAndLink() {
    auto g = made(-40);
    g.set(G::Flip, 1);
    const auto quiet = run(g, constant(4800, fromDb(-50)));
    near(quiet.back(), fromDb(-50), 1e-7, "Flip: below the threshold the signal passes");
    const auto loud = run(g, constant(4800, fromDb(-30)));
    check(loud.back() == 0.0f, "and above it, it is held at Floor");
    // With a Floor above silence, Flip swaps unity and Floor exactly.
    auto f = made(-40, 3, -20);
    f.set(G::Flip, 1);
    const auto closed = run(f, constant(48000, fromDb(-50)));
    near(closed.back(), fromDb(-50), 1e-7, "Flip, Floor -20 dB: closed, the signal passes at unity");
    const auto opened = run(f, constant(48000, fromDb(-30)));
    near(opened.back(), fromDb(-30) * fromDb(-20), 1e-7, "and open, it is held at Floor, 20 dB down");
    // Linked: the louder channel opens the gate for both.
    auto l = made(-40);
    std::vector<float> left(4800, static_cast<float>(fromDb(-30))), right(4800, static_cast<float>(fromDb(-60)));
    std::vector<float> ol(4800), orr(4800);
    l.process(left.data(), right.data(), ol.data(), orr.data(), 4800);
    near(orr.back(), fromDb(-60), 1e-8, "linked: a loud left opens the gate for a quiet right too");
    // A moved Floor keeps the gain between Floor and unity, and Flip's passing
    // signal at unity, whatever the ballistics (a found defect: Flip mixed the
    // smoothed gain with the new Floor, +6 dB or silence for a Release).
    auto m = made(-40);
    m.set(G::Flip, 1);
    m.set(G::Attack, 150);
    m.set(G::Release, 3000);
    run(m, constant(4800, fromDb(-50)));
    double worst = 0;
    for (double floorDb : {0.0, -80.0, -10.0, -30.0}) {
        m.set(G::Floor, floorDb);
        for (float y : run(m, constant(4800, fromDb(-50)))) {
            worst = std::max(worst, std::abs(static_cast<double>(y) / fromDb(-50) - 1.0));
        }
    }
    check(worst < 1e-6, "Flip, closed: a moved Floor leaves the passing signal at unity");
    auto n = made(-40, 3, -20);
    n.set(G::Attack, 150);
    n.set(G::Release, 3000);
    run(n, constant(4800, fromDb(-30)));
    run(n, constant(9600, fromDb(-50)));  // closing, mid-release
    bool inside = true;
    for (double floorDb : {-6.0, -80.0, 0.0}) {
        n.set(G::Floor, floorDb);
        run(n, constant(480, fromDb(-50)));
        const double lowest = floorDb <= -80 ? 0.0 : fromDb(floorDb);
        inside = inside && n.gain() >= lowest - 1e-12 && n.gain() <= 1.0 + 1e-12;
    }
    check(inside, "a Floor moved mid-release keeps the gain between Floor and unity");
}

void lookahead() {
    for (double rate : {44100.0, 48000.0, 96000.0}) {
        auto g = made(-40);
        g.set(G::Lookahead, 2);
        g.prepare(rate);
        const int want = static_cast<int>(std::lround(rate / 100));
        check(g.latency() == want, "Lookahead 10 ms at " + std::to_string(static_cast<int>(rate)) + " Hz: " +
                                       std::to_string(g.latency()) + " samples (" + std::to_string(want) + ")");
    }
    // At 48 kHz, 480 samples: an impulse leaves exactly 480 late, and the gate
    // has opened for the transient that opened it.
    auto g = made(-40);
    g.set(G::Lookahead, 2);
    g.set(G::Attack, 1);   // 48 samples: ten time constants inside the lookahead
    std::vector<float> in(2000, 0.f);
    for (std::size_t i = 1000; i < in.size(); ++i) {
        in[i] = 0.25f;
    }
    const auto out = run(g, in);
    check(out[1479] == 0.0f && out[1480] > 0.25f * 0.99f,
          "Lookahead 10 ms: the step at 1000 leaves at 1480, already at full gain (" + std::to_string(out[1480]) + ")");
    g.set(G::Lookahead, 1);
    check(g.latency() == 48, "Lookahead 1 ms: 48 samples");
    g.set(G::Lookahead, 0);
    check(g.latency() == 0, "and off: none");
}

void robustness() {
    auto a = made(-40), b = made(-40);
    std::vector<float> withNaN(4800), withZero(4800);
    for (std::size_t i = 0; i < withNaN.size(); ++i) {
        const float v = static_cast<float>(0.3 * std::sin(0.05 * static_cast<double>(i)));
        withNaN[i] = withZero[i] = v;
    }
    for (std::size_t i = 1000; i < 1400; ++i) {
        withNaN[i] = std::numeric_limits<float>::quiet_NaN();
        withZero[i] = 0.f;
    }
    check(run(a, withNaN) == run(b, withZero), "a NaN block reads as silence, exactly");
    auto g = made(-40);
    g.set(G::Lookahead, 2);
    std::vector<float> l(512, 0.5f), r(512, -0.5f), ol(512), orr(512);
    counting = true;
    for (int i = 0; i < 100; ++i) {
        g.process(l.data(), r.data(), ol.data(), orr.data(), 512);
    }
    counting = false;
    check(allocations == 0, "process allocates nothing");
    // In place.
    auto p = made(-40);
    auto q = made(-40);
    std::vector<float> x(4800, 0.25f), y(4800, 0.25f), oy(4800), oz(4800);
    p.process(x.data(), x.data(), x.data(), x.data(), x.size());
    q.process(y.data(), y.data(), oy.data(), oz.data(), y.size());
    check(x == oy, "in place, the same as into other buffers");
}

}  // namespace

int main() {
    ranges();
    opensAndCloses();
    hysteresisAndHold();
    ballistics();
    flipAndLink();
    lookahead();
    robustness();
    std::printf("%s -- %d checks, %d failure(s)\n", failures ? "FAILED" : "PASS", checks, failures);
    return failures ? 1 : 0;
}
