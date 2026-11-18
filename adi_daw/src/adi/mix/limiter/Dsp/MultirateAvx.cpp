// SPDX-License-Identifier: AGPL-3.0-or-later
#include "Dsp/MultirateKernels.h"
#include <immintrin.h>
#include <cstdint>
#include <limits>
#include <algorithm>

namespace adi::mix::limiter::multirate_detail
{
namespace
{
bool disjointSpans(std::uintptr_t first, std::size_t firstBytes,
                   std::uintptr_t second, std::size_t secondBytes) noexcept
{
    constexpr auto maximum = std::numeric_limits<std::uintptr_t>::max();
    if (firstBytes > maximum - first || secondBytes > maximum - second)
        return false;
    return first + firstBytes <= second || second + secondBytes <= first;
}

bool independentGroups(const float* samples, float* destination, int count,
                       const DoubleCoefficient* coefficients, std::size_t history,
                       bool interpolation) noexcept
{
    if (count < 8 || count > 65536 * 32 || history < 2 || history > 65536)
        return false;
    const auto countValue = static_cast<std::size_t>(count);
    const auto prefixBytes = (history - 1) * sizeof(float);
    const auto sourceBytes = (history - 1 + countValue * (interpolation ? 1 : 2)) * sizeof(float);
    const auto destinationBytes = countValue * (interpolation ? 2 : 1) * sizeof(float);
    const auto coefficientBytes = (history / 2 + (interpolation ? 0 : 1)) * sizeof(DoubleCoefficient);
    const auto sourceAddress = reinterpret_cast<std::uintptr_t>(samples);
    const auto destinationAddress = reinterpret_cast<std::uintptr_t>(destination);
    if (sourceAddress < prefixBytes)
        return false;
    return disjointSpans(sourceAddress - prefixBytes, sourceBytes, destinationAddress, destinationBytes)
        && disjointSpans(reinterpret_cast<std::uintptr_t>(coefficients), coefficientBytes,
                         destinationAddress, destinationBytes);
}
// Private experiment: reuse exact float-to-double conversions across FIR taps.
// The arithmetic and tap-accumulation order for every output stay unchanged.
// Bounded stack scratch only; existing AVX8 remains the fallback.
int upConverted(const float* samples, float* destination, int count,
                const DoubleCoefficient* coefficients, std::size_t history) noexcept
{
    constexpr int batch = 128;
    alignas(32) double converted[256 + batch];
    int completed = 0;
    while (completed + 7 < count)
    {
        const auto run = std::min(batch, (count - completed) & ~7);
        const auto prefix = static_cast<int>(history) - 1;
        const auto total = prefix + run;
        const auto* original = samples + completed - prefix;
        int index = 0;
        for (; index + 3 < total; index += 4)
            _mm256_storeu_pd(converted + index, _mm256_cvtps_pd(_mm_loadu_ps(original + index)));
        for (; index < total; ++index) converted[index] = static_cast<double>(original[index]);
        const auto* current = converted + prefix;
        for (int frame = 0; frame < run; frame += 8)
        {
            auto firstSum = _mm256_setzero_pd();
            auto secondSum = _mm256_setzero_pd();
            for (std::size_t tap = 0; tap < history / 2; ++tap)
            {
                const auto first = _mm256_loadu_pd(current + frame - tap);
                const auto second = _mm256_loadu_pd(current + frame - (history - 1 - tap));
                const auto nextFirst = _mm256_loadu_pd(current + frame + 4 - tap);
                const auto nextSecond = _mm256_loadu_pd(current + frame + 4 - (history - 1 - tap));
                const auto coefficient = _mm256_broadcast_sd(coefficients[tap].lanes);
                firstSum = _mm256_add_pd(firstSum, _mm256_mul_pd(coefficient, _mm256_add_pd(first, second)));
                secondSum = _mm256_add_pd(secondSum, _mm256_mul_pd(coefficient, _mm256_add_pd(nextFirst, nextSecond)));
            }
            const auto outputFrame = completed + frame;
            // The even phase remains a raw float copy (including NaN payloads).
            const auto firstEven = _mm_loadu_ps(samples + outputFrame - history / 2);
            const auto secondEven = _mm_loadu_ps(samples + outputFrame + 4 - history / 2);
            const auto firstOdd = _mm256_cvtpd_ps(firstSum);
            const auto secondOdd = _mm256_cvtpd_ps(secondSum);
            _mm_storeu_ps(destination + 2 * outputFrame, _mm_unpacklo_ps(firstEven, firstOdd));
            _mm_storeu_ps(destination + 2 * outputFrame + 4, _mm_unpackhi_ps(firstEven, firstOdd));
            _mm_storeu_ps(destination + 2 * outputFrame + 8, _mm_unpacklo_ps(secondEven, secondOdd));
            _mm_storeu_ps(destination + 2 * outputFrame + 12, _mm_unpackhi_ps(secondEven, secondOdd));
        }
        completed += run;
    }
    return completed;
}

int downConverted(const float* samples, float* destination, int count,
                  const DoubleCoefficient* coefficients, std::size_t history) noexcept
{
    constexpr int batch = 128;
    alignas(32) double odd[128 + batch];
    const auto prefix = static_cast<int>(history / 2);
    const auto alternate = [](const float* values)
    {
        return _mm_shuffle_ps(_mm_loadu_ps(values), _mm_loadu_ps(values + 4), _MM_SHUFFLE(2, 0, 2, 0));
    };
    int completed = 0;
    while (completed + 7 < count)
    {
        const auto run = std::min(batch, (count - completed) & ~7);
        const auto* original = samples + 2 * completed - 2 * prefix + 1;
        const auto total = prefix + run;
        int index = 0;
        // alternate loads eight floats; its unused last lane must also fit
        // within the caller's 2*count source extent.
        for (; index + 4 < total; index += 4)
            _mm256_storeu_pd(odd + index, _mm256_cvtps_pd(alternate(original + 2 * index)));
        for (; index < total; ++index) odd[index] = static_cast<double>(original[2 * index]);
        const auto* current = odd + prefix;
        for (int frame = 0; frame < run; frame += 8)
        {
            const auto middle = history / 2;
            const auto firstCentral = (middle & 1u) != 0
                ? _mm256_loadu_pd(current + frame - (middle + 1) / 2)
                : _mm256_cvtps_pd(alternate(samples + 2 * (completed + frame) - middle));
            const auto secondCentral = (middle & 1u) != 0
                ? _mm256_loadu_pd(current + frame + 4 - (middle + 1) / 2)
                : _mm256_cvtps_pd(alternate(samples + 2 * (completed + frame) + 8 - middle));
            const auto middleCoefficient = _mm256_broadcast_sd(coefficients[middle].lanes);
            auto firstSum = _mm256_mul_pd(middleCoefficient, firstCentral);
            auto secondSum = _mm256_mul_pd(middleCoefficient, secondCentral);
            for (std::size_t tap = 1; tap < middle; tap += 2)
            {
                const auto first = _mm256_loadu_pd(current + frame - (tap + 1) / 2);
                const auto second = _mm256_loadu_pd(current + frame - (history - tap) / 2);
                const auto nextFirst = _mm256_loadu_pd(current + frame + 4 - (tap + 1) / 2);
                const auto nextSecond = _mm256_loadu_pd(current + frame + 4 - (history - tap) / 2);
                const auto coefficient = _mm256_broadcast_sd(coefficients[tap].lanes);
                firstSum = _mm256_add_pd(firstSum, _mm256_mul_pd(coefficient, _mm256_add_pd(first, second)));
                secondSum = _mm256_add_pd(secondSum, _mm256_mul_pd(coefficient, _mm256_add_pd(nextFirst, nextSecond)));
            }
            _mm_storeu_ps(destination + completed + frame, _mm256_cvtpd_ps(firstSum));
            _mm_storeu_ps(destination + completed + frame + 4, _mm256_cvtpd_ps(secondSum));
        }
        completed += run;
    }
    return completed;
}

}

int upAvx(const float* samples, float* destination, int count,
          const DoubleCoefficient* coefficients, std::size_t history) noexcept
{
    int frame = 0;
    if (count >= 32 && history >= 16 && history <= 256
        && independentGroups(samples, destination, count, coefficients, history, true))
        frame = upConverted(samples, destination, count, coefficients, history);
    if (independentGroups(samples, destination, count, coefficients, history, true))
        for (; frame <= count - 8; frame += 8)
        {
            auto firstSum = _mm256_setzero_pd();
            auto secondSum = _mm256_setzero_pd();
            for (std::size_t index = 0; index < history / 2; ++index)
            {
                const auto first = _mm256_cvtps_pd(_mm_loadu_ps(samples + frame - index));
                const auto second = _mm256_cvtps_pd(_mm_loadu_ps(samples + frame - (history - 1 - index)));
                const auto nextFirst = _mm256_cvtps_pd(_mm_loadu_ps(samples + frame + 4 - index));
                const auto nextSecond = _mm256_cvtps_pd(_mm_loadu_ps(samples + frame + 4 - (history - 1 - index)));
                const auto coefficient = _mm256_broadcast_sd(coefficients[index].lanes);
                firstSum = _mm256_add_pd(firstSum, _mm256_mul_pd(coefficient, _mm256_add_pd(first, second)));
                secondSum = _mm256_add_pd(secondSum, _mm256_mul_pd(coefficient, _mm256_add_pd(nextFirst, nextSecond)));
            }
            const auto firstEven = _mm_loadu_ps(samples + frame - history / 2);
            const auto secondEven = _mm_loadu_ps(samples + frame + 4 - history / 2);
            const auto firstOdd = _mm256_cvtpd_ps(firstSum);
            const auto secondOdd = _mm256_cvtpd_ps(secondSum);
            _mm_storeu_ps(destination + 2 * frame, _mm_unpacklo_ps(firstEven, firstOdd));
            _mm_storeu_ps(destination + 2 * frame + 4, _mm_unpackhi_ps(firstEven, firstOdd));
            _mm_storeu_ps(destination + 2 * frame + 8, _mm_unpacklo_ps(secondEven, secondOdd));
            _mm_storeu_ps(destination + 2 * frame + 12, _mm_unpackhi_ps(secondEven, secondOdd));
        }
    for (; frame + 3 < count; frame += 4)
    {
        auto sum = _mm256_setzero_pd();
        for (std::size_t index = 0; index < history / 2; ++index)
        {
            const auto first = _mm256_cvtps_pd(_mm_loadu_ps(samples + frame - index));
            const auto second = _mm256_cvtps_pd(_mm_loadu_ps(samples + frame - (history - 1 - index)));
            const auto coefficient = _mm256_broadcast_sd(coefficients[index].lanes);
            sum = _mm256_add_pd(sum, _mm256_mul_pd(coefficient, _mm256_add_pd(first, second)));
        }
        const auto even = _mm_loadu_ps(samples + frame - history / 2);
        const auto odd = _mm256_cvtpd_ps(sum);
        _mm_storeu_ps(destination + 2 * frame, _mm_unpacklo_ps(even, odd));
        _mm_storeu_ps(destination + 2 * frame + 4, _mm_unpackhi_ps(even, odd));
    }
    _mm256_zeroupper();
    return frame;
}

int downAvx(const float* samples, float* destination, int count,
            const DoubleCoefficient* coefficients, std::size_t history) noexcept
{
    const auto alternate = [](const float* values)
    {
        return _mm_shuffle_ps(_mm_loadu_ps(values), _mm_loadu_ps(values + 4), _MM_SHUFFLE(2, 0, 2, 0));
    };
    const auto middle = history / 2;
    int frame = 0;
    if (count >= 32 && history >= 17 && history <= 255 && (history & 1u) != 0
        && independentGroups(samples, destination, count, coefficients, history, false))
        frame = downConverted(samples, destination, count, coefficients, history);
    if (independentGroups(samples, destination, count, coefficients, history, false))
        for (; frame <= count - 8; frame += 8)
        {
            const auto firstCentral = _mm256_cvtps_pd(alternate(samples + 2 * frame - middle));
            const auto secondCentral = _mm256_cvtps_pd(alternate(samples + 2 * frame + 8 - middle));
            const auto middleCoefficient = _mm256_broadcast_sd(coefficients[middle].lanes);
            auto firstSum = _mm256_mul_pd(middleCoefficient, firstCentral);
            auto secondSum = _mm256_mul_pd(middleCoefficient, secondCentral);
            for (std::size_t index = 1; index < middle; index += 2)
            {
                const auto first = _mm256_cvtps_pd(alternate(samples + 2 * frame - index));
                const auto second = _mm256_cvtps_pd(alternate(samples + 2 * frame - (history - 1 - index)));
                const auto nextFirst = _mm256_cvtps_pd(alternate(samples + 2 * frame + 8 - index));
                const auto nextSecond = _mm256_cvtps_pd(alternate(samples + 2 * frame + 8 - (history - 1 - index)));
                const auto coefficient = _mm256_broadcast_sd(coefficients[index].lanes);
                firstSum = _mm256_add_pd(firstSum, _mm256_mul_pd(coefficient, _mm256_add_pd(first, second)));
                secondSum = _mm256_add_pd(secondSum, _mm256_mul_pd(coefficient, _mm256_add_pd(nextFirst, nextSecond)));
            }
            _mm_storeu_ps(destination + frame, _mm256_cvtpd_ps(firstSum));
            _mm_storeu_ps(destination + frame + 4, _mm256_cvtpd_ps(secondSum));
        }
    for (; frame + 3 < count; frame += 4)
    {
        const auto central = _mm256_cvtps_pd(alternate(samples + 2 * frame - middle));
        auto sum = _mm256_mul_pd(_mm256_broadcast_sd(coefficients[middle].lanes), central);
        for (std::size_t index = 1; index < middle; index += 2)
        {
            const auto first = _mm256_cvtps_pd(alternate(samples + 2 * frame - index));
            const auto second = _mm256_cvtps_pd(alternate(samples + 2 * frame - (history - 1 - index)));
            const auto coefficient = _mm256_broadcast_sd(coefficients[index].lanes);
            sum = _mm256_add_pd(sum, _mm256_mul_pd(coefficient, _mm256_add_pd(first, second)));
        }
        _mm_storeu_ps(destination + frame, _mm256_cvtpd_ps(sum));
    }
    _mm256_zeroupper();
    return frame;
}
}
