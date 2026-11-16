// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include "FiniteSample.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <stdexcept>
#include <vector>
#include <cstdint>
#include <limits>
#if defined(__SSE2__) || defined(_M_X64)
#include <emmintrin.h>
#endif

namespace adi::mix::limiter
{
class VariableAverage
{
public:
    void prepare(double sampleRate, std::size_t maximumWindow)
    {
        if (!std::isfinite(sampleRate) || sampleRate <= 0.0 || maximumWindow == 0 || maximumWindow > 1048576)
            throw std::invalid_argument("Invalid average configuration");
        values.assign(maximumWindow, 0.0f);
        window = 1;
        reset();
    }

    void reset() noexcept
    {
        std::fill(values.begin(), values.end(), 0.0f);
        cursor = 0;
        total = 0.0;
    }

    bool setWindow(std::size_t samples) noexcept
    {
        if (samples == 0 || samples > values.size())
            return false;
        if (samples > window)
            for (auto age = window; age < samples; ++age)
                total += read(age);
        else
            for (auto age = samples; age < window; ++age)
                total -= read(age);
        window = samples;
        return true;
    }

    float process(float input) noexcept
    {
        input = finiteSample(input);
        total += static_cast<double>(input);
        total -= static_cast<double>(read(window - 1));
        values[cursor] = input;
        cursor = (cursor + 1) % values.size();
        return static_cast<float>(total / static_cast<double>(window));
    }

    template<typename Input, typename Output>
    void processBlock(std::size_t samples, Input&& input, Output&& output) noexcept
    {
        if (samples == 0)
            return;
        const auto capacity = values.size();
        auto writePosition = cursor;
        auto readPosition = writePosition >= window ? writePosition - window : writePosition + capacity - window;
        auto accumulated = total;
        const auto divisor = static_cast<double>(window);
        std::size_t first = 0;
        while (first < samples)
        {
            const auto span = std::min({samples - first, capacity - writePosition, capacity - readPosition});
            for (std::size_t offset = 0; offset < span; ++offset)
            {
                const auto value = finiteSample(input(first + offset));
                accumulated += static_cast<double>(value);
                accumulated -= static_cast<double>(values[readPosition + offset]);
                values[writePosition + offset] = value;
                output(first + offset, static_cast<float>(accumulated / divisor));
            }
            first += span;
            writePosition += span;
            readPosition += span;
            if (writePosition == capacity)
                writePosition = 0;
            if (readPosition == capacity)
                readPosition = 0;
        }
        cursor = writePosition;
        total = accumulated;
    }

#if defined(__SSE2__) || defined(_M_X64)
    static __m128d divideForFloatStereo(__m128d numerator, __m128d divisor,
                                       __m128d reciprocal, bool nearest) noexcept
    {
        if (nearest)
        {
            const auto candidate = _mm_mul_pd(numerator, reciprocal);
            const auto bits = _mm_castpd_si128(candidate);
            const auto exponent = _mm_and_si128(_mm_srli_epi64(bits, 52), _mm_set1_epi32(2047));
            auto safe = _mm_and_si128(_mm_cmpgt_epi32(exponent, _mm_set1_epi32(897)),
                                     _mm_cmpgt_epi32(_mm_set1_epi32(1150), exponent));
            safe = _mm_or_si128(safe, _mm_castpd_si128(_mm_cmpeq_pd(numerator, _mm_setzero_pd())));
            const auto tail = _mm_and_si128(bits, _mm_set1_epi32((1 << 29) - 1));
            const auto nearMidpoint = _mm_and_si128(_mm_cmpgt_epi32(tail, _mm_set1_epi32((1 << 28) - 5)),
                                                  _mm_cmpgt_epi32(_mm_set1_epi32((1 << 28) + 5), tail));
            if ((_mm_movemask_ps(_mm_castsi128_ps(safe)) & 5) == 5
                && (_mm_movemask_ps(_mm_castsi128_ps(nearMidpoint)) & 5) == 0)
                return candidate;
        }
        return _mm_div_pd(numerator, divisor);
    }
#endif

    // Independent histories and sums. Pair only synchronized ring geometry.
    bool processStereoBlock(VariableAverage& right, std::size_t samples,
                            const float* input, float* output) noexcept
    {
#if !defined(ADI_DSP_FORCE_SCALAR) && !defined(ADI_DISABLE_STEREO_AVERAGE) && (defined(__SSE2__) || defined(_M_X64))
        if (this == &right || values.empty() || values.size() != right.values.size()
            || window != right.window || cursor != right.cursor || input == nullptr || output == nullptr)
            return false;
        if (samples == 0) return true;
        if (samples > std::numeric_limits<std::size_t>::max() / (2 * sizeof(float))) return false;
        const auto inAddress = reinterpret_cast<std::uintptr_t>(input);
        const auto outAddress = reinterpret_cast<std::uintptr_t>(output);
        const auto distance = inAddress > outAddress ? inAddress - outAddress : outAddress - inAddress;
        if (input != output && distance < samples * 2 * sizeof(float)) return false;
        const auto capacity = values.size();
        auto writePosition = cursor;
        auto readPosition = writePosition >= window ? writePosition - window : writePosition + capacity - window;
        auto accumulated = _mm_set_pd(right.total, total);
        const auto divisor = _mm_set1_pd(static_cast<double>(window));
        const auto nearest = (_mm_getcsr() & 0x6000) == 0;
        const auto reciprocal = _mm_set1_pd(nearest ? 1.0 / static_cast<double>(window) : 0.0);
        std::size_t first = 0;
        while (first < samples)
        {
            const auto span = std::min({samples - first, capacity - writePosition, capacity - readPosition});
            for (std::size_t offset = 0; offset < span; ++offset)
            {
                const auto index = 2 * (first + offset);
                auto bits = _mm_loadl_epi64(reinterpret_cast<const __m128i*>(input + index));
                const auto nonfinite = _mm_cmpgt_epi32(_mm_and_si128(bits, _mm_set1_epi32(0x7fffffff)), _mm_set1_epi32(0x7f7fffff));
                bits = _mm_andnot_si128(nonfinite, bits);
                const auto value = _mm_castsi128_ps(bits);
                accumulated = _mm_add_pd(accumulated, _mm_cvtps_pd(value));
                const auto previous = _mm_set_pd(static_cast<double>(right.values[readPosition + offset]),
                                                static_cast<double>(values[readPosition + offset]));
                accumulated = _mm_sub_pd(accumulated, previous);
                _mm_store_ss(values.data() + writePosition + offset, value);
                _mm_store_ss(right.values.data() + writePosition + offset, _mm_shuffle_ps(value, value, _MM_SHUFFLE(1,1,1,1)));
                const auto result = divideForFloatStereo(accumulated, divisor, reciprocal, nearest);
                _mm_storel_epi64(reinterpret_cast<__m128i*>(output + index), _mm_castps_si128(_mm_cvtpd_ps(result)));
            }
            first += span;
            writePosition += span;
            readPosition += span;
            if (writePosition == capacity) writePosition = 0;
            if (readPosition == capacity) readPosition = 0;
        }
        cursor = right.cursor = writePosition;
        total = _mm_cvtsd_f64(accumulated);
        right.total = _mm_cvtsd_f64(_mm_unpackhi_pd(accumulated, accumulated));
        return true;
#else
        (void)right; (void)samples; (void)input; (void)output;
        return false;
#endif
    }

    std::size_t windowSize() const noexcept { return window; }

private:
    float read(std::size_t age) const noexcept { return values[(cursor + values.size() - 1 - age) % values.size()]; }
    std::vector<float> values;
    std::size_t cursor = 0, window = 1;
    double total = 0.0;
};
}
