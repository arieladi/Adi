// SPDX-License-Identifier: GPL-3.0-or-later
//
// Live 12's Spectral Time, the core (DEV-22). The references are the
// transform's own exactness and the header's laws: delayed by d whole frames
// of 512 samples, an impulse returns after one window (the latency) plus
// d x 512 samples, alone and whole, each repeat Feedback times the last;
// Tilt delays the high bins more; outside the Mask bins come straight
// through; a frozen tone rings on at its level and pitch. Parity with Live
// waits on a reading.
#include "adi/dsp/spectral_time.hpp"

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
using S = adi::dsp::SpectralTime;
constexpr double kRate = 48000;
constexpr std::size_t kN = S::kSize, kH = S::kHop;

void run(S& s, const std::vector<float>& x, std::vector<float>& l, std::vector<float>& r, std::size_t block = 500) {
    l.assign(x.size(), 0.f);
    r.assign(x.size(), 0.f);
    for (std::size_t at = 0; at < x.size(); at += block) {
        const auto n = std::min(block, x.size() - at);
        s.process(x.data() + at, x.data() + at, l.data() + at, r.data() + at, n);
    }
}

void delays() {
    // Time 4 frames exactly (4 x 512 samples = 42.67 ms), no feedback, fully
    // wet: an impulse at 5,000 comes out at 5,000 + 2,048 + 2,048, alone.
    S s;
    s.set(S::Time, 4.0 * kH / kRate * 1000);
    s.set(S::Feedback, 0);
    s.set(S::DryWet, 100);
    s.prepare(kRate);
    check(s.delay(10) == 4 && s.delay(900) == 4 && s.latency() == static_cast<int>(kN), "Time of 4 frames: every bin 4 frames; the latency is a window");
    std::vector<float> x(20000, 0.f), l, r;
    x[5000] = 1.f;
    run(s, x, l, r);
    const std::size_t at = 5000 + kN + 4 * kH;
    double rest = 0;
    for (std::size_t i = 0; i < l.size(); ++i) {
        if (i != at) {
            rest = std::max(rest, static_cast<double>(std::abs(l[i])));
        }
    }
    check(std::abs(l[at] - 1.f) < 1e-5 && rest < 1e-5, "an impulse returns after the latency and 4 x 512 samples, alone and whole (" +
                                                           std::to_string(l[at]) + ", the rest under " + std::to_string(rest) + ")");
    // Feedback 50 %: the next repeat 4 frames on, at half.
    S f;
    f.set(S::Time, 4.0 * kH / kRate * 1000);
    f.set(S::Feedback, 50);
    f.set(S::DryWet, 100);
    f.prepare(kRate);
    run(f, x, l, r);
    check(std::abs(l[at] - 1.f) < 1e-5 && std::abs(l[at + 4 * kH] - 0.5f) < 1e-5 && std::abs(l[at + 8 * kH] - 0.25f) < 1e-5,
          "Feedback 50 %: each repeat 4 frames on, half the last");
    // Dry/Wet 0: the input, delayed by the latency, exactly.
    S d;
    d.set(S::DryWet, 0);
    d.prepare(kRate);
    run(d, x, l, r);
    check(l[5000 + kN] == 1.f && std::count_if(l.begin(), l.end(), [](float v) { return v != 0.f; }) == 1, "Dry/Wet 0: the input, one window late");
}

void shaping() {
    S t;
    t.set(S::Time, 200);
    t.set(S::Tilt, 100);
    t.prepare(kRate);
    check(t.delay(S::kBins - 1) > 3 * t.delay(1), "Tilt +100: the high bins delayed about four times the low");
    S m;
    m.set(S::Time, 200);
    m.set(S::Mask, 100);
    m.prepare(kRate);
    check(m.delay(10) == 0 && m.delay(S::kBins - 10) > 0, "Mask +100: the low half straight through, the high half delayed");
    S sp;
    sp.set(S::Time, 200);
    sp.set(S::Spray, 100);
    sp.prepare(kRate);
    bool apart = false;
    for (std::size_t k = 2; k < S::kBins; ++k) {
        apart = apart || sp.delay(k) != sp.delay(1);
    }
    check(apart, "Spray scatters the bins' delays");
}

void freeze() {
    // A 1,003 Hz tone (off any bin), frozen, then the input stops: the
    // output rings on at the tone's level and pitch.
    S s;
    s.set(S::DryWet, 100);
    s.prepare(kRate);
    std::vector<float> x(48000 * 3, 0.f), l(x.size()), r(x.size());
    for (std::size_t i = 0; i < 48000; ++i) {
        x[i] = static_cast<float>(0.4 * std::sin(2 * std::numbers::pi * 1003 * static_cast<double>(i) / kRate));
    }
    s.process(x.data(), x.data(), l.data(), r.data(), 40000);
    s.set(S::Freeze, 1);
    s.process(x.data() + 40000, x.data() + 40000, l.data() + 40000, r.data() + 40000, x.size() - 40000);
    // A second after the input stopped: its level and its strongest pitch.
    const std::size_t from = 48000 * 2, n = 48000;
    double e = 0;
    for (std::size_t i = from; i < from + n; ++i) {
        e += static_cast<double>(l[i]) * l[i];
    }
    const double rms = std::sqrt(e / static_cast<double>(n)), want = 0.4 / std::sqrt(2.0);
    double best = 0, top = -1;
    for (double hz = 990; hz <= 1016; hz += 0.25) {
        std::complex<double> sum = 0;
        for (std::size_t i = from; i < from + n; ++i) {
            sum += static_cast<double>(l[i]) * std::polar(1.0, -2 * std::numbers::pi * hz * static_cast<double>(i) / kRate);
        }
        if (std::abs(sum) > top) {
            top = std::abs(sum);
            best = hz;
        }
    }
    check(std::abs(20 * std::log10(rms / want)) < 1, "frozen, a second after the input stopped: the tone's level within 1 dB (" + std::to_string(rms) + ")");
    check(std::abs(best - 1003) < 1, "and its pitch within 1 Hz (" + std::to_string(best) + ")");
}

void robustness() {
    std::vector<float> x(30000), l, r;
    for (std::size_t i = 0; i < x.size(); ++i) {
        x[i] = static_cast<float>(0.3 * std::sin(0.013 * static_cast<double>(i)) + 0.1 * std::sin(0.9 * static_cast<double>(i)));
    }
    std::vector<float> first;
    bool identical = true;
    for (std::size_t block : {1u, 64u, 333u, 4096u}) {
        S s;
        s.set(S::Spray, 50);
        s.set(S::Tilt, -40);
        s.prepare(kRate);
        run(s, x, l, r, block);
        if (first.empty()) {
            first = l;
        } else {
            identical = identical && l == first;
        }
    }
    check(identical, "every block size gives the same samples");
    S s;
    std::vector<float> bad(4096, std::numeric_limits<float>::quiet_NaN()), o(4096), p(4096);
    counting = true;
    s.process(bad.data(), bad.data(), o.data(), p.data(), bad.size());
    s.set(S::Freeze, 1);
    s.set(S::Time, 2000);
    s.set(S::Tilt, 100);
    s.set(S::Spray, 100);
    s.process(x.data(), x.data(), o.data(), p.data(), 4096);
    counting = false;
    check(allocations == 0, "processing and moved controls allocate nothing");
    check(std::all_of(o.begin(), o.end(), [](float v) { return std::isfinite(v); }), "a NaN input leaves the output finite");
    S z;
    std::vector<float> left(4800, 0.25f), shared(4800, -0.5f), rightOut(4800);
    z.process(left.data(), shared.data(), shared.data(), rightOut.data(), 4800);
    check(std::all_of(shared.begin() + 3000, shared.end(), [](float v) { return std::isfinite(v); }), "an output plane that is the other channel's input is safe");
}

}  // namespace

int main() {
    delays();
    shaping();
    freeze();
    robustness();
    std::printf("%s -- %d checks, %d failure(s)\n", failures ? "FAILED" : "PASS", checks, failures);
    return failures ? 1 : 0;
}
