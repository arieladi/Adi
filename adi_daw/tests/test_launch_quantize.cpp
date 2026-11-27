// SPDX-License-Identifier: GPL-3.0-or-later
#include "adi/engine/launch_quantize.hpp"
#include "adi/textproj.hpp"

#include <array>
#include <cstdio>
#include <limits>

namespace {
int checks = 0;
int failures = 0;
void check(bool value, const char* name) {
    ++checks;
    if (!value) {
        ++failures;
        std::printf("FAIL: %s\n", name);
    }
}
}

int main() {
    using namespace adi;
    using namespace adi::engine::launch_quantize;
    constexpr auto quarter = textproj::kPPQ;
    constexpr auto whole = textproj::kWhole;
    constexpr std::array six{rows::TimeSignature{0, 6, 8}};
    constexpr std::array seven{rows::TimeSignature{0, 7, 8}};
    check(nextGridTick(0, LaunchQuantization::Bar, {}) == 0, "the start is an inclusive grid point");
    check(nextGridTick(whole, LaunchQuantization::Bar, {}) == whole,
          "a press exactly on a grid line can fire on that line");
    check(nextGridTick(whole + 1, LaunchQuantization::Bar, {}) == 2 * whole,
          "a press just after a grid line waits for the following line");
    check(nextGridTick(123, LaunchQuantization::None, {}) == 123, "None is immediate");
    check(nextGridTick(3 * quarter + quarter / 2 + 1, LaunchQuantization::Quarter, seven) == 4 * quarter,
          "quarter-note grid is anchored to song zero in 7/8");
    check(nextGridTick(10 * quarter + 1, LaunchQuantization::Quarter, seven) == 11 * quarter,
          "third 7/8 bar follows the measured eleven-quarter launch");
    check(nextGridTick(7 * quarter / 2 + 1, LaunchQuantization::Eighth, seven) == 4 * quarter &&
          nextGridTick(7 * quarter / 2 + 1, LaunchQuantization::EighthTriplet, seven) == 11 * quarter / 3,
          "7/8 eighths and eighth triplets count from song zero");
    check(nextGridTick(3 * quarter + 1, LaunchQuantization::Quarter, six) == 4 * quarter &&
          nextGridTick(3 * quarter + 1, LaunchQuantization::Eighth, six) == 7 * quarter / 2 &&
          nextGridTick(3 * quarter + 1, LaunchQuantization::EighthTriplet, six) == 10 * quarter / 3,
          "6/8 uses the same quarter, eighth and eighth-triplet intervals");
    check(nextGridTick(3 * quarter + 1, LaunchQuantization::QuarterTriplet, six) ==
          3 * quarter + quarter / 3, "6/8 keeps quarter-note triplets on the absolute grid");
    check(nextGridTick(3 * quarter + 1, LaunchQuantization::Half, six) == 4 * quarter,
          "a half-note grid continues through an odd 6/8 bar line");
    check(nextGridTick(7 * quarter / 2 + 1, LaunchQuantization::Bar, seven) == 7 * quarter,
          "one bar follows the 7/8 meter");
    constexpr std::array mixed{rows::TimeSignature{0, 4, 4}, rows::TimeSignature{2 * whole, 3, 4}};
    check(nextGridTick(whole + 1, LaunchQuantization::FourBars, mixed) == 2 * whole,
          "a signature change starts a new whole-bar grouping");
    check(nextGridTick(2 * whole + 1, LaunchQuantization::TwoBars, mixed) == 2 * whole + 6 * quarter,
          "multi-bar groups use the active signature");
    check(!nextGridTick(std::numeric_limits<std::int64_t>::max(), LaunchQuantization::Bar, {}),
          "the grid refuses an overflowing next tick");
    check(quantumBeats(0, LaunchQuantization::TwoBars, seven) == 7.0,
          "Link quantum uses the bar length in quarters");
    check(quantumBeats(0, LaunchQuantization::Sixteenth, seven) == 0.25 &&
          !quantumBeats(0, LaunchQuantization::None, seven), "sub-bar and None use their documented Link meanings");
    std::printf("%s -- %d checks, %d failures\n", failures ? "FAIL" : "PASS", checks, failures);
    return failures ? 1 : 0;
}
