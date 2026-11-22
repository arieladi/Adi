// SPDX-License-Identifier: GPL-3.0-or-later
//
// Live 12's Filter Delay, the core (DEV-22). The reference numbers are the
// controls' own meanings, apart from the code: an impulse comes back after
// exactly Time (or Sixteenths at the tempo), each repeat Feedback times the
// last; a band of Width octaves peaks at 0 dB at Freq and is 3 dB down
// Width octaves apart; the pan is equal power; Dry 0 dB with the lines
// silent is the input itself. Parity with Live waits on a reading.
#include "adi/dsp/filter_delay.hpp"
#include "adi/dsp/spectrum.hpp"

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
using F = adi::dsp::FilterDelay;
constexpr double kRate = 48000;

/// One line alone: the others and the dry silent, the band off, no feedback.
F alone(std::size_t line) {
    F f;
    f.set(F::Dry, -70);
    for (std::size_t k = 0; k < F::kLines; ++k) {
        f.set(F::param(k, F::Volume), k == line ? 0 : -70);
        f.set(F::param(k, F::Filter), 0);
        f.set(F::param(k, F::Feedback), 0);
        f.set(F::param(k, F::Sync), 0);
    }
    f.prepare(kRate);
    return f;
}
void run(F& f, const std::vector<float>& l, const std::vector<float>& r, std::vector<float>& outL, std::vector<float>& outR,
         std::size_t block = 512) {
    outL.assign(l.size(), 0.f);
    outR.assign(l.size(), 0.f);
    for (std::size_t at = 0; at < l.size(); at += block) {
        const auto n = std::min(block, l.size() - at);
        f.process(l.data() + at, r.data() + at, outL.data() + at, outR.data() + at, n);
    }
}
std::vector<float> impulse(std::size_t frames, std::size_t at = 0) {
    std::vector<float> x(frames, 0.f);
    x[at] = 1.f;
    return x;
}

void echoes() {
    // The left line, 100 ms, Feedback 50 %: echoes at 4800 k, each half the last.
    F f = alone(0);
    f.set(F::param(0, F::Time), 100);
    f.set(F::param(0, F::Feedback), 50);
    f.prepare(kRate);
    const auto x = impulse(30000), zero = std::vector<float>(30000, 0.f);
    std::vector<float> l, r;
    run(f, x, zero, l, r);
    bool exact = true;
    for (std::size_t i = 0; i < l.size(); ++i) {
        const bool echo = i > 0 && i % 4800 == 0;
        const double want = echo ? std::pow(0.5, static_cast<double>(i / 4800 - 1)) : 0.0;
        exact = exact && std::abs(l[i] - want) < 1e-7 && std::abs(r[i]) < 1e-7;
    }
    check(exact, "Time 100 ms, Feedback 50 %: echoes at exactly 4800 k samples, each half the last, panned left");
    // The mono line hears half of each input; centred, each side 1/sqrt 2.
    F mid = alone(1);
    mid.set(F::param(1, F::Time), 10);
    mid.prepare(kRate);
    run(mid, x, zero, l, r);
    check(std::abs(l[480] - 0.5 / std::sqrt(2.0)) < 1e-7 && std::abs(r[480] - 0.5 / std::sqrt(2.0)) < 1e-7,
          "the L+R line takes the mono sum, and centred it is equal power");
    // The right line hears only the right input.
    F right = alone(2);
    right.set(F::param(2, F::Time), 10);
    right.prepare(kRate);
    run(right, x, zero, l, r);
    check(std::all_of(l.begin(), l.end(), [](float v) { return v == 0.f; }) && std::all_of(r.begin(), r.end(), [](float v) { return v == 0.f; }),
          "the R line does not hear the left input");
    // Pan at -25: cos and sin of a quarter of a right angle.
    F pan = alone(0);
    pan.set(F::param(0, F::Time), 10);
    pan.set(F::param(0, F::Pan), -25);
    pan.prepare(kRate);
    run(pan, x, zero, l, r);
    check(std::abs(l[480] - std::cos(std::numbers::pi / 8)) < 1e-6 && std::abs(r[480] - std::sin(std::numbers::pi / 8)) < 1e-6,
          "the pan is equal power");
    // On off: the input is not taken, but echoes already in the line ring out.
    F ring = alone(0);
    ring.set(F::param(0, F::Time), 10);
    ring.set(F::param(0, F::Feedback), 50);
    ring.prepare(kRate);
    std::vector<float> a(240), b(240), c(240), d(240);
    const auto pulse = impulse(240);
    ring.process(pulse.data(), zero.data(), a.data(), b.data(), 240);
    ring.set(F::param(0, F::On), 0);
    std::vector<float> rest(4800, 0.f), restL(4800), restR(4800);
    std::vector<float> more = impulse(4800, 100);
    ring.process(more.data(), more.data(), restL.data(), restR.data(), 4800);
    check(std::abs(restL[240] - 1.f) < 1e-7 && std::abs(restL[720] - 0.5f) < 1e-7 && restL[580] == 0.f,
          "On off: the echoes already in the line ring out, the new input is not taken");
}

void sync() {
    F f = alone(0);
    f.set(F::param(0, F::Sync), 1);
    f.set(F::param(0, F::Sixteenths), 3);
    f.tempo(120);
    f.prepare(kRate);
    check(f.target(0) == 18000, "3 sixteenths at 120 BPM: 375 ms");
    f.tempo(90);
    check(f.target(0) == 24000, "at 90 BPM: 500 ms");
    // After the glide the tap sits on a whole sample: the echo is exact.
    std::vector<float> quiet(48000, 0.f), l(48000), r(48000);
    f.process(quiet.data(), quiet.data(), l.data(), r.data(), quiet.size());
    const auto x = impulse(30000);
    std::vector<float> ol, orr;
    run(f, x, quiet, ol, orr);
    check(std::abs(ol[24000] - 1.f) < 1e-7 && std::abs(ol[23999]) < 1e-7 && std::abs(ol[24001]) < 1e-7,
          "a moved synced time arrives at a whole sample (an exact echo after the glide)");
    f.set(F::param(0, F::Sync), 0);
    f.set(F::param(0, F::Time), 3000);
    check(f.target(0) == 144000, "Sync off: Time in ms");
}

void band() {
    for (double width : {1.0, 3.0}) {
        F f = alone(0);
        f.set(F::param(0, F::Filter), 1);
        f.set(F::param(0, F::Freq), 1000);
        f.set(F::param(0, F::Width), width);
        f.set(F::param(0, F::Time), 1);   // 48 samples
        f.prepare(kRate);
        const std::size_t n = 65536;
        const auto x = impulse(n);
        const std::vector<float> zero(n, 0.f);
        std::vector<float> l, r;
        run(f, x, zero, l, r);
        adi::dsp::RealFft fft(static_cast<std::int32_t>(n));
        std::vector<std::complex<float>> bins(n / 2 + 1);
        fft.forward(l, bins);
        const auto magDb = [&](double hz) {
            const auto k = static_cast<std::size_t>(std::lround(hz * static_cast<double>(n) / kRate));
            return 20 * std::log10(std::abs(bins[k]));
        };
        // The peak, at 1 kHz, is 0 dB; the band edges, sqrt(2^W) either side
        // (geometrically centred), are 3 dB down.
        const double edge = std::pow(2.0, width / 2);
        double peak = -100, at = 0;
        for (std::size_t k = 1; k < bins.size(); ++k) {
            const double m = 20 * std::log10(std::abs(bins[k]));
            if (m > peak) {
                peak = m;
                at = static_cast<double>(k) * kRate / static_cast<double>(n);
            }
        }
        check(std::abs(peak) < 0.05 && std::abs(at - 1000) < 15,
              "Width " + std::to_string(width) + ": the band peaks at 0 dB at Freq (" + std::to_string(peak) + " dB at " + std::to_string(at) + ")");
        const double low = magDb(1000 / edge), high = magDb(1000 * edge);
        check(std::abs(low + 3.0103) < 0.15 && std::abs(high + 3.0103) < 0.15,
              "Width " + std::to_string(width) + " octaves: 3 dB down at its edges (" + std::to_string(low) + ", " + std::to_string(high) + ")");
    }
    // Feedback 95 % through a narrow band stays bounded.
    F loud;
    loud.set(F::param(0, F::Feedback), 95);
    loud.set(F::param(1, F::Feedback), 95);
    loud.set(F::param(2, F::Feedback), 95);
    loud.prepare(kRate);
    std::vector<float> noise(96000), l, r;
    for (std::size_t i = 0; i < noise.size(); ++i) {
        noise[i] = static_cast<float>(0.5 * std::sin(0.0654 * static_cast<double>(i)));
    }
    run(loud, noise, noise, l, r);
    check(std::all_of(l.begin(), l.end(), [](float v) { return std::isfinite(v) && std::abs(v) < 30; }), "Feedback 95 % stays bounded");
}

void identityAndRobustness() {
    std::vector<float> x(20000), y(20000);
    for (std::size_t i = 0; i < x.size(); ++i) {
        x[i] = static_cast<float>(0.3 * std::sin(0.013 * static_cast<double>(i)) + 0.2 * std::sin(1.9 * static_cast<double>(i)));
        y[i] = static_cast<float>(0.1 * std::sin(0.7 * static_cast<double>(i)));
    }
    F dry;
    for (std::size_t k = 0; k < F::kLines; ++k) {
        dry.set(F::param(k, F::Volume), -70);
    }
    std::vector<float> l, r;
    run(dry, x, y, l, r);
    check(l == x && r == y, "Dry 0 dB with the lines silent is the input itself");
    std::vector<float> first;
    bool identical = true;
    for (std::size_t block : {1u, 64u, 333u, 4096u}) {
        F f;
        f.tempo(128);
        f.prepare(kRate);
        f.set(F::param(1, F::Time), 77);
        f.set(F::param(1, F::Sync), 0);
        run(f, x, y, l, r, block);
        if (first.empty()) {
            first = l;
        } else {
            identical = identical && l == first;
        }
    }
    check(identical, "every block size gives the same samples (a gliding time included)");
    F f;
    std::vector<float> bad(512, std::numeric_limits<float>::quiet_NaN()), o(512), p(512);
    counting = true;
    f.process(bad.data(), bad.data(), o.data(), p.data(), bad.size());
    f.set(F::param(0, F::Time), 1000);
    f.set(F::param(0, F::Sync), 0);
    f.set(F::param(2, F::Width), 9);
    f.tempo(140);
    f.process(x.data(), x.data(), o.data(), p.data(), 512);
    counting = false;
    check(allocations == 0, "processing, tempo and moved controls allocate nothing");
    check(std::all_of(o.begin(), o.end(), [](float v) { return std::isfinite(v); }), "a NaN input leaves the output finite");
    F z;
    for (std::size_t k = 0; k < F::kLines; ++k) {
        z.set(F::param(k, F::Volume), -70);
    }
    std::vector<float> left(4800, 0.25f), shared(4800, -0.5f), rightOut(4800);
    z.process(left.data(), shared.data(), shared.data(), rightOut.data(), 4800);
    check(rightOut.back() == -0.5f && shared.back() == 0.25f, "an output plane that is the other channel's input is read first");
    check(F::range(F::param(2, F::Pan)).initial == 50 && F::range(F::Dry).high == 6, "each line's defaults, and Dry's range");
}

}  // namespace

int main() {
    echoes();
    sync();
    band();
    identityAndRobustness();
    std::printf("%s -- %d checks, %d failure(s)\n", failures ? "FAILED" : "PASS", checks, failures);
    return failures ? 1 : 0;
}
