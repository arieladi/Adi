// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>
#include <stdexcept>

namespace adi::mix::limiter
{
class FloatTarget
{
public:
    void prepare(float sampleRate, float minimum, float maximum, float adaptiveRange = 0.0f)
    {
        if (!std::isfinite(sampleRate) || sampleRate <= 0.0f || !std::isfinite(minimum)
            || !std::isfinite(maximum) || minimum >= maximum || !std::isfinite(adaptiveRange)
            || adaptiveRange < 0.0f)
            throw std::invalid_argument("Invalid target configuration");
        rate = sampleRate;
        lower = minimum;
        upper = maximum;
        range = adaptiveRange > 0.0f ? adaptiveRange : maximum - minimum;
        reset(std::clamp(0.0f, lower, upper));
    }

    void reset(float value) noexcept
    {
        target = start = std::clamp(std::isfinite(value) ? value : 0.0f, lower, upper);
        position = length = 0;
        increment = 0.0f;
    }

    bool setTarget(float value, bool continuous = true, float durationMilliseconds = -1.0f) noexcept
    {
        if (!std::isfinite(value) || !std::isfinite(durationMilliseconds))
            return false;
        value = std::clamp(value, lower, upper);
        if (std::abs(value - target) <= std::numeric_limits<float>::epsilon())
            return false;
        const auto nextStart = continuous && position < length ? static_cast<float>(position) * increment + start : target;
        if (durationMilliseconds < 0.0f)
        {
            const auto distance = std::max(0.0f, 0.3f - std::abs(value - nextStart) / range);
            durationMilliseconds = continuous ? ((150.0f - 20.0f) * distance) * (distance * 11.111111f) + 20.0f : 0.0f;
        }
        const auto samples = (durationMilliseconds * rate) * 0.001f;
        if (!std::isfinite(samples) || samples > static_cast<float>(std::numeric_limits<int>::max() / 2))
            return false;
        start = nextStart;
        target = value;
        length = static_cast<std::size_t>(std::max(samples, 0.0f));
        position = 0;
        increment = length > 0 ? (target - start) / static_cast<float>(length) : 0.0f;
        beginBlock();
        return true;
    }

    // Position-based interpolation needs no cache repair at host block boundaries.
    void beginBlock() noexcept {}

    float process() noexcept
    {
        if (position >= length)
            return target;
        const auto result = static_cast<float>(position) * increment + start;
        ++position;
        return result;
    }

    void skip(std::size_t samples) noexcept
    {
        position += std::min(samples, length - std::min(position, length));
        beginBlock();
    }

    float destination() const noexcept { return target; }
    std::size_t remaining() const noexcept { return length - std::min(length, position); }

private:
    float rate = 48000.0f, lower = 0.0f, upper = 1.0f, range = 1.0f;
    float target = 0.0f, start = 0.0f, increment = 0.0f;
    std::size_t position = 0, length = 0;
};
}
