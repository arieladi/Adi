// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include "EnvelopePrimitives.h"
#if defined(_M_X64) || defined(__SSE2__)
#include <emmintrin.h>
#endif

namespace adi::mix::limiter
{
inline void approximateDecibelsBlock(const float* input, float* output, std::size_t count) noexcept
{
    std::size_t sample = 0;
#if defined(_M_X64) || defined(__SSE2__)
    const auto sign = _mm_set1_ps(-0.0f);
    for (; sample + 4 <= count; sample += 4)
    {
        const auto magnitude = _mm_max_ps(_mm_andnot_ps(sign, _mm_loadu_ps(input + sample)), _mm_set1_ps(1.0e-10f));
        const auto bits = _mm_castps_si128(magnitude);
        const auto mantissa = _mm_castsi128_ps(_mm_or_si128(_mm_and_si128(bits, _mm_set1_epi32(0x007fffff)), _mm_set1_epi32(0x3f000000)));
        const auto low = _mm_cmplt_ps(mantissa, _mm_set1_ps(0.70710677f));
        const auto reduced = _mm_add_ps(_mm_sub_ps(mantissa, _mm_set1_ps(1.0f)), _mm_and_ps(low, mantissa));
        const auto exponent = _mm_cvtepi32_ps(_mm_sub_epi32(_mm_sub_epi32(_mm_srli_epi32(bits, 23), _mm_set1_epi32(126)),
            _mm_and_si128(_mm_castps_si128(low), _mm_set1_epi32(1))));
        auto polynomial = _mm_set1_ps(0.070376836f);
        for (const auto coefficient : {-0.1151461f, 0.116769984f, -0.12420141f, 0.14249323f,
                                       -0.16668057f, 0.20000714f, -0.24999994f, 0.3333333f})
            polynomial = _mm_add_ps(_mm_mul_ps(polynomial, reduced), _mm_set1_ps(coefficient));
        polynomial = _mm_mul_ps(_mm_mul_ps(polynomial, reduced), _mm_mul_ps(reduced, reduced));
        polynomial = _mm_add_ps(polynomial, _mm_mul_ps(exponent, _mm_set1_ps(-0.00021219444f)));
        polynomial = _mm_sub_ps(polynomial, _mm_mul_ps(_mm_mul_ps(reduced, reduced), _mm_set1_ps(0.5f)));
        polynomial = _mm_add_ps(polynomial, reduced);
        polynomial = _mm_add_ps(polynomial, _mm_mul_ps(exponent, _mm_set1_ps(0.6933594f)));
        _mm_storeu_ps(output + sample, _mm_max_ps(_mm_mul_ps(polynomial, _mm_set1_ps(8.685889f)), _mm_set1_ps(-180.0f)));
    }
#endif
    for (; sample < count; ++sample) output[sample] = approximateDecibels(input[sample]);
}

inline void approximateAmplitudeBlock(const float* input, float* output, std::size_t count) noexcept
{
    std::size_t sample = 0;
#if defined(_M_X64) || defined(__SSE2__)
    for (; sample + 4 <= count; sample += 4)
    {
        const auto logarithm = _mm_min_ps(_mm_max_ps(_mm_mul_ps(_mm_loadu_ps(input + sample), _mm_set1_ps(0.115129255f)),
            _mm_set1_ps(-88.37625885009766f)), _mm_set1_ps(88.37625885009766f));
        const auto mapped = _mm_add_ps(_mm_mul_ps(logarithm, _mm_set1_ps(1.442695f)), _mm_set1_ps(0.5f));
        const auto truncated = _mm_cvttps_epi32(mapped);
        const auto exponentBits = _mm_sub_epi32(truncated, _mm_and_si128(
            _mm_castps_si128(_mm_cmpgt_ps(_mm_cvtepi32_ps(truncated), mapped)), _mm_set1_epi32(1)));
        const auto exponent = _mm_cvtepi32_ps(exponentBits);
        const auto reduced = _mm_sub_ps(_mm_sub_ps(logarithm, _mm_mul_ps(exponent, _mm_set1_ps(0.6933594f))),
            _mm_mul_ps(exponent, _mm_set1_ps(-0.00021219444f)));
        auto polynomial = _mm_set1_ps(0.00019875691f);
        for (const auto coefficient : {0.0013981999f, 0.008333452f, 0.041665796f, 0.16666666f, 0.5f})
            polynomial = _mm_add_ps(_mm_mul_ps(polynomial, reduced), _mm_set1_ps(coefficient));
        polynomial = _mm_mul_ps(polynomial, _mm_mul_ps(reduced, reduced));
        polynomial = _mm_add_ps(_mm_add_ps(polynomial, reduced), _mm_set1_ps(1.0f));
        auto powerBits = _mm_slli_epi32(_mm_add_epi32(exponentBits, _mm_set1_epi32(127)), 23);
        powerBits = _mm_or_si128(powerBits, _mm_and_si128(_mm_cmpeq_epi32(exponentBits, _mm_set1_epi32(-127)), _mm_set1_epi32(0x00400000)));
        _mm_storeu_ps(output + sample, _mm_mul_ps(polynomial, _mm_castsi128_ps(powerBits)));
    }
#endif
    for (; sample < count; ++sample) output[sample] = approximateAmplitude(input[sample]);
}
}
