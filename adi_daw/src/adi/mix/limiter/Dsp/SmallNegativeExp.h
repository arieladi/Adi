// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include <cmath>
#include "ExpandedExp.h"
#if defined(__SSE2__) || defined(_M_X64)
#include <xmmintrin.h>
#endif

// Requires round-to-nearest; caller must select the library path for other modes.
// Private mathematical experiment. No native implementation or table copied.
// A low-order expm1 estimate is sufficient only when the rounded float is far
// enough from a rounding boundary. Ambiguous and out-of-domain cases retain
// the existing library call. Full float-domain qualification is required.
inline float smallNegativeExpNearest(float value) noexcept
{
    if (value >= -0.125f && value < -0.001f) return expandedNegativeExpNearest(value);
    if (!(value >= -0.001f && value < -0x1p-25f)) return std::exp(value);
    const bool quadratic = value >= -0.0001f;
    const float minusOne = quadratic ? value + (0.5f * value) * value
        : value * (1.0f + value * (0.5f + value * (1.0f / 6.0f)));
    const double estimate = 1.0 + static_cast<double>(minusOne);
    const float rounded = static_cast<float>(estimate);
    const double distance = std::abs(static_cast<double>(rounded) - estimate);
    const double bound = quadratic ? 4e-11 : 4e-10;
    // Keep the library call near representable values as well as rounding boundaries.
    if (distance < bound || distance > 0x1p-25 - bound) return std::exp(value);
    return rounded;
}

inline float smallNegativeExpChecked(float value) noexcept
{
#if defined(__SSE2__) || defined(_M_X64)
    if ((_mm_getcsr() & _MM_ROUND_MASK) == _MM_ROUND_NEAREST)
        return smallNegativeExpNearest(value);
#endif
    return std::exp(value);
}
