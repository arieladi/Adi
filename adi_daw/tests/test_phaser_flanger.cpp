// SPDX-License-Identifier: MIT
#include "adi/dsp/phaser_flanger.hpp"
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
using Redux = adi::dsp::PhaserFlanger;
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
    C c;
    c.set(C::Notches, 1);
    c.set(C::Amount, 0);
    c.set(C::Center, 1000);
    c.set(C::Mix, 50);
    c.prepare(48000);
    auto x = tone(1000);
    auto notch = render(c, x);
    check(bin(notch, 7011) < 1e-7, "phaser floor first");
    check(bin(notch, 1000) < 1e-6, "single allpass mixed dry creates exact center notch");
    c.set(C::Mix, 100);
    c.prepare(48000);
    auto ap = render(c, x);
    check(std::abs(bin(ap, 1000) - .4) < 1e-6,
          "allpass preserves magnitude while reversing phase at center");
    c.set(C::Mode, 1);
    c.set(C::Time, 2);
    c.set(C::Mix, 50);
    c.prepare(48000);
    auto comb = render(c, tone(250));
    check(bin(comb, 250) < 1e-6, "2 ms Flanger comb first notch is 250 Hz");
    c.set(C::Mode, 2);
    c.set(C::Time, 20);
    c.set(C::Mix, 100);
    c.prepare(48000);
    std::vector<float> impulse(4000), a(4000), b(4000);
    impulse[0] = 1;
    c.process(impulse.data(), impulse.data(), a.data(), b.data(), a.size());
    check(a[960] == 1, "Doubler delay is exactly 20 ms without modulation");
    c.set(C::Feedback, 50);
    c.set(C::Invert, 1);
    c.prepare(48000);
    c.process(impulse.data(), impulse.data(), a.data(), b.data(), a.size());
    check(a[1920] == -.5f, "negative feedback alternates echo polarity");
    c.set(C::Amount, 40);
    c.set(C::Feedback, 0);
    c.set(C::Sync, 1);
    c.set(C::Beats, 2);
    c.tempo(90);
    c.prepare(48000);
    auto synced = render(c, x);
    c.set(C::Sync, 0);
    c.set(C::Rate, .75);
    c.prepare(48000);
    check(render(c, x) == synced, "90 BPM two-quarter-note sync equals 0.75 Hz");
    c.set(C::Lfo2Mix, 100);
    c.set(C::Sync2, 1);
    c.set(C::Beats2, 3);
    c.prepare(48000);
    auto sync2 = render(c, x);
    c.set(C::Sync2, 0);
    c.set(C::Rate2, .5);
    c.prepare(48000);
    check(render(c, x) == sync2, "second LFO sync equals expected Hz");
    c.set(C::Mode, 0);
    c.set(C::Amount, 0);
    c.set(C::Lfo2Mix, 0);
    c.set(C::SafeBassOn, 1);
    c.set(C::SafeBass, 3000);
    c.prepare(48000);
    auto bass = tone(10);
    check(error(render(c, bass), bass) < 1e-5,
          "Safe Bass protects sub-bass from allpass processing");
    c.set(C::SafeBassOn, 0);
    c.set(C::EnvOn, 1);
    c.set(C::EnvAmount, 100);
    c.prepare(48000);
    auto env = render(c, x);
    c.set(C::EnvOn, 0);
    c.prepare(48000);
    check(error(env, render(c, x)) > .001, "envelope follower changes the notch position");
    for (int wave = 0; wave < 10; ++wave) {
        c.set(C::Wave, wave);
        c.set(C::Amount, 100);
        c.set(C::Duty, 100);
        c.prepare(48000);
        auto y = render(c, x);
        check(std::all_of(y.begin(), y.end(), [](float v) { return std::isfinite(v); }),
              "all ten waveforms remain finite at duty endpoint");
    }
}
void partitions() {
    const auto x = tone(731);
    std::vector<float> expected;
    for (std::size_t block : {32u, 64u, 128u, 256u, 512u, 1024u, 2048u, 4096u}) {
        C c;
        c.set(C::Mode, 1);
        c.set(C::Wave, 8);
        c.set(C::Feedback, 65);
        c.set(C::Lfo2Mix, 33);
        c.set(C::EnvOn, 1);
        c.set(C::EnvAmount, 20);
        c.prepare(48000);
        auto y = render(c, x, block);
        if (expected.empty())
            expected = y;
        check(y == expected, "modulation partitions identical 32..4096");
    }
    C c;
    c.prepare(48000);
    std::vector<float> a(x.size()), b(x.size());
    allocations = 0;
    counting = true;
    c.process(x.data(), x.data(), a.data(), b.data(), x.size());
    counting = false;
    check(allocations == 0, "no process allocations");
}
} // namespace
int main() {
    measurements();
    partitions();
    std::printf("%s -- %d checks, %d failure(s)\n", failures ? "FAIL" : "PASS", checks, failures);
    return failures ? 1 : 0;
}
