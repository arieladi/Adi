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
    auto read = [&](float l, float r, double delay) {
        time.processStereo(l, r, delay);
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
    check(read(.5f, -.25f, .25) == std::array{.375f,-.1875f}, "startup fraction uses zero prehistory");
    check(read(0.f,0.f,.25) == std::array{.125f,-.0625f}, "fractional impulse tail");
    time.reset();
    for (const double invalid : {static_cast<double>(Time::maximumDelay + 1), -1.0,
                                 std::numeric_limits<double>::quiet_NaN(), std::numeric_limits<double>::infinity()})
        check(read(.25f,-.125f,invalid) == std::array{0.f,0.f}, "invalid tap returns wet silence");
    check(read(0.f,0.f,1.0) == std::array{.25f,-.125f}, "history survives unavailable reads");
    check(time.prepare(), "reprepare");
    check(read(0.f,0.f,1.) == std::array{0.f,0.f}, "reprepare invalidates previous storage");
    return failures == 0 ? 0 : 1;
}
