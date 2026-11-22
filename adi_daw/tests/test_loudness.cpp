// SPDX-License-Identifier: GPL-3.0-or-later
//
// DEV-28: the one loudness meter, proved against references from outside it.
//
//   - EBU Tech 3341 (2023) minimum-requirement signals, synthesised here:
//     cases 1-6 (I, M, S), 9 and 12 (S and M constant), 10, 11, 13 and 14
//     (maximum S and M, file-based and live), 15-23 (true peak; 20-23 made at
//     4 fs, low-passed and decimated at offsets 0-3 as the table says).
//     Cases 7 and 8 are authentic programme files and are not used.
//   - EBU Tech 3342 (2023) cases 1-4 (loudness range); 5 and 6 are files.
//   - A brute-force BS.1770 reference written here, with no histogram: the
//     same K-weighting, then every 400 ms gating block, both gates and Tech
//     3342's MATLAB percentile rule, evaluated exactly. The meter must agree
//     with it within 0.02 LU (its 0.01 LU bins), and with the EBU's tables
//     within their tolerances.
//   - 44.1, 48 and 96 kHz; any block size gives the same readings; process()
//     allocates nothing; pause keeps a section out of I, LRA and the maxima.

#include "adi/dsp/auto_gain.hpp"
#include "adi/dsp/loudness.hpp"

#include "true_peak_reference.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <limits>
#include <new>
#include <numbers>
#include <string>
#include <vector>

namespace {
bool counting = false;
std::size_t allocations = 0;
}  // namespace
void* operator new(std::size_t n) {
    if (counting) ++allocations;
    if (auto* p = std::malloc(n ? n : 1)) return p;
    throw std::bad_alloc();
}
void* operator new[](std::size_t n) { return operator new(n); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }

namespace {

using Meter = adi::dsp::LoudnessMeter;
constexpr double pi = std::numbers::pi;
int checks = 0, failures = 0;

void check(bool ok, const std::string& what) {
    ++checks;
    if (!ok) {
        ++failures;
        std::printf("FAIL %s\n", what.c_str());
    }
}
bool near(double a, double b, double tolerance) { return std::abs(a - b) <= tolerance; }

using Signal = std::vector<std::vector<double>>;   // [channel][frame]

std::vector<float> floats(const std::vector<double>& v) {
    std::vector<float> f(v.size());
    for (std::size_t i = 0; i < v.size(); ++i) f[i] = static_cast<float>(v[i]);
    return f;
}

/// A 1 kHz sine, in phase on every channel, at `dbfs` per-channel peak.
struct Segment {
    double seconds;
    double dbfs;   // -inf for silence
};
Signal tones(double rate, const std::vector<Segment>& segments, std::vector<double> gainsDb = {0, 0}) {
    Signal x(gainsDb.size());
    double t = 0;
    for (const auto& s : segments) {
        const auto n = static_cast<std::size_t>(std::llround(s.seconds * rate));
        for (std::size_t c = 0; c < x.size(); ++c) {
            const double a = std::isfinite(s.dbfs) ? std::pow(10.0, (s.dbfs + gainsDb[c]) / 20.0) : 0.0;
            for (std::size_t i = 0; i < n; ++i)
                x[c].push_back(a * std::sin(2 * pi * 1000.0 * (t + static_cast<double>(i) / rate)));
        }
        t += static_cast<double>(n) / rate;
    }
    return x;
}

/// Runs a signal through a fresh meter in blocks of `block` (floats).
Meter::Readings measure(const Signal& x, double rate, const std::vector<double>& weights,
                        int block = 512, const std::function<void(Meter&, std::size_t)>& each = {}) {
    Meter m;
    m.prepare(rate, weights);
    std::vector<std::vector<float>> f(x.size());
    for (std::size_t c = 0; c < x.size(); ++c) f[c] = floats(x[c]);
    std::vector<const float*> p(x.size());
    const std::size_t total = x.empty() ? 0 : x[0].size();
    for (std::size_t at = 0; at < total; at += static_cast<std::size_t>(block)) {
        const int n = static_cast<int>(std::min<std::size_t>(static_cast<std::size_t>(block), total - at));
        for (std::size_t c = 0; c < x.size(); ++c) p[c] = f[c].data() + at;
        m.process(p.data(), n);
        if (each) each(m, at + static_cast<std::size_t>(n));
    }
    return m.readings();
}

/// The brute-force reference: K-weighted energy per frame, then BS.1770's
/// gating blocks and Tech 3342's algorithm exactly, with sorting.
struct Reference {
    double integrated = -Meter::kInfinity, range = 0;
    /// M and S at the end of every 10 ms sub-block (-inf before their windows
    /// first fill), and the frame each sub-block ends at: the meter's maxima
    /// are the largest of these.
    std::vector<double> momentary, shortTerm;
    std::vector<std::size_t> ends;
};

/// The largest of a reference's per-sub-block readings over the sub-blocks
/// that have ended by `frames`.
double largestBy(const std::vector<double>& readings, const std::vector<std::size_t>& ends, std::size_t frames) {
    double largest = -Meter::kInfinity;
    for (std::size_t k = 0; k < readings.size() && ends[k] <= frames; ++k) largest = std::max(largest, readings[k]);
    return largest;
}
Reference reference(const Signal& x, double rate, const std::vector<double>& weights) {
    const auto frames = x[0].size();
    std::vector<double> e(frames, 0.0);
    for (std::size_t c = 0; c < x.size(); ++c) {
        const auto s = adi::dsp::AutoGain::kShelf(rate), h = adi::dsp::AutoGain::kHighPass(rate);
        double s1 = 0, s2 = 0, sy1 = 0, sy2 = 0, h1 = 0, h2 = 0, hy1 = 0, hy2 = 0;
        for (std::size_t i = 0; i < frames; ++i) {
            const double v = x[c][i];
            const double y = s.b0 * v + s.b1 * s1 + s.b2 * s2 - s.a1 * sy1 - s.a2 * sy2;
            s2 = s1; s1 = v; sy2 = sy1; sy1 = y;
            const double z = h.b0 * y + h.b1 * h1 + h.b2 * h2 - h.a1 * hy1 - h.a2 * hy2;
            h2 = h1; h1 = y; hy2 = hy1; hy1 = z;
            e[i] += weights[c] * z * z;
        }
    }
    // Window means from running sums of the energy, exact to about 1e-13 of a
    // window's energy here: every 400 ms and 3 s window without walking it.
    std::vector<double> prefix(frames + 1, 0.0);
    for (std::size_t i = 0; i < frames; ++i) prefix[i + 1] = prefix[i] + e[i];
    auto mean = [&](std::size_t from, std::size_t to) {
        return (prefix[to] - prefix[from]) / static_cast<double>(to - from);
    };
    auto boundary = [&](std::int64_t k) { return static_cast<std::size_t>(std::llround(static_cast<double>(k) * rate / 100.0)); };
    Reference r;
    // Every sub-block's M and S.
    auto windowed = [&](std::int64_t k, std::int64_t length) {
        return Meter::loudness(mean(boundary(k - length), boundary(k)));
    };
    for (std::int64_t k = 1; boundary(k) <= frames; ++k) {
        r.ends.push_back(boundary(k));
        r.momentary.push_back(k >= 40 ? windowed(k, 40) : -Meter::kInfinity);
        r.shortTerm.push_back(k >= 300 ? windowed(k, 300) : -Meter::kInfinity);
    }
    std::vector<double> blocks, shortTerm;
    for (std::int64_t k = 40; boundary(k) <= frames; k += 10) {
        const double l = Meter::loudness(mean(boundary(k - 40), boundary(k)));
        if (l > -70) blocks.push_back(l);
    }
    for (std::int64_t k = 300; boundary(k) <= frames; k += 10) {
        const double l = Meter::loudness(mean(boundary(k - 300), boundary(k)));
        if (l > -70) shortTerm.push_back(l);
    }
    auto power = [](const std::vector<double>& v) {
        double sum = 0;
        for (double l : v) sum += std::pow(10.0, (l + 0.691) / 10.0);
        return sum / static_cast<double>(v.size());
    };
    if (!blocks.empty()) {
        const double gate = Meter::loudness(power(blocks)) - 10.0;
        std::vector<double> kept;
        for (double l : blocks) if (l >= gate) kept.push_back(l);
        if (!kept.empty()) r.integrated = Meter::loudness(power(kept));
    }
    if (!shortTerm.empty()) {
        const double gate = Meter::loudness(power(shortTerm)) - 20.0;
        std::vector<double> kept;
        for (double l : shortTerm) if (l >= gate) kept.push_back(l);
        std::sort(kept.begin(), kept.end());
        const auto n = static_cast<double>(kept.size());
        auto at = [&](double p) {
            const double i = (n - 1) * p / 100.0 + 1.0;
            return kept[static_cast<std::size_t>(std::floor(i + 0.5)) - 1];
        };
        if (!kept.empty()) r.range = at(95) - at(10);
    }
    return r;
}

const std::vector<double> kStereo{1.0, 1.0};
constexpr double kInf = Meter::kInfinity;

// --- Tech 3341 cases 1-6: I (and M, S) ------------------------------------------

void integratedCases(double rate, bool all) {
    const std::string at = " at " + std::to_string(static_cast<int>(rate)) + " Hz";
    struct Case { int number; std::vector<Segment> segments; double expected; bool ms; };
    const std::vector<Case> cases{
        {1, {{20, -23}}, -23, true},
        {2, {{20, -33}}, -33, true},
        {3, {{10, -36}, {60, -23}, {10, -36}}, -23, false},
        {4, {{10, -72}, {10, -36}, {60, -23}, {10, -36}, {10, -72}}, -23, false},
        {5, {{20, -26}, {20.1, -20}, {20, -26}}, -23, false},
    };
    for (const auto& c : cases) {
        if (!all && c.number != 1 && c.number != 5) continue;
        const auto x = tones(rate, c.segments);
        const auto r = measure(x, rate, kStereo);
        const auto ref = reference(x, rate, kStereo);
        const std::string name = "Tech 3341 case " + std::to_string(c.number) + at;
        check(near(r.integrated, c.expected, 0.1), name + ": I = " + std::to_string(c.expected) + " +-0.1 (" + std::to_string(r.integrated) + ")");
        check(near(r.integrated, ref.integrated, 0.02), name + ": I agrees with the brute-force reference within 0.02 LU");
        if (c.ms)
            check(near(r.momentary, c.expected, 0.1) && near(r.shortTerm, c.expected, 0.1),
                  name + ": M and S = " + std::to_string(c.expected) + " +-0.1");
    }
    // Case 6: 5.0, 1 kHz, L R -28, C -24, Ls Rs -30 dBFS, BS.1770's weights.
    const std::vector<double> weights{1, 1, 1, Meter::kSurroundWeight, Meter::kSurroundWeight};
    const auto x = tones(rate, {{20, 0}}, {-28, -28, -24, -30, -30});
    const auto r = measure(x, rate, weights);
    check(near(r.integrated, -23, 0.1), "Tech 3341 case 6" + at + ": 5.0 I = -23 +-0.1 (" + std::to_string(r.integrated) + ")");
    check(near(r.integrated, reference(x, rate, weights).integrated, 0.02), "Tech 3341 case 6" + at + ": agrees with the reference");
}

// --- Tech 3341 cases 9-14: constant and maximum M and S -------------------------

void momentaryAndShortTerm() {
    const double rate = 48000;
    {   // 9: (1.34 s at -20, 1.66 s at -30) x5: S = -23 constant after 3 s.
        std::vector<Segment> s;
        for (int i = 0; i < 5; ++i) { s.push_back({1.34, -20}); s.push_back({1.66, -30}); }
        double worst = 0;
        measure(tones(rate, s), rate, kStereo, 480, [&](Meter& m, std::size_t frames) {
            if (frames >= 3 * 48000 + 480) worst = std::max(worst, std::abs(m.readings().shortTerm + 23));
        });
        check(worst <= 0.1, "Tech 3341 case 9: S = -23 +-0.1, constant after 3 s (worst " + std::to_string(worst) + ")");
    }
    {   // 12: (0.18 s at -20, 0.22 s at -30) x25: M = -23 constant after 1 s.
        std::vector<Segment> s;
        for (int i = 0; i < 25; ++i) { s.push_back({0.18, -20}); s.push_back({0.22, -30}); }
        double worst = 0;
        measure(tones(rate, s), rate, kStereo, 480, [&](Meter& m, std::size_t frames) {
            if (frames >= 48000) worst = std::max(worst, std::abs(m.readings().momentary + 23));
        });
        check(worst <= 0.1, "Tech 3341 case 12: M = -23 +-0.1, constant after 1 s (worst " + std::to_string(worst) + ")");
    }
    // Beyond the EBU's +-0.1: the maxima on the 10 ms grid are exactly the
    // largest of the reference's per-sub-block M and S (float input aside).
    constexpr double kGrid = 0.001;
    bool file10 = true, file13 = true, grid10 = true, grid13 = true;
    double worst10 = 0, worst13 = 0;
    for (int i = 0; i < 20; ++i) {   // 10 and 13, file-based: each segment measured alone
        const auto x10 = tones(rate, {{i * 0.15, -kInf}, {3, -23}, {1, -kInf}});
        const auto s10 = measure(x10, rate, kStereo);
        const auto r10 = reference(x10, rate, kStereo);
        file10 &= near(s10.maxShortTerm, -23, 0.1);
        const double d10 = std::abs(s10.maxShortTerm - largestBy(r10.shortTerm, r10.ends, x10[0].size()));
        worst10 = std::max(worst10, d10);
        grid10 &= d10 <= kGrid;
        const auto x13 = tones(rate, {{i * 0.02, -kInf}, {0.4, -23}, {1, -kInf}});
        const auto s13 = measure(x13, rate, kStereo);
        const auto r13 = reference(x13, rate, kStereo);
        file13 &= near(s13.maxMomentary, -23, 0.1);
        const double d13 = std::abs(s13.maxMomentary - largestBy(r13.momentary, r13.ends, x13[0].size()));
        worst13 = std::max(worst13, d13);
        grid13 &= d13 <= kGrid;
    }
    check(file10, "Tech 3341 case 10: max S = -23 +-0.1 for each of the 20 file segments");
    check(file13, "Tech 3341 case 13: max M = -23 +-0.1 for each of the 20 file segments");
    check(grid10, "Tech 3341 case 10: max S within 0.001 of the reference's on the 10 ms grid (worst " +
                      std::to_string(worst10) + ")");
    check(grid13, "Tech 3341 case 13: max M within 0.001 of the reference's on the 10 ms grid (worst " +
                      std::to_string(worst13) + ")");
    // 11 and 14, live: one signal, a growing maximum read after each segment.
    std::vector<Segment> s11, s14;
    std::vector<std::size_t> ends11, ends14;
    double t11 = 0, t14 = 0;
    for (int i = 0; i < 20; ++i) {
        s11.insert(s11.end(), {{i * 0.15, -kInf}, {3, -38.0 + i}, {3 - i * 0.15, -kInf}});
        t11 += 6; ends11.push_back(static_cast<std::size_t>(std::llround(t11 * rate)));
        s14.insert(s14.end(), {{i * 0.02, -kInf}, {0.4, -38.0 + i}, {0.4 - i * 0.02, -kInf}});
        t14 += 0.8; ends14.push_back(static_cast<std::size_t>(std::llround(t14 * rate)));
    }
    // Each live reading is also held to the reference's largest M or S over
    // the sub-blocks that have ended by then.
    double worstLive = 0;
    auto live = [&](const std::vector<Segment>& s, const std::vector<std::size_t>& ends, bool shortTerm) {
        std::size_t next = 0;
        bool ok = true;
        const auto x = tones(rate, s);
        const auto ref = reference(x, rate, kStereo);
        measure(x, rate, kStereo, 480, [&](Meter& m, std::size_t frames) {
            if (next < ends.size() && frames == ends[next]) {
                const auto r = m.readings();
                const double got = shortTerm ? r.maxShortTerm : r.maxMomentary;
                ok &= near(got, -38.0 + static_cast<double>(next), 0.1);
                const double d = std::abs(got - largestBy(shortTerm ? ref.shortTerm : ref.momentary, ref.ends, frames));
                worstLive = std::max(worstLive, d);
                ok &= d <= kGrid;
                ++next;
            }
        });
        return ok && next == ends.size();
    };
    check(live(s11, ends11, true), "Tech 3341 case 11: live max S = -38, -37, ..., -19 +-0.1, and within 0.001 of the reference's");
    check(live(s14, ends14, false), "Tech 3341 case 14: live max M = -38, -37, ..., -19 +-0.1, and within 0.001 of the reference's");
    std::printf("    [measured] cases 10, 11, 13, 14: the largest gap to the reference's maxima, %.6f / %.6f / %.6f LU\n",
                worst10, worst13, worstLive);
}

// --- Tech 3341 cases 15-23: true peak ---------------------------------------------

double truePeakOf(const Signal& x, double rate) { return measure(x, rate, kStereo).truePeak; }

Signal taperedSine(double rate, double frequency, double amplitude, double phaseDeg) {
    const auto n = static_cast<std::size_t>(rate);   // one second
    const auto fade = static_cast<std::size_t>(rate * 0.01);
    Signal x(2, std::vector<double>(n));
    for (std::size_t i = 0; i < n; ++i) {
        double g = 1;
        if (i < fade) g = static_cast<double>(i) / static_cast<double>(fade);
        if (n - 1 - i < fade) g = static_cast<double>(n - 1 - i) / static_cast<double>(fade);
        const double v = g * amplitude * std::sin(2 * pi * frequency * static_cast<double>(i) / rate + phaseDeg * pi / 180);
        x[0][i] = x[1][i] = v;
    }
    return x;
}

/// THE SIGNAL'S OWN TRUE PEAK, read by tests/true_peak_reference.hpp (which
/// shares nothing with the meter) over +-256 samples around its largest
/// sample, in dBTP.
double referenceTruePeakDb(const Signal& x) {
    std::size_t at = 0;
    for (std::size_t i = 0; i < x[0].size(); ++i) {
        if (std::abs(x[0][i]) > std::abs(x[0][at])) {
            at = i;
        }
    }
    const std::size_t from = at > 256 ? at - 256 : 0;
    return 20 * std::log10(adi::test::independentTruePeak(x, from, at + 256));
}

/// How far below its crest the meter's 4x interpolator reads a sine of
/// `cycles` per sample whose crests fall `crestOffset` (0 to 1) into a sample
/// interval: its points are the samples and the odd eighths of each interval,
/// so the crest is d from the nearest (never more than an eighth) and the
/// reading is cos(2 pi cycles d) low.
double gridMissDb(double cycles, double crestOffset) {
    double d = 1.0;
    for (double point : {0.0, 0.125, 0.375, 0.625, 0.875, 1.0}) {
        d = std::min(d, std::abs(crestOffset - point));
    }
    return -20 * std::log10(std::cos(2 * pi * cycles * d));
}

void truePeakCases() {
    const double rate = 48000;
    struct Case { int number; double divisor, amplitude, phase, expected; };
    for (const Case& c : {Case{15, 4, .5, 0, -6}, Case{16, 4, .5, 45, -6}, Case{17, 6, .5, 60, -6},
                          Case{18, 8, .5, 67.5, -6}, Case{19, 4, 1.41, 45, 3}}) {
        const auto x = taperedSine(rate, rate / c.divisor, c.amplitude, c.phase);
        const double tp = truePeakOf(x, rate);
        check(tp <= c.expected + 0.2 && tp >= c.expected - 0.4,
              "Tech 3341 case " + std::to_string(c.number) + ": true peak " + std::to_string(c.expected) +
                  " +0.2/-0.4 dBTP (" + std::to_string(tp) + ")");
        // The synthesis first: the reference reads the sine's own crest.
        const double ref = referenceTruePeakDb(x);
        check(near(ref, 20 * std::log10(c.amplitude), 0.01),
              "Tech 3341 case " + std::to_string(c.number) + ": the independent reference reads the crest, " +
                  std::to_string(ref) + " dBTP");
        // Then the meter, held to that crest: never above it, and below it by
        // no more than its 4x grid misses this sine. Its crests fall at
        // (90 - phase) / 360 periods, and every half period after: the same
        // offset into a sample interval each time (each divisor is even). Case
        // 15's are on samples (no miss); 16 to 19's at the half (an eighth
        // from a point).
        double crest = (90 - c.phase) / 360 * c.divisor;
        crest -= std::floor(crest);
        const double miss = gridMissDb(1 / c.divisor, crest);
        std::printf("    [measured] Tech 3341 case %d: meter %+.4f, reference %+.4f dBTP (the bound: %.4f below)\n",
                    c.number, tp, ref, miss);
        check(tp <= ref + 0.01 && tp >= ref - miss - 0.01,
              "Tech 3341 case " + std::to_string(c.number) + ": the meter's " + std::to_string(tp) +
                  " dBTP is the reference's " + std::to_string(ref) + " less at most its bound of " +
                  std::to_string(miss) + " dB");
    }
    // 20-23: at 4 fs, a sine at fs/6 of 0.5 holding one period of fs/4 at 1.0,
    // continuous in phase on both sides; low-passed and decimated by 4 at
    // offsets 0..3.
    const int up = 4;
    const std::size_t n4 = 4 * 48000;
    std::vector<double> hi(n4);
    const std::size_t at = n4 / 2 - (n4 / 2) % 24;   // a rising zero of the fs/6 sine
    for (std::size_t i = 0; i < n4; ++i) {
        if (i < at) hi[i] = .5 * std::sin(2 * pi * static_cast<double>(i) / 24);
        else if (i < at + 16) hi[i] = std::sin(2 * pi * static_cast<double>(i - at) / 16);
        else hi[i] = .5 * std::sin(2 * pi * static_cast<double>(i - at - 16) / 24);
    }
    for (std::size_t i = 0; i < 1920; ++i) {   // 10 ms tapers
        const double g = static_cast<double>(i) / 1920;
        hi[i] *= g;
        hi[n4 - 1 - i] *= g;
    }
    // Anti-aliasing: a Blackman-windowed sinc at fs/2, 511 taps.
    const int taps = 511;
    std::vector<double> h(taps);
    double sum = 0;
    for (int k = 0; k < taps; ++k) {
        const double t = k - (taps - 1) / 2.0;
        const double sinc = t == 0 ? 1.0 / up : std::sin(pi * t / up) / (pi * t);
        const double w = 0.42 - 0.5 * std::cos(2 * pi * k / (taps - 1)) + 0.08 * std::cos(4 * pi * k / (taps - 1));
        h[static_cast<std::size_t>(k)] = sinc * w;
        sum += sinc * w;
    }
    for (auto& v : h) v /= sum;
    for (int offset = 0; offset < 4; ++offset) {
        Signal x(2);
        for (std::size_t i = static_cast<std::size_t>(offset); i + static_cast<std::size_t>(taps) < n4; i += up) {
            double y = 0;
            for (int k = 0; k < taps; ++k) y += h[static_cast<std::size_t>(k)] * hi[i + static_cast<std::size_t>(k)];
            x[0].push_back(y);
            x[1].push_back(y);
        }
        const double tp = truePeakOf(x, rate);
        check(tp <= 0.2 && tp >= -0.4, "Tech 3341 case " + std::to_string(20 + offset) +
                                           ": true peak 0.0 +0.2/-0.4 dBTP (" + std::to_string(tp) + ")");
        // The decimated signal's own crest, read independently: the EBU's
        // 0.0 is its design, this is what the synthesis here made. No sine's
        // bound applies: the joins at the burst's ends put content up to the
        // low-pass's edge, where the meter's phases roll off. So the bound is
        // the measured miss rounded up: 0.013, 0.052, 0.013 and 0.073 dB at
        // offsets 0 to 3, held to 0.1, which a meter reading 0.15 dB low fails.
        const double ref = referenceTruePeakDb(x);
        const double miss = 0.09;
        std::printf("    [measured] Tech 3341 case %d: meter %+.4f, reference %+.4f dBTP (the bound: %.4f below)\n",
                    20 + offset, tp, ref, miss);
        // Measured: -0.077 dBTP, what the 511-tap low-pass leaves of the
        // burst's 1.0 (the EBU's 0.0 is the design value, +0.2/-0.4).
        check(near(ref, 0.0, 0.1), "Tech 3341 case " + std::to_string(20 + offset) +
                                       ": the independent reference reads the synthesis within 0.1 of 0.0 dBTP (" +
                                       std::to_string(ref) + ")");
        check(tp <= ref + 0.01 && tp >= ref - miss - 0.01,
              "Tech 3341 case " + std::to_string(20 + offset) + ": the meter's " + std::to_string(tp) +
                  " dBTP is the reference's " + std::to_string(ref) + " less at most its bound of " +
                  std::to_string(miss) + " dB");
    }
}

// --- Tech 3342 cases 1-4: loudness range ------------------------------------------

void rangeCases(double rate, bool all) {
    struct Case { int number; std::vector<double> levels; double expected; };
    for (const Case& c : {Case{1, {-20, -30}, 10}, Case{2, {-20, -15}, 5}, Case{3, {-40, -20}, 20},
                          Case{4, {-50, -35, -20, -35, -50}, 15}}) {
        if (!all && c.number != 1) continue;
        std::vector<Segment> s;
        for (double l : c.levels) s.push_back({20, l});
        const auto x = tones(rate, s);
        const auto r = measure(x, rate, kStereo);
        const auto ref = reference(x, rate, kStereo);
        const std::string name = "Tech 3342 case " + std::to_string(c.number) + " at " + std::to_string(static_cast<int>(rate)) + " Hz";
        check(r.rangeValid && near(r.range, c.expected, 1.0),
              name + ": LRA = " + std::to_string(c.expected) + " +-1 (" + std::to_string(r.range) + ")");
        check(near(r.range, ref.range, 0.02), name + ": LRA agrees with the reference's MATLAB rule within 0.02 LU (" +
                                                  std::to_string(ref.range) + ")");
    }
}

// --- invariants ------------------------------------------------------------

void invariants() {
    const double rate = 44100;
    std::vector<Segment> s;
    for (int i = 0; i < 6; ++i) { s.push_back({1.7, -18.0 - 3 * i}); s.push_back({0.9, -40}); }
    const auto x = tones(rate, s);
    const auto a = measure(x, rate, kStereo, 1);
    bool same = true;
    for (int block : {7, 64, 441, 1000, 4096}) {
        const auto b = measure(x, rate, kStereo, block);
        same &= a.integrated == b.integrated && a.range == b.range && a.momentary == b.momentary &&
                a.shortTerm == b.shortTerm && a.maxMomentary == b.maxMomentary && a.truePeak == b.truePeak;
    }
    check(same, "every block size gives the same readings, bit for bit");

    Meter m;
    m.prepare(rate, kStereo);
    std::vector<std::vector<float>> f(2);
    for (int c = 0; c < 2; ++c) f[static_cast<std::size_t>(c)] = floats(x[static_cast<std::size_t>(c)]);
    const float* p[2] = {f[0].data(), f[1].data()};
    allocations = 0;
    counting = true;
    m.process(p, static_cast<int>(f[0].size()));
    (void)m.readings();
    m.reset();
    counting = false;
    check(allocations == 0, "process, readings and reset allocate nothing");

    // Pause: a loud section measured while paused stays out of I and the maxima.
    const auto quiet = tones(rate, {{10, -30}}), loud = tones(rate, {{10, -10}});
    Meter q;
    q.prepare(rate, kStereo);
    auto feed = [&](Meter& meter, const Signal& sig) {
        std::vector<std::vector<float>> g(2);
        for (int c = 0; c < 2; ++c) g[static_cast<std::size_t>(c)] = floats(sig[static_cast<std::size_t>(c)]);
        const float* gp[2] = {g[0].data(), g[1].data()};
        meter.process(gp, static_cast<int>(g[0].size()));
    };
    feed(q, quiet);
    q.setRunning(false);
    feed(q, loud);
    const auto paused = q.readings();
    check(near(paused.integrated, -30, 0.1) && paused.maxMomentary < -29 && near(paused.momentary, -10, 0.1),
          "paused, the loud section stays out of I and max M while M goes on");
    q.setRunning(true);
    feed(q, quiet);
    check(near(q.readings().integrated, -30, 0.1), "running again, I resumes where it stood");
    q.reset();
    check(q.readings().integrated == -kInf && q.readings().momentary == -kInf && !q.readings().rangeValid,
          "reset clears every reading");

    // The double path reads the same as the float path for float-exact input.
    Meter d;
    d.prepare(rate, kStereo);
    const double* dp[2] = {quiet[0].data(), quiet[1].data()};
    d.process(dp, static_cast<int>(quiet[0].size()));
    check(near(d.readings().integrated, -30, 0.1), "the double path measures too");
    // Before its window is full a reading is -inf.
    Meter w;
    w.prepare(48000, kStereo);
    const auto shortSig = tones(48000, {{0.39, -20}});
    std::vector<std::vector<float>> g(2);
    for (int c = 0; c < 2; ++c) g[static_cast<std::size_t>(c)] = floats(shortSig[static_cast<std::size_t>(c)]);
    const float* gp[2] = {g[0].data(), g[1].data()};
    w.process(gp, static_cast<int>(g[0].size()));
    check(w.readings().momentary == -kInf && w.readings().shortTerm == -kInf, "M and S read -inf until their windows fill");
}

// Robustness before the meter replaces Main's (N13 guards these too): a
// non-finite sample reads as silence, a huge one cannot overflow the energy,
// and the end of a programme reads the true peak of its last samples without
// padding reaching I.
void robustness() {
    const double rate = 48000;
    const auto tone = tones(rate, {{3, -20}});
    auto feedWith = [&](Meter& meter, float middle) {
        // 1 s of tone, a 4,800-sample block of `middle`, 2 s of tone.
        const int n = static_cast<int>(tone[0].size());
        std::vector<float> l(static_cast<std::size_t>(n) + 4800), r(l.size());
        for (int i = 0; i < n; ++i) {
            const auto at = static_cast<std::size_t>(i < 48000 ? i : i + 4800);
            l[at] = static_cast<float>(tone[0][static_cast<std::size_t>(i)]);
            r[at] = static_cast<float>(tone[1][static_cast<std::size_t>(i)]);
        }
        for (std::size_t i = 48000; i < 52800; ++i) {
            l[i] = r[i] = middle;
        }
        const float* p[2] = {l.data(), r.data()};
        meter.process(p, static_cast<int>(l.size()));
        return meter.readings();
    };
    Meter withNaN, withZero, withHuge;
    withNaN.prepare(rate, kStereo);
    withZero.prepare(rate, kStereo);
    withHuge.prepare(rate, kStereo);
    const auto a = feedWith(withNaN, std::numeric_limits<float>::quiet_NaN());
    const auto b = feedWith(withZero, 0.f);
    check(std::isfinite(a.momentary) && std::isfinite(a.shortTerm) && std::isfinite(a.integrated) &&
              a.momentary == b.momentary && a.shortTerm == b.shortTerm && a.integrated == b.integrated &&
              a.range == b.range,
          "a block of NaN reads exactly as a block of silence: M, S, I and LRA recover and stay finite");
    const auto h = feedWith(withHuge, 3.0e38f);
    check(std::isfinite(h.momentary) && std::isfinite(h.shortTerm) && std::isfinite(h.integrated),
          "a block of huge samples cannot overflow the readings");

    // The end of a programme: 2 s of tone at -20 dBFS, then a last pair of
    // opposite full-scale steps, whose inter-sample overshoot only the true
    // peak interpolator's tail can see.
    std::vector<float> l(96000), r(96000);
    for (std::size_t i = 0; i < l.size(); ++i) {
        l[i] = r[i] = static_cast<float>(0.1 * std::sin(2 * std::numbers::pi * 997 * static_cast<double>(i) / rate));
    }
    const std::size_t e = l.size();
    l[e - 4] = r[e - 4] = -1.f;
    l[e - 3] = r[e - 3] = -1.f;
    l[e - 2] = r[e - 2] = 1.f;
    l[e - 1] = r[e - 1] = 1.f;
    const float* p[2] = {l.data(), r.data()};
    Meter finished, open, padded;
    for (Meter* m : {&finished, &open, &padded}) {
        m->prepare(rate, kStereo);
        m->process(p, static_cast<int>(l.size()));
    }
    allocations = 0;
    counting = true;
    finished.finish();
    counting = false;
    check(allocations == 0, "finish() allocates nothing");
    std::vector<float> zeros(64, 0.f);
    const float* z[2] = {zeros.data(), zeros.data()};
    padded.process(z, 64);
    const auto f = finished.readings(), o = open.readings(), pz = padded.readings();
    check(f.truePeak > o.truePeak + 0.1 && f.truePeak == pz.truePeak,
          "finish() reads the true peak of the last samples (" + std::to_string(f.truePeak) + " dBTP, " +
              std::to_string(o.truePeak) + " without), as padding them with silence would");
    check(f.integrated == o.integrated && f.momentary == o.momentary && f.shortTerm == o.shortTerm,
          "and leaves I, M and S exactly as they were: no padding reaches them");
}

}  // namespace

int main() {
    for (double rate : {44100.0, 48000.0, 96000.0}) integratedCases(rate, rate == 48000.0);
    momentaryAndShortTerm();
    truePeakCases();
    for (double rate : {44100.0, 48000.0, 96000.0}) rangeCases(rate, rate == 48000.0);
    invariants();
    robustness();
    std::printf("%s -- %d checks, %d failure(s)\n", failures ? "FAIL" : "PASS", checks, failures);
    return failures ? 1 : 0;
}
