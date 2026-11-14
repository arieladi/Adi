// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include "FiniteSample.h"
#include "PeakWindow.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <numbers>
#include <span>
#include <stdexcept>
#include <vector>

namespace adi::mix::limiter
{
class SampleHistory
{
public:
    void prepare(std::size_t maximumDelay)
    {
        values.assign(std::bit_ceil(maximumDelay + 1), 0.0f);
        mask = values.size() - 1;
        cursor = 0;
    }

    void reset() noexcept { std::fill(values.begin(), values.end(), 0.0f); cursor = 0; }
    bool synchronizeStateFrom(const SampleHistory& source) noexcept
    {
        if (values.size() != source.values.size() || mask != source.mask) return false;
        std::copy(source.values.begin(), source.values.end(), values.begin());
        cursor = source.cursor;
        return true;
    }
    std::size_t synchronizationBytes() const noexcept { return values.size() * sizeof(float) + sizeof(cursor); }
    void push(float input) noexcept { cursor = (cursor + 1) & mask; values[cursor] = input; }
    float read(std::size_t age) const noexcept { return values[(cursor - age) & mask]; }
    float process(float input, std::size_t age) noexcept { push(input); return read(age); }

    class Block
    {
    public:
        explicit Block(SampleHistory& history) noexcept
            : owner(history), samples(history.values.data()), cursor(history.cursor), mask(history.mask) {}
        Block(const Block&) = delete;
        Block& operator=(const Block&) = delete;
        ~Block() { owner.cursor = cursor; }
        float process(float input, std::size_t age) noexcept
        {
            cursor = (cursor + 1) & mask;
            samples[cursor] = input;
            return samples[(cursor - age) & mask];
        }

    private:
        SampleHistory& owner;
        float* samples;
        std::size_t cursor, mask;
    };

private:
    std::vector<float> values;
    std::size_t cursor = 0, mask = 0;
};

enum class KernelFamily { triangular, polynomial, cosine, trapezoidal };

inline bool smoothingKernel(std::span<float> destination, KernelFamily family,
                            float smoothing, float offset, float power) noexcept
{
    if (destination.empty() || !std::isfinite(smoothing) || smoothing < 0.0f || smoothing > 1.0f
        || !std::isfinite(offset) || offset < 0.0f || offset > 1.0f
        || !std::isfinite(power) || power < 0.0f || power > 1.0f)
        return false;
    const auto pivot = 1.0f - offset;
    const auto exponent = 1.0 + 3.0 * static_cast<double>(power);
    double sum = 0.0;
    for (std::size_t index = 0; index < destination.size(); ++index)
    {
        auto position = (static_cast<double>(index) + 1.0) / (static_cast<double>(destination.size()) + 1.0);
        if (pivot <= 1.1920929e-7f || 1.0f - pivot <= 1.1920929e-7f)
            position = (position + 1.0 - pivot) * 0.5;
        else
            position = position < pivot ? position / pivot * 0.5 : ((position - pivot) / (1.0 - pivot) + 1.0) * 0.5;
        double weight = 1.0;
        if (destination.size() > 1)
        {
            switch (family)
            {
                case KernelFamily::triangular:
                    weight = 0.5 - smoothing * std::abs(position - 0.5);
                    break;
                case KernelFamily::polynomial:
                    weight = 1.0 + (4.0f * smoothing) * (position * (1.0 - position) - 0.25);
                    break;
                case KernelFamily::cosine:
                    weight = 0.5 * (1.0 + std::cos(((position - 0.5) * smoothing) * (2.0 * std::numbers::pi)));
                    break;
                case KernelFamily::trapezoidal:
                    weight = smoothing <= 1.1920929e-7f ? 1.0
                             : std::min(2.0 * (0.5 - std::abs(position - 0.5)) / smoothing, static_cast<double>(smoothing));
                    break;
                default: return false;
            }
            weight = std::pow(std::max(0.0, weight), exponent);
        }
        destination[index] = static_cast<float>(weight);
        sum += destination[index];
    }
    if (sum < 1.1920929e-7)
        std::fill(destination.begin(), destination.end(), 1.0f / static_cast<float>(destination.size()));
    else
    {
        const auto reciprocal = static_cast<float>(1.0 / sum);
        for (auto& coefficient : destination)
            coefficient *= reciprocal;
    }
    return true;
}

class ButterworthSection
{
public:
    void prepare(double rate, float frequency, bool highpass)
    {
        if (!std::isfinite(rate) || rate <= 0.0 || !std::isfinite(frequency) || frequency <= 0.0f)
            throw std::invalid_argument("Invalid filter configuration");
        const auto omega = std::min(static_cast<float>(2.0 * std::numbers::pi * frequency / rate), 3.1315928f);
        const auto warped = std::tan(static_cast<double>(omega) / 2.0);
        const auto denominator = 1.0 + std::sqrt(2.0) * warped + warped * warped;
        first = (highpass ? 1.0 : warped * warped) / denominator;
        middle = (highpass ? -2.0 : 2.0) * first;
        feedbackFirst = 2.0 * (warped * warped - 1.0) / denominator;
        feedbackSecond = (1.0 - std::sqrt(2.0) * warped + warped * warped) / denominator;
        reset();
    }

    void reset() noexcept { stateFirst = 0.0; stateSecond = 0.0; }

    float process(float input) noexcept
    {
        const auto value = static_cast<double>(finiteSample(input));
        const auto output = first * value + stateFirst;
        stateFirst = middle * value - feedbackFirst * output + stateSecond;
        stateSecond = first * value - feedbackSecond * output;
        if (std::abs(stateFirst) < 1e-30) stateFirst = 0.0;
        if (std::abs(stateSecond) < 1e-30) stateSecond = 0.0;
        return static_cast<float>(output);
    }

private:
    double first = 1.0, middle = 0.0, feedbackFirst = 0.0, feedbackSecond = 0.0;
    double stateFirst = 0.0, stateSecond = 0.0;
};
}
