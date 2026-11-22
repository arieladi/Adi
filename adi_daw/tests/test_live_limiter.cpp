// SPDX-License-Identifier: GPL-3.0-or-later
//
// Live 12's Limiter, the core (DEV-22, ADR-0249 d7), on ADI's lookahead
// brickwall. Each property is checked against a number derived apart from the
// code: the latency from the lookahead in whole samples (plus the brickwall's
// true-peak delay of 10 at 48 kHz), the ceiling on every sample (and between
// them, on the Annex's reading, in True Peak mode), unity below the ceiling,
// and each link's independence. Parity with Live's sound waits on a Live 12.4
// reading of the Limiter.
#include "adi/dsp/live_limiter.hpp"
#include "adi/dsp/true_peak.hpp"

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
using L = adi::dsp::LiveLimiter;
constexpr double kPi = 3.14159265358979323846;
double fromDb(double db) { return std::pow(10.0, db / 20.0); }

struct Stereo {
    std::vector<float> l, r;
};
Stereo run(L& lim, const Stereo& in, std::size_t block = 97) {
    Stereo out{std::vector<float>(in.l.size()), std::vector<float>(in.r.size())};
    for (std::size_t at = 0; at < in.l.size(); at += block) {
        const auto n = std::min(block, in.l.size() - at);
        lim.process(in.l.data() + at, in.r.data() + at, out.l.data() + at, out.r.data() + at, n);
    }
    return out;
}
L made(double ceilingDb, int mode = L::Standard, int link = L::Stereo, int lookahead = 1) {
    L lim;
    lim.set(L::Ceiling, ceilingDb);
    lim.set(L::Mode, mode);
    lim.set(L::Link, link);
    lim.set(L::Lookahead, lookahead);
    lim.prepare(48000);
    return lim;
}
Stereo noise(std::size_t n, double amplitude, std::uint32_t seed = 2026u) {
    Stereo x{std::vector<float>(n), std::vector<float>(n)};
    for (std::size_t i = 0; i < n; ++i) {
        seed = seed * 1664525u + 1013904223u;
        x.l[i] = static_cast<float>(amplitude * 2 * ((seed >> 8) / 16777216.0 - 0.5));
        seed = seed * 1664525u + 1013904223u;
        x.r[i] = static_cast<float>(amplitude * 2 * ((seed >> 8) / 16777216.0 - 0.5));
    }
    return x;
}
double peakOf(const std::vector<float>& v) {
    double p = 0;
    for (float s : v) {
        p = std::max(p, static_cast<double>(std::abs(s)));
    }
    return p;
}

void latency() {
    const int lookahead[3] = {72, 144, 288};   // 1.5, 3 and 6 ms at 48 kHz
    for (int choice = 0; choice < 3; ++choice) {
        auto lim = made(-1, L::Standard, L::Stereo, choice);
        check(lim.latency() == lookahead[choice], "Lookahead " + std::to_string(L::kLookaheadMs[choice]) + " ms: " +
                                                      std::to_string(lim.latency()) + " samples");
        auto tp = made(-1, L::TruePeak, L::Stereo, choice);
        check(tp.latency() == lookahead[choice] + 10, "and in True Peak mode, 10 more (the detector's delay): " +
                                                          std::to_string(tp.latency()));
    }
}

void belowTheCeiling() {
    // Under the ceiling every mode is a delay, bit for bit, at Gain 0 dB.
    Stereo x{std::vector<float>(9600), std::vector<float>(9600)};
    for (std::size_t i = 0; i < x.l.size(); ++i) {
        x.l[i] = static_cast<float>(0.25 * std::sin(2 * kPi * 440 * static_cast<double>(i) / 48000));
        x.r[i] = static_cast<float>(0.2 * std::sin(2 * kPi * 997 * static_cast<double>(i) / 48000));
    }
    for (int mode : {L::Standard, L::TruePeak}) {
        for (int link : {L::Stereo, L::LeftRight, L::MidSide}) {
            auto lim = made(-1, mode, link);
            const auto y = run(lim, x);
            const auto d = static_cast<std::size_t>(lim.latency());
            bool same = true;
            for (std::size_t i = d; i < x.l.size() && same; ++i) {
                same = std::abs(y.l[i] - x.l[i - d]) <= 1e-7f && std::abs(y.r[i] - x.r[i - d]) <= 1e-7f;
            }
            check(same, "mode " + std::to_string(mode) + ", link " + std::to_string(link) +
                            ": under the ceiling the output is the input, " + std::to_string(d) + " samples late");
        }
    }
    // Gain: +6 dB before the limiter, still under the ceiling.
    auto lim = made(0);
    lim.set(L::Gain, 6);
    Stereo quiet{std::vector<float>(4800, 0.1f), std::vector<float>(4800, -0.1f)};
    const auto y = run(lim, quiet);
    check(std::abs(y.l.back() - 0.1f * static_cast<float>(fromDb(6))) < 1e-6f,
          "Gain +6 dB: a 0.1 signal leaves at " + std::to_string(y.l.back()));
}

void theCeiling() {
    const double ceiling = fromDb(-1);
    const auto loud = noise(48000, 4.0);   // +12 dB over full scale
    for (int link : {L::Stereo, L::LeftRight}) {
        auto lim = made(-1, L::Standard, link);
        const auto y = run(lim, loud);
        check(std::max(peakOf(y.l), peakOf(y.r)) <= ceiling * (1 + 1e-6),
              "Standard, link " + std::to_string(link) + ": every sample at or under the -1 dB ceiling");
    }
    // True Peak: under the ceiling between the samples too, on the Annex's reading.
    auto tp = made(-1, L::TruePeak, L::Stereo, 2);
    const auto y = run(tp, loud);
    adi::dsp::TruePeakMeter meter;
    meter.prepare(48000, 2);
    const float* p[2] = {y.l.data(), y.r.data()};
    meter.process(p, 2, static_cast<int>(y.l.size()));
    check(meter.peakDb() <= -1 + 0.001, "True Peak: the output's true peak " + std::to_string(meter.peakDb()) +
                                            " dBTP against a -1 dB ceiling");
    // Soft Clip: never at the ceiling, a quiet signal as itself.
    auto soft = made(-1, L::SoftClip);
    const auto s = run(soft, loud);
    // The brickwall holds the soft clip's input to the ceiling, so its output
    // tops out at ceiling * tanh(1), about 2.4 dB under it, and gets there.
    const double softPeak = std::max(peakOf(s.l), peakOf(s.r));
    check(softPeak <= ceiling * std::tanh(1.0) * (1 + 1e-6) && softPeak > 0.99 * ceiling * std::tanh(1.0),
          "Soft Clip: the peak is the ceiling's tanh(1) (" + std::to_string(softPeak / ceiling) + " of it)");
    auto quietSoft = made(-1, L::SoftClip);
    Stereo quiet{std::vector<float>(4800, 0.01f), std::vector<float>(4800, 0.01f)};
    const auto q = run(quietSoft, quiet);
    check(std::abs(q.l.back() - 0.01f) < 1e-6f, "and a -40 dBFS signal passes as itself (" +
                                                    std::to_string(q.l.back()) + ")");
}

void links() {
    // A loud left and a quiet right.
    Stereo x{std::vector<float>(24000), std::vector<float>(24000)};
    for (std::size_t i = 0; i < x.l.size(); ++i) {
        x.l[i] = static_cast<float>(3.0 * std::sin(2 * kPi * 200 * static_cast<double>(i) / 48000));
        x.r[i] = static_cast<float>(0.1 * std::sin(2 * kPi * 300 * static_cast<double>(i) / 48000));
    }
    auto stereo = made(-1, L::Standard, L::Stereo);
    auto separate = made(-1, L::Standard, L::LeftRight);
    const auto ys = run(stereo, x), yl = run(separate, x);
    const auto d = static_cast<std::size_t>(stereo.latency());
    double rs = 0, rl = 0;
    for (std::size_t i = 12000; i < x.r.size(); ++i) {
        rs = std::max(rs, static_cast<double>(std::abs(ys.r[i])));
        rl = std::max(rl, static_cast<double>(std::abs(yl.r[i] - x.r[i - d])));
    }
    check(rs < 0.05, "Stereo: the quiet right ducks with the loud left (peak " + std::to_string(rs) + ")");
    check(rl < 1e-7, "L/R: the quiet right passes as itself (largest change " + std::to_string(rl) + ")");
    // M/S: the same signal in both channels has no side; mid is limited.
    Stereo mono{x.l, x.l};
    auto ms = made(-1, L::Standard, L::MidSide);
    const auto ym = run(ms, mono);
    check(peakOf(ym.l) <= fromDb(-1) * (1 + 1e-6) && ym.l == ym.r,
          "M/S: a mono signal is limited as its mid, left and right equal");
}

void changesWhileRunning() {
    // Found defects: a changed link spliced in a stale or empty delay, and a
    // lowered ceiling let the delayed samples leave above it.
    Stereo x{std::vector<float>(14400), std::vector<float>(14400)};
    for (std::size_t i = 0; i < x.l.size(); ++i) {
        x.l[i] = static_cast<float>(0.25 * std::sin(2 * kPi * 440 * static_cast<double>(i) / 48000));
        x.r[i] = static_cast<float>(0.2 * std::sin(2 * kPi * 997 * static_cast<double>(i) / 48000));
    }
    for (int mode : {L::Standard, L::TruePeak}) {
      for (std::size_t every : {1u, 2u, 3u, 7u}) {
        auto lim = made(-1, mode);
        Stereo y{std::vector<float>(x.l.size()), std::vector<float>(x.r.size())};
        const int order[] = {L::LeftRight, L::MidSide, L::Stereo, L::MidSide, L::LeftRight, L::LeftRight,
                             L::Stereo,    L::MidSide, L::Stereo};
        int next = 0;
        for (std::size_t at = 0; at < x.l.size(); at += 97) {
            if (at > 0 && at % (97 * every) == 0) {
                // Every 97 to 679 samples: mid-hold, mid-fade, back to the
                // path fading out, and after the fade.
                lim.set(L::Link, order[next++ % 9]);
            }
            const auto n = std::min<std::size_t>(97, x.l.size() - at);
            lim.process(x.l.data() + at, x.r.data() + at, y.l.data() + at, y.r.data() + at, n);
        }
        const auto d = static_cast<std::size_t>(lim.latency());
        double worst = 0;
        for (std::size_t i = d; i < x.l.size(); ++i) {
            worst = std::max({worst, static_cast<double>(std::abs(y.l[i] - x.l[i - d])),
                              static_cast<double>(std::abs(y.r[i] - x.r[i - d]))});
        }
        check(worst <= 1e-7, "mode " + std::to_string(mode) + ", a link change every " + std::to_string(97 * every) +
                                 " samples under the ceiling leaves the input, delayed (largest change " +
                                 std::to_string(worst) + ")");
      }
    }
    // At the ceiling, on a correlated (mono) programme, through every change.
    const auto loud = noise(24000, 4.0);
    const Stereo mono{loud.l, loud.l};
    for (int mode : {L::Standard, L::TruePeak}) {  // (Soft Clip's tanh never reaches the ceiling: no test)
        auto lim = made(-1, mode, L::Stereo);
        Stereo y{std::vector<float>(mono.l.size()), std::vector<float>(mono.r.size())};
        for (std::size_t at = 0; at < mono.l.size(); at += 97) {
            if (at == 97 * 50) {
                lim.set(L::Link, L::LeftRight);
            } else if (at == 97 * 100) {
                lim.set(L::Link, L::MidSide);
            } else if (at == 97 * 150) {
                lim.set(L::Link, L::Stereo);
            }
            const auto n = std::min<std::size_t>(97, mono.l.size() - at);
            lim.process(mono.l.data() + at, mono.r.data() + at, y.l.data() + at, y.r.data() + at, n);
        }
        check(std::max(peakOf(y.l), peakOf(y.r)) <= fromDb(-1) * (1 + 1e-6),
              "mode " + std::to_string(mode) + ": Stereo to L/R to M/S to Stereo at the ceiling, every sample under it");
    }
    // The ceiling lowered from 0 to -36 dB on a full-scale programme: every
    // sample after the change under the new one.
    for (int link : {L::Stereo, L::LeftRight}) {
        for (int mode : {L::Standard}) {
            auto lim = made(0, mode, link, 2);
            const auto full = noise(9600, 1.0);
            Stereo y{std::vector<float>(full.l.size()), std::vector<float>(full.r.size())};
            const std::size_t change = 97 * 49;
            for (std::size_t at = 0; at < full.l.size(); at += 97) {
                if (at == change) {
                    lim.set(L::Ceiling, -36);
                }
                const auto n = std::min<std::size_t>(97, full.l.size() - at);
                lim.process(full.l.data() + at, full.r.data() + at, y.l.data() + at, y.r.data() + at, n);
            }
            double after = 0;
            for (std::size_t i = change; i < y.l.size(); ++i) {
                after = std::max({after, static_cast<double>(std::abs(y.l[i])), static_cast<double>(std::abs(y.r[i]))});
            }
            check(after <= fromDb(-36) * (1 + 1e-6), "link " + std::to_string(link) + ", mode " +
                                                         std::to_string(mode) +
                                                         ": a ceiling lowered to -36 dB holds from the next sample (" +
                                                         std::to_string(20 * std::log10(after)) + " dB)");
        }
    }
    // True Peak: the same change, read between the samples by the Annex's
    // meter from just after it (a found defect: a clamp's corners overshot).
    for (int link : {L::Stereo, L::LeftRight}) {
        auto lim = made(0, L::TruePeak, link, 2);
        const auto full = noise(9600, 1.0);
        Stereo y{std::vector<float>(full.l.size()), std::vector<float>(full.r.size())};
        const std::size_t change = 97 * 49;
        for (std::size_t at = 0; at < full.l.size(); at += 97) {
            if (at == change) {
                lim.set(L::Ceiling, -36);
            }
            const auto n = std::min<std::size_t>(97, full.l.size() - at);
            lim.process(full.l.data() + at, full.r.data() + at, y.l.data() + at, y.r.data() + at, n);
        }
        adi::dsp::TruePeakMeter meter;
        meter.prepare(48000, 2);
        // The meter reads 8 samples behind: fed through change + 16 and
        // cleared, it reads from 8 samples after the change on.
        const std::size_t split = change + 16;
        const float* head[2] = {y.l.data(), y.r.data()};
        meter.process(head, 2, static_cast<int>(split));
        meter.resetPeak();
        const float* tail[2] = {y.l.data() + split, y.r.data() + split};
        meter.process(tail, 2, static_cast<int>(y.l.size() - split));
        check(meter.peakDb() <= -36 + 0.05, "True Peak, link " + std::to_string(link) +
                                                ": a ceiling lowered to -36 dB holds between the samples (" +
                                                std::to_string(meter.peakDb()) + " dBTP)");
    }
    // Above the ceiling, a hot left (DC 4.0) and a quiet right (DC 0.1), the
    // link changed every 97 to 291 samples: mid-hold, mid-fade, back to a
    // path fading out, a third path. On DC each link's right settles to a
    // constant (Stereo ducks it, L/R passes it, M/S cancels it), so a fade can
    // move the right by at most the largest gap between them over the fade's
    // length a sample; a splice jumps (a found defect: a third path mid-fade
    // cut in at once).
    constexpr std::size_t kSamples = 24000;
    const Stereo dc{std::vector<float>(kSamples, 4.0f), std::vector<float>(kSamples, 0.1f)};
    double lowest = 1e9, highest = -1e9;
    for (int link : {L::Stereo, L::LeftRight, L::MidSide}) {
        auto lim = made(-1, L::Standard, link);
        const double settled = run(lim, dc).r.back();
        lowest = std::min(lowest, settled);
        highest = std::max(highest, settled);
    }
    // A fade is at least a latency long (144 samples at 3 ms) and never under 64.
    const double fadeLength = std::max(static_cast<double>(made(-1).latency()), 64.0);
    const double bound = 1.5 * (highest - lowest) / fadeLength + 1e-6;
    for (bool withMidSide : {false, true}) {
        for (std::size_t every : {1u, 2u, 3u}) {
            auto lim = made(-1, L::Standard, L::Stereo);
            Stereo y{std::vector<float>(kSamples), std::vector<float>(kSamples)};
            const int three[] = {L::LeftRight, L::MidSide, L::LeftRight, L::Stereo, L::MidSide, L::Stereo};
            const int two[] = {L::LeftRight, L::Stereo, L::LeftRight, L::LeftRight, L::Stereo, L::Stereo};
            const int* order = withMidSide ? three : two;
            std::size_t next = 0;
            for (std::size_t at = 0; at < kSamples; at += 97) {
                if (at >= 970 && at % (97 * every) == 0) {
                    lim.set(L::Link, order[next++ % 6]);
                }
                const auto n = std::min<std::size_t>(97, kSamples - at);
                lim.process(dc.l.data() + at, dc.r.data() + at, y.l.data() + at, y.r.data() + at, n);
            }
            double step = 0;
            for (std::size_t i = 600; i < kSamples; ++i) {
                step = std::max(step, static_cast<double>(std::abs(y.r[i] - y.r[i - 1])));
            }
            // M/S holds the ceiling on mid and side, not on left and right, so
            // the ceiling on every sample is checked without it.
            const bool under = withMidSide || std::max(peakOf(y.l), peakOf(y.r)) <= fromDb(-1) * (1 + 1e-6);
            check(under && step <= bound, std::string(withMidSide ? "Stereo, L/R and M/S" : "Stereo and L/R") +
                                              ", changed every " + std::to_string(97 * every) +
                                              " samples above the ceiling: the right's largest step " +
                                              std::to_string(step) + ", a fade's at most " + std::to_string(bound) +
                                              (under ? "" : "; OVER the ceiling"));
        }
    }
}

void robustness() {
    // A non-finite sample on one channel only: the other channel is that of
    // the same programme with silence there, on every link.
    for (int link : {L::Stereo, L::LeftRight, L::MidSide}) {
        auto a = made(-1, L::Standard, link), b = made(-1, L::Standard, link);
        auto withNaN = noise(9600, 2.0), withZero = withNaN;
        for (std::size_t i = 3000; i < 3300; ++i) {
            withNaN.l[i] = i % 2 ? std::numeric_limits<float>::quiet_NaN() : std::numeric_limits<float>::infinity();
            withZero.l[i] = 0.f;
        }
        const auto ya = run(a, withNaN), yb = run(b, withZero);
        check(ya.l == yb.l && ya.r == yb.r,
              "link " + std::to_string(link) + ": a non-finite left reads as silence there, the right untouched");
    }
    auto a = made(-1), b = made(-1);
    auto withNaN = noise(9600, 2.0), withZero = withNaN;
    for (std::size_t i = 3000; i < 3300; ++i) {
        withNaN.l[i] = std::numeric_limits<float>::quiet_NaN();
        withNaN.r[i] = std::numeric_limits<float>::infinity();
        withZero.l[i] = withZero.r[i] = 0.f;
    }
    const auto ya = run(a, withNaN), yb = run(b, withZero);
    check(ya.l == yb.l && ya.r == yb.r, "a NaN or infinite block reads as silence, exactly");
    // Changing every control while running allocates nothing.
    auto lim = made(-1);
    const auto x = noise(512, 2.0);
    std::vector<float> ol(512), orr(512);
    counting = true;
    for (int i = 0; i < 60; ++i) {
        lim.set(L::Lookahead, i % 3);
        lim.set(L::Mode, (i / 3) % 3);
        lim.set(L::Link, (i / 9) % 3);
        lim.set(L::Release, 10.0 + i);
        lim.process(x.l.data(), x.r.data(), ol.data(), orr.data(), 512);
    }
    counting = false;
    check(allocations == 0, "process and every control change allocate nothing");
}

}  // namespace

int main() {
    latency();
    belowTheCeiling();
    theCeiling();
    links();
    changesWhileRunning();
    robustness();
    std::printf("%s -- %d checks, %d failure(s)\n", failures ? "FAILED" : "PASS", checks, failures);
    return failures ? 1 : 0;
}
