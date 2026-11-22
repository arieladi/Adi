// SPDX-License-Identifier: GPL-3.0-or-later
//
// Live 12's Compressor, the core (DEV-22). Each property is checked against a
// number derived independently of the code: the static curve from the textbook
// gain computer, the ballistics from a one-pole's time constant, the lookahead
// from the 480 samples Live 12.4 reports at 10 ms (the research's latency
// table). Parity with Live's sound waits on reading 17.
#include "adi/dsp/live_compressor.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
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
    if (counting) ++allocations;
    if (void* p = std::malloc(n ? n : 1)) return p;
    throw std::bad_alloc();
}
void operator delete(void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }

namespace {
using C = adi::dsp::LiveCompressor;
double db(double x) { return 20 * std::log10(std::abs(x)); }

// Constant input on both channels; returns the left output.
std::vector<float> run(C& c, const std::vector<float>& in) {
    std::vector<float> l(in.size()), r(in.size());
    c.process(in.data(), in.data(), l.data(), r.data(), in.size());
    return l;
}
C made(double threshold, double ratio, double knee = 0, int mode = C::Peak) {
    C c;
    c.set(C::Threshold, threshold);
    c.set(C::Ratio, ratio);
    c.set(C::Knee, knee);
    c.set(C::Mode, mode);
    c.set(C::Attack, 0.01);
    c.set(C::Release, 1);
    c.prepare(48000);
    return c;
}

void staticCurve() {
    // Below the threshold: bit-exact unity.
    {
        auto c = made(-20, 4);
        std::vector<float> in(4800, 0.05f);  // -26 dBFS
        const auto out = run(c, in);
        check(out == in, "below the threshold the audio is untouched, bit for bit");
    }
    // Hard knee, 4:1: -6.02 dBFS in, -20 + 13.98/4 = -16.505 dBFS out.
    {
        auto c = made(-20, 4);
        const auto out = run(c, std::vector<float>(4800, 0.5f));
        near(db(out.back()), -20 + (db(0.5) + 20) / 4, 0.001, "4:1 above a -20 dB threshold");
    }
    // Infinite ratio holds the output at the threshold.
    {
        auto c = made(-20, 100);
        const auto out = run(c, std::vector<float>(4800, 0.5f));
        near(db(out.back()), -20, 0.001, "an infinite ratio holds the threshold");
    }
    // Soft knee 12 dB, input at the threshold: (6)^2 / 24 * (1/4 - 1) = -1.125 dB.
    {
        const auto c = made(-20, 4, 12);
        near(c.staticGainDb(-20), -1.125, 1e-9, "a 12 dB knee at the threshold: -1.125 dB");
        near(c.staticGainDb(-26), 0, 1e-12, "the knee starts half its width below");
        near(c.staticGainDb(-14), -4.5, 1e-9, "and meets the 4:1 line half its width above");
        // Continuous slope: the derivative at both ends matches the lines.
        near((c.staticGainDb(-14) - c.staticGainDb(-14.001)) / 0.001, -0.75, 1e-3, "smooth into the 4:1 line");
        near((c.staticGainDb(-26) - c.staticGainDb(-26.001)) / 0.001, 0, 1e-3, "smooth out of unity");
    }
    // Expand, 4:1 below -20: -30 dB in, -20 + (-10) * 4 = -60 dB out.
    {
        auto c = made(-20, 4, 0, C::Expand);
        const auto out = run(c, std::vector<float>(4800, static_cast<float>(std::pow(10, -30 / 20.0))));
        near(db(out.back()), -60, 0.01, "Expand 1:4 below a -20 dB threshold");
        const auto loud = made(-20, 4, 0, C::Expand);
        near(loud.staticGainDb(-10), 0, 1e-12, "Expand leaves what is above the threshold");
        const auto soft = made(-20, 4, 12, C::Expand);
        near(soft.staticGainDb(-14), 0, 1e-12, "Expand's knee ends half its width above");
        near(soft.staticGainDb(-26), -18, 1e-9, "and meets the 1:4 line half its width below");
        near((soft.staticGainDb(-26) - soft.staticGainDb(-26.001)) / 0.001, 3, 1e-3, "smoothly");
        // Inside the knee: at the threshold, -(3)(6)^2 / 24 = -4.5 dB, and the
        // slope leaving the 1:4 line from inside the knee is still 3.
        near(soft.staticGainDb(-20), -4.5, 1e-9, "Expand's 12 dB knee at the threshold: -4.5 dB");
        near((soft.staticGainDb(-25.999) - soft.staticGainDb(-26)) / 0.001, 3, 2e-3, "from inside the knee too");
        near((soft.staticGainDb(-14) - soft.staticGainDb(-14.001)) / 0.001, 0, 2e-3, "and flat into unity");
    }
}

void ballistics() {
    // Log envelope: the reduction in dB moves as a one-pole with the time
    // constant asked for: 1 - 1/e of the way after Attack, e^-1 left after Release.
    C c;
    c.set(C::Threshold, -20);
    c.set(C::Ratio, 100);
    c.set(C::Knee, 0);
    c.set(C::Attack, 10);
    c.set(C::Release, 100);
    c.prepare(48000);
    const double full = -20 - db(0.5);  // the final reduction for 0.5 in
    std::vector<float> loud(480, 0.5f);
    (void)run(c, loud);  // 10 ms = one time constant
    near(c.gainReductionDb() / full, 1 - std::exp(-1.0), 0.002, "Attack 10 ms: 63.2 % of the reduction after 10 ms");
    (void)run(c, std::vector<float>(48000 - 480, 0.5f));
    near(c.gainReductionDb(), full, 1e-6, "and all of it after a second");
    (void)run(c, std::vector<float>(4800, 0.01f));  // well below: 100 ms = one time constant
    near(c.gainReductionDb() / full, std::exp(-1.0), 0.002, "Release 100 ms: 36.8 % left after 100 ms");
    // Lin envelope: the level, not the dB, is smoothed; at a steady level both agree.
    C lin = made(-20, 4);
    lin.set(C::Envelope, C::Lin);
    const auto out = run(lin, std::vector<float>(4800, 0.5f));
    near(db(out.back()), -20 + (db(0.5) + 20) / 4, 0.001, "Lin settles on the same static curve");
}

void lookahead() {
    for (const double rate : {44100.0, 48000.0, 96000.0}) {
        C c;
        c.set(C::Lookahead, 2);
        c.prepare(rate);
        check(c.latency() == static_cast<int>(std::lround(rate / 100)),
              "10 ms of lookahead is " + std::to_string(c.latency()) + " samples at " + std::to_string(static_cast<int>(rate)));
    }
    C c;
    c.set(C::Lookahead, 2);
    c.prepare(48000);
    check(c.latency() == 480, "480 at 48 kHz, as Live 12.4 reports for 10 ms");
    c.set(C::Lookahead, 1);
    check(c.latency() == 48, "1 ms is 48");
    c.set(C::Lookahead, 0);
    check(c.latency() == 0, "none is 0");
    // An impulse below the threshold comes out at exactly the lookahead.
    C d;
    d.set(C::Lookahead, 2);
    d.prepare(48000);
    std::vector<float> in(1024, 0.f);
    in[0] = 0.05f;
    const auto out = run(d, in);
    check(out[480] == 0.05f && std::count(out.begin(), out.end(), 0.f) == 1023, "the impulse comes out at 480, alone");
    // A step: the reduction has begun before the step leaves the delay.
    C e = made(-20, 4);
    e.set(C::Attack, 1);
    e.set(C::Lookahead, 2);
    std::vector<float> step(960, 0.f);
    std::fill(step.begin() + 100, step.end(), 0.5f);
    const auto stepped = run(e, step);
    check(std::abs(stepped[580]) < 0.5f * 0.9f, "with lookahead the step leaves already reduced");
}

void gainStages() {
    // Makeup (provisional law: the curve's reduction at 0 dBFS, in full).
    C c = made(-20, 4);
    c.set(C::Makeup, 1);
    const auto out = run(c, std::vector<float>(4800, 1.f));
    near(db(out.back()), 0, 0.001, "Makeup brings a 0 dBFS level back to 0 dBFS");
    // Output gain.
    C o;
    o.set(C::Threshold, 0);
    o.set(C::Output, 6);
    o.prepare(48000);
    const auto louder = run(o, std::vector<float>(4800, 0.1f));
    near(louder.back(), 0.1 * std::pow(10, 0.3), 1e-6, "Output +6 dB");
    // Dry/Wet 0 is the dry signal, bit for bit, delayed with the wet.
    C dry = made(-20, 4);
    dry.set(C::DryWet, 0);
    dry.prepare(48000);
    const std::vector<float> in(4800, 0.5f);
    check(run(dry, in) == in, "Dry/Wet 0 is the input, bit for bit");
}

void robustness() {
    // Stereo-linked: a loud left reduces a quiet right by the same amount.
    C c = made(-20, 4);
    std::vector<float> left(4800, 0.5f), right(4800, 0.05f), ol(4800), orr(4800);
    c.process(left.data(), right.data(), ol.data(), orr.data(), left.size());
    near(db(orr.back()) - db(0.05), db(ol.back()) - db(0.5), 1e-5, "the channels share one gain");
    // Non-finite input does not poison the state.
    std::vector<float> bad(64, std::nanf("")), out(64);
    c.process(bad.data(), bad.data(), out.data(), out.data(), bad.size());
    const auto after = run(c, std::vector<float>(4800, 0.5f));
    check(std::isfinite(after.back()) && std::isfinite(c.gainReductionDb()), "a NaN block leaves the compressor finite");
    // No allocation while processing.
    C r = made(-20, 4, 6, C::Rms);
    std::vector<float> x(4800), y(4800);
    for (std::size_t i = 0; i < x.size(); ++i) x[i] = static_cast<float>(0.5 * std::sin(static_cast<double>(i) * 0.13));
    allocations = 0;
    counting = true;
    r.process(x.data(), x.data(), y.data(), y.data(), x.size());
    counting = false;
    check(allocations == 0, "process allocates nothing");
    // RMS of a sine: its level reads 3 dB below its peak.
    near(r.gainReductionDb(), r.staticGainDb(db(0.5 / std::sqrt(2.0))), 0.05, "RMS reads a sine 3 dB below its peak");
}
}  // namespace

int main() {
    staticCurve();
    ballistics();
    lookahead();
    gainStages();
    robustness();
    std::printf("%s -- %d checks, %d failure(s)\n", failures ? "FAILED" : "PASS", checks, failures);
    return failures ? 1 : 0;
}
