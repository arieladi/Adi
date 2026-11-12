// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include "SmallNegativeExp.h"
#include "CoefficientCountInterval.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <limits>
#if defined(_M_X64) || defined(__SSE2__)
#include <emmintrin.h>
#endif

namespace adi::mix::limiter
{
class ClearUnpairedEnvelope
{
public:
    static void prepareTable() noexcept { static_cast<void>(smoothingTable()); }

    float process(double& value, float sampleRate, float input, float attack,
                  float release, float releaseShape, float releaseSmoothing,
                  bool smoothingActive, std::size_t localSample, bool nearestExp = false) noexcept
    {
        const auto difference = static_cast<double>(input) - value;
        const auto falling = difference <= -static_cast<double>(std::numeric_limits<float>::epsilon());
        auto duration = falling ? release : attack;
        if (falling)
        {
            const auto scaled = static_cast<float>(static_cast<double>(duration) * difference * -0.1);
            duration = (duration - scaled) * releaseShape + scaled;
            if (smoothingActive && difference > -10.0)
            {
                const auto changeLimit = difference * difference * difference * difference
                    * static_cast<double>(1e-5f) + static_cast<double>(4e-5f);
                if ((localSample & 15u) == 0
                    || std::abs(static_cast<double>(cachedDifference) - difference) > changeLimit)
                {
                    cachedDifference = static_cast<float>(difference);
                    cachedSmoothing = static_cast<float>(interpolateSmoothing(difference));
                }
                duration = duration + cachedSmoothing * releaseSmoothing;
            }
        }
        const auto count = duration * sampleRate;
        const auto direction = static_cast<std::size_t>(falling);
        const auto alpha = coefficient<false>(count, direction, nearestExp);
        value = value + difference * alpha;
        return static_cast<float>(value);
    }

#if !defined(ADI_DSP_FORCE_SCALAR) && (defined(_M_X64) || defined(__SSE2__))
    // Internal block path: all five source arrays are disjoint from output,
    // interleaved stereo, and use the same rate/local-sample clock.
    template<bool ShareCoefficient, bool ConstantInput>
    __declspec(noinline) static void processStereo(ClearUnpairedEnvelope& left, ClearUnpairedEnvelope& right,
        double& leftValue, double& rightValue, float sampleRate, std::size_t frames,
        const float* input, float inputScalar, const float* attack, const float* release,
        const float* releaseShape, const float* releaseSmoothing, float* output, bool nearestExp) noexcept
    {
        auto state = _mm_set_pd(rightValue, leftValue);
        const auto rate = _mm_set1_ps(sampleRate);
        const auto epsilon = _mm_set1_pd(-static_cast<double>(std::numeric_limits<float>::epsilon()));
        const auto absolute = _mm_castsi128_pd(_mm_set1_epi64x(0x7fffffffffffffffLL));
        for (std::size_t sample = 0; sample < frames; ++sample)
        {
            const auto index = sample * 2;
            const auto inputs = [&]
            {
                if constexpr (ConstantInput) return _mm_set1_ps(inputScalar);
                else return _mm_castsi128_ps(_mm_loadl_epi64(reinterpret_cast<const __m128i*>(input + index)));
            }();
            const auto difference = _mm_sub_pd(_mm_cvtps_pd(inputs), state);
            if (_mm_movemask_pd(_mm_cmple_pd(difference, epsilon)) != 3)
            {
                alignas(16) double values[2];
                alignas(16) float inputValues[4];
                _mm_store_ps(inputValues, inputs);
                _mm_store_pd(values, state);
                output[index] = left.process(values[0], sampleRate, inputValues[0], attack[index], release[index],
                    releaseShape[index], releaseSmoothing[index], true, sample, nearestExp);
                output[index + 1] = right.process(values[1], sampleRate, inputValues[1], attack[index + 1], release[index + 1],
                    releaseShape[index + 1], releaseSmoothing[index + 1], true, sample, nearestExp);
                state = _mm_load_pd(values);
                continue;
            }
            const auto releases = _mm_castsi128_ps(_mm_loadl_epi64(reinterpret_cast<const __m128i*>(release + index)));
            const auto shapes = _mm_castsi128_ps(_mm_loadl_epi64(reinterpret_cast<const __m128i*>(releaseShape + index)));
            const auto scaled = _mm_cvtpd_ps(_mm_mul_pd(_mm_mul_pd(_mm_cvtps_pd(releases), difference), _mm_set1_pd(-0.1)));
            auto duration = _mm_add_ps(_mm_mul_ps(_mm_sub_ps(releases, scaled), shapes), scaled);
            const auto smoothingEligible = _mm_cmpgt_pd(difference, _mm_set1_pd(-10.0));
            auto update = smoothingEligible;
            if ((sample & 15u) != 0)
            {
                // Preserve the original left-associated fourth power.
                const auto fourth = _mm_mul_pd(_mm_mul_pd(_mm_mul_pd(difference, difference), difference), difference);
                const auto limit = _mm_add_pd(_mm_mul_pd(fourth, _mm_set1_pd(static_cast<double>(1e-5f))),
                    _mm_set1_pd(static_cast<double>(4e-5f)));
                const auto cached = _mm_set_pd(static_cast<double>(right.cachedDifference), static_cast<double>(left.cachedDifference));
                const auto change = _mm_and_pd(_mm_sub_pd(cached, difference), absolute);
                update = _mm_and_pd(smoothingEligible, _mm_cmpgt_pd(change, limit));
            }
            const auto updateMask = _mm_movemask_pd(update);
            if (updateMask != 0)
            {
                alignas(16) double differences[2];
                _mm_store_pd(differences, difference);
                if ((updateMask & 1) != 0)
                {
                    left.cachedDifference = static_cast<float>(differences[0]);
                    left.cachedSmoothing = static_cast<float>(interpolateSmoothing(differences[0]));
                }
                if ((updateMask & 2) != 0)
                {
                    right.cachedDifference = static_cast<float>(differences[1]);
                    right.cachedSmoothing = static_cast<float>(interpolateSmoothing(differences[1]));
                }
            }
            const auto smooth = _mm_setr_ps(left.cachedSmoothing, right.cachedSmoothing, 0.0f, 0.0f);
            const auto weights = _mm_castsi128_ps(_mm_loadl_epi64(reinterpret_cast<const __m128i*>(releaseSmoothing + index)));
            const auto smoothed = _mm_add_ps(duration, _mm_mul_ps(smooth, weights));
            const auto mask = _mm_castsi128_ps(_mm_shuffle_epi32(_mm_castpd_si128(smoothingEligible), _MM_SHUFFLE(2, 0, 2, 0)));
            duration = _mm_or_ps(_mm_and_ps(mask, smoothed), _mm_andnot_ps(mask, duration));
            alignas(16) float counts[4];
            _mm_store_ps(counts, _mm_mul_ps(duration, rate));
            bool sameFreshCount = false;
            if constexpr (ShareCoefficient)
                sameFreshCount = left.durationCounts[1] != counts[0] && right.durationCounts[1] != counts[1]
                    && counts[0] == counts[1];
            const auto leftAlpha = left.coefficient<false>(counts[0], 1, nearestExp);
            double rightAlpha;
            if constexpr (ShareCoefficient)
                rightAlpha = sameFreshCount ? right.coefficient<true>(counts[1], 1, nearestExp, leftAlpha)
                                           : right.coefficient<false>(counts[1], 1, nearestExp);
            else
                rightAlpha = right.coefficient<false>(counts[1], 1, nearestExp);
            state = _mm_add_pd(state, _mm_mul_pd(difference, _mm_set_pd(rightAlpha, leftAlpha)));
            _mm_storel_epi64(reinterpret_cast<__m128i*>(output + index), _mm_castps_si128(_mm_cvtpd_ps(state)));
        }
        alignas(16) double values[2];
        _mm_store_pd(values, state);
        leftValue = values[0];
        rightValue = values[1];
    }
#endif

private:
    friend struct ClearEnvelopeTestAccess;

    template<bool UseHint>
    double coefficient(float count, std::size_t direction, bool nearestExp, double hint = 0.0) noexcept
    {
        if (count != durationCounts[direction])
        {
            durationCounts[direction] = count;
            auto& interval = countIntervals[direction];
            if (count <= std::numeric_limits<float>::epsilon())
            {
                alphas[direction] = 1.0;
                interval.reset();
            }
            else if (!(nearestExp && interval.contains(count)))
            {
                const float exponential = [&]
                {
                    if constexpr (UseHint) return static_cast<float>(1.0 - hint);
                    else return nearestExp ? smallNegativeExpNearest(-2.3025851f / count) : std::exp(-2.3025851f / count);
                }();
                const double next = UseHint ? hint : 1.0 - static_cast<double>(exponential);
                if (!nearestExp || next != alphas[direction]) interval.reset();
                else if (!interval.valid()) interval.learn(exponential);
                alphas[direction] = next;
            }
        }
        return alphas[direction];
    }



    static const std::array<double, 8193>& smoothingTable() noexcept
    {
        static const auto result = []
        {
            std::array<double, 8193> values;
            constexpr auto spacing = 25.0 / 8191.0;
            auto argument = -25.0;
            values[0] = std::exp(argument);
            for (std::size_t index = 1; index < 8191; ++index)
            {
                argument = argument + spacing;
                values[index] = std::exp(argument);
            }
            values[8191] = 1.0;
            values[8192] = 1.0;
            return values;
        }();
        return result;
    }

    static double interpolateSmoothing(double difference) noexcept
    {
        constexpr auto scale = 1.0 / (25.0 / 8191.0);
        constexpr auto offset = (8191.0 * -25.0) * 0.04;
        const auto coordinate = std::max(-25.0, difference) * scale - offset;
        const auto index = static_cast<std::size_t>(static_cast<int>(coordinate) & 8191);
        const auto& values = smoothingTable();
        return (values[index + 1] - values[index]) * (coordinate - static_cast<double>(index)) + values[index];
    }

    std::array<float, 2> durationCounts{-1.0f, -1.0f};
    std::array<double, 2> alphas{};
    std::array<CoefficientCountInterval, 2> countIntervals{};
    float cachedDifference = 0.0f;
    float cachedSmoothing = 1.0f;
};
}
