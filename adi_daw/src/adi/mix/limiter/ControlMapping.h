// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include <algorithm>
#include <cmath>

namespace adi::mix::limiter
{
inline double roundedDisplayValue(double value, int decimals) noexcept
{
    const auto scale = std::pow(10.0, decimals);
    const auto rounded = std::round(value * scale) / scale;
    return rounded == 0.0 ? 0.0 : rounded;
}

inline double normalizedControl(double value) noexcept
{
    return std::isfinite(value) ? std::clamp(value, 0.0, 1.0) : 0.0;
}

inline float snapDecimalControl(float value, float minimum, float maximum, int subdivisions) noexcept
{
    const auto lower = static_cast<double>(minimum);
    const auto upper = static_cast<double>(maximum);
    const auto bounded = std::clamp(std::isfinite(value) ? static_cast<double>(value) : lower, lower, upper);
    if (subdivisions <= 0) return static_cast<float>(bounded);
    const auto scale = static_cast<double>(subdivisions);
    const auto steps = std::floor((bounded - lower) * scale + 0.5);
    return static_cast<float>(std::clamp(lower + steps / scale, lower, upper));
}

inline double inputGainFromNormalized(double value) noexcept
{
    return 30.0 * normalizedControl(value);
}

inline double inputGainToNormalized(double decibels) noexcept
{
    return normalizedControl(decibels / 30.0);
}

inline double outputLevelFromNormalized(double value) noexcept
{
    return 30.0 * normalizedControl(value) - 30.0;
}

inline double outputLevelToNormalized(double decibels) noexcept
{
    return normalizedControl((decibels + 30.0) / 30.0);
}

inline double lookaheadFromNormalized(double value) noexcept
{
    return 5.0 * normalizedControl(value);
}

inline double lookaheadToNormalized(double milliseconds) noexcept
{
    return normalizedControl(milliseconds / 5.0);
}

inline double attackFromNormalized(double value) noexcept
{
    const auto bounded = normalizedControl(value);
    const auto squared = bounded * bounded;
    return 10000.0 * squared * squared;
}

inline double attackToNormalized(double milliseconds) noexcept
{
    return std::sqrt(std::sqrt(normalizedControl(milliseconds / 10000.0)));
}

inline double releaseFromNormalized(double value) noexcept
{
    const auto bounded = normalizedControl(value);
    if (bounded > 0.9) return 5000.0 + (bounded - 0.9) * 50000.0;
    const auto ratio = bounded / 0.9;
    return 5000.0 * ratio * ratio * ratio;
}

inline double releaseToNormalized(double milliseconds) noexcept
{
    const auto bounded = normalizedControl(milliseconds / 10000.0) * 10000.0;
    return bounded > 5000.0 ? 0.9 + (bounded - 5000.0) / 50000.0 : 0.9 * std::cbrt(bounded / 5000.0);
}

inline double channelLinkFromNormalized(double value, bool surround) noexcept
{
    return std::min(200.0 * normalizedControl(value), surround ? 200.0 : 100.0);
}

inline double channelLinkToNormalized(double percent, bool surround) noexcept
{
    const auto bounded = std::isfinite(percent) ? std::clamp(percent, 0.0, surround ? 200.0 : 100.0) : 0.0;
    return bounded / 200.0;
}
}
