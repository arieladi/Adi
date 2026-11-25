// SPDX-License-Identifier: GPL-3.0-or-later
#include "adi/dsp/adi_shaper_time.hpp"
#include <array>
#include <cmath>
#include <cstdio>
#include <limits>

int main() {
    using Time = adi::dsp::AdiShaperTime;
    Time time;
    int failures = 0;
    auto check = [&](bool ok, const char* label) {
        if (!ok) { ++failures; std::fprintf(stderr, "FAIL %s\n", label); }
    };
    auto read = [&](float l, float r, double delay, bool interpolate = false) {
        time.processStereo(l, r, delay, interpolate);
        return std::array{l, r};
    };
    check(read(.5f, -.125f, 0) == std::array{.5f,-.125f}, "zero offset before prepare");
    check(read(.5f, .5f, 1) == std::array{0.f,0.f}, "no stale values before prepare");
    check(time.prepare(), "history allocation");
    for (std::size_t i = 0; i <= Time::capacity * 2 + 64; ++i) {
        const bool mark = i == 20 || i == Time::capacity + 35;
        const auto a = read(mark ? .5f : 0.f, mark ? -.375f : 0.f, Time::maximumDelay);
        const bool heard = i == Time::maximumDelay + 20 || i == Time::capacity + 35 + Time::maximumDelay;
        if (a != (heard ? std::array{.5f,-.375f} : std::array{0.f,0.f})) {
            check(false, "long tap and stereo ring wrapping");
            break;
        }
    }
    time.reset();
    check(read(.5f,-.25f,.875) == std::array{.5f,-.25f}, "stationary fractional reads truncate to a sample");
    time.reset();
    check(read(.5f, -.25f, .25, true) == std::array{.375f,-.1875f}, "startup fraction uses zero prehistory");
    check(read(0.f,0.f,.25,true) == std::array{.125f,-.0625f}, "fractional impulse tail");
    time.reset();
    for (const double invalid : {static_cast<double>(Time::maximumDelay + 1), -1.0,
                                 std::numeric_limits<double>::quiet_NaN(), std::numeric_limits<double>::infinity()})
        check(read(.25f,-.125f,invalid) == std::array{0.f,0.f}, "invalid tap returns wet silence");
    check(read(0.f,0.f,1.0) == std::array{.25f,-.125f}, "history survives unavailable reads");
    time.reset();
    std::array<float, 8> left{}, right{};
    for (std::size_t i = 0; i < left.size(); ++i) {
        left[i] = static_cast<float>(i + 1);
        right[i] = -left[i] * .25f;
    }
    check(!time.processFixedBlock(left.data(), right.data(), left.data(), right.data(), left.size(), 2.75),
          "empty history rejects a block without consuming input");
    (void)read(11.f, -.5f, 0.);
    (void)read(12.f, -.75f, 0.);
    check(time.processFixedBlock(left.data(), right.data(), right.data(), left.data(), left.size(), 2.75),
          "fixed read can cross its write cursor and accepts crossed channel buffers");
    check(left == std::array<float, 8>{-.5f, -.75f, -.25f, -.5f, -.75f, -1.f, -1.25f, -1.5f}
          && right == std::array<float, 8>{11, 12, 1, 2, 3, 4, 5, 6},
          "fixed block truncates age and captures both input channels before overwriting them");
    check(read(0, 0, 1) == std::array{8.f, -2.f}, "moving read follows fixed block without losing input");
    time.reset();
    (void)read(std::numeric_limits<float>::infinity(), -.5f, 0.);
    check(read(.125f, .25f, 1.) == std::array{0.f, -.5f}, "moving Time history replaces a bad sample on one channel");
    time.reset();
    (void)read(.125f, .5f, 0.);
    (void)read(.25f, .75f, 0.);
    std::array bad{std::numeric_limits<float>::quiet_NaN(), .5f};
    std::array finite{-.75f, -.5f};
    check(time.processFixedBlock(bad.data(), finite.data(), bad.data(), finite.data(), bad.size(), 2.),
          "fixed Time history accepts an in-place block once prepared");
    check(read(.25f, .125f, 2.) == std::array{0.f, -.75f}, "fixed Time history replaces a bad sample on one channel");
    check(time.prepare(), "reprepare");
    check(read(0.f,0.f,1.) == std::array{0.f,0.f}, "reprepare invalidates previous storage");
    return failures == 0 ? 0 : 1;
}
