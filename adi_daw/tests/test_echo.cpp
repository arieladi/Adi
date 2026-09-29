// SPDX-License-Identifier: MIT
#include "adi/dsp/echo.hpp"
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
using Redux = adi::dsp::Echo;
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
    c.set(C::TimeL, 100);
    c.set(C::Feedback, 0);
    c.set(C::Mix, 100);
    c.prepare(48000);
    std::vector<float> impulse(96000), zero(96000), a(96000), b(96000);
    impulse[0] = 1;
    auto run = [&] {
        c.prepare(48000);
        c.process(impulse.data(), zero.data(), a.data(), b.data(), a.size());
    };
    run();
    check(a[4800] == 1 && b[4800] == 0, "Stereo preserves input channel with 100 ms delay");
    c.set(C::Mode, 1);
    c.set(C::Feedback, 50);
    run();
    check(a[4800] == 1 && b[9600] == .5f && a[14400] == .25f,
          "PingPong alternates channels with measured feedback");
    c.set(C::Invert, 1);
    run();
    check(b[9600] == -.5f, "inverted ping-pong feedback");
    c.set(C::Mode, 2);
    c.set(C::Feedback, 0);
    run();
    check(a[4800] == 1 && b[4800] == 0, "equal M/S delays decode back to original stereo");
    c.set(C::Mode, 0);
    c.set(C::SyncL, 1);
    c.set(C::BeatsL, 1);
    c.tempo(90);
    for (int mode = 0; mode < 4; ++mode) {
        c.set(C::SyncModeL, mode);
        run();
        double sum = 0, moment = 0;
        for (std::size_t i = 0; i < a.size(); ++i) {
            sum += a[i];
            moment += a[i] * static_cast<double>(i);
        }
        const std::array<double, 4> factors{1, 2. / 3, 1.5, .25};
        check(std::abs(sum - 1) < 1e-6 &&
                  std::abs(moment / sum - 32000 * factors[static_cast<std::size_t>(mode)]) < .001,
              "synced note mode impulse centroid matches exact time");
    }
    c.set(C::SyncL, 0);
    c.set(C::TimeR, 80);
    run();
    check(a[3840] == 1, "linked right edit changes both delay times");
    c.set(C::Link, 0);
    c.set(C::TimeL, 100);
    c.set(C::OffsetL, 25);
    run();
    check(a[6000] == 1, "offset scales only its channel time");
    c.set(C::OffsetL, 0);
    auto x = tone(1000);
    c.set(C::Link, 1);
    c.set(C::TimeL, 10);
    c.set(C::FilterOn, 1);
    c.set(C::LP, 100);
    c.prepare(48000);
    auto dark = render(c, x);
    check(bin(dark, 1000) < .01, "lowpass attenuates echo above cutoff");
    c.set(C::FilterOn, 0);
    c.set(C::GateOn, 1);
    c.set(C::GateThreshold, -6);
    c.prepare(48000);
    auto gated = render(c, x);
    check(std::all_of(gated.begin(), gated.end(), [](float v) { return v == 0; }),
          "gate rejects input below threshold");
    c.set(C::GateOn, 0);
    c.set(C::DuckOn, 1);
    c.set(C::DuckThreshold, -30);
    c.prepare(48000);
    auto ducked = render(c, x);
    c.set(C::DuckOn, 0);
    c.prepare(48000);
    auto plain = render(c, x);
    check(bin(ducked, 1000) < bin(plain, 1000) * .2,
          "ducking reduces wet level while input is present");
    c.set(C::NoiseOn, 1);
    c.set(C::NoiseAmount, 50);
    c.prepare(48000);
    auto noise = render(c, zero);
    check(error(noise, zero) > 1e-6, "Noise creates nonzero wet character");
    c.prepare(48000);
    check(render(c, zero) == noise, "Noise deterministic after prepare");
    c.set(C::NoiseOn, 0);
    c.set(C::WobbleOn, 1);
    c.set(C::WobbleAmount, 100);
    c.prepare(48000);
    auto wobble = render(c, x);
    check(error(wobble, plain) > .001, "Floaty-derived wobble changes delayed pitch");
    c.set(C::WobbleOn, 0);
    c.set(C::ModDelay, 80);
    c.set(C::LfoSync, 1);
    c.set(C::LfoBeats, 2);
    c.prepare(48000);
    auto sync = render(c, x);
    c.set(C::LfoSync, 0);
    c.set(C::LfoRate, .75);
    c.prepare(48000);
    check(render(c, x) == sync, "LFO tempo sync matches 90 BPM / two quarter notes");
    c.set(C::ModDelay, 0);
    c.set(C::Reverb, 50);
    for (int location = 0; location < 3; ++location) {
        c.set(C::ReverbLocation, location);
        c.set(C::Feedback, 50);
        run();
        check(std::count_if(a.begin() + 10000, a.end(),
                            [](float v) { return std::abs(v) > 1e-8f; }) > 100,
              "every reverb position creates a diffuse tail");
    }
}
void transitions() {
    const auto input = tone(911);
    std::vector<float> a(input.size()), b(input.size());
    for (int repitch : {0, 1}) {
        C c;
        c.set(C::TimeL, 30);
        c.set(C::Feedback, 0);
        c.set(C::Mix, 100);
        c.set(C::Repitch, repitch);
        c.prepare(48000);
        c.process(input.data(), input.data(), a.data(), b.data(), 48000);
        c.set(C::TimeL, 70.13);
        c.process(input.data() + 48000, input.data() + 48000, a.data() + 48000, b.data() + 48000,
                  48000);
        double step = 0;
        for (std::size_t j = 48000; j < 49000; ++j)
            step = std::max(step, std::abs(static_cast<double>(a[j]) - a[j - 1]));
        check(std::isfinite(step), "time edits remain finite");
        if (repitch == 0)
            check(step < .055, "crossfade time edit avoids discontinuous read-head jump");
        std::printf("METRIC Echo repitch %d max edit step %.9f\n", repitch, step);
    }
}
void partitions() {
    auto x = tone(731);
    x.resize(16384);
    std::vector<float> expected;
    for (std::size_t block : {32u, 64u, 128u, 256u, 512u, 1024u, 2048u, 4096u}) {
        C c;
        c.set(C::TimeL, 10);
        c.set(C::ModDelay, 50);
        c.set(C::NoiseOn, 1);
        c.set(C::NoiseAmount, 10);
        c.set(C::WobbleOn, 1);
        c.set(C::WobbleAmount, 35);
        c.set(C::Reverb, 30);
        c.prepare(48000);
        auto y = render(c, x, block);
        if (expected.empty())
            expected = y;
        check(y == expected, "noise/wobble/reverb partitions identical 32..4096");
    }
    C c;
    c.prepare(48000);
    std::vector<float> a(x.size()), b(x.size());
    allocations = 0;
    counting = true;
    c.process(x.data(), x.data(), a.data(), b.data(), x.size());
    counting = false;
    check(allocations == 0, "Echo process allocates nothing");
}
} // namespace
int main() {
    measurements();
    transitions();
    partitions();
    std::printf("%s -- %d checks, %d failure(s)\n", failures ? "FAIL" : "PASS", checks, failures);
    return failures ? 1 : 0;
}
