// SPDX-License-Identifier: AGPL-3.0-or-later
#include "adi/time_base.hpp"
#include <array>
#include <iostream>
#include <limits>

namespace {
using namespace adi::time_base;
using enum Rounding;

int checks = 0, failures = 0;
void check(bool ok, const char *name) {
    ++checks;
    if (!ok) {
        std::cerr << name << '\n';
        ++failures;
    }
}

void scale() {
    constexpr auto max = std::numeric_limits<std::int64_t>::max();
    constexpr auto min = std::numeric_limits<std::int64_t>::min();
    check(rescale(max, max, max, Exact) == max && rescale(min, max, max, Exact) == min,
          "the full signed domain survives a full-width identity ratio");
    check(rescale(min, max - 1, max, Floor) == min + 1 &&
              rescale(max, max - 1, max, Exact) == max - 1 &&
              rescale(max - 2, max - 1, max, Nearest) == max - 3,
          "the high-product path retains the exact quotient and remainder");
    check(!rescale(max, max, max - 1, Floor) && !rescale(min, max, max - 1, Nearest) &&
              !rescale(max, 2, 1, Exact) && !rescale(min, 2, 1, Exact),
          "both signs refuse overflow on the narrow and high-product paths");
    check(!rescale(0, 0, 1, Exact) && !rescale(1, 1, 0, Floor) &&
              !rescale(1, -1, 1, Nearest),
          "invalid ratios are refused");
    check(rescale(3, 1, 2, Floor) == 1 && rescale(-3, 1, 2, Floor) == -2 &&
              rescale(3, 1, 2, Nearest) == 2 && rescale(-3, 1, 2, Nearest) == -2 &&
              !rescale(3, 1, 2, Exact),
          "fractional time follows the explicit signed rounding rule");
    check(rescale(-1, 1, max, Nearest) == 0 && rescale(-1, 1, max, Floor) == -1,
          "subunit positions can round to zero or to the preceding unit");
    check(rescale(960, 5'765'760, 960, Exact) == 5'765'760 &&
              rescale(-5'765'760, 960, 5'765'760, Exact) == -960,
          "ADI and MIDI ticks can use the same rational conversion");
}

void linearTime() {
    constexpr std::array<std::uint32_t, 15> sampleRates = {
        8000, 11025, 16000, 22050, 32000, 44100, 48000, 88200, 96000, 176400,
        192000, 352800, 384000, 705600, 768000};
    for (const auto rate : sampleRates) {
        check(framesToNanoseconds(rate, {rate}, Exact) == 1'000'000'000 &&
                  nanosecondsToFrames(-1'000'000'000, {rate}, Exact) == -std::int64_t{rate},
              "each supported sample clock reaches the exact whole second");
        constexpr std::int64_t frame = 999'995;
        const auto ns = framesToNanoseconds(frame, {rate}, Nearest);
        check(ns && nanosecondsToFrames(*ns, {rate}, Nearest) == frame,
              "nearest nanoseconds round-trip to the original sample at all rates");
        check(nanosecondsToFrames(-1, {rate}, Floor) == -1 &&
                  nanosecondsToFrames(-1, {rate}, Nearest) == 0 &&
                  !nanosecondsToFrames(-1, {rate}, Exact),
              "negative sub-sample positions floor in the correct direction");
    }
    const Rate ntsc{30000, 1001};
    check(framesToNanoseconds(1, ntsc, Floor) == 33'366'666 &&
              framesToNanoseconds(1, ntsc, Nearest) == 33'366'667 &&
              framesToNanoseconds(3, ntsc, Exact) == 100'100'000,
          "NTSC uses the exact 1001/30000 second ratio");
    check(nanosecondsToFrames(100'100'000, ntsc, Exact) == 3 &&
              nanosecondsToFrames(100'099'999, ntsc, Floor) == 2 &&
              nanosecondsToFrames(-100'099'999, ntsc, Floor) == -3,
          "fractional video frames use integer edges");
    check(!framesToNanoseconds(1, {48000, 0}, Exact) && !nanosecondsToFrames(1, {}, Floor) &&
              !framesToNanoseconds(std::numeric_limits<std::int64_t>::max(), {48000}, Floor),
          "an invalid rate or unrepresentable nanosecond position is refused");
}

void smpte() {
    const Rate ntsc{30000, 1001}, doubleNtsc{60000, 1001};
    struct Vector { std::int64_t index; Timecode code; };
    constexpr std::array<Vector, 9> vectors = {{{0, {0, 0, 0, 0}},
                                                {1799, {0, 0, 59, 29}},
                                                {1800, {0, 1, 0, 2}},
                                                {17981, {0, 9, 59, 29}},
                                                {17982, {0, 10, 0, 0}},
                                                {19782, {0, 11, 0, 2}},
                                                {107891, {0, 59, 59, 29}},
                                                {107892, {1, 0, 0, 0}},
                                                {2589407, {23, 59, 59, 29}}}};
    for (const auto &vector : vectors) {
        check(frameToTimecode(vector.index, ntsc, true) == vector.code &&
                  timecodeToFrame(vector.code, ntsc, true) == vector.index,
              "29.97 drop-frame vector is exact in both directions");
    }
    check(frameToTimecode(3600, doubleNtsc, true) == Timecode{0, 1, 0, 4} &&
              frameToTimecode(35964, doubleNtsc, true) == Timecode{0, 10, 0, 0} &&
              timecodeToFrame({0, 1, 0, 4}, doubleNtsc, true) == 3600,
          "59.94 drop-frame skips four labels except every tenth minute");
    check(timecodeToFrame({0, 1, 0, 0}, ntsc, false) == 1800 &&
              frameToTimecode(1800, ntsc, false) == Timecode{0, 1, 0, 0} &&
              timecodeToFrame({1, 0, 0, 0}, {24000, 1001}, false) == 86400 &&
              timecodeToFrame({1, 0, 0, 0}, {25}, false) == 90000 &&
              timecodeToFrame({0, 1, 0, 2}, {60000, 2002}, true) == 1800,
          "non-drop labels use their nominal rate and equivalent rates reduce exactly");
    check(!timecodeToFrame({0, 1, 0, 1}, ntsc, true) &&
              !timecodeToFrame({0, 9, 0, 3}, doubleNtsc, true) &&
              !timecodeToFrame({0, 0, 0, 30}, ntsc, false) &&
              !timecodeToFrame({24, 0, 0, 0}, ntsc, true) &&
              !timecodeToFrame({0, -1, 0, 0}, ntsc, false) &&
              !timecodeToFrame({0, 0, 60, 0}, ntsc, false) &&
              !frameToTimecode(0, {2997, 100}, true) &&
              !frameToTimecode(0, {30}, true) && !frameToTimecode(-1, ntsc, true) &&
              !frameToTimecode(2589408, ntsc, true),
          "skipped labels, invalid rate or fields, and positions beyond one day are rejected");
    for (const auto &format : {ntsc, doubleNtsc}) {
        bool valid = true;
        const auto end = format.numerator == ntsc.numerator ? 2589408 : 5178816;
        for (std::int64_t i = 0; i < end && valid; ++i) {
            const auto label = frameToTimecode(i, format, true);
            valid = label && timecodeToFrame(*label, format, true) == i;
        }
        check(valid, "every frame in the drop-frame day maps to a unique valid label");
    }
}
} // namespace

int main() {
    scale();
    linearTime();
    smpte();
    std::cout << checks << " checks, " << failures << " failures\n";
    return failures != 0;
}
