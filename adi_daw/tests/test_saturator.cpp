// SPDX-License-Identifier: MIT
#include "adi/dsp/saturator.hpp"
#include <algorithm>
#include <bit>
#include <cmath>
#include <complex>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <limits>
#include <new>
#include <numbers>
#include <vector>
namespace {
bool counting = false;
std::size_t allocations = 0;
} // namespace
void *operator new(std::size_t n) {
    if (counting)
        ++allocations;
    if (auto p = std::malloc(n ? n : 1))
        return p;
    throw std::bad_alloc();
}
void *operator new[](std::size_t n) { return operator new(n); }
void operator delete(void *p) noexcept { std::free(p); }
void operator delete[](void *p) noexcept { std::free(p); }
void operator delete(void *p, std::size_t) noexcept { std::free(p); }
void operator delete[](void *p, std::size_t) noexcept { std::free(p); }

namespace {
using Redux = adi::dsp::Saturator;
using C = Redux;
int checks = 0, failures = 0;
void check(bool ok, const char *what) {
    ++checks;
    if (!ok) {
        ++failures;
        std::printf("FAIL %s\n", what);
    }
}
constexpr double pi = std::numbers::pi;
std::vector<float> tone(double hz, double amplitude = 0.4) {
    std::vector<float> x(96000);
    for (std::size_t i = 0; i < x.size(); ++i)
        x[i] =
            static_cast<float>(amplitude * std::sin(2 * pi * hz * static_cast<double>(i) / 48000));
    return x;
}
std::vector<float> render(Redux &c, const std::vector<float> &in, std::size_t block = 64) {
    std::vector<float> out(in.size()), right(in.size());
    for (std::size_t i = 0; i < in.size(); i += block)
        c.process(in.data() + i, in.data() + i, out.data() + i, right.data() + i,
                  std::min(block, in.size() - i));
    return out;
}
double bin(const std::vector<float> &x, double hz) {
    std::complex<double> sum = 0, phase = 1, step = std::polar(1., -2 * pi * hz / 48000);
    for (std::size_t i = 48000; i < x.size(); ++i) {
        sum += static_cast<double>(x[i]) * phase;
        phase *= step;
    }
    return 2 * std::abs(sum) / 48000;
}
double error(const std::vector<float> &a, const std::vector<float> &b) {
    double sum = 0;
    for (std::size_t i = 48000; i < a.size(); ++i)
        sum += std::pow(static_cast<double>(a[i]) - b[i], 2);
    return sum / 48000;
}

void measurements() {
    auto x = tone(1000);
    C c;
    c.prepare(48000);
    check(render(c, x) == x, "Analog stays exactly linear below its knee");
    c.set(C::CurveType, 6);
    c.set(C::Drive, 18);
    c.prepare(48000);
    auto hard = render(c, x);
    check(*std::max_element(hard.begin(), hard.end()) == 1, "Digital clips exactly at unity");
    check(bin(hard, 7011) < 1e-7, "harmonic floor first");
    check(bin(hard, 3000) > .1, "driven hard clipping creates odd harmonics");
    c.set(C::CurveType, 2);
    c.set(C::Drive, 0);
    c.set(C::BassThreshold, -6);
    c.prepare(48000);
    check(render(c, x) == x, "Bass Shaper is linear below threshold");
    c.set(C::BassThreshold, -30);
    c.prepare(48000);
    check(error(render(c, x), x) > .00001, "lower Bass threshold softens response");
    c.set(C::CurveType, 5);
    c.prepare(48000);
    float in1 = .5f, in2 = 1.f, l = 0, r = 0;
    c.process(&in1, &in1, &l, &r, 1);
    check(std::abs(l - 1) < 1e-6, "Sinoid reaches its first crest");
    c.process(&in2, &in2, &l, &r, 1);
    check(std::abs(l) < 1e-6, "Sinoid folds back as input increases");
    c.set(C::CurveType, 7);
    c.set(C::ShaperDrive, 0);
    c.prepare(48000);
    check(render(c, x) == x, "custom drive zero removes custom shaping");
    c.set(C::ShaperDrive, 100);
    for (auto param : {C::Curve, C::Depth, C::Linear, C::Damp, C::Period}) {
        C a;
        a.set(C::CurveType, 7);
        a.set(C::ShaperDrive, 100);
        a.set(C::Depth, 30);
        a.prepare(48000);
        auto base = render(a, x);
        a.set(param, param == C::Period ? 7 : 90);
        a.prepare(48000);
        check(error(base, render(a, x)) > 1e-5, "each custom control changes the curve");
    }
    c.set(C::CurveType, 0);
    c.set(C::ColorOn, 1);
    c.set(C::AmtLo, 18);
    c.set(C::AmtHi, -18);
    c.set(C::Frequency, 1000);
    c.prepare(48000);
    auto tiny = tone(173, .0001);
    check(error(render(c, tiny), tiny) < 1e-15,
          "inverse Color cancels pre-emphasis in linear region");
    c.set(C::CurveType, 7);
    c.set(C::Depth, 100);
    c.set(C::Curve, 100);
    c.set(C::Drive, 24);
    c.set(C::Output, -6);
    for (int clip : {1, 2}) {
        c.set(C::PostClip, clip);
        c.prepare(48000);
        auto y = render(c, x);
        check(std::all_of(y.begin(), y.end(), [](float v) { return std::abs(v) <= .501188f; }),
              "post clipping limits output even after Color boost");
    }
    C dc;
    dc.set(C::PreDC, 1);
    dc.prepare(48000);
    std::vector<float> constant(96000, .3f);
    auto zero = render(dc, constant);
    check(std::abs(zero.back()) < 1e-10, "Pre-DC rejects constant input");
    C hq;
    hq.set(C::CurveType, 6);
    hq.set(C::Drive, 18);
    hq.prepare(48000);
    auto high = tone(11000, .7), raw = render(hq, high);
    hq.set(C::HiQuality, 1);
    hq.prepare(48000);
    auto filtered = render(hq, high);
    const double before = bin(raw, 15000), after = bin(filtered, 15000);
    check(after < before * .2, "4x HiQuality suppresses folded third harmonic by more than 14 dB");
    check(bin(filtered, 11000) > .2, "HiQuality keeps wanted energy");
    check(hq.latency() == 16, "HQ advertises 16-sample group delay");
    std::printf("METRIC alias raw %.9f HQ %.9f\n", before, after);
    hq.set(C::Mix, 0);
    hq.prepare(48000);
    auto dry = render(hq, x);
    bool aligned = true;
    for (std::size_t i = 16; i < x.size(); ++i)
        aligned &= dry[i] == x[i - 16];
    check(aligned, "HQ dry signal aligned by 16 samples");
}
void partitions() {
    auto x = tone(731);
    x.resize(8192);
    std::vector<float> expected;
    for (std::size_t block : {32u, 64u, 128u, 256u, 512u, 1024u, 2048u, 4096u}) {
        C c;
        c.set(C::HiQuality, 1);
        c.set(C::Drive, 18);
        c.set(C::ColorOn, 1);
        c.set(C::AmtLo, 6);
        c.prepare(48000);
        auto y = render(c, x, block);
        if (expected.empty())
            expected = y;
        check(y == expected, "HQ partition identity 32..4096");
    }
    C c;
    c.set(C::HiQuality, 1);
    c.prepare(48000);
    std::vector<float> a(x.size()), b(x.size());
    allocations = 0;
    counting = true;
    c.process(x.data(), x.data(), a.data(), b.data(), x.size());
    counting = false;
    check(allocations == 0, "HQ process allocates nothing");
}
} // namespace
int main() {
    measurements();
    partitions();
    std::printf("%s -- %d checks, %d failure(s)\n", failures ? "FAIL" : "PASS", checks, failures);
    return failures ? 1 : 0;
}
