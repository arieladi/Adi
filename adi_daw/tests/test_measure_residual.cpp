// SPDX-License-Identifier: AGPL-3.0-or-later
#include "adi/measure/residual.hpp"

#include <array>
#include <cmath>
#include <cstdio>
#include <limits>
#include <span>

namespace {
using adi::measure::ResidualAccumulator;
int checks = 0, failures = 0;
void check(bool okay, const char* what) {
    ++checks;
    if (!okay) {
        ++failures;
        std::printf("FAIL %s\n", what);
    }
}

void knownResidual() {
    constexpr std::array<float, 5> reference{1.0f, -1.0f, 0.5f, -0.5f, 0.0f};
    constexpr std::array<float, 5> actual{0.75f, -0.75f, 0.25f, -0.25f, 0.0f};
    ResidualAccumulator whole;
    check(!whole.finish(), "an empty input has no comparison result");
    check(whole.add(reference, actual), "interleaved float samples are admitted");
    const auto result = whole.finish();
    check(result && result->sampleCount == 5 && !result->bitExact && result->peakResidual == 0.25,
          "peak, count and bit-exact flag match the analytic vector");
    check(result && std::abs(result->referenceRms - std::sqrt(0.5)) < 1e-15 &&
              std::abs(result->actualRms - 0.5) < 1e-15 &&
              std::abs(result->residualRms - std::sqrt(0.05)) < 1e-15 && result->errorSignalDb &&
              std::abs(*result->errorSignalDb + 10.0) < 1e-13,
          "RMS and relative residual match independent power arithmetic");
    ResidualAccumulator sliced;
    check(sliced.add(std::span(reference).first(1), std::span(actual).first(1)) &&
              sliced.add(std::span(reference).subspan(1, 2), std::span(actual).subspan(1, 2)) &&
              sliced.add(std::span(reference).last(2), std::span(actual).last(2)) && sliced.finish() == result,
          "changing render or read block sizes does not move the metrics");
}

void formatsAndRefusals() {
    ResidualAccumulator window;
    constexpr std::array<double, 2> a{1e300, 1e-300};
    constexpr std::array<double, 2> b{1e300, 0};
    check(window.add(a, b), "very quiet errors survive next to large finite values");
    const auto previous = window.finish();
    check(previous && previous->referenceRms > 7e299 && previous->residualRms > 7e-301 &&
              previous->errorSignalDb && std::abs(*previous->errorSignalDb + 12'000.0) < 1e-9,
          "the ratio remains finite with a 600 decade range");

    constexpr std::array<float, 2> invalid{1, std::numeric_limits<float>::quiet_NaN()};
    constexpr std::array<float, 2> test{0, 0};
    constexpr std::array<double, 1> tooHigh{std::numeric_limits<double>::max()};
    constexpr std::array<double, 1> tooLow{-std::numeric_limits<double>::max()};
    check(!window.add(invalid, test) && !window.add(std::span(test).first(1), test) &&
              !window.add(tooLow, tooHigh) && window.finish() == previous,
          "bad lengths, nonfinite audio and overflow leave the completed prefix intact");

    ResidualAccumulator zero;
    const std::array<float, 2> signedZero{0, -0.0f};
    check(zero.add(test, signedZero) && zero.finish() && !zero.finish()->bitExact &&
              zero.finish()->residualRms == 0 && !zero.finish()->errorSignalDb,
          "signed zero is numerically silent and distinguishes an exact bit comparison");
    ResidualAccumulator exact;
    check(exact.add(a, a) && exact.finish() && exact.finish()->bitExact && !exact.finish()->errorSignalDb,
          "exact data has a zero residual and no finite relative-decibel value");
}
}

int main() {
    knownResidual();
    formatsAndRefusals();
    std::printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
