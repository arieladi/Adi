// SPDX-License-Identifier: MIT
#include "adi/dsp/chorus_ensemble.hpp"
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
using Redux = adi::dsp::ChorusEnsemble;
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
    auto input = tone(1000);
    C c;
    c.set(C::DelayAuto, 0);
    c.set(C::Time, 10);
    c.set(C::Amount, 0);
    c.set(C::Mix, 100);
    c.prepare(48000);
    std::vector<float> impulse(2400), r(2400), o(2400);
    impulse[0] = 1;
    c.process(impulse.data(), impulse.data(), o.data(), r.data(), o.size());
    check(o[480] == 1, "fixed 10 ms delay exact at 48 kHz");
    check(std::count_if(o.begin(), o.end(), [](float v) { return v != 0; }) == 1,
          "zero feedback impulse has one delayed event");
    c.set(C::Feedback, 50);
    c.prepare(48000);
    c.process(impulse.data(), impulse.data(), o.data(), r.data(), o.size());
    check(std::abs(o[960] - .5f) < 1e-6, "feedback repeat is half amplitude");
    c.set(C::Invert, 1);
    c.prepare(48000);
    c.process(impulse.data(), impulse.data(), o.data(), r.data(), o.size());
    check(std::abs(o[960] + .5f) < 1e-6, "feedback invert changes repeat polarity");
    c.set(C::Feedback, 0);
    c.set(C::Invert, 0);
    c.set(C::Amount, 30);
    c.set(C::Rate, 2);
    c.prepare(48000);
    auto mod = render(c, input);
    check(bin(mod, 7011) < 1e-5, "modulation far bin first");
    check(bin(mod, 998) + bin(mod, 1002) < .0001, "opposite chorus taps cancel odd modulation sidebands");
    check(bin(mod, 996) + bin(mod, 1004) > .005, "two-tap chorus retains even modulation sidebands");
    c.set(C::Taps,1);c.prepare(48000);auto single=render(c,input);
    check(bin(single,998)+bin(single,1002)>.005,"single tap retains rate-spaced sidebands");
    c.set(C::Taps,2);
    c.set(C::Mode, 2);
    c.set(C::Mix, 0);
    c.set(C::Feedback, 95);
    c.prepare(48000);
    auto vibrato = render(c, input);
    c.set(C::Mix, 100);
    c.set(C::Feedback, 0);
    c.prepare(48000);
    check(render(c, input) == vibrato, "Vibrato ignores Mix and Feedback");
    c.set(C::Amount, 20);
    c.prepare(48000);
    vibrato = render(c, input);
    double lo = 2000, hi = 0, last = -1;
    for (std::size_t i = 48001; i < vibrato.size(); ++i)
        if (vibrato[i - 1] <= 0 && vibrato[i] > 0) {
            double crossing = static_cast<double>(i - 1) -
                              vibrato[i - 1] / static_cast<double>(vibrato[i] - vibrato[i - 1]);
            if (last > 0) {
                double hz = 48000 / (crossing - last);
                lo = std::min(lo, hz);
                hi = std::max(hi, hz);
            }
            last = crossing;
        }
    check(lo > 978 && lo < 982 && hi > 1018 && hi < 1022,
          "Vibrato 1.6 ms depth at 2 Hz produces predicted +/-20.1 Hz excursion");
    std::printf("METRIC vibrato %.6f..%.6f Hz\n", lo, hi);
    c.set(C::Mode, 1);
    c.set(C::Amount, 50);
    c.prepare(48000);
    auto ensemble = render(c, input);
    c.set(C::Mode, 0);
    c.prepare(48000);
    check(error(ensemble, render(c, input)) > .001,
          "three-phase Ensemble differs from two-tap Chorus");
    c.set(C::HighPassOn, 1);
    c.set(C::HighPass, 2000);
    c.prepare(48000);
    auto bass = tone(10);
    auto protectedBass = render(c, bass);
    check(error(protectedBass, bass) < 1e-5, "high pass keeps bass out of modulation");
    c.set(C::HighPassOn, 0);
    c.set(C::Amount, 0);
    c.set(C::Time, 10);
    c.set(C::Warmth, 100);
    c.prepare(48000);
    auto warm = render(c, tone(1000, .9));
    check(bin(warm, 3000) > .001, "Warmth adds harmonic distortion");
}
void partitions() {
    const auto input = tone(731);
    std::vector<float> expected;
    for (std::size_t block : {32u, 64u, 128u, 256u, 512u, 1024u, 2048u, 4096u}) {
        C c;
        c.set(C::Mode, 1);
        c.set(C::Feedback, 70);
        c.set(C::Warmth, 35);
        c.prepare(48000);
        auto out = render(c, input, block);
        if (expected.empty())
            expected = out;
        check(out == expected, "all block partitions identical");
    }
    C c;
    c.prepare(48000);
    std::vector<float> out(input.size()), r(input.size());
    allocations = 0;
    counting = true;
    c.process(input.data(), input.data(), out.data(), r.data(), out.size());
    counting = false;
    check(allocations == 0, "no processing allocations");
    c.set(C::Width, 0);
    c.prepare(48000);
    c.process(input.data(), input.data(), out.data(), r.data(), out.size());
    check(out == r, "Width zero is mono");
    c.set(C::Mix, 0);
    c.prepare(48000);
    check(render(c, input) == input, "zero mix exact dry");
}
} // namespace
int main() {
    measurements();
    partitions();
    std::printf("%s -- %d checks, %d failure(s)\n", failures ? "FAIL" : "PASS", checks, failures);
    return failures ? 1 : 0;
}
