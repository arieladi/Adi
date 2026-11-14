// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include <bit>
#include <cstdint>
#include <limits>

// Original inverse-rounding-cell optimization; caller preserves the existing exp result.
// Caller must use this only in round-to-nearest and invalidate whenever its
// cached exponential result changes. The underlying expression must be the
// float division -2.3025851f / count followed by the existing float exp.
namespace adi::mix::limiter
{
struct CoefficientCountInterval
{
    float minimum = std::numeric_limits<float>::infinity();
    float maximum = -std::numeric_limits<float>::infinity();

    bool contains(float count) const noexcept { return count >= minimum && count <= maximum; }
    bool valid() const noexcept { return minimum <= maximum; }
    void reset() noexcept { *this = {}; }

    static double logNearOne(double minusOne) noexcept
    {
        return minusOne * (1.0 + minusOne * (-0.5 + minusOne * (1.0 / 3.0 - minusOne * 0.25)));
    }

    void learn(float exponential) noexcept
    {
        reset();
        if (!(exponential >= 0.999f && exponential < 1.0f)) return;
        // All admitted results lie in the [0.5,1) float binade. Half an ULP
        // is exactly2^-25. The log1p degree4 remainder is <3e-16 here;
        // margin2^-40 also exceeds the intermediate double rounding error.
        constexpr double halfUlp = 0x1p-25;
        constexpr double margin = 0x1p-40;
        constexpr double divisionError = 0x1p-24;
        constexpr double constant = static_cast<double>(2.3025851f);
        const double minusOne = static_cast<double>(exponential) - 1.0;
        const double positiveLow = -logNearOne(minusOne + halfUlp) + margin;
        const double positiveHigh = -logNearOne(minusOne - halfUlp) - margin;
        const float lower = static_cast<float>(constant * (1.0 + divisionError) / positiveHigh);
        const float upper = static_cast<float>(constant * (1.0 - divisionError) / positiveLow);
        // One full float ULP inward absorbs rounding of the bounds themselves.
        minimum = std::bit_cast<float>(std::bit_cast<std::uint32_t>(lower) + 1u);
        maximum = std::bit_cast<float>(std::bit_cast<std::uint32_t>(upper) - 1u);
    }
};

}
