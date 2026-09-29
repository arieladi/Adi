// SPDX-License-Identifier: MIT
#include "adi/dsp/overdrive.hpp"
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
using Core = adi::dsp::Overdrive;
using C = Core;
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
std::vector<float> render(Core &c, const std::vector<float> &in, std::size_t block = 64) {
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
void measurements() {
    auto x = tone(1000);
    C c;
    c.set(C::Mix, 0);
    c.prepare(48000);
    check(render(c, x) == x, "zero wet is bit-exact dry");
    c.set(C::Mix, 100);
    c.set(C::Dynamics, 0);
    c.set(C::Tone, 100);
    c.set(C::Drive, 0);
    c.prepare(48000);
    auto zero = render(c, x);
    check(bin(zero, 7011) < 1e-7, "zero-drive floor bin first");
    check(bin(zero, 2000) > 1e-3, "zero Drive still distorts at asymmetric knee");
    c.set(C::Drive, 100);
    c.prepare(48000);
    auto driven = render(c, x);
    check(bin(driven, 3000) > bin(zero, 3000) * 2, "Drive adds harmonic energy");
    c.set(C::Tone, 0);
    c.prepare(48000);
    auto dark = render(c, x);
    check(bin(dark, 3000) < bin(driven, 3000) * .3, "post tone darkens high harmonics");
    c.set(C::Tone, 100);
    c.set(C::Drive, 0);
    c.set(C::Bandwidth, .1);
    c.set(C::Frequency, 1000);
    c.prepare(48000);
    const double pass = bin(render(c, tone(1000, .01)), 1000);
    c.prepare(48000);
    const double reject = bin(render(c, tone(4000, .01)), 4000);
    check(pass > reject * 20, "pre band-pass rejects off-band input before shaping");
    auto ratio = [&](double dynamics) {
        C a;
        a.set(C::Drive, 100);
        a.set(C::Dynamics, dynamics);
        a.prepare(48000);
        auto quiet = render(a, tone(1000, .04));
        a.prepare(48000);
        auto loud = render(a, tone(1000, .4));
        return bin(loud, 1000) / bin(quiet, 1000);
    };
    const auto compressed = ratio(0), preserved = ratio(100);
    std::printf("METRIC dynamics low %.6f high %.6f\n", compressed, preserved);
    check(preserved > compressed * 2, "high Dynamics preserves more input level difference");
    C base;
    base.prepare(48000);
    auto expected = render(base, x, 32);
    for (std::size_t block : {32u, 64u, 128u, 256u, 512u, 1024u, 2048u, 4096u}) {
        C same;
        same.prepare(48000);
        check(render(same, x, block) == expected, "sample stream identical across block sizes");
    }
    std::vector<float> left(4096, .2f), right(4096), out(4096);
    C rt;
    rt.prepare(48000);
    allocations = 0;
    counting = true;
    for (int i = 0; i < 10; ++i)
        rt.process(left.data(), left.data(), out.data(), right.data(), left.size());
    counting = false;
    check(allocations == 0, "process allocates zero");
    C automation;
    automation.prepare(48000);
    for (int p = 0; p < C::Count; ++p) {
        automation.set(static_cast<C::Param>(p), std::numeric_limits<double>::quiet_NaN());
        automation.set(static_cast<C::Param>(p), 1e100);
    }
    auto stable = render(automation, x);
    check(std::all_of(stable.begin(), stable.end(), [](float v) { return std::isfinite(v); }),
          "clamped parameters and smoothing remain finite");
}
} // namespace
int main() {
    measurements();
    std::printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
