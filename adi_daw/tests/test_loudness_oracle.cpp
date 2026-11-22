// SPDX-License-Identifier: GPL-3.0-or-later
//
// DEV-28 / MX-27 -- the one loudness meter against an OUTSIDE oracle.
//
// test_loudness.cpp proves dsp::LoudnessMeter against the Tech 3341/3342 cases
// and a brute-force reference written in this tree. Both of those are ours. This
// suite runs signals through ADI's meter and libebur128 (LD3: MIT, test-only, never
// linked into the app) and asks for agreement within the plan's 0.1 LU --
// held here to 0.01 (kSame) -- at 44.1, 48 and 96 kHz, on pink noise, a
// speech-like signal with pauses (so the gates do work), a sine sweep and a
// 5.1 mix with BS.1770's surround weights and the LFE left out. libebur128 is fed in 10 ms chunks and asked for its momentary and
// short-term loudness after each one, which are the instants ADI's meter
// computes them at, so the maxima compare like for like.
//
// LRA is two checks. Tech 3342 hops the short-term window at 10 Hz; libebur128
// 1.2.6 hops it at 1 Hz (a 3 s window every second) and takes its percentiles
// by nearest rank, so on a signal whose loudness moves -- the sweep, the
// speech -- the two sample different short-term values and differ by up to
// 0.45 LU, both correctly. So ADI's own short-term readings are also taken on
// libebur128's 1 Hz grid, gated and ranked its way, and THAT range must agree
// within kSame: the short-term values are the same, the difference is the
// grid. ADI's 10 Hz LRA is then held within 0.5 LU of libebur128's.
// True peak is two different interpolators (ADI's Kaiser sinc, 16 taps a
// phase; libebur128's own), so it too gets its own bound.
//
// Built only when libebur128 has been fetched (cmake/devices/loudness.cmake).

#include "adi/dsp/loudness.hpp"

#include <ebur128.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace {

using adi::dsp::LoudnessMeter;

int checks = 0, failures = 0;

void check(bool ok, const std::string& what) {
    ++checks;
    if (!ok) ++failures;
    std::printf("  %s  %s\n", ok ? "ok  " : "FAIL", what.c_str());
}

std::string fmt(double v) {
    char b[32];
    std::snprintf(b, sizeof b, "%.4f", v);
    return b;
}

constexpr double kPi = 3.14159265358979323846;

/// The plan's gate is 0.1 LU. The two agree to 0.0000 on every reading below,
/// so the suite asks for 0.01: a wrong gate or weight that moves a reading by
/// a tenth would otherwise pass on the signals where it moves it by less.
constexpr double kSame = 0.01;

/// A small deterministic generator: the same signals on every machine.
struct Rng {
    std::uint32_t s;
    double uniform() {   // [-1, 1)
        s = s * 1664525u + 1013904223u;
        return static_cast<double>(s >> 8) / 8388608.0 - 1.0;
    }
};

using Signal = std::vector<std::vector<double>>;   // [channel][frame]

/// Pink noise by Paul Kellet's refined filter, one generator per channel.
Signal pink(double rate, double seconds, int channels, double gain) {
    const auto n = static_cast<std::size_t>(rate * seconds);
    Signal x(static_cast<std::size_t>(channels), std::vector<double>(n));
    for (int c = 0; c < channels; ++c) {
        Rng r{0x9e3779b9u * static_cast<std::uint32_t>(c + 1)};
        double b0 = 0, b1 = 0, b2 = 0, b3 = 0, b4 = 0, b5 = 0, b6 = 0;
        for (std::size_t i = 0; i < n; ++i) {
            const double w = r.uniform();
            b0 = 0.99886 * b0 + w * 0.0555179;
            b1 = 0.99332 * b1 + w * 0.0750759;
            b2 = 0.96900 * b2 + w * 0.1538520;
            b3 = 0.86650 * b3 + w * 0.3104856;
            b4 = 0.55000 * b4 + w * 0.5329522;
            b5 = -0.7616 * b5 - w * 0.0168980;
            x[static_cast<std::size_t>(c)][i] = gain * (b0 + b1 + b2 + b3 + b4 + b5 + b6 + w * 0.5362);
            b6 = w * 0.115926;
        }
    }
    return x;
}

/// Speech-like: noise through three formant resonators, in syllables of
/// 120-300 ms, phrases of 1.5-4 s at levels 12 dB apart, and silent pauses of
/// 0.4-1.5 s between them -- what the absolute and relative gates are for.
Signal speechLike(double rate, double seconds) {
    const auto n = static_cast<std::size_t>(rate * seconds);
    Signal x(2, std::vector<double>(n, 0.0));
    Rng r{12345u};
    struct Bp { double b0, b2, a1, a2, x1 = 0, x2 = 0, y1 = 0, y2 = 0; };
    const auto bandpass = [rate](double f, double q) {
        const double w = 2.0 * kPi * f / rate, al = std::sin(w) / (2.0 * q), a0 = 1.0 + al;
        return Bp{al / a0, -al / a0, -2.0 * std::cos(w) / a0, (1.0 - al) / a0};
    };
    Bp f[3] = {bandpass(500.0, 4.0), bandpass(1500.0, 6.0), bandpass(2500.0, 8.0)};
    std::size_t i = 0;
    while (i < n) {
        const double level = 0.25 * std::pow(10.0, (r.uniform() * 6.0) / 20.0);
        const auto phraseEnd = std::min(n, i + static_cast<std::size_t>(rate * (2.75 + 1.25 * r.uniform())));
        while (i < phraseEnd) {
            const auto syl = static_cast<std::size_t>(rate * (0.21 + 0.09 * r.uniform()));
            for (std::size_t k = 0; k < syl && i < phraseEnd; ++k, ++i) {
                const double env = std::sin(kPi * static_cast<double>(k) / static_cast<double>(syl));
                const double w = r.uniform();
                double y = 0.0;
                for (Bp& b : f) {
                    const double o = b.b0 * w + b.b2 * b.x2 - b.a1 * b.y1 - b.a2 * b.y2;
                    b.x2 = b.x1; b.x1 = w; b.y2 = b.y1; b.y1 = o;
                    y += o;
                }
                x[0][i] = x[1][i] = level * env * env * y * 4.0;
            }
        }
        i += static_cast<std::size_t>(rate * (0.95 + 0.55 * r.uniform()));   // the pause
    }
    return x;
}

/// A logarithmic sine sweep from 20 Hz to 20 kHz (or 0.45 of the rate).
Signal sweep(double rate, double seconds) {
    const auto n = static_cast<std::size_t>(rate * seconds);
    Signal x(2, std::vector<double>(n));
    const double f0 = 20.0, f1 = std::min(20000.0, 0.45 * rate);
    const double k = std::log(f1 / f0);
    for (std::size_t i = 0; i < n; ++i) {
        const double t = static_cast<double>(i) / rate;
        const double phase = 2.0 * kPi * f0 * seconds / k * (std::exp(t / seconds * k) - 1.0);
        x[0][i] = 0.5 * std::sin(phase);
        x[1][i] = 0.25 * std::sin(phase + 0.7);
    }
    return x;
}

struct Result {
    double integrated, maxM, maxS, range, truePeak, lastM, lastS;
    double rangeOn1Hz = 0.0;   ///< ADI only: LRA from its short-term values every 1 s
};

/// Tech 3342's gates with libebur128's 1.2.6 nearest-rank percentiles.
double rangeNearestRank(const std::vector<double>& shortTermLufs) {
    std::vector<double> e;
    for (double l : shortTermLufs)
        if (l >= -70.0) e.push_back(std::pow(10.0, (l + 0.691) / 10.0));
    if (e.empty()) return 0.0;
    double mean = 0.0;
    for (double v : e) mean += v;
    mean /= static_cast<double>(e.size());
    const double gate = mean * std::pow(10.0, -20.0 / 10.0);
    std::vector<double> kept;
    for (double v : e)
        if (v >= gate) kept.push_back(v);
    std::sort(kept.begin(), kept.end());
    if (kept.empty()) return 0.0;
    const auto at = [&](double q) {
        return kept[static_cast<std::size_t>(static_cast<double>(kept.size() - 1) * q + 0.5)];
    };
    return 10.0 * std::log10(at(0.95)) - 10.0 * std::log10(at(0.10));
}

Result viaAdi(const Signal& x, double rate, const std::vector<double>& weights) {
    LoudnessMeter m;
    m.prepare(rate, weights);
    const auto frames = x[0].size();
    std::vector<const double*> p(x.size());
    // 10 ms blocks, so the short-term reading can be taken on whole seconds.
    // (Any block size gives the same readings: test_loudness.cpp.)
    const auto block = static_cast<std::size_t>(std::lround(rate / 100.0));
    const auto second = static_cast<std::size_t>(std::lround(rate));
    std::vector<double> everySecond;
    for (std::size_t at = 0; at < frames; at += block) {
        for (std::size_t c = 0; c < x.size(); ++c) p[c] = x[c].data() + at;
        const auto n = std::min(block, frames - at);
        m.process(p.data(), static_cast<int>(n));
        const auto done = at + n;
        if (done >= 3 * second && done % second == 0) everySecond.push_back(m.readings().shortTerm);
    }
    const auto r = m.readings();
    return {r.integrated, r.maxMomentary, r.maxShortTerm, r.range, r.truePeak, r.momentary,
            r.shortTerm, rangeNearestRank(everySecond)};
}

Result viaLibebur128(const Signal& x, double rate, bool surround) {
    const auto channels = static_cast<unsigned>(x.size());
    ebur128_state* st = ebur128_init(channels, static_cast<unsigned long>(rate),
                                     EBUR128_MODE_I | EBUR128_MODE_LRA | EBUR128_MODE_TRUE_PEAK);
    if (surround) {   // L R C LFE Ls Rs
        const int map[6] = {EBUR128_LEFT, EBUR128_RIGHT, EBUR128_CENTER, EBUR128_UNUSED,
                            EBUR128_LEFT_SURROUND, EBUR128_RIGHT_SURROUND};
        for (unsigned c = 0; c < channels; ++c) ebur128_set_channel(st, c, map[c]);
    }
    const auto frames = x[0].size();
    const auto chunk = static_cast<std::size_t>(std::lround(rate / 100.0));   // 10 ms
    std::vector<double> inter(chunk * channels);
    double maxM = -HUGE_VAL, maxS = -HUGE_VAL, lastM = -HUGE_VAL, lastS = -HUGE_VAL;
    for (std::size_t at = 0; at + chunk <= frames; at += chunk) {
        for (std::size_t i = 0; i < chunk; ++i)
            for (unsigned c = 0; c < channels; ++c) inter[i * channels + c] = x[c][at + i];
        ebur128_add_frames_double(st, inter.data(), chunk);
        const std::size_t done = at + chunk;
        if (done >= static_cast<std::size_t>(0.4 * rate)) {
            ebur128_loudness_momentary(st, &lastM);
            maxM = std::max(maxM, lastM);
        }
        if (done >= static_cast<std::size_t>(3.0 * rate)) {
            ebur128_loudness_shortterm(st, &lastS);
            maxS = std::max(maxS, lastS);
        }
    }
    double integrated = 0, range = 0, tp = 0;
    ebur128_loudness_global(st, &integrated);
    ebur128_loudness_range(st, &range);
    for (unsigned c = 0; c < channels; ++c) {
        double v = 0;
        ebur128_true_peak(st, c, &v);
        tp = std::max(tp, v);
    }
    ebur128_destroy(&st);
    return {integrated, maxM, maxS, range, 20.0 * std::log10(tp), lastM, lastS};
}

void compare(const std::string& name, const Signal& x, double rate, bool surround) {
    const std::vector<double> weights =
        surround ? std::vector<double>{1.0, 1.0, 1.0, 0.0, LoudnessMeter::kSurroundWeight,
                                       LoudnessMeter::kSurroundWeight}
                 : std::vector<double>(x.size(), 1.0);
    const Result a = viaAdi(x, rate, weights);
    const Result e = viaLibebur128(x, rate, surround);
    const std::string at = name + " at " + std::to_string(static_cast<int>(rate)) + " Hz: ";
    const auto lu = [&](const char* what, double ours, double theirs, double tol) {
        check(std::isfinite(ours) && std::fabs(ours - theirs) <= tol,
              at + what + " ADI " + fmt(ours) + ", libebur128 " + fmt(theirs) + " (diff " +
                  fmt(ours - theirs) + ", allowed " + fmt(tol) + ")");
    };
    lu("integrated", a.integrated, e.integrated, kSame);
    lu("max momentary", a.maxM, e.maxM, kSame);
    lu("max short-term", a.maxS, e.maxS, kSame);
    lu("momentary at the end", a.lastM, e.lastM, kSame);
    lu("short-term at the end", a.lastS, e.lastS, kSame);
    lu("LRA on libebur128's 1 Hz grid", a.rangeOn1Hz, e.range, kSame);
    lu("LRA at Tech 3342's 10 Hz", a.range, e.range, 0.5);
    lu("true peak (dBTP)", a.truePeak, e.truePeak, 0.2);
}

}  // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    int major = 0, minor = 0, patch = 0;
    ebur128_get_version(&major, &minor, &patch);
    std::printf("adi_loudness_oracle_tests -- dsp::LoudnessMeter against libebur128 %d.%d.%d\n\n",
                major, minor, patch);
    for (double rate : {44100.0, 48000.0, 96000.0}) {
        std::printf("[%d Hz]\n", static_cast<int>(rate));
        compare("pink noise", pink(rate, 20.0, 2, 0.1), rate, false);
        compare("speech-like with pauses", speechLike(rate, 30.0), rate, false);
        compare("a 20 Hz-20 kHz sweep", sweep(rate, 20.0), rate, false);
        compare("5.1 pink noise, Ls/Rs at 1.41, LFE out", pink(rate, 12.0, 6, 0.08), rate, true);
    }
    std::printf("\n%s -- %d checks, %d failure(s)\n", failures ? "FAILED" : "PASS", checks, failures);
    return failures ? 1 : 0;
}
