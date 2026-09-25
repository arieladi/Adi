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
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
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

    // The classic case: a quarter of the rate, sampled 45 degrees off its crest.
    double sp = 0.0;
    const double tp = truePeakOfSine(m, 48000.0, 12000.0, kPi / 4.0, &sp);
    near(20.0 * std::log10(sp), -9.03, 0.02, "fs/4 at 45 degrees: the samples peak 3 dB under the crest");
    // Oversampling 4x leaves a grid: a crest between two of its points reads
    // low, by cos(pi / 16) = -0.17 dB at fs/4. That is the Annex's method, and
    // why EBU Tech 3341 gives a true-peak meter +0.2/-0.4 dB.
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
    testRmsDetectionWouldHaveMissedTheSpike();
    testTheLimiterIsLinkedAcrossChannels();
    testTheLimiterAllocatesNothing();
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
