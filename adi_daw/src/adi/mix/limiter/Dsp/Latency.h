// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>

namespace adi::mix::limiter
{
enum class Style { transparent, punchy, dynamic, allround, aggressive, modern, bus, safe };

inline int resamplingLatency(float rate, int factor, float attenuation, float cutoff) noexcept
{
    auto latency = 0;
    const auto scale = (attenuation - 7.95f) * -0.13927576f;
    while (factor > 1)
    {
        const auto width = std::max((rate - 2.0f * cutoff) / rate, 0.09f);
        const auto order = factor == 2 ? scale / width : 2.0f * scale;
        const auto halfOrder = 2 * ((2 - static_cast<int>(order)) / 4);
        latency += 2 * ((halfOrder + factor - 1) / factor);
        factor /= 2;
    }
    return latency;
}

inline int truePeakLookahead(float rate) noexcept
{
    auto factor = 8;
    while (factor > 2 && rate * static_cast<float>(factor) > 384001.0f)
        factor /= 2;
    return resamplingLatency(rate, factor, 60.0f, rate * 0.45351472f) / 2;
}

inline int processingLatency(Style style, double sampleRate, int factor, bool truePeak) noexcept
{
    const auto index = static_cast<int>(style);
    if (index < 0 || index > 7 || !std::isfinite(sampleRate) || sampleRate < 8000.0 || sampleRate > 6144000.0
        || factor < 1 || factor > 32 || !std::has_single_bit(static_cast<unsigned>(factor)))
        return -1;
    struct Stages { int lookahead, smoothing, peakGuard; };
    constexpr std::array stages{Stages{1, 1, 0}, Stages{1, 1, 0}, Stages{2, 2, 0}, Stages{1, 1, 0},
                                Stages{1, 1, 1}, Stages{2, 0, 1}, Stages{1, 1, 1}, Stages{1, 0, 1}};
    const auto topology = stages[static_cast<std::size_t>(index)];
    const auto rate = static_cast<float>(sampleRate);
    const auto internalRate = rate * static_cast<float>(factor);
    auto internalDelay = topology.lookahead * static_cast<int>(internalRate * 0.005f)
                         + topology.smoothing * 256 + topology.peakGuard * static_cast<int>(internalRate * 0.05f);
    if (truePeak)
        internalDelay += truePeakLookahead(internalRate);
    auto externalDelay = resamplingLatency(rate, factor, 90.0f, 20000.0f);
    if (factor > 1 || truePeak)
    {
        externalDelay += 128 + static_cast<int>(rate * 0.00133f);
        if (truePeak)
            externalDelay += truePeakLookahead(rate);
    }
    return (internalDelay + factor - 1) / factor + externalDelay;
}
}
