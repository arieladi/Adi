// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#if !defined(ADI_DSP_FORCE_SCALAR) && (defined(_M_X64) || defined(__SSE2__))
#include <emmintrin.h>
namespace adi::mix::limiter
{
// Two independent scalar amounts, preserving std::max's first-operand behavior
// for ties/unordered values and the original double division/subtraction.
inline __m128 smartAmountsStereo(__m128 input, double threshold) noexcept
{
    const auto values = _mm_cvtps_pd(input);
    const auto boundary = _mm_set1_pd(threshold);
    // MAXPD returns its second operand for ties/unordered comparisons.
    // Reversed operands exactly preserve std::max(threshold, input).
    const auto denominator = _mm_max_pd(values, boundary);
    const auto amount = _mm_sub_pd(_mm_set1_pd(1.0), _mm_div_pd(boundary, denominator));
    return _mm_cvtpd_ps(_mm_max_pd(amount, _mm_setzero_pd()));
}
}
#endif
