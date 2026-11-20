// SPDX-License-Identifier: GPL-3.0-or-later
//
// The DSP corrections — src/adi/dsp/, ADR-0096.
//
// Three goals from ADR-0093 each carried a defect as written, and each defect was
// a stability problem rather than a matter of taste:
//
//   * Pd's biquad~ feedback coefficients are the cookbook's NEGATED. Copied
//     straight in, the filter runs away.
//   * The limiter was specified with env~, an RMS follower. An RMS limiter
//     lets a spike straight through.
//   * RMSC's rectified key exceeds 1 on a hot kick, the gain goes negative, and
//     the music comes out upside down.
//
// Each test is built so the defect it names is what makes it fail.
//
// Its own binary: it replaces global `operator new` to observe that the audio-
// thread paths allocate nothing.

#include "adi/dsp/auto_gain.hpp"
#include "adi/dsp/biquad.hpp"
#include "adi/dsp/limiter.hpp"
#include "adi/dsp/rmsc.hpp"
#include "adi/dsp/true_peak.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <limits>
#include <new>
#include <sstream>
#include <string>
#include <vector>

// --- allocation counter -----------------------------------------------------
namespace {
std::atomic<long> g_allocs{0};
std::atomic<bool> g_counting{false};
}  // namespace

void* operator new(std::size_t n) {
    if (g_counting.load(std::memory_order_relaxed))
        g_allocs.fetch_add(1, std::memory_order_relaxed);
    void* p = std::malloc(n == 0 ? 1 : n);
    if (p == nullptr) throw std::bad_alloc();
    return p;
}
void* operator new[](std::size_t n) { return operator new(n); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }

namespace {

using namespace adi::dsp;

int g_failures = 0;
int g_checks = 0;

void check(bool cond, const std::string& what) {
    ++g_checks;
    if (!cond) { ++g_failures; std::printf("  FAIL  %s\n", what.c_str()); }
}
void near(double got, double want, double tol, const std::string& what) {
    ++g_checks;
    if (!(std::fabs(got - want) <= tol)) {
        ++g_failures;
        std::printf("  FAIL  %s\n          got %.6f, want %.6f (+/- %.4g)\n",
                    what.c_str(), got, want, tol);
    }
}
void section(const char* s) { std::printf("[%s]\n", s); }

constexpr double kPi = 3.14159265358979323846;
constexpr double kFs = 48000.0;

/// The cookbook's recursion, direct form I.
std::vector<double> runCookbook(const Biquad& c, const std::vector<double>& x) {
    std::vector<double> y(x.size());
    double x1 = 0, x2 = 0, y1 = 0, y2 = 0;
    for (std::size_t i = 0; i < x.size(); ++i) {
        y[i] = c.b0 * x[i] + c.b1 * x1 + c.b2 * x2 - c.a1 * y1 - c.a2 * y2;
        x2 = x1; x1 = x[i]; y2 = y1; y1 = y[i];
    }
    return y;
}

/// Pd's biquad~, exactly as its help file states it:
///     w = x + fb1 w1 + fb2 w2,   y = ff1 w + ff2 w1 + ff3 w2
std::vector<double> runPd(const PdBiquad& p, const std::vector<double>& x) {
    std::vector<double> y(x.size());
    double w1 = 0, w2 = 0;
    for (std::size_t i = 0; i < x.size(); ++i) {
        const double w = x[i] + p.fb1 * w1 + p.fb2 * w2;
        y[i] = p.ff1 * w + p.ff2 * w1 + p.ff3 * w2;
        w2 = w1; w1 = w;
    }
    return y;
}

std::vector<double> impulse(std::size_t n) {
    std::vector<double> x(n, 0.0);
    x[0] = 1.0;
    return x;
}

// ===========================================================================
// biquad~
// ===========================================================================

void testPdNeedsTheFeedbackNegated() {
    section("ADR-0096 -- Pd's biquad~ feedback terms are the cookbook's, negated");

    const Biquad lp = lowPass(kFs, 1000.0, 1.0 / std::sqrt(2.0));
    const Biquad bell = peaking(kFs, 1000.0, 1.0, 6.0);
    const std::vector<double> x = impulse(4096);

    for (const Biquad* c : {&lp, &bell}) {
        const std::vector<double> ref = runCookbook(*c, x);
        const std::vector<double> pd = runPd(toPd(*c), x);
        double worst = 0.0;
        for (std::size_t i = 0; i < x.size(); ++i) worst = std::max(worst, std::fabs(ref[i] - pd[i]));
        check(worst < 1e-12,
              "through toPd(), biquad~ reproduces the cookbook filter sample for sample "
              "(worst difference " + std::to_string(worst) + ")");

        // THE BUG, driven rather than described: the cookbook's a1, a2 copied
        // straight into biquad~'s fb1, fb2.
        PdBiquad wrong;
        wrong.fb1 = c->a1; wrong.fb2 = c->a2;
        wrong.ff1 = c->b0; wrong.ff2 = c->b1; wrong.ff3 = c->b2;
        const std::vector<double> bad = runPd(wrong, x);
        double peak = 0.0;
        for (double v : bad) peak = std::max(peak, std::fabs(v));
        check(peak > 1e6 || !std::isfinite(peak),
              "and with the signs left as the cookbook writes them, the same "
              "impulse RUNS AWAY -- peak " + std::to_string(peak));
    }
}

void testTheCookbookDesignsSayWhatTheyMean() {
    section("ADR-0096 -- each design has the response its parameters promise");

    near(magnitudeDb(peaking(kFs, 1000.0, 1.0, 6.0), kFs, 1000.0), 6.0, 0.01,
         "a +6 dB bell is +6 dB at its centre");
    near(magnitudeDb(peaking(kFs, 1000.0, 1.0, -9.0), kFs, 1000.0), -9.0, 0.01,
         "a -9 dB bell is -9 dB at its centre");
    near(magnitudeDb(peaking(kFs, 1000.0, 1.0, 6.0), kFs, 20.0), 0.0, 0.05,
         "and flat far below it");
    near(magnitudeDb(lowShelf(kFs, 200.0, 6.0), kFs, 5.0), 6.0, 0.05,
         "a +6 dB low shelf lifts the lows by 6");
    near(magnitudeDb(lowShelf(kFs, 200.0, 6.0), kFs, 15000.0), 0.0, 0.05,
         "and leaves the highs alone");
    near(magnitudeDb(highShelf(kFs, 5000.0, -6.0), kFs, 23900.0), -6.0, 0.1,
         "a -6 dB high shelf cuts the top by 6");
    near(magnitudeDb(highShelf(kFs, 5000.0, -6.0), kFs, 30.0), 0.0, 0.05,
         "and leaves the lows alone");
}

void testSteepCutsAreButterworthCascades() {
    section("ADR-0096 -- 12, 24, 36 and 48 dB/oct, each -3 dB at its cutoff");

    for (int slope : {12, 24, 36, 48}) {
        const auto hc = cut(CutKind::HighCut, kFs, 1000.0, slope);
        const auto lc = cut(CutKind::LowCut, kFs, 1000.0, slope);
        check(static_cast<int>(hc.size()) == slope / 12,
              std::to_string(slope) + " dB/oct is " + std::to_string(slope / 12) + " sections");
        near(magnitudeDb(hc, kFs, 1000.0), -3.0103, 0.02,
             std::to_string(slope) + " dB/oct high cut: -3 dB at the cutoff");
        near(magnitudeDb(lc, kFs, 1000.0), -3.0103, 0.02,
             std::to_string(slope) + " dB/oct low cut: -3 dB at the cutoff");
    }

    // An octave past an 8th-order Butterworth: |H|^2 = 1 / (1 + 2^16).
    near(magnitudeDb(cut(CutKind::HighCut, kFs, 1000.0, 48), kFs, 2000.0), -48.16, 0.6,
         "and 48 dB/oct means 48 dB an octave out");

    // k = 1 is the pole pair nearest the imaginary axis: the sharpest section.
    near(butterworthQ(8, 1), 2.5629, 0.0005, "section 1 Q, the sharp one");
    near(butterworthQ(8, 2), 0.9000, 0.0005, "section 2 Q");
    near(butterworthQ(8, 3), 0.6013, 0.0005, "section 3 Q");
    near(butterworthQ(8, 4), 0.5098, 0.0005, "section 4 Q, the broad one");

    // THE CORRECTION TO ADR-0093, measured. "Four biquads" is the right
    // count and the wrong filter if the four are identical.
    std::vector<Biquad> naive(4, lowPass(kFs, 1000.0, 1.0 / std::sqrt(2.0)));
    near(magnitudeDb(naive, kFs, 1000.0), -12.04, 0.05,
         "four IDENTICAL Q = 0.707 sections sag to -12 dB at the cutoff, not -3");
}

void testEveryDesignIsStable() {
    section("ADR-0096 -- stable across the whole parameter range");

    int designs = 0, unstable = 0;
    for (double f0 : {20.0, 60.0, 250.0, 1000.0, 4000.0, 12000.0, 20000.0}) {
        for (double g : {-24.0, -6.0, 0.0, 6.0, 24.0}) {
            for (double q : {0.3, 0.707, 2.0, 10.0}) {
                ++designs; if (!isStable(peaking(kFs, f0, q, g))) ++unstable;
            }
            ++designs; if (!isStable(lowShelf(kFs, f0, g))) ++unstable;
            ++designs; if (!isStable(highShelf(kFs, f0, g))) ++unstable;
        }
        for (int slope : {12, 24, 36, 48})
            for (const Biquad& c : cut(CutKind::LowCut, kFs, f0, slope)) {
                ++designs; if (!isStable(c)) ++unstable;
            }
    }
    check(unstable == 0, std::to_string(designs) + " designs, " +
                             std::to_string(unstable) + " unstable");

    // And the check can say no: biquad~'s view of an un-negated low-pass.
    const Biquad lp = lowPass(kFs, 1000.0, 0.707);
    Biquad flipped = lp;
    flipped.a1 = -lp.a1;
    flipped.a2 = -lp.a2;
    check(!isStable(flipped), "the sign-flipped filter is recognised as unstable");
}

void testThePdMessageCarriesTheExactDoubles() {
    section("ADR-0096 -- the host sends biquad~ exactly what it computed");

    const PdBiquad p = toPd(peaking(kFs, 3170.0, 1.3, -4.5));
    std::istringstream in(pdMessage(p));
    double v[5] = {};
    for (double& x : v) in >> x;
    check(v[0] == p.fb1 && v[1] == p.fb2 && v[2] == p.ff1 && v[3] == p.ff2 && v[4] == p.ff3,
          "all five survive the round trip bit for bit: " + pdMessage(p));
}

// ===========================================================================
// the limiter
// ===========================================================================

struct Buf {
    std::vector<float> l, r;
    explicit Buf(std::size_t n) : l(n, 0.0f), r(n, 0.0f) {}
};

/// Run a whole signal through the limiter in 256-frame blocks.
Buf limit(LookaheadLimiter& lim, const Buf& in) {
    Buf out(in.l.size());
    for (std::size_t off = 0; off < in.l.size(); off += 256) {
        const int n = static_cast<int>(std::min<std::size_t>(256, in.l.size() - off));
        const float* ip[2] = {in.l.data() + off, in.r.data() + off};
        float* op[2] = {out.l.data() + off, out.r.data() + off};
        lim.process(ip, op, n);
    }
    return out;
}

double peakOf(const Buf& b) {
    double p = 0.0;
    for (float v : b.l) p = std::max(p, std::fabs(static_cast<double>(v)));
    for (float v : b.r) p = std::max(p, std::fabs(static_cast<double>(v)));
    return p;
}

void testTheLimiterIsTransparentBelowTheCeiling() {
    section("ADR-0096 -- below the ceiling the limiter is a pure delay");

    LookaheadLimiter lim;
    lim.prepare(kFs, 2, 1024);
    lim.setLookaheadMs(1.5);
    lim.setCeiling(1.0);
    check(lim.latencySamples() == 72, "1.5 ms at 48 kHz is 72 samples of latency: " +
                                          std::to_string(lim.latencySamples()));

    Buf in(4096);
    for (std::size_t i = 0; i < in.l.size(); ++i) {
        in.l[i] = static_cast<float>(0.5 * std::sin(2.0 * kPi * 440.0 * static_cast<double>(i) / kFs));
        in.r[i] = in.l[i];
    }
    const Buf out = limit(lim, in);
    bool exact = true;
    for (std::size_t i = 72; i < out.l.size(); ++i)
        if (out.l[i] != in.l[i - 72]) { exact = false; break; }
    check(exact, "a -6 dB sine comes out as itself, 72 samples late, bit for bit");
}

void testTheLimiterNeverPassesTheCeiling() {
    section("ADR-0096 -- the ceiling holds on every sample, for every signal");

    const double ceiling = std::pow(10.0, -0.3 / 20.0);
    std::vector<std::pair<std::string, Buf>> signals;
    {
        Buf b(8192);
        for (std::size_t i = 0; i < b.l.size(); ++i)
            b.l[i] = b.r[i] = static_cast<float>(4.0 * std::sin(2.0 * kPi * 100.0 * static_cast<double>(i) / kFs));
        signals.emplace_back("a +12 dB sine", b);
    }
    {
        Buf b(8192);
        b.l[3000] = 10.0f;
        signals.emplace_back("a single-sample spike at +20 dB", b);
    }
    {
        Buf b(8192);
        for (std::size_t i = 0; i < b.l.size(); ++i)
            b.l[i] = b.r[i] = ((i / 64) % 2 != 0 && (i / 1024) % 2 == 0) ? 3.0f : 0.0f;
        signals.emplace_back("square bursts at +10 dB", b);
    }
    {
        Buf b(8192);
        std::uint32_t s = 12345u;
        for (std::size_t i = 0; i < b.l.size(); ++i) {
            s = s * 1664525u + 1013904223u;
            b.l[i] = static_cast<float>(8.0 * ((s >> 8) / 16777216.0 - 0.5));
            s = s * 1664525u + 1013904223u;
            b.r[i] = static_cast<float>(8.0 * ((s >> 8) / 16777216.0 - 0.5));
        }
        signals.emplace_back("noise at +12 dB", b);
    }

    for (int L : {0, 72, 144, 288}) {
        for (const auto& [name, sig] : signals) {
            LookaheadLimiter lim;
            lim.prepare(kFs, 2, 1024);
            lim.setLookahead(L);
            lim.setCeiling(ceiling);
            const double p = peakOf(limit(lim, sig));
            check(p <= ceiling * (1.0 + 1e-6),
                  name + ", lookahead " + std::to_string(L) + ": peak " +
                      std::to_string(p) + " against a ceiling of " + std::to_string(ceiling));
        }
    }
}

void testTheAttackIsARampAndTheReleaseIsSlow() {
    section("ADR-0096 -- gain reduction arrives smoothly and leaves slowly");

    LookaheadLimiter lim;
    lim.prepare(kFs, 2, 1024);
    lim.setLookahead(64);
    lim.setCeiling(1.0);
    lim.setReleaseSeconds(0.05);

    Buf in(24000);
    in.l[1000] = in.r[1000] = 4.0f;       // a spike needing 0.25x
    std::vector<double> gains;
    for (std::size_t i = 0; i < in.l.size(); ++i) {
        const float ip0[1] = {in.l[i]}, ip1[1] = {in.r[i]};
        float o0[1], o1[1];
        const float* ip[2] = {ip0, ip1};
        float* op[2] = {o0, o1};
        lim.process(ip, op, 1);
        gains.push_back(lim.lastGain());
    }
    // The spike enters at 1000 and leaves the delay at 1064. The gain must be
    // at 0.25 by 1064, and must have got there over the lookahead.
    near(gains[1064], 0.25, 1e-9, "fully reduced when the spike leaves the delay");
    const double firstStep = 1.0 - gains[1000];
    check(firstStep < 0.1 * 0.75,
          "the first step down is under a tenth of the reduction -- a RAMP, not a "
          "step: " + std::to_string(firstStep));
    check(gains[1032] < gains[1000] && gains[1032] > gains[1064],
          "and it is still on its way halfway through the lookahead");
    check(gains[1200] < 0.99, "released SLOWLY: still reduced 136 samples later");

    // THE RELEASE IS AN EXPONENTIAL WITH THE TIME CONSTANT ASKED FOR. The hold
    // lets go at 1065, when the spike leaves the lookahead window; one time
    // constant (2400 samples at 50 ms) later the released gain is
    // 1 - 0.75/e = 0.724, and the boxcar reads it about half a window late.
    // The first version of this check said "back to unity after 60 ms", which
    // is 1.3 time constants and 0.79 -- a wrong expectation, not a wrong filter.
    near(gains[1065 + 2400], 1.0 - 0.75 * std::exp(-(2400.0 - 32.0) / 2400.0), 0.01,
         "one time constant after release: 1 - 0.75/e");
    check(gains[23999] > 0.99, "and back to unity after about five time constants");
}

void testLimiterReleaseTimeSurvivesPrepare() {
    section("ADR-0096 -- preparing at another rate preserves release time in seconds");

    // With no lookahead, a 4x spike gives gain 0.25. After N silent samples,
    // the documented exponential recovery is 1 - 0.75 * exp(-N / (time * rate)).
    // Reusing the old coefficient changes that recovery when the rate changes.
    const auto verify = [](LookaheadLimiter& lim, double rate, double seconds,
                           const char* setting) {
        lim.prepare(rate, 2, 8);
        lim.setLookahead(0);
        lim.setCeiling(1.0);
        const float spike = 4.0f;
        float left = 0.0f, right = 0.0f;
        const float* spikeIn[2] = {&spike, &spike};
        float* spikeOut[2] = {&left, &right};
        lim.process(spikeIn, spikeOut, 1);

        const int frames = std::max(1, static_cast<int>(std::lround(rate * seconds)));
        Buf silence(static_cast<std::size_t>(frames)), out(static_cast<std::size_t>(frames));
        const float* ip[2] = {silence.l.data(), silence.r.data()};
        float* op[2] = {out.l.data(), out.r.data()};
        lim.process(ip, op, frames);
        const double expected = 1.0 - 0.75 * std::exp(-static_cast<double>(frames) / (seconds * rate));
        near(lim.lastGain(), expected, 1e-11,
             std::string(setting) + " release survives prepare at " + std::to_string(rate) + " Hz");
    };

    LookaheadLimiter defaults;
    for (double rate : {44100.0, 96000.0, 48000.0, 32000.0})
        verify(defaults, rate, 0.05, "default 50 ms");

    LookaheadLimiter beforePrepare;
    beforePrepare.setReleaseSeconds(0.08);
    verify(beforePrepare, 96000.0, 0.08, "80 ms set before first prepare");

    LookaheadLimiter afterPrepare;
    afterPrepare.prepare(48000.0, 2, 8);
    afterPrepare.setReleaseSeconds(0.08);
    for (double rate : {48000.0, 96000.0, 44100.0})
        verify(afterPrepare, rate, 0.08, "80 ms set after prepare");

    LookaheadLimiter clamped;
    clamped.setReleaseSeconds(0.0);
    for (double rate : {96000.0, 44100.0})
        verify(clamped, rate, 1e-5, "minimum release");
}

void testRmsDetectionWouldHaveMissedTheSpike() {
    section("ADR-0096 -- why peak, not RMS: the spike env~ would never see");

    // A one-sample +20 dB spike has an RMS over a 72-sample window of
    // 10 / sqrt(72) = 1.18 -- barely over the ceiling -- so an RMS detector
    // asks for almost no reduction and the spike leaves at nearly 10. The
    // peak detector asks for 0.1 and gets it.
    LookaheadLimiter lim;
    lim.prepare(kFs, 2, 1024);
    lim.setLookahead(72);
    lim.setCeiling(1.0);
    Buf in(2048);
    in.l[500] = in.r[500] = 10.0f;
    const Buf out = limit(lim, in);
    near(out.l[572], 1.0, 1e-6, "the spike comes out AT the ceiling, not near 10");
}

void testTheLimiterIsLinkedAcrossChannels() {
    section("ADR-0096 -- one gain for all channels, so the image does not move");

    LookaheadLimiter lim;
    lim.prepare(kFs, 2, 1024);
    lim.setLookahead(32);
    lim.setCeiling(1.0);
    Buf in(1024);
    for (std::size_t i = 0; i < in.l.size(); ++i) in.r[i] = 0.5f;
    in.l[300] = 2.0f;                 // only the left overshoots
    const Buf out = limit(lim, in);
    near(out.r[332], 0.25, 1e-6,
         "the right channel is reduced by the LEFT's peak, by the same gain");
}

void testTheLimiterAllocatesNothing() {
    section("ADR-0010 -- the limiter's process() allocates nothing");

    LookaheadLimiter lim;
    lim.prepare(kFs, 2, 2048);
    lim.setLookahead(288);
    Buf in(256), out(256);
    for (std::size_t i = 0; i < in.l.size(); ++i) in.l[i] = in.r[i] = 3.0f;
    const float* ip[2] = {in.l.data(), in.r.data()};
    float* op[2] = {out.l.data(), out.r.data()};
    g_allocs.store(0);
    g_counting.store(true);
    for (int b = 0; b < 50; ++b) lim.process(ip, op, 256);
    g_counting.store(false);
    check(g_allocs.load() == 0, "fifty loud blocks, no allocation");
}

// --- DEV-29 (c): the true-peak brickwall ------------------------------------

/// A sine at a quarter of the rate, 45 degrees off its crest: every sample is
/// at amplitude / sqrt(2) and the wave between them reaches the amplitude.
/// `gate` > 0 switches it on and off every `gate` samples (hard edges).
std::vector<double> quarterRateAt45(std::size_t n, double amplitude, std::size_t gate = 0) {
    std::vector<double> x(n, 0.0);
    for (std::size_t i = 0; i < n; ++i) {
        if (gate > 0 && (i / gate) % 2 == 1) continue;
        x[i] = amplitude * std::sin(kPi / 2.0 * static_cast<double>(i) + kPi / 4.0);
    }
    return x;
}

struct Limited {
    std::vector<double> l, r;
};

/// Through the limiter in 256-frame blocks, float or double.
template <class Sample>
Limited limitAs(LookaheadLimiter& lim, const std::vector<double>& l, const std::vector<double>& r) {
    std::vector<Sample> il(l.size()), ir(r.size()), ol(l.size()), orr(l.size());
    for (std::size_t i = 0; i < l.size(); ++i) {
        il[i] = static_cast<Sample>(l[i]);
        ir[i] = static_cast<Sample>(r[i]);
    }
    for (std::size_t off = 0; off < l.size(); off += 256) {
        const int n = static_cast<int>(std::min<std::size_t>(256, l.size() - off));
        const Sample* ip[2] = {il.data() + off, ir.data() + off};
        Sample* op[2] = {ol.data() + off, orr.data() + off};
        lim.process(ip, op, n);
    }
    return {std::vector<double>(ol.begin(), ol.end()), std::vector<double>(orr.begin(), orr.end())};
}

double truePeakOf(double fs, const Limited& y) {
    TruePeakMeter m;
    m.prepare(fs, 2);
    const double* p[2] = {y.l.data(), y.r.data()};
    m.process(p, 2, static_cast<int>(y.l.size()));
    return m.peak();
}

void testTheTruePeakLimiterIsAPureDelayBelowTheCeiling() {
    section("DEV-29 c -- true peak is off by default; on, below the ceiling it is a delay");

    LookaheadLimiter lim;
    lim.prepare(kFs, 2, 1024);
    lim.setLookaheadMs(1.5);
    check(!lim.truePeak() && lim.latencySamples() == 72,
          "off by default: the latency is the lookahead alone");
    lim.setTruePeak(true);
    check(lim.truePeak() && lim.truePeakDelay() == 10 && lim.latencySamples() == 82,
          "on at 48 kHz: 72 + 8 (the interpolator) + 2 (the spread) = 82: " +
              std::to_string(lim.latencySamples()));

    for (double rate : {44100.0, 96000.0, 176400.0, 192000.0}) {
        LookaheadLimiter other;
        other.prepare(rate, 2, 1024);
        other.setLookahead(64);
        other.setTruePeak(true);
        const int expected = rate >= 176400.0 ? 64 : 74;
        check(other.latencySamples() == expected,
              "at " + std::to_string(static_cast<int>(rate)) + " Hz the latency is " +
                  std::to_string(other.latencySamples()) + " (expected " +
                  std::to_string(expected) + "; none added once the Annex oversamples by 1)");
    }

    // Under the ceiling in TRUE peak: the quarter-rate sine at 45 degrees with
    // its crest at -1.5 dBTP against -1 dBTP, plus a 440 Hz tone on the right.
    const double ceiling = std::pow(10.0, -1.0 / 20.0);
    std::vector<double> l = quarterRateAt45(8192, std::pow(10.0, -1.5 / 20.0));
    std::vector<double> r(8192);
    for (std::size_t i = 0; i < r.size(); ++i)
        r[i] = 0.8 * std::sin(2.0 * kPi * 440.0 * static_cast<double>(i) / kFs);
    for (bool asDouble : {false, true}) {
        LookaheadLimiter tp;
        tp.prepare(kFs, 2, 1024);
        tp.setLookaheadMs(1.5);
        tp.setCeiling(ceiling);
        tp.setTruePeak(true);
        const int d = tp.latencySamples();
        const Limited y = asDouble ? limitAs<double>(tp, l, r) : limitAs<float>(tp, l, r);
        bool exact = true;
        for (std::size_t i = 0; i < y.l.size() && exact; ++i) {
            const double wantL = i >= static_cast<std::size_t>(d) ? l[i - static_cast<std::size_t>(d)] : 0.0;
            const double wantR = i >= static_cast<std::size_t>(d) ? r[i - static_cast<std::size_t>(d)] : 0.0;
            const double gotL = y.l[i], gotR = y.r[i];
            if (asDouble) exact = gotL == wantL && gotR == wantR;
            else exact = gotL == static_cast<double>(static_cast<float>(wantL)) &&
                         gotR == static_cast<double>(static_cast<float>(wantR));
        }
        check(exact, std::string(asDouble ? "double" : "float") +
                         ": comes out as itself, exactly latencySamples() late, bit for bit");
    }
}

void testTheTruePeakLimiterHoldsTheCeilingBetweenTheSamples() {
    section("DEV-29 c -- the ceiling holds between the samples, not only on them");

    const double ceilingDb = -1.0;
    const double ceiling = std::pow(10.0, ceilingDb / 20.0);
    const auto db = [](double v) { return 20.0 * std::log10(v); };

    // THE CASE A SAMPLE-PEAK BRICKWALL CANNOT SEE. Every sample of the +6 dBTP
    // quarter-rate sine is turned down to the ceiling exactly -- and the wave
    // between them leaves 3 dB over it.
    {
        const std::vector<double> x = quarterRateAt45(16384, 2.0);
        LookaheadLimiter sp;
        sp.prepare(kFs, 2, 1024);
        sp.setLookaheadMs(1.5);
        sp.setCeiling(ceiling);
        const Limited y = limitAs<float>(sp, x, x);
        const double over = db(truePeakOf(kFs, y)) - ceilingDb;
        check(over > 2.5 && over < 3.1,
              "sample-peak mode: the quarter-rate sine leaves " + std::to_string(over) +
                  " dB over a -1 dB ceiling between the samples");
    }

    struct Fixture {
        std::string name;
        std::vector<double> l, r;
    };
    std::vector<Fixture> fixtures;
    {
        auto x = quarterRateAt45(16384, 2.0);
        fixtures.push_back({"quarter-rate sine at 45 degrees, +6 dBTP", x, x});
    }
    {
        std::vector<double> l(16384), r(16384);
        for (std::size_t i = 0; i < l.size(); ++i) {
            l[i] = 3.0 * std::sin(2.0 * kPi * 11025.0 * 1.003 * static_cast<double>(i) / kFs + 0.3);
            r[i] = 2.0 * std::sin(2.0 * kPi * 997.0 * static_cast<double>(i) / kFs);
        }
        fixtures.push_back({"11 kHz on the left (+9.5 dB), 997 Hz on the right", l, r});
    }
    {
        auto x = quarterRateAt45(16384, 2.0, 1500);
        fixtures.push_back({"quarter-rate bursts at 45 degrees, +6 dBTP, hard-gated", x, x});
    }
    {
        std::vector<double> l(16384), r(16384);
        std::uint32_t s = 2024u;
        for (std::size_t i = 0; i < l.size(); ++i) {
            s = s * 1664525u + 1013904223u;
            l[i] = 8.0 * ((s >> 8) / 16777216.0 - 0.5);
            s = s * 1664525u + 1013904223u;
            r[i] = 8.0 * ((s >> 8) / 16777216.0 - 0.5);
        }
        fixtures.push_back({"white noise at +12 dB", l, r});
    }
    {
        std::vector<double> l(16384, 0.0);
        for (std::size_t i = 2000; i < l.size(); i += 3000) {
            l[i] = 4.0;
            l[i + 1] = -4.0;   // a doublet: its true peak is far above its samples
        }
        fixtures.push_back({"+12 dB doublets", l, std::vector<double>(16384, 0.0)});
    }

    // THE RESIDUE. Where the gain moves inside the interpolator's span the
    // output's true peak is not exactly the gain times the input's. With a
    // lookahead the gain moves slowly and the residue is under a thousandth
    // of a dB (measured: 0.0003 on noise at 64 samples); with none the gain
    // can step from one sample to the next (measured: 0.025 on noise). The
    // encoded check after dither is the final gate either way.

    for (double rate : {44100.0, 48000.0, 96000.0}) {
        for (int L : {0, 64, 288}) {
            for (const auto& f : fixtures) {
                LookaheadLimiter lim;
                lim.prepare(rate, 2, 1024);
                lim.setLookahead(L);
                lim.setCeiling(ceiling);
                lim.setTruePeak(true);
                const Limited y = limitAs<double>(lim, f.l, f.r);
                double sample = 0.0;
                for (std::size_t i = 0; i < y.l.size(); ++i)
                    sample = std::max({sample, std::fabs(y.l[i]), std::fabs(y.r[i])});
                const double over = db(truePeakOf(rate, y)) - ceilingDb;
                const std::string at = " at " + std::to_string(static_cast<int>(rate)) +
                                       " Hz, lookahead " + std::to_string(L);
                check(sample <= ceiling * (1.0 + 1e-12),
                      f.name + at + ": every sample at or under the ceiling");
                const double allowed = L > 0 ? 0.001 : 0.05;
                check(over <= allowed,
                      f.name + at + ": true peak " + std::to_string(over) +
                          " dB against the ceiling (allowed " + std::to_string(allowed) + ")");
            }
        }
    }
}

void testTheTruePeakLimiterAllocatesNothing() {
    section("ADR-0010 -- true-peak mode allocates nothing either");

    LookaheadLimiter lim;
    lim.prepare(kFs, 2, 2048);
    lim.setLookahead(288);
    lim.setTruePeak(true);
    std::vector<double> l(256, 3.0), r(256, -3.0), ol(256), orr(256);
    const double* ip[2] = {l.data(), r.data()};
    double* op[2] = {ol.data(), orr.data()};
    g_allocs.store(0);
    g_counting.store(true);
    for (int b = 0; b < 50; ++b) lim.process(ip, op, 256);
    g_counting.store(false);
    check(g_allocs.load() == 0, "fifty loud double blocks in true peak, no allocation");
}


// ---------------------------------------------------------------------------
// EX4 -- the brickwall as an offline stage, and the true-peak mode's edges
// ---------------------------------------------------------------------------

/// Planar channels of doubles.
using Planar = std::vector<std::vector<double>>;

/// Through the limiter in 256-frame blocks, any channel count, float or
/// double, optionally in place. Returns what came out, as doubles.
template <class Sample>
Planar streamAs(LookaheadLimiter& lim, const Planar& x, bool inPlace = false) {
    const std::size_t C = x.size(), n = x[0].size();
    std::vector<std::vector<Sample>> in(C, std::vector<Sample>(n)), out(C, std::vector<Sample>(n));
    for (std::size_t c = 0; c < C; ++c) {
        for (std::size_t i = 0; i < n; ++i) {
            in[c][i] = static_cast<Sample>(x[c][i]);
        }
    }
    std::vector<const Sample*> ip(C);
    std::vector<Sample*> op(C);
    for (std::size_t off = 0; off < n; off += 256) {
        const int k = static_cast<int>(std::min<std::size_t>(256, n - off));
        for (std::size_t c = 0; c < C; ++c) {
            ip[c] = in[c].data() + off;
            op[c] = (inPlace ? in[c].data() : out[c].data()) + off;
        }
        lim.process(ip.data(), op.data(), k);
    }
    const auto& result = inPlace ? in : out;
    Planar y(C);
    for (std::size_t c = 0; c < C; ++c) {
        y[c].assign(result[c].begin(), result[c].end());
    }
    return y;
}

/// processOffline over the whole programme, float or double, optionally in
/// place. Returns what came out, as doubles.
template <class Sample>
Planar offlineAs(LookaheadLimiter& lim, const Planar& x, bool inPlace = false) {
    const std::size_t C = x.size(), n = x.empty() ? 0 : x[0].size();
    std::vector<std::vector<Sample>> in(C, std::vector<Sample>(n)), out(C, std::vector<Sample>(n, Sample(7)));
    for (std::size_t c = 0; c < C; ++c) {
        for (std::size_t i = 0; i < n; ++i) {
            in[c][i] = static_cast<Sample>(x[c][i]);
        }
    }
    std::vector<const Sample*> ip(C);
    std::vector<Sample*> op(C);
    for (std::size_t c = 0; c < C; ++c) {
        ip[c] = in[c].data();
        op[c] = inPlace ? in[c].data() : out[c].data();
    }
    lim.processOffline(ip.data(), op.data(), static_cast<std::int64_t>(n));
    const auto& result = inPlace ? in : out;
    Planar y(C);
    for (std::size_t c = 0; c < C; ++c) {
        y[c].assign(result[c].begin(), result[c].end());
    }
    return y;
}

double truePeakOfPlanar(double fs, const Planar& y) {
    TruePeakMeter m;
    m.prepare(fs, static_cast<int>(y.size()));
    std::vector<const double*> p;
    for (const auto& ch : y) {
        p.push_back(ch.data());
    }
    m.process(p.data(), static_cast<int>(y.size()), static_cast<int>(y[0].size()));
    return m.peak();
}

double samplePeakOf(const Planar& y) {
    double peak = 0.0;
    for (const auto& ch : y) {
        for (double v : ch) {
            peak = std::max(peak, std::fabs(v));
        }
    }
    return peak;
}

/// AN INDEPENDENT TRUE PEAK, sharing nothing with TruePeakMeter::design: a
/// Kaiser-windowed sinc of +-48 samples (beta 9), read at 32 points in every
/// sample interval from `from` to `to`, each read with its whole kernel (48
/// samples either side). Slow, and only for tests. Its own accuracy is
/// checked against sines whose crest is known, before it judges the limiter.
double independentTruePeak(const Planar& y, std::size_t from, std::size_t to) {
    constexpr int kHalf = 48, kPoints = 32;
    constexpr double kBeta = 9.0;
    const auto besselI0 = [](double v) {
        double sum = 1.0, term = 1.0;
        for (int k = 1; k < 64; ++k) {
            term *= (v / (2.0 * k)) * (v / (2.0 * k));
            sum += term;
        }
        return sum;
    };
    // kernel[j][m + kHalf] weighs x[n + m] for the point n + j / kPoints.
    std::vector<double> kernel(static_cast<std::size_t>(kPoints) * (2 * kHalf + 1));
    for (int j = 0; j < kPoints; ++j) {
        const double frac = static_cast<double>(j) / kPoints;
        for (int m = -kHalf; m <= kHalf; ++m) {
            const double t = frac - static_cast<double>(m);   // distance from x[n + m]
            const double sinc = std::fabs(t) < 1e-12 ? 1.0 : std::sin(kPi * t) / (kPi * t);
            const double r = t / (kHalf + 1.0);
            const double w = std::fabs(r) >= 1.0 ? 0.0 : besselI0(kBeta * std::sqrt(1.0 - r * r)) / besselI0(kBeta);
            kernel[static_cast<std::size_t>(j * (2 * kHalf + 1) + m + kHalf)] = sinc * w;
        }
    }
    double peak = 0.0;
    for (const auto& ch : y) {
        const auto first = static_cast<int>(std::max<std::size_t>(from, kHalf));
        const auto last = static_cast<int>(std::min<std::size_t>(to, ch.size() - kHalf));
        for (int i = first; i < last; ++i) {
            peak = std::max(peak, std::fabs(ch[static_cast<std::size_t>(i)]));
            for (int j = 1; j < kPoints; ++j) {
                const double* k = kernel.data() + static_cast<std::size_t>(j * (2 * kHalf + 1));
                double v = 0.0;
                for (int m = -kHalf; m <= kHalf; ++m) {
                    v += k[m + kHalf] * ch[static_cast<std::size_t>(i + m)];
                }
                peak = std::max(peak, std::fabs(v));
            }
        }
    }
    return peak;
}

/// The fixtures the over-ceiling checks share: every one builds overs
/// between the samples.
struct Programme {
    std::string name;
    Planar x;
};

std::vector<Programme> overProgrammes(double rate, std::size_t n) {
    std::vector<Programme> out;
    {
        auto x = quarterRateAt45(n, 2.0);
        out.push_back({"quarter-rate sine at 45 degrees, +6 dBTP", {x, x}});
    }
    {
        Planar x(2, std::vector<double>(n));
        for (std::size_t i = 0; i < n; ++i) {
            x[0][i] = 3.0 * std::sin(2.0 * kPi * 0.2297 * static_cast<double>(i) + 0.3);
            x[1][i] = 2.0 * std::sin(2.0 * kPi * 997.0 * static_cast<double>(i) / rate);
        }
        out.push_back({"0.23 fs on the left (+9.5 dB), 997 Hz on the right", x});
    }
    {
        Planar x(2, std::vector<double>(n));
        std::uint32_t s = 2024u;
        for (std::size_t i = 0; i < n; ++i) {
            for (int c = 0; c < 2; ++c) {
                s = s * 1664525u + 1013904223u;
                x[static_cast<std::size_t>(c)][i] = 8.0 * ((s >> 8) / 16777216.0 - 0.5);
            }
        }
        out.push_back({"white noise at +12 dB", x});
    }
    {
        Planar x(2, std::vector<double>(n, 0.0));
        for (std::size_t i = n / 8; i + 1 < n; i += n / 4) {
            x[0][i] = 4.0;
            x[0][i + 1] = -4.0;
        }
        out.push_back({"+12 dB doublets", x});
    }
    return out;
}

void testTheOfflineBrickwallKeepsLengthAndAlignment() {
    section("EX4 -- offline, the latency is trimmed and the tail flushed: same length, aligned");

    const double ceiling = std::pow(10.0, -1.0 / 20.0);
    // N is prime, so no block size divides it.
    const std::size_t N = 10007;
    Planar under(2, std::vector<double>(N));
    {
        const auto l = quarterRateAt45(N, std::pow(10.0, -1.5 / 20.0));
        for (std::size_t i = 0; i < N; ++i) {
            under[0][i] = l[i];
            under[1][i] = 0.8 * std::sin(2.0 * kPi * 440.0 * static_cast<double>(i) / kFs);
        }
    }
    Planar over = overProgrammes(kFs, N)[2].x;
    over[0][N - 1] = 6.0;   // an over on the very last sample: only the flush reaches it

    for (bool tp : {false, true}) {
        for (int L : {0, 64, 288}) {
            const std::string at = std::string(tp ? "true peak" : "sample peak") + ", lookahead " + std::to_string(L);
            auto fresh = [&] {
                LookaheadLimiter lim;
                lim.prepare(kFs, 2, 1024);
                lim.setLookahead(L);
                lim.setCeiling(ceiling);
                lim.setTruePeak(tp);
                return lim;
            };
            // Below the ceiling: the input itself, the same length, not one
            // sample late -- bit for bit, in double and in float.
            {
                auto lim = fresh();
                const Planar y = offlineAs<double>(lim, under);
                check(y[0].size() == N && y[0] == under[0] && y[1] == under[1],
                      at + ", double: below the ceiling the output IS the input, aligned and the same length");
                auto limF = fresh();
                const Planar yf = offlineAs<float>(limF, under);
                bool same = yf[0].size() == N;
                for (std::size_t i = 0; i < N && same; ++i) {
                    same = yf[0][i] == static_cast<double>(static_cast<float>(under[0][i])) &&
                           yf[1][i] == static_cast<double>(static_cast<float>(under[1][i]));
                }
                check(same, at + ", float: the same");
            }
            // Over it: exactly what streaming gives once the latency is
            // dropped from the front and silence pushed through the back.
            {
                auto offline = fresh();
                const Planar y = offlineAs<double>(offline, over);
                auto streaming = fresh();
                const int d = streaming.latencySamples();
                Planar padded = over;
                for (auto& ch : padded) {
                    ch.resize(N + static_cast<std::size_t>(d), 0.0);
                }
                const Planar s = streamAs<double>(streaming, padded);
                bool same = y[0].size() == N && y[1].size() == N;
                for (std::size_t c = 0; c < 2 && same; ++c) {
                    same = std::equal(y[c].begin(), y[c].end(), s[c].begin() + d);
                }
                check(same, at + ": over the ceiling, the streamed output with its first " + std::to_string(d) +
                                " samples dropped and its tail flushed, bit for bit");
                check(samplePeakOf(y) <= ceiling * (1.0 + 1e-12) && std::fabs(y[0][N - 1]) > 0.5 * ceiling,
                      at + ": the last sample's over is limited, not cut off: " + std::to_string(y[0][N - 1]));
                auto inPlace = fresh();
                check(offlineAs<double>(inPlace, over, true) == y, at + ": in place, the same bit for bit");
            }
        }
    }

    // Edges: nothing, one sample, and a second programme on the same limiter
    // starting from silence rather than from the first one's tail.
    LookaheadLimiter lim;
    lim.prepare(kFs, 2, 1024);
    lim.setLookahead(64);
    lim.setCeiling(ceiling);
    lim.setTruePeak(true);
    std::vector<double> l0{7.0}, r0{7.0};
    double* op[2] = {l0.data(), r0.data()};
    lim.processOffline(static_cast<const double* const*>(op), op, 0);
    check(l0[0] == 7.0 && r0[0] == 7.0, "zero frames: nothing is written");
    const Planar one = offlineAs<double>(lim, Planar{{0.25}, {-0.5}});
    check(one[0][0] == 0.25 && one[1][0] == -0.5, "one frame below the ceiling: itself");
    const Planar first = offlineAs<double>(lim, over);
    const Planar second = offlineAs<double>(lim, over);
    check(first == second, "each programme starts from silence: the same programme twice, the same output");
}

void testTheTruePeakLimiterInFloatAndAtLowRates() {
    section("EX4 -- true peak in float, and at 32 and 22.05 kHz (6x and 8x)");

    const double ceilingDb = -1.0;
    const double ceiling = std::pow(10.0, ceilingDb / 20.0);
    const auto db = [](double v) { return 20.0 * std::log10(v); };
    // A float output is the double product rounded once: at most half an
    // ulp above it.
    const double floatCeiling = ceiling * (1.0 + std::ldexp(1.0, -24));

    for (double rate : {22050.0, 32000.0, 44100.0, 48000.0}) {
        for (int L : {0, 64}) {
            LookaheadLimiter probe;
            probe.prepare(rate, 2, 1024);
            probe.setLookahead(L);
            probe.setTruePeak(true);
            const std::string at = " at " + std::to_string(static_cast<int>(rate)) + " Hz, lookahead " + std::to_string(L);
            if (rate < 40000.0) {
                check(probe.latencySamples() == L + 10, "latency" + at + " is the lookahead + 10: " +
                                                            std::to_string(probe.latencySamples()));
            }
            for (const auto& f : overProgrammes(rate, 16384)) {
                for (bool asFloat : {false, true}) {
                    if (!asFloat && rate > 40000.0) {
                        continue;   // double at 44.1 and 48 kHz: the test above, and the reference's below
                    }
                    LookaheadLimiter lim;
                    lim.prepare(rate, 2, 1024);
                    lim.setLookahead(L);
                    lim.setCeiling(ceiling);
                    lim.setTruePeak(true);
                    const Planar y = asFloat ? streamAs<float>(lim, f.x) : streamAs<double>(lim, f.x);
                    const std::string what = f.name + at + (asFloat ? ", float" : ", double");
                    check(samplePeakOf(y) <= (asFloat ? floatCeiling : ceiling * (1.0 + 1e-12)),
                          what + ": every sample at or under the ceiling");
                    const double over = db(truePeakOfPlanar(rate, y)) - ceilingDb;
                    const double allowed = L > 0 ? 0.001 : 0.05;
                    check(over <= allowed, what + ": true peak " + std::to_string(over) +
                                               " dB against the ceiling (allowed " + std::to_string(allowed) + ")");
                }
            }
        }
    }
}

void testTheTruePeakLimiterInPlaceMonoAndSixChannels() {
    section("EX4 -- true peak in place, in mono, and on six linked channels");

    const double ceilingDb = -1.0;
    const double ceiling = std::pow(10.0, ceilingDb / 20.0);
    const auto db = [](double v) { return 20.0 * std::log10(v); };
    auto make = [&](int channels) {
        LookaheadLimiter lim;
        lim.prepare(kFs, channels, 1024);
        lim.setLookahead(64);
        lim.setCeiling(ceiling);
        lim.setTruePeak(true);
        return lim;
    };
    const Planar noise = overProgrammes(kFs, 8192)[2].x;
    {
        auto a = make(2), b = make(2), c = make(2), d = make(2);
        check(streamAs<double>(a, noise, true) == streamAs<double>(b, noise),
              "double, in place: the same as into other buffers, bit for bit");
        check(streamAs<float>(c, noise, true) == streamAs<float>(d, noise), "float, in place: the same");
    }
    {
        auto lim = make(1);
        const Planar y = streamAs<double>(lim, Planar{quarterRateAt45(8192, 2.0)});
        const double over = db(truePeakOfPlanar(kFs, y)) - ceilingDb;
        check(over <= 0.001, "mono: the quarter-rate sine's true peak " + std::to_string(over) + " dB against the ceiling");
    }
    {
        // Five quiet channels and one with the over: one gain for all six.
        Planar x(6, std::vector<double>(8192));
        for (std::size_t c = 0; c < 6; ++c) {
            for (std::size_t i = 0; i < 8192; ++i) {
                x[c][i] = 0.3 * std::sin(2.0 * kPi * (300.0 + 200.0 * static_cast<double>(c)) *
                                         static_cast<double>(i) / kFs);
            }
        }
        x[4] = quarterRateAt45(8192, 2.0);
        auto lim = make(6);
        const int d = lim.latencySamples();
        const Planar y = streamAs<double>(lim, x);
        const double over = db(truePeakOfPlanar(kFs, y)) - ceilingDb;
        check(over <= 0.001, "six channels: true peak " + std::to_string(over) + " dB against the ceiling");
        bool linked = true;
        double ducked = 1.0;
        for (std::size_t i = static_cast<std::size_t>(d); i < 8192 && linked; ++i) {
            const double in4 = x[4][i - static_cast<std::size_t>(d)];
            if (std::fabs(in4) < 0.1) {
                continue;
            }
            const double g = y[4][i] / in4;
            for (std::size_t c = 0; c < 6 && linked; ++c) {
                const double inC = x[c][i - static_cast<std::size_t>(d)];
                if (c != 4 && std::fabs(inC) > 0.05) {
                    linked = std::fabs(y[c][i] / inC - g) < 1e-9;
                    ducked = std::min(ducked, y[c][i] / inC);
                }
            }
        }
        check(linked && ducked < 0.6, "and the gain is linked: every quiet channel ducks with the loud one (to " +
                                          std::to_string(ducked) + ")");
    }
}

void testNonFiniteInputIsSilenceToTheLimiter() {
    section("EX4 -- a NaN or an infinity reads as silence, in detection and in the audio");

    const double ceiling = std::pow(10.0, -1.0 / 20.0);
    const double inf = std::numeric_limits<double>::infinity();
    const double nan = std::numeric_limits<double>::quiet_NaN();
    const Planar base = overProgrammes(kFs, 8192)[2].x;
    Planar poisoned = base, silenced = base;
    for (std::size_t i = 3000; i < 3300; ++i) {
        poisoned[0][i] = i % 2 == 1 ? nan : inf;
        poisoned[1][i] = i % 3 != 0 ? -inf : nan;
        silenced[0][i] = silenced[1][i] = 0.0;
    }
    poisoned[0][5000] = nan;   // and one alone
    silenced[0][5000] = 0.0;
    for (bool tp : {false, true}) {
        for (bool asFloat : {false, true}) {
            auto run = [&](const Planar& x) {
                LookaheadLimiter lim;
                lim.prepare(kFs, 2, 1024);
                lim.setLookahead(64);
                lim.setCeiling(ceiling);
                lim.setTruePeak(tp);
                return asFloat ? streamAs<float>(lim, x) : streamAs<double>(lim, x);
            };
            const Planar y = run(poisoned);
            bool finite = true;
            for (const auto& ch : y) {
                for (double v : ch) {
                    finite = finite && std::isfinite(v);
                }
            }
            const std::string what = std::string(tp ? "true peak" : "sample peak") + (asFloat ? ", float" : ", double");
            check(finite && samplePeakOf(y) <= ceiling * (1.0 + 1e-6),
                  what + ": every sample out is finite and under the ceiling");
            check(y == run(silenced), what + ": exactly the output of the same programme with silence there");
        }
    }
}

void testTheTruePeakLimiterAgainstAnIndependentReference() {
    section("EX4 -- the ceiling measured by an interpolator the limiter does not share");

    const double ceilingDb = -1.0;
    const double ceiling = std::pow(10.0, ceilingDb / 20.0);
    const auto db = [](double v) { return 20.0 * std::log10(v); };

    // The reference first: sines whose crest is 1, at awkward phases, up to
    // 0.41 of the rate (in cycles per sample: it reads any rate alike). From
    // about 0.47 of the rate its own response rolls off too.
    double worst = 0.0;
    for (double f : {0.0226, 0.125, 0.2304, 0.3125, 0.375, 0.41}) {
        for (double phase : {0.1, 0.77, 2.3}) {
            std::vector<double> x(1024);
            for (std::size_t i = 0; i < x.size(); ++i) {
                x[i] = std::sin(2.0 * kPi * f * static_cast<double>(i) + phase);
            }
            worst = std::max(worst, std::fabs(db(independentTruePeak(Planar{x}, 128, 896))));
        }
    }
    const double rates[] = {22050.0, 32000.0, 44100.0, 48000.0};
    check(worst < 0.01, "the reference reads known crests within " + std::to_string(worst) + " dB");

    // THE LIMITER IS HELD TO THE ANNEX'S READING, AND THE ANNEX READS 4x. The
    // tests above prove the output at the ceiling on that reading; this one
    // states how far an ideal reconstruction of the same output goes past
    // it. Two causes, both the 4x method's, not the gain's:
    //  - between its points. At 4x the interpolator reads at the odd eighths
    //    of each interval (TruePeakMeter::design), and the quarter-rate sine
    //    at 45 degrees has its crest at the half, an eighth from the nearest
    //    point: cos(pi / 16) is 0.168 dB low.
    //  - near the Nyquist frequency, where its 16 taps roll off: broadband
    //    noise at +12 dB, limited, reads 1.6 dB higher on the reference --
    //    and the reference rolls off there too, so for that content the
    //    figure is a lower bound on what an ideal reconstruction reads.
    // At 6x and 8x (32 and 22.05 kHz) the points are finer and the first
    // cause shrinks. The bounds are the measured values rounded up, so the gap
    // is on the record and cannot grow unseen. Whether the brickwall should
    // read finer than the meter is a decision for the director (the log).
    struct Gap {
        const char* name;
        double bound;   // dB over the ceiling, on the reference
    };
    const Gap gaps[] = {{"quarter-rate", 0.17}, {"0.23 fs", 0.02}, {"noise", 1.7}, {"doublets", 0.25}};
    for (double rate : rates) {
        for (const auto& f : overProgrammes(rate, 4096)) {
            LookaheadLimiter lim;
            lim.prepare(rate, 2, 1024);
            lim.setLookahead(64);
            lim.setCeiling(ceiling);
            lim.setTruePeak(true);
            const Planar y = streamAs<double>(lim, f.x);
            // Past the latency, with the kernel's whole span either side.
            const double over =
                db(independentTruePeak(y, static_cast<std::size_t>(lim.latencySamples()) + 64, y[0].size() - 64)) -
                ceilingDb;
            const double annex = db(truePeakOfPlanar(rate, y)) - ceilingDb;
            double bound = 0.0;
            for (const auto& g : gaps) {
                if (f.name.find(g.name) != std::string::npos) {
                    bound = g.bound;
                }
            }
            std::printf("    [measured] %s at %d Hz: %+.4f dB on the Annex's reading, %+.4f dB on the reference\n",
                        f.name.c_str(), static_cast<int>(rate), annex, over);
            check(bound > 0.0 && over <= bound,
                  f.name + " at " + std::to_string(static_cast<int>(rate)) + " Hz: on the reference " +
                      std::to_string(over) + " dB over the ceiling (bound " + std::to_string(bound) + ")");
        }
    }
}

// ===========================================================================
// RMSC
// ===========================================================================

double goertzel(const std::vector<double>& x, double freq, double fs) {
    const double w = 2.0 * kPi * freq / fs;
    const double coeff = 2.0 * std::cos(w);
    double s1 = 0.0, s2 = 0.0;
    for (double v : x) {
        const double s = v + coeff * s1 - s2;
        s2 = s1;
        s1 = s;
    }
    const double power = s1 * s1 + s2 * s2 - coeff * s1 * s2;
    return 2.0 * std::sqrt(std::max(power, 0.0)) / static_cast<double>(x.size());
}

std::vector<double> runRmsc(RingModSidechain& r, const std::vector<float>& main,
                            const std::vector<float>& key) {
    std::vector<float> out(main.size());
    for (std::size_t off = 0; off < main.size(); off += 256) {
        const int n = static_cast<int>(std::min<std::size_t>(256, main.size() - off));
        const float* ip[1] = {main.data() + off};
        float* op[1] = {out.data() + off};
        r.process(ip, op, 1, key.data() + off, n);
    }
    return std::vector<double>(out.begin(), out.end());
}

void testRmscIsInstantAndDepthZeroIsBypass() {
    section("ADR-0096 -- zero latency, and depth 0 changes nothing");

    std::vector<float> main(512, 0.8f), key(512, 0.0f);
    for (std::size_t i = 100; i < key.size(); ++i) key[i] = 1.0f;
    RingModSidechain r;
    r.prepare(kFs);
    r.setDepth(1.0);
    const std::vector<double> out = runRmsc(r, main, key);
    check(std::fabs(out[99] - 0.8) < 1e-7 && std::fabs(out[100]) < 1e-7,
          "the key's first sample ducks THAT sample -- no attack, no latency");

    r.setDepth(0.0);
    r.prepare(kFs);
    const std::vector<double> dry = runRmsc(r, main, key);
    bool same = true;
    for (std::size_t i = 0; i < dry.size(); ++i)
        if (dry[i] != static_cast<double>(main[i])) same = false;
    check(same, "at depth 0 the main signal is untouched, bit for bit");
}

void testAHotKeyNeverInvertsTheMusic() {
    section("ADR-0096 -- a key above full scale is clamped, not turned into a flip");

    // A kick peaking at +6 dBFS: |key| reaches 2, and unclamped the gain is
    // 1 - 2 = -1. The music comes out polarity-inverted on every kick.
    std::vector<float> main(4800), key(4800);
    for (std::size_t i = 0; i < main.size(); ++i) {
        main[i] = static_cast<float>(0.5 * std::sin(2.0 * kPi * 440.0 * static_cast<double>(i) / kFs));
        key[i] = static_cast<float>(2.0 * std::sin(2.0 * kPi * 50.0 * static_cast<double>(i) / kFs));
    }
    RingModSidechain r;
    r.prepare(kFs);
    r.setDepth(1.0);
    const std::vector<double> out = runRmsc(r, main, key);
    bool flipped = false;
    for (std::size_t i = 0; i < out.size(); ++i)
        if (out[i] * static_cast<double>(main[i]) < -1e-12) flipped = true;
    check(!flipped, "no output sample ever has the opposite sign to its input");
}

void testTheSidebandsAreACharacterYouCanTurnOff() {
    section("ADR-0096 -- RMSC's sidebands, and the smoothing that removes them");

    // Main 1 kHz, key 60 Hz. |sin 60 Hz| ripples at 120 Hz, so the raw effect
    // puts sidebands at 880 and 1120 Hz -- about -16 dB against the carrier at
    // depth 0.5. That is what RMSC sounds like. A 10 Hz smoothing on the
    // rectified key removes most of the ripple, and with it the sidebands.
    const std::size_t n = 48000 * 2;
    std::vector<float> main(n), key(n);
    for (std::size_t i = 0; i < n; ++i) {
        main[i] = static_cast<float>(0.5 * std::sin(2.0 * kPi * 1000.0 * static_cast<double>(i) / kFs));
        key[i] = static_cast<float>(0.9 * std::sin(2.0 * kPi * 60.0 * static_cast<double>(i) / kFs));
    }
    auto sidebandDb = [&](double smoothingHz) {
        RingModSidechain r;
        r.prepare(kFs);
        r.setDepth(0.5);
        r.setSmoothingHz(smoothingHz);
        const std::vector<double> all = runRmsc(r, main, key);
        const std::vector<double> tail(all.begin() + 48000, all.end());   // settled
        const double carrier = goertzel(tail, 1000.0, kFs);
        const double sb = std::max(goertzel(tail, 880.0, kFs), goertzel(tail, 1120.0, kFs));
        return 20.0 * std::log10(sb / carrier);
    };
    const double raw = sidebandDb(0.0);
    const double smooth = sidebandDb(10.0);
    check(raw > -20.0, "raw RMSC HAS sidebands: " + std::to_string(raw) + " dB");
    check(smooth < raw - 18.0,
          "10 Hz smoothing cuts them by more than 18 dB: " + std::to_string(smooth) + " dB");
}

void testRmscAllocatesNothing() {
    section("ADR-0010 -- RMSC's process() allocates nothing");

    std::vector<float> main(256, 0.5f), key(256, 0.7f), out(256);
    RingModSidechain r;
    r.prepare(kFs);
    r.setSmoothingHz(20.0);
    const float* ip[1] = {main.data()};
    float* op[1] = {out.data()};
    g_allocs.store(0);
    g_counting.store(true);
    for (int b = 0; b < 50; ++b) r.process(ip, op, 1, key.data(), 256);
    g_counting.store(false);
    check(g_allocs.load() == 0, "fifty blocks, no allocation");
}

void testRmscThresholdAndRelease() {
    section("ADR-0166 -- RMSC's threshold makes the duck total, its release keeps the attack instant");

    // A kick peaking at -12 dBFS against a -12 dB threshold: the bass is muted
    // outright, where the classic 0 dBFS scale would only take 75 % off it.
    const float kick = static_cast<float>(std::pow(10.0, -12.0 / 20.0));
    std::vector<float> main(2000, 0.5f), key(2000, 0.0f);
    for (std::size_t i = 200; i < 300; ++i) key[i] = kick;
    {
        RingModSidechain r;
        r.prepare(kFs);
        r.setDepth(1.0);
        r.setThresholdDb(-12.0);
        const std::vector<double> out = runRmsc(r, main, key);
        check(std::fabs(out[250]) < 1e-6, "a kick at the threshold mutes the bass completely");
        RingModSidechain classic;
        classic.prepare(kFs);
        classic.setDepth(1.0);
        const std::vector<double> c = runRmsc(classic, main, key);
        check(std::fabs(c[250] - 0.5 * (1.0 - kick)) < 1e-6, "at the default 0 dBFS it takes only |key| off");
        std::vector<float> quiet(2000, 0.0f);
        for (std::size_t i = 200; i < 300; ++i) quiet[i] = kick / 2.0f;
        RingModSidechain half;
        half.prepare(kFs);
        half.setDepth(1.0);
        half.setThresholdDb(-12.0);
        const std::vector<double> h = runRmsc(half, main, quiet);
        check(std::fabs(h[250] - 0.25) < 1e-6, "6 dB under the threshold ducks halfway");
    }
    {
        // Release 10 ms: instant down on the kick's first sample, then back up
        // with a 10 ms time constant: 1/e of the duck left 480 samples later.
        RingModSidechain r;
        r.prepare(kFs);
        r.setDepth(1.0);
        r.setThresholdDb(-12.0);
        r.setReleaseMs(10.0);
        std::vector<float> gain(main.size());
        std::vector<float> out(main.size());
        const float* ip[1] = {main.data()};
        float* op[1] = {out.data()};
        r.process(ip, op, 1, key.data(), static_cast<int>(main.size()), gain.data());
        check(gain[199] == 1.0f && std::fabs(gain[200]) < 1e-6f,
              "the release does not slow the attack: the kick's first sample is ducked fully");
        check(std::fabs(gain[299 + 480] - (1.0 - std::exp(-1.0))) < 2e-3,
              "10 ms after the kick ends, 1/e of the duck remains: " + std::to_string(gain[299 + 480]));
        bool rising = true;
        for (std::size_t i = 301; i < 1500; ++i)
            if (gain[i] < gain[i - 1]) rising = false;
        check(rising, "and the gain only climbs back after it: no ripple, no click");
        bool matches = true;
        for (std::size_t i = 0; i < main.size(); ++i)
            if (std::fabs(out[i] - main[i] * gain[i]) > 1e-6f) matches = false;
        check(matches, "gainOut is the gain that was applied, sample for sample");
    }
    {
        // Defaults unchanged: threshold 0 dBFS and release 0 are the classic
        // effect, bit for bit, however they were reached.
        std::vector<float> k2(4800);
        for (std::size_t i = 0; i < k2.size(); ++i)
            k2[i] = static_cast<float>(0.9 * std::sin(2.0 * kPi * 60.0 * static_cast<double>(i) / kFs));
        std::vector<float> m2(4800, 0.3f);
        RingModSidechain a, b;
        a.prepare(kFs);
        b.prepare(kFs);
        b.setThresholdDb(-20.0);
        b.setReleaseMs(50.0);
        b.setThresholdDb(0.0);
        b.setReleaseMs(0.0);
        a.setDepth(0.7);
        b.setDepth(0.7);
        const std::vector<double> x = runRmsc(a, m2, k2), y = runRmsc(b, m2, k2);
        check(x == y, "threshold 0 dBFS and release 0 are the classic RMSC, bit for bit");
    }
}

// ===========================================================================
// Auto gain (ADR-0166)
// ===========================================================================

/// Deterministic programme material: a 220 Hz tone under white noise, stereo.
std::vector<std::vector<float>> autoGainSource(int frames, double level) {
    std::vector<std::vector<float>> x(2, std::vector<float>(static_cast<std::size_t>(frames)));
    unsigned seed = 12345u;
    for (int i = 0; i < frames; ++i) {
        const double tone = std::sin(2.0 * kPi * 220.0 * i / kFs);
        for (int c = 0; c < 2; ++c) {
            seed = seed * 1664525u + 1013904223u;
            const double noise = (static_cast<double>(seed >> 8) / 16777216.0) * 2.0 - 1.0;
            x[static_cast<std::size_t>(c)][static_cast<std::size_t>(i)] =
                static_cast<float>(level * (0.7 * tone + 0.3 * noise));
        }
    }
    return x;
}

/// Runs `effect` on `dry` block by block with auto gain after it; returns the
/// output and records the gain (dB) at the end of every block.
template <class Effect>
std::vector<std::vector<float>> runAutoGain(AutoGain& ag,
                                            const std::vector<std::vector<float>>& dry,
                                            Effect effect, std::vector<double>* gains = nullptr,
                                            int block = 512) {
    const int frames = static_cast<int>(dry[0].size());
    std::vector<std::vector<float>> out = dry;
    for (int s = 0; s < frames; s += block) {
        const int n = frames - s < block ? frames - s : block;
        for (auto& ch : out)
            for (int i = s; i < s + n; ++i)
                ch[static_cast<std::size_t>(i)] = effect(ch[static_cast<std::size_t>(i)], i);
        const float* d[2] = {dry[0].data() + s, dry[1].data() + s};
        float* w[2] = {out[0].data() + s, out[1].data() + s};
        ag.process(d, w, 2, n);
        if (gains != nullptr) gains->push_back(ag.gainDb());
    }
    return out;
}

double rmsDb(const std::vector<float>& x, std::size_t from, std::size_t to) {
    double e = 0.0;
    for (std::size_t i = from; i < to; ++i) e += static_cast<double>(x[i]) * x[i];
    return 10.0 * std::log10(e / static_cast<double>(to - from));
}

void testAutoGainKWeightingIsTheStandard() {
    section("ADR-0166 -- auto gain K-weights both sides: BS.1770-4's own coefficients at 48 kHz");

    // ITU-R BS.1770-4, Tables 1 and 2.
    const Biquad s = AutoGain::kShelf(48000.0);
    near(s.b0, 1.53512485958697, 1e-9, "shelf b0");
    near(s.b1, -2.69169618940638, 1e-9, "shelf b1");
    near(s.b2, 1.19839281085285, 1e-9, "shelf b2");
    near(s.a1, -1.69065929318241, 1e-9, "shelf a1");
    near(s.a2, 0.73248077421585, 1e-9, "shelf a2");
    const Biquad h = AutoGain::kHighPass(48000.0);
    check(h.b0 == 1.0 && h.b1 == -2.0 && h.b2 == 1.0, "high-pass numerator 1, -2, 1");
    near(h.a1, -1.99004745483398, 1e-9, "high-pass a1");
    near(h.a2, 0.99007225036621, 1e-9, "high-pass a2");
    // And the curve, not just the numbers: the same shape at 44.1 and 96 kHz.
    for (double fs : {44100.0, 96000.0}) {
        near(magnitudeDb(AutoGain::kShelf(fs), fs, 10000.0),
             magnitudeDb(AutoGain::kShelf(48000.0), 48000.0, 10000.0), 0.05,
             "the shelf at 10 kHz is the same at " + std::to_string(static_cast<int>(fs)) + " Hz");
        near(magnitudeDb(AutoGain::kHighPass(fs), fs, 20.0),
             magnitudeDb(AutoGain::kHighPass(48000.0), 48000.0, 20.0), 0.05,
             "the high-pass at 20 Hz is the same at " + std::to_string(static_cast<int>(fs)) + " Hz");
    }
}

void testAutoGainUndoesALevelChange() {
    section("ADR-0166 -- auto gain puts the output back at the input's loudness, and leaves unity alone");

    const auto dry = autoGainSource(static_cast<int>(6 * kFs), 0.1);
    for (double db : {12.0, -12.0, 6.0}) {
        AutoGain ag;
        ag.prepare(kFs);
        const float g = static_cast<float>(std::pow(10.0, db / 20.0));
        const auto out = runAutoGain(ag, dry, [g](float x, int) { return x * g; });
        near(ag.gainDb(), -db, 0.05, "a " + std::to_string(static_cast<int>(db)) +
                                         " dB effect is answered with the opposite gain");
        const std::size_t n = dry[0].size();
        near(rmsDb(out[0], n - 48000, n), rmsDb(dry[0], n - 48000, n), 0.05,
             "and the last second out is as loud as the last second in");
    }
    {
        // From the first block: both windows start empty together, so their
        // ratio is right before either has filled.
        AutoGain ag;
        ag.prepare(kFs);
        std::vector<double> gains;
        runAutoGain(ag, dry, [](float x, int) { return x * 4.0f; }, &gains);
        near(gains[0], -12.04, 0.05, "right after the first block, not a second later");
    }
    {
        // Unity: the two sides are measured by the same filters from the same
        // samples, so the ratio is exactly 1 and the output is the input.
        AutoGain ag;
        ag.prepare(kFs);
        const auto out = runAutoGain(ag, dry, [](float x, int) { return x; });
        check(out == dry, "an effect that changes nothing is passed bit for bit");
    }
}

void testAutoGainHoldsThroughSilence() {
    section("ADR-0166 -- silence holds the gain: no +24 dB at the next note");

    constexpr int sec = static_cast<int>(kFs);
    auto dry = autoGainSource(26 * sec, 0.1);
    // Three seconds of programme, twenty of silence, three of programme again.
    for (auto& ch : dry)
        for (int i = 3 * sec; i < 23 * sec; ++i) ch[static_cast<std::size_t>(i)] = 0.0f;
    AutoGain ag;
    ag.prepare(kFs);
    std::vector<double> gains;
    runAutoGain(ag, dry, [](float x, int) { return x * 4.0f; }, &gains);
    const std::size_t before = static_cast<std::size_t>(3 * sec / 512 - 1);
    const std::size_t after = static_cast<std::size_t>(23 * sec / 512 + 1);
    near(gains[before], -12.04, 0.05, "-12 dB before the silence");
    double worst = 0.0;
    for (std::size_t b = before; b < gains.size(); ++b)
        worst = std::fmax(worst, std::fabs(gains[b] + 12.04));
    check(worst < 0.1, "held through twenty seconds of silence and the restart: worst " +
                           std::to_string(worst) + " dB off");
    near(gains[after], -12.04, 0.05, "and right on the first block after it");

    // An effect that mutes (a gate, a kill switch): the input goes on, the
    // output is silent. The ratio would climb to the limit; it holds instead.
    AutoGain mute;
    mute.prepare(kFs);
    const auto live = autoGainSource(8 * sec, 0.1);
    std::vector<double> mg;
    runAutoGain(mute, live, [](float x, int i) { return (i >= 2 * sec && i < 6 * sec) ? 0.0f : x * 2.0f; }, &mg);
    double loudest = -100.0;
    for (double g : mg) loudest = std::fmax(loudest, g);
    check(loudest < -5.9, "a muting effect never drives the gain up: at most " +
                              std::to_string(loudest) + " dB");
}

void testAutoGainLimitsAndSwitchesCleanly() {
    section("ADR-0166 -- auto gain stops at +/-24 dB; off ramps to unity and keeps measuring");

    const auto dry = autoGainSource(static_cast<int>(4 * kFs), 0.1);
    {
        // 40 dB is past what the gain corrects and inside what counts as live.
        AutoGain ag;
        ag.prepare(kFs);
        runAutoGain(ag, dry, [](float x, int) { return x * 100.0f; });
        near(ag.gainDb(), -24.0, 1e-9, "+40 dB is answered with -24 dB, no more");
        AutoGain up;
        up.prepare(kFs);
        runAutoGain(up, dry, [](float x, int) { return x * 0.01f; });
        near(up.gainDb(), 24.0, 1e-9, "-40 dB is answered with +24 dB, no more");
    }
    {
        AutoGain ag;
        ag.prepare(kFs);
        runAutoGain(ag, dry, [](float x, int) { return x * 2.0f; });
        ag.setEnabled(false);
        const std::vector<float> in(64, 0.05f);
        std::vector<float> l(64, 0.1f), r(64, 0.1f);   // the effect: x2
        const float* d[2] = {in.data(), in.data()};
        float* w[2] = {l.data(), r.data()};
        ag.process(d, w, 2, 64);
        near(ag.gainDb(), 0.0, 1e-12, "off: unity within one 32-frame ramp");
        check(l[32] == 0.1f && l[63] == 0.1f && r[63] == 0.1f,
              "and after the ramp the effect's own output passes untouched");
        ag.setEnabled(true);
        std::fill(l.begin(), l.end(), 0.1f);
        std::fill(r.begin(), r.end(), 0.1f);
        ag.process(d, w, 2, 32);
        near(ag.gainDb(), -6.02, 0.05, "back on: at the right gain in one ramp, not a second later");
    }
}

void testAutoGainAllocatesNothing() {
    section("ADR-0166 -- auto gain's process() allocates nothing");

    std::vector<float> a(256, 0.25f), b(256, 0.5f);
    AutoGain ag;
    ag.prepare(kFs);
    const float* d[2] = {a.data(), a.data()};
    float* w[2] = {b.data(), b.data()};
    g_allocs.store(0);
    g_counting.store(true);
    for (int i = 0; i < 50; ++i) ag.process(d, w, 2, 256);
    g_counting.store(false);
    check(g_allocs.load() == 0, "fifty blocks, no allocation");
}

// ===========================================================================
// True peak (ADR-0167 d5, ADR-0175)
// ===========================================================================

/// The true peak of 0.5 s of a stereo sine at `hz` and `phase`, amplitude 0.5.
double truePeakOfSine(TruePeakMeter& m, double fs, double hz, double phase, double* samplePeak = nullptr) {
    const int n = static_cast<int>(fs / 2.0);
    std::vector<float> x(static_cast<std::size_t>(n));
    double sp = 0.0;
    for (int i = 0; i < n; ++i) {
        x[static_cast<std::size_t>(i)] = static_cast<float>(0.5 * std::sin(2.0 * kPi * hz * i / fs + phase));
        sp = std::fmax(sp, std::fabs(static_cast<double>(x[static_cast<std::size_t>(i)])));
    }
    m.prepare(fs);
    // Steady state: a sine switched on at sample 0 really does ring past its
    // crest at the onset, and that is not what is being measured here.
    const float* ch[2] = {x.data(), x.data()};
    for (int s = 0; s < n; s += 512) {
        if (s == 2048) m.resetPeak();
        m.process(ch, 2, std::min(512, n - s));
        ch[0] += 512;
        ch[1] += 512;
    }
    if (samplePeak != nullptr) *samplePeak = sp;
    return m.peak();
}

void testTruePeakFindsTheCrestBetweenSamples() {
    section("ADR-0175 -- true peak finds the crest the samples miss (BS.1770-4 Annex 2's method)");

    TruePeakMeter m;
    m.prepare(44100.0);
    check(m.oversampling() == 4, "44.1 kHz: 4x");
    m.prepare(48000.0);
    check(m.oversampling() == 4, "48 kHz: 4x");
    m.prepare(96000.0);
    check(m.oversampling() == 2, "96 kHz: 2x");
    m.prepare(192000.0);
    check(m.oversampling() == 1, "192 kHz: none, already past 176.4 kHz");
    // DF-E20 (d): the header promises at least 176.4 kHz after oversampling;
    // at 32 kHz the old rule gave 4x, 128 kHz.
    m.prepare(32000.0);
    check(m.oversampling() == 6, "32 kHz: 6x, 192 kHz");
    m.prepare(22050.0);
    check(m.oversampling() == 8, "22.05 kHz: 8x, 176.4 kHz");
    for (double fs : {22050.0, 32000.0, 44100.0, 48000.0, 88200.0, 96000.0, 176400.0}) {
        m.prepare(fs);
        check(fs * m.oversampling() >= 176400.0 - 1e-6, "every rate reaches 176.4 kHz");
    }
    {
        // DEV-28: any channel count, and a double path. A crest only in the
        // twelfth channel is found; float and double input read the same.
        TruePeakMeter wide;
        wide.prepare(48000.0, 12);
        std::vector<std::vector<float>> f(12, std::vector<float>(4800, 0.f));
        std::vector<std::vector<double>> d(12, std::vector<double>(4800, 0.0));
        for (std::size_t i = 0; i < 4800; ++i) {
            const double v = 0.5 * std::sin(2.0 * kPi * 12000.0 * static_cast<double>(i) / 48000.0 + kPi / 4.0);
            f[11][i] = static_cast<float>(v);
            d[11][i] = static_cast<double>(f[11][i]);
        }
        std::vector<const float*> fp;
        std::vector<const double*> dp;
        for (int c = 0; c < 12; ++c) {
            fp.push_back(f[static_cast<std::size_t>(c)].data());
            dp.push_back(d[static_cast<std::size_t>(c)].data());
        }
        wide.process(fp.data(), 12, 4800);
        const double fromFloat = wide.peak();
        check(wide.channels() == 12 && 20.0 * std::log10(fromFloat) > -6.42,
              "a crest in the twelfth of twelve channels is found");
        wide.reset();
        wide.process(dp.data(), 12, 4800);
        check(wide.peak() == fromFloat, "the double path reads exactly what the float path reads");
    }

    // The classic case: a quarter of the rate, sampled 45 degrees off its crest.
    double sp = 0.0;
    const double tp = truePeakOfSine(m, 48000.0, 12000.0, kPi / 4.0, &sp);
    near(20.0 * std::log10(sp), -9.03, 0.02, "fs/4 at 45 degrees: the samples peak 3 dB under the crest");
    // Oversampling 4x leaves a grid: a crest between two of its points reads
    // low, by cos(pi / 16) = -0.17 dB at fs/4. That is the Annex's method, and
    // why EBU Tech 3341 (2023, section 2.6 and Table 1, signals 15 to 23) gives
    // a true-peak meter +0.2/-0.4 dB, the upsampling filter's error included.
    const double tpDb = 20.0 * std::log10(tp);
    check(tpDb > -6.42 && tpDb < -5.82, "and the true peak is the crest, -6.02 dBFS, within EBU Tech 3341's "
                                        "+0.2/-0.4 dB: " + std::to_string(tpDb));

    // Every frequency in the audible band, to 20 kHz, at four phases: within
    // EBU Tech 3341's +0.2/-0.4 dB of the crest, and never under the sample
    // peak. (38.4 kHz at 96 kHz reads 0.44 dB low; nobody hears it.)
    double worstLow = 0.0, worstHigh = 0.0;
    bool neverUnder = true;
    for (double fs : {48000.0, 96000.0}) {
        for (double frac : {0.002, 0.02, 0.1, 0.2, 0.3, 0.35, 0.4}) {
            if (frac * fs > 20000.0) continue;
            for (double phase : {0.0, 0.3, 0.77, 1.3}) {
                double s = 0.0;
                const double p = truePeakOfSine(m, fs, frac * fs, phase, &s);
                const double err = 20.0 * std::log10(p / 0.5);
                worstLow = std::fmin(worstLow, err);
                worstHigh = std::fmax(worstHigh, err);
                neverUnder = neverUnder && p >= s;
            }
        }
    }
    std::printf("        true-peak error, to 20 kHz at 48 and 96 kHz: %+.3f to %+.3f dB\n", worstLow, worstHigh);
    check(worstLow > -0.4 && worstHigh < 0.2, "every sine to 20 kHz reads within +0.2/-0.4 dB of its crest");
    check(neverUnder, "and never under its sample peak");

    // Past 176.4 kHz the samples are dense enough: true peak is sample peak.
    const double tp192 = truePeakOfSine(m, 192000.0, 1000.0, 0.3, &sp);
    check(tp192 == sp, "at 192 kHz the true peak is the sample peak, exactly");

    m.prepare(48000.0);
    std::vector<float> a(256, 0.25f);
    const float* ch[2] = {a.data(), a.data()};
    g_allocs.store(0);
    g_counting.store(true);
    for (int i = 0; i < 50; ++i) m.process(ch, 2, 256);
    g_counting.store(false);
    check(g_allocs.load() == 0, "fifty blocks, no allocation");
    m.reset();
    check(m.peak() == 0.0, "reset clears the held peak");

    float signal[16] = {};
    for (int i = 1; i < 16; ++i)
        signal[i] = (i % 4) < 2 ? 0.75f : -0.75f;
    const float* input[] = {signal};
    for (double rate : {48000.0, 192000.0}) {
        signal[0] = 0.0f;
        m.prepare(rate);
        m.process(input, 1, 16);
        const double reference = m.peak();
        check(rate == 48000.0 ? reference > 0.75 : reference == 0.75,
              "the finite fixture exercises interpolation or the sample-only path");
        for (float invalid : {std::numeric_limits<float>::quiet_NaN(),
                              std::numeric_limits<float>::infinity(),
                              -std::numeric_limits<float>::infinity()}) {
            signal[0] = invalid;
            m.reset();
            m.process(input, 1, 1);
            const float* rest[] = {signal + 1};
            m.process(rest, 1, 15);
            check(m.peak() == reference,
                  "a non-finite sample is silence across blocks at " + std::to_string(rate));
        }
    }
}

// ===========================================================================
// The Pd patches, checked structurally (they are unrun: no Pd on this machine)
// ===========================================================================

/// Just enough of Pd's file format to check a patch's wiring: canvases,
/// objects in creation order, and connections by index.
struct PdCanvas {
    std::string name;                     ///< "" for the top level
    std::vector<std::string> objs;        ///< the text after "#X obj x y", or "pd <name>"
    std::vector<int> child;               ///< index into canvases for a [pd ...], else -1
    struct Conn { int from, outlet, to, inlet; };
    std::vector<Conn> conns;
};

struct PdFile {
    std::vector<PdCanvas> canvases;       ///< [0] is the top level
    std::string error;
};

PdFile parsePd(const std::string& path) {
    PdFile f;
    // std::ifstream rather than fopen, which MSVC deprecates -- and -Werror
    // makes a deprecation a build failure.
    std::ifstream file(path, std::ios::binary);
    if (!file) { f.error = "cannot open " + path; return f; }
    std::ostringstream slurp;
    slurp << file.rdbuf();
    const std::string text = slurp.str();

    std::vector<int> stack;
    std::istringstream lines(text);
    std::string line;
    while (std::getline(lines, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) continue;
        if (line.back() == ';') line.pop_back();
        std::istringstream in(line);
        std::string a, b;
        in >> a >> b;
        if (a == "#N" && b == "canvas") {
            PdCanvas c;
            int x, y, w, h;
            in >> x >> y >> w >> h;
            std::string name;
            if (!stack.empty()) in >> name;          // a subpatch header names itself
            c.name = name;
            f.canvases.push_back(c);
            stack.push_back(static_cast<int>(f.canvases.size()) - 1);
        } else if (a == "#X" && stack.empty()) {
            f.error = "an #X record outside any canvas";
            return f;
        } else if (a == "#X" && b == "restore") {
            const int child = stack.back();
            stack.pop_back();
            if (stack.empty()) { f.error = "restore with no parent"; return f; }
            PdCanvas& parent = f.canvases[static_cast<std::size_t>(stack.back())];
            parent.objs.push_back("pd " + f.canvases[static_cast<std::size_t>(child)].name);
            parent.child.push_back(child);
        } else if (a == "#X" && (b == "obj" || b == "msg" || b == "text" || b == "floatatom")) {
            int x, y;
            in >> x >> y;
            std::string rest;
            std::getline(in, rest);
            if (!rest.empty() && rest.front() == ' ') rest.erase(0, 1);
            PdCanvas& c = f.canvases[static_cast<std::size_t>(stack.back())];
            c.objs.push_back(b == "obj" ? rest : "#" + b + " " + rest);
            c.child.push_back(-1);
        } else if (a == "#X" && b == "connect") {
            PdCanvas::Conn k{};
            in >> k.from >> k.outlet >> k.to >> k.inlet;
            f.canvases[static_cast<std::size_t>(stack.back())].conns.push_back(k);
        }
    }
    if (stack.size() != 1) f.error = "unbalanced canvases";
    return f;
}

std::string pdPath(const char* name) { return std::string(ADI_SOURCE_DIR) + "/pd/" + name; }

/// Every connection names an object that exists, in every canvas.
bool wiringValid(const PdFile& f, std::string& why) {
    for (const PdCanvas& c : f.canvases)
        for (const PdCanvas::Conn& k : c.conns) {
            const int n = static_cast<int>(c.objs.size());
            if (k.from < 0 || k.from >= n || k.to < 0 || k.to >= n || k.outlet < 0 || k.inlet < 0) {
                why = "canvas '" + c.name + "': connect " + std::to_string(k.from) + " " +
                      std::to_string(k.outlet) + " " + std::to_string(k.to) + " " +
                      std::to_string(k.inlet) + " with " + std::to_string(n) + " objects";
                return false;
            }
        }
    return true;
}

int findObj(const PdCanvas& c, const std::string& text) {
    for (std::size_t i = 0; i < c.objs.size(); ++i)
        if (c.objs[i] == text) return static_cast<int>(i);
    return -1;
}

bool startsWith(const std::string& s, const std::string& p) {
    return s.compare(0, p.size(), p) == 0;
}

std::vector<PdCanvas::Conn> from(const PdCanvas& c, int obj) {
    std::vector<PdCanvas::Conn> out;
    for (const auto& k : c.conns) if (k.from == obj) out.push_back(k);
    return out;
}

void testTheLimiterPatchIsWiredAsDesigned() {
    section("ADR-0095/0096 -- the Pd limiter: peak, whole samples, and a correct report");

    const PdFile f = parsePd(pdPath("adi-limiter.pd"));
    check(f.error.empty(), "it parses: " + f.error);
    if (!f.error.empty()) return;
    std::string why;
    check(wiringValid(f, why), "every connection names a real object " + why);
    const PdCanvas& top = f.canvases[0];

    bool env = false, peak = false, clip = false;
    for (const PdCanvas& c : f.canvases)
        for (const std::string& o : c.objs) {
            if (startsWith(o, "env~")) env = true;
            if (startsWith(o, "fexpr~") && o.find("max(") != std::string::npos) peak = true;
            if (startsWith(o, "clip~")) clip = true;
        }
    check(!env, "no env~ anywhere -- env~ is RMS, and RMS lets a spike through");
    check(peak, "a PEAK follower: fexpr~ with max()");
    check(clip, "and a clip~ as the safety net at the ceiling");

    // THE REPORT READS THE RATE FIRST. The query must reach a trigger, not
    // fan out -- Pd does not order a fan-out, and an unordered one can report
    // at the old rate (the first version of this patch did exactly that).
    const int query = findObj(top, "r \\$0-query_latency");
    check(query >= 0, "it answers $0-query_latency");
    const auto qOut = from(top, query);
    check(qOut.size() == 1 && top.objs[static_cast<std::size_t>(qOut[0].to)] == "t b b",
          "and the query goes to ONE place, a [t b b], not a fan-out");
    const int trig = qOut.empty() ? -1 : qOut[0].to;
    int srObj = -1, keepObj = -1;
    for (const auto& k : from(top, trig)) {
        const std::string& t = top.objs[static_cast<std::size_t>(k.to)];
        if (k.outlet == 1 && t == "samplerate~") srObj = k.to;
        if (k.outlet == 0 && startsWith(t, "f ")) keepObj = k.to;
    }
    check(srObj >= 0, "the trigger's RIGHT outlet -- which fires first -- reads the rate");
    check(keepObj >= 0, "and its left outlet, which fires second, releases the lookahead");

    // THE RATE REACHES THE ARITHMETIC. The first version stored it in an [f]
    // that nothing banged, so the report was always 0.
    const int report = findObj(top, "s \\$0-report_latency");
    check(report >= 0, "it reports on $0-report_latency");
    int calc = -1;
    for (const auto& k : top.conns)
        if (k.to == report && startsWith(top.objs[static_cast<std::size_t>(k.from)], "expr int("))
            calc = k.from;
    check(calc >= 0, "the report is a whole number of samples: expr int(...)");
    bool rateIn = false;
    for (const auto& k : from(top, srObj)) if (k.to == calc && k.inlet == 1) rateIn = true;
    check(rateIn, "and samplerate~ feeds that arithmetic DIRECTLY, not through a parked [f]");

    // WRITE SORTED BEFORE READ: separate subpatches, joined by a connection.
    const int w = findObj(top, "pd write"), r = findObj(top, "pd read");
    check(w >= 0 && r >= 0, "delwrite~ and delread~ live in [pd write] and [pd read]");
    bool ordered = false;
    for (const auto& k : from(top, w)) if (k.to == r) ordered = true;
    check(ordered, "and [pd write] is wired into [pd read], which is what orders Pd's DSP sort");
    bool dwInWrite = false, drInRead = false;
    if (w >= 0) for (const auto& o : f.canvases[static_cast<std::size_t>(top.child[static_cast<std::size_t>(w)])].objs)
        if (startsWith(o, "delwrite~")) dwInWrite = true;
    if (r >= 0) for (const auto& o : f.canvases[static_cast<std::size_t>(top.child[static_cast<std::size_t>(r)])].objs)
        if (startsWith(o, "delread~")) drInRead = true;
    check(dwInWrite && drInRead, "with each delay object in the subpatch its name says");
}

void testTheEqPatchTakesItsCoefficientsFromTheHost() {
    section("ADR-0096 -- the Pd EQ: 8 bands x 4 sections, and no maths of its own");

    const PdFile f = parsePd(pdPath("adi-eq8.pd"));
    check(f.error.empty(), "it parses: " + f.error);
    if (!f.error.empty()) return;
    std::string why;
    check(wiringValid(f, why), "every connection names a real object " + why);
    const PdCanvas& top = f.canvases[0];

    int biquads = 0;
    bool expr = false;
    for (const std::string& o : top.objs) {
        if (startsWith(o, "biquad~")) ++biquads;
        if (o.find("expr") != std::string::npos) expr = true;
    }
    check(biquads == 64, "64 biquad~: 8 bands x 4 sections x 2 channels, got " +
                             std::to_string(biquads));
    check(!expr,
          "no expr in the patch: the coefficients -- and the SIGN FLIP -- come from "
          "dsp::toPd on the host, where they are tested");

    int receives = 0, wiredToBoth = 0;
    for (int b = 1; b <= 8; ++b)
        for (int k = 1; k <= 4; ++k) {
            const int idx = findObj(top, "r \\$0-b" + std::to_string(b) + "-s" + std::to_string(k));
            if (idx < 0) continue;
            ++receives;
            const auto out = from(top, idx);
            int toBiquad = 0;
            for (const auto& c : out)
                if (startsWith(top.objs[static_cast<std::size_t>(c.to)], "biquad~") && c.inlet == 0)
                    ++toBiquad;
            if (toBiquad == 2) ++wiredToBoth;
        }
    check(receives == 32, "a receive for every section: " + std::to_string(receives));
    check(wiredToBoth == 32, "each feeding the same section on BOTH channels: " +
                                 std::to_string(wiredToBoth));
}

void testTheRmscPatchClampsAndMultiplies() {
    section("ADR-0096 -- the Pd RMSC: rectify, clamp, multiply");

    const PdFile f = parsePd(pdPath("adi-rmsc.pd"));
    check(f.error.empty(), "it parses: " + f.error);
    if (!f.error.empty()) return;
    std::string why;
    check(wiringValid(f, why), "every connection names a real object " + why);
    const PdCanvas& top = f.canvases[0];
    check(findObj(top, "abs~") >= 0, "the key is rectified with abs~");
    check(findObj(top, "clip~ 0 1") >= 0,
          "and CLAMPED to 0..1, so a hot key cannot invert the music");
    check(findObj(top, "expr~ 1 - $v1") >= 0, "the gain is 1 - depth * env");
    int mul = 0;
    for (const std::string& o : top.objs) if (o == "*~") ++mul;
    check(mul == 2, "and it MULTIPLIES each channel by it -- never subtracts the key");
}

}  // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::printf("adi_dsp_tests -- the DSP corrections\n\n");
    testPdNeedsTheFeedbackNegated();
    testTheCookbookDesignsSayWhatTheyMean();
    testSteepCutsAreButterworthCascades();
    testEveryDesignIsStable();
    testThePdMessageCarriesTheExactDoubles();
    testTheLimiterIsTransparentBelowTheCeiling();
    testTheLimiterNeverPassesTheCeiling();
    testTheAttackIsARampAndTheReleaseIsSlow();
    testLimiterReleaseTimeSurvivesPrepare();
    testRmsDetectionWouldHaveMissedTheSpike();
    testTheLimiterIsLinkedAcrossChannels();
    testTheLimiterAllocatesNothing();
    testTheTruePeakLimiterIsAPureDelayBelowTheCeiling();
    testTheTruePeakLimiterHoldsTheCeilingBetweenTheSamples();
    testTheTruePeakLimiterAllocatesNothing();
    testTheOfflineBrickwallKeepsLengthAndAlignment();
    testTheTruePeakLimiterInFloatAndAtLowRates();
    testTheTruePeakLimiterInPlaceMonoAndSixChannels();
    testNonFiniteInputIsSilenceToTheLimiter();
    testTheTruePeakLimiterAgainstAnIndependentReference();
    testRmscIsInstantAndDepthZeroIsBypass();
    testAHotKeyNeverInvertsTheMusic();
    testTheSidebandsAreACharacterYouCanTurnOff();
    testRmscAllocatesNothing();
    testRmscThresholdAndRelease();
    testAutoGainKWeightingIsTheStandard();
    testAutoGainUndoesALevelChange();
    testAutoGainHoldsThroughSilence();
    testAutoGainLimitsAndSwitchesCleanly();
    testAutoGainAllocatesNothing();
    testTruePeakFindsTheCrestBetweenSamples();
    testTheLimiterPatchIsWiredAsDesigned();
    testTheEqPatchTakesItsCoefficientsFromTheHost();
    testTheRmscPatchClampsAndMultiplies();
    std::printf("\n%s -- %d checks, %d failure(s)\n",
                g_failures ? "FAILED" : "PASS", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
