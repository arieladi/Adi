// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include "FiniteSample.h"
#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>
#if defined(__SSE2__) || defined(_M_X64)
#include <emmintrin.h>
#endif

namespace adi::mix::limiter
{
class PeakWindow
{
public:
    struct Scratch
    {
        static constexpr std::size_t capacity = 512;
        alignas(16) std::array<float, capacity> magnitudes;
        alignas(16) std::array<float, capacity> endpoints;
        alignas(16) std::array<float, capacity> events;
        std::array<std::uint16_t, capacity> arrivals;
        std::array<float, capacity> peakValues;
    };

    void prepare(std::size_t maximumPeriods)
    {
        maximum = maximumPeriods;
        queue.resize(std::bit_ceil(maximumPeriods + 2));
        history.resize(std::bit_ceil(maximumPeriods + 1));
        queueMask = queue.size() - 1;
        historyMask = history.size() - 1;
        reset();
    }

    void reset() noexcept
    {
        std::fill(history.begin(), history.end(), 0.0f);
        first = size = selected = cursor = previousWindow = 0;
        time = 0;
        previous = 0.0f;
        hasWindow = falling = false;
        zeroHistory = true;
    }

    float process(float input, std::size_t periods) noexcept
    {
        const auto magnitude = std::abs(finiteSample(input));
        if (std::bit_cast<std::uint32_t>(magnitude) != 0) zeroHistory = false;
        cursor = (cursor + 1) & historyMask;
        history[cursor] = magnitude;
        const auto window = std::min(periods, maximum);
        const auto endpoint = time >= window ? history[(cursor - window) & historyMask] : 0.0f;
        const auto event = advance(magnitude, window);
        return maximumKeepingLeft(maximumKeepingLeft(magnitude, endpoint), event);
    }

    template<typename Input>
    void processBlock(Input&& input, float* output, std::size_t stride,
                      std::size_t frames, std::size_t periods) noexcept
    {
        for (std::size_t sample = 0; sample < frames; ++sample)
            output[sample * stride] = process(input(sample), periods);
    }

    void processConstantBlock(float input, float* output, std::size_t stride,
                              std::size_t frames, std::size_t periods) noexcept
    {
        processBlock([input](std::size_t) { return input; }, output, stride, frames, periods);
    }

    void processConstantBuffer(float input, float* output, std::size_t stride,
                               std::size_t frames, std::size_t periods, Scratch& scratch) noexcept
    {
        if (frames == 0) return;
        const auto magnitude = std::abs(finiteSample(input));
        if (zeroHistory && std::bit_cast<std::uint32_t>(magnitude) == 0)
        {
            time += frames;
            cursor = (cursor + frames) & historyMask;
            previousWindow = std::min(periods, maximum);
            hasWindow = true;
            for (std::size_t sample = 0; sample < frames; ++sample) output[sample * stride] = 0.0f;
            return;
        }
        processBuffer(&input, 0, output, stride, frames, periods, scratch);
    }

    void processBuffer(const float* input, std::size_t inputStride, float* output,
                       std::size_t outputStride, std::size_t frames, std::size_t periods,
                       Scratch& scratch) noexcept
    {
        if (frames == 0) return;
        const auto inputBegin = reinterpret_cast<std::uintptr_t>(input);
        const auto outputBegin = reinterpret_cast<std::uintptr_t>(output);
        const auto inputEnd = inputBegin + ((frames - 1) * inputStride + 1) * sizeof(float);
        const auto outputEnd = outputBegin + ((frames - 1) * outputStride + 1) * sizeof(float);
        const auto overlap = inputBegin < outputEnd && outputBegin < inputEnd;
        const auto identical = input == output && inputStride == outputStride && inputStride != 0;
        if (overlap && !identical)
        {
            processBlock([input, inputStride](std::size_t sample) { return input[sample * inputStride]; },
                         output, outputStride, frames, periods);
            return;
        }
        while (frames != 0)
        {
            const auto count = std::min(frames, Scratch::capacity);
            processSlice(input, inputStride, output, outputStride, count, std::min(periods, maximum), scratch);
            input += count * inputStride;
            output += count * outputStride;
            frames -= count;
        }
    }

private:
    struct Entry { std::uint64_t time = 0; float value = 0.0f; };

    static float maximumKeepingLeft(float left, float right) noexcept
    {
#if defined(__SSE2__) || defined(_M_X64)
        const auto leftValue = _mm_set_ss(left);
        const auto rightValue = _mm_set_ss(right);
        const auto replace = _mm_cmplt_ss(leftValue, rightValue);
        return _mm_cvtss_f32(_mm_or_ps(_mm_and_ps(replace, rightValue), _mm_andnot_ps(replace, leftValue)));
#else
        return std::max(left, right);
#endif
    }

    float advance(float magnitude, std::size_t window) noexcept
    {
        if (previous <= magnitude)
            falling = false;
        else if (!falling)
        {
            falling = true;
            while (size != 0 && queue[(first + size - 1) & queueMask].value <= previous)
                --size;
            auto& entry = queue[(first + size) & queueMask];
            entry.time = time - 1;
            entry.value = previous;
            ++size;
            selected = std::min(selected, size - 1);
        }
        while (size != 0 && time > maximum && queue[first].time < time - maximum)
        {
            first = (first + 1) & queueMask;
            --size;
            if (selected != 0) --selected;
        }
        const auto earliest = time > window ? time - window : 0;
        if (hasWindow && previousWindow == window)
        {
            while (selected < size && queue[(first + selected) & queueMask].time < earliest)
                ++selected;
        }
        else
        {
            std::size_t lower = 0, upper = size;
            while (lower < upper)
            {
                const auto middle = lower + (upper - lower) / 2;
                if (queue[(first + middle) & queueMask].time < earliest)
                    lower = middle + 1;
                else
                    upper = middle;
            }
            selected = lower;
        }
        previousWindow = window;
        hasWindow = true;
        previous = magnitude;
        ++time;
        return selected < size ? queue[(first + selected) & queueMask].value : 0.0f;
    }

    void copyOldEndpoints(float* destination, std::size_t count, std::size_t window) const noexcept
    {
        const auto begin = (cursor + 1 - window) & historyMask;
        const auto firstCount = std::min(count, history.size() - begin);
        std::copy_n(history.data() + begin, firstCount, destination);
        std::copy_n(history.data(), count - firstCount, destination + firstCount);
    }

    void appendHistory(const float* input, std::size_t count) noexcept
    {
        const auto retained = std::min(count, history.size());
        const auto begin = (cursor + count - retained + 1) & historyMask;
        const auto firstCount = std::min(retained, history.size() - begin);
        const auto* source = input + count - retained;
        std::copy_n(source, firstCount, history.data() + begin);
        std::copy_n(source + firstCount, retained - firstCount, history.data());
        cursor = (cursor + count) & historyMask;
    }

    std::size_t findArrivals(std::size_t count, Scratch& scratch) noexcept
    {
        std::size_t arrivals = 0, sample = 0;
        auto priorMagnitude = previous;
        auto priorDown = falling;
        const auto append = [&](std::size_t position)
        {
            scratch.arrivals[arrivals] = static_cast<std::uint16_t>(position);
            scratch.peakValues[arrivals] = position == 0 ? previous : scratch.magnitudes[position - 1];
            ++arrivals;
        };
#if defined(__SSE2__) || defined(_M_X64)
        for (; sample + 4 <= count; sample += 4)
        {
            const auto current = _mm_load_ps(scratch.magnitudes.data() + sample);
            const auto shifted = _mm_castsi128_ps(_mm_slli_si128(_mm_castps_si128(current), 4));
            const auto predecessors = _mm_move_ss(shifted, _mm_set_ss(priorMagnitude));
            const auto down = static_cast<unsigned>(_mm_movemask_ps(_mm_cmpgt_ps(predecessors, current)));
            auto arrivalsMask = down & ~((down << 1) | static_cast<unsigned>(priorDown));
            while (arrivalsMask != 0)
            {
                const auto lane = static_cast<std::size_t>(std::countr_zero(arrivalsMask));
                append(sample + lane);
                arrivalsMask &= arrivalsMask - 1;
            }
            priorMagnitude = scratch.magnitudes[sample + 3];
            priorDown = (down & 8u) != 0;
        }
#endif
        for (; sample < count; ++sample)
        {
            const auto down = priorMagnitude > scratch.magnitudes[sample];
            if (down && !priorDown) append(sample);
            priorMagnitude = scratch.magnitudes[sample];
            priorDown = down;
        }
        previous = priorMagnitude;
        falling = priorDown;
        return arrivals;
    }

    void expireHistory(std::uint64_t now) noexcept
    {
        while (size != 0 && now > maximum && queue[first].time < now - maximum)
        {
            first = (first + 1) & queueMask;
            --size;
            if (selected != 0) --selected;
        }
    }

    void renderEventSpans(std::size_t count, std::size_t window, Scratch& scratch) noexcept
    {
        const auto arrivals = findArrivals(count, scratch);
        auto needsSearch = !hasWindow || previousWindow != window;
        std::size_t position = 0;
        const auto emitUntil = [&](std::size_t stop)
        {
            while (position < stop)
            {
                const auto now = time + position;
                const auto earliest = now > window ? now - window : 0;
                if (needsSearch)
                {
                    std::size_t lower = 0, upper = size;
                    while (lower < upper)
                    {
                        const auto middle = lower + (upper - lower) / 2;
                        if (queue[(first + middle) & queueMask].time < earliest)
                            lower = middle + 1;
                        else
                            upper = middle;
                    }
                    selected = lower;
                    needsSearch = false;
                }
                else
                    while (selected < size && queue[(first + selected) & queueMask].time < earliest)
                        ++selected;
                auto span = stop - position;
                auto value = 0.0f;
                if (selected < size)
                {
                    const auto& entry = queue[(first + selected) & queueMask];
                    const auto age = static_cast<std::size_t>(now - entry.time);
                    span = std::min(span - 1, window - age) + 1;
                    value = entry.value;
                }
                std::fill_n(scratch.events.data() + position, span, value);
                position += span;
            }
        };
        for (std::size_t arrival = 0; arrival < arrivals; ++arrival)
        {
            const auto arrivalPosition = static_cast<std::size_t>(scratch.arrivals[arrival]);
            emitUntil(arrivalPosition);
            const auto now = time + arrivalPosition;
            expireHistory(now);
            const auto value = scratch.peakValues[arrival];
            while (size != 0 && queue[(first + size - 1) & queueMask].value <= value)
                --size;
            auto& entry = queue[(first + size) & queueMask];
            entry.time = now - 1;
            entry.value = value;
            ++size;
            selected = std::min(selected, size - 1);
            expireHistory(now);
        }
        emitUntil(count);
        expireHistory(time + count - 1);
        previousWindow = window;
        hasWindow = true;
        time += count;
    }

    void processSlice(const float* input, std::size_t inputStride, float* output,
                      std::size_t outputStride, std::size_t count, std::size_t window,
                      Scratch& scratch) noexcept
    {
        for (std::size_t sample = 0; sample < count; ++sample)
            scratch.magnitudes[sample] = std::abs(finiteSample(input[sample * inputStride]));
        if (zeroHistory)
            for (std::size_t sample = 0; sample < count; ++sample)
                if (std::bit_cast<std::uint32_t>(scratch.magnitudes[sample]) != 0)
                {
                    zeroHistory = false;
                    break;
                }
        const auto fromHistory = std::min(count, window);
        copyOldEndpoints(scratch.endpoints.data(), fromHistory, window);
        if (time < window)
            std::fill_n(scratch.endpoints.data(), std::min(fromHistory, window - static_cast<std::size_t>(time)), 0.0f);
        std::copy_n(scratch.magnitudes.data(), count - fromHistory, scratch.endpoints.data() + fromHistory);
        appendHistory(scratch.magnitudes.data(), count);
        renderEventSpans(count, window, scratch);
        std::size_t sample = 0;
#if defined(__SSE2__) || defined(_M_X64)
        const auto flushInputs = (_mm_getcsr() & 0x40u) != 0;
        const auto maximumVector = [flushInputs](__m128 left, __m128 right)
        {
            const auto leftBits = _mm_castps_si128(left);
            const auto rightBits = _mm_castps_si128(right);
            auto leftKey = leftBits;
            auto rightKey = rightBits;
            if (flushInputs)
            {
                const auto largestSubnormal = _mm_set1_epi32(0x007fffff);
                leftKey = _mm_and_si128(leftKey, _mm_cmpgt_epi32(leftKey, largestSubnormal));
                rightKey = _mm_and_si128(rightKey, _mm_cmpgt_epi32(rightKey, largestSubnormal));
            }
            const auto replace = _mm_cmpgt_epi32(rightKey, leftKey);
            return _mm_castsi128_ps(_mm_or_si128(_mm_and_si128(replace, rightBits), _mm_andnot_si128(replace, leftBits)));
        };
        for (; sample + 4 <= count; sample += 4)
        {
            const auto endpoints = maximumVector(_mm_load_ps(scratch.magnitudes.data() + sample),
                                           _mm_load_ps(scratch.endpoints.data() + sample));
            const auto result = maximumVector(endpoints, _mm_load_ps(scratch.events.data() + sample));
            _mm_store_ps(scratch.magnitudes.data() + sample, result);
        }
#endif
        for (; sample < count; ++sample)
            scratch.magnitudes[sample] = maximumKeepingLeft(maximumKeepingLeft(scratch.magnitudes[sample], scratch.endpoints[sample]), scratch.events[sample]);
        if (outputStride == 1)
            std::copy_n(scratch.magnitudes.data(), count, output);
        else
            for (std::size_t index = 0; index < count; ++index)
                output[index * outputStride] = scratch.magnitudes[index];
    }

    std::vector<Entry> queue;
    std::vector<float> history;
    std::size_t maximum = 0, queueMask = 0, historyMask = 0;
    std::size_t first = 0, size = 0, selected = 0, cursor = 0, previousWindow = 0;
    std::uint64_t time = 0;
    float previous = 0.0f;
    bool hasWindow = false, falling = false;
    bool zeroHistory = true;
};
}
