// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include "DetectorEndpointCache.h"
#include <array>
#include <bit>
#include <cstdint>

namespace adi::mix::limiter::detector_adjacent
{
inline bool finite(float value) noexcept
{
    return (std::bit_cast<std::uint32_t>(value) & 0x7f800000u) != 0x7f800000u;
}

inline bool sameKey(float leftMagnitude, float rightMagnitude, float leftThreshold,
                    float rightThreshold, float leftRatio, float rightRatio) noexcept
{
    if (!finite(leftMagnitude) || !finite(rightMagnitude) || !finite(leftThreshold)
        || !finite(rightThreshold) || !finite(leftRatio) || !finite(rightRatio)) return false;
    if (std::bit_cast<std::uint32_t>(leftThreshold) != std::bit_cast<std::uint32_t>(rightThreshold)
        || std::bit_cast<std::uint32_t>(leftRatio) != std::bit_cast<std::uint32_t>(rightRatio)) return false;
    const auto leftArgument = std::max(std::abs(leftMagnitude), 1e-10f);
    const auto rightArgument = std::max(std::abs(rightMagnitude), 1e-10f);
    return std::bit_cast<std::uint32_t>(leftArgument) == std::bit_cast<std::uint32_t>(rightArgument);
}

template <typename Endpoints>
inline std::array<float, 2> processPair(Endpoints& endpoints,
                                      float leftMagnitude, float rightMagnitude,
                                      float leftThreshold, float rightThreshold,
                                      float leftRatio, float rightRatio, float knee, float limit,
                                      bool reusable) noexcept
{
    const auto left = endpoints.process(leftMagnitude, leftThreshold, knee, leftRatio, limit);
    const auto right = reusable ? left
        : endpoints.process(rightMagnitude, rightThreshold, knee, rightRatio, limit);
    return {left, right};
}

#if defined(ADIL_PRIVATE_ADJACENT_STATS)
struct Statistics
{
    std::uint64_t genericSamples = 0, eligiblePairs = 0, reusedPairs = 0;
    std::uint64_t magnitudeOnlyMatches = 0, controlOnlyMatches = 0, nonfinitePairs = 0;
};
inline thread_local std::array<Statistics, 8> statistics{};

inline void recordPair(int style, bool reusable, float leftMagnitude, float rightMagnitude,
                       float leftThreshold, float rightThreshold, float leftRatio, float rightRatio) noexcept
{
    auto& state = statistics[static_cast<std::size_t>(style)];
    ++state.eligiblePairs;
    state.reusedPairs += reusable;
    if (!finite(leftMagnitude) || !finite(rightMagnitude) || !finite(leftThreshold)
        || !finite(rightThreshold) || !finite(leftRatio) || !finite(rightRatio))
    {
        ++state.nonfinitePairs;
        return;
    }
    state.magnitudeOnlyMatches += std::bit_cast<std::uint32_t>(std::max(std::abs(leftMagnitude), 1e-10f))
        == std::bit_cast<std::uint32_t>(std::max(std::abs(rightMagnitude), 1e-10f));
    state.controlOnlyMatches += std::bit_cast<std::uint32_t>(leftThreshold) == std::bit_cast<std::uint32_t>(rightThreshold)
        && std::bit_cast<std::uint32_t>(leftRatio) == std::bit_cast<std::uint32_t>(rightRatio);
}
#endif
}
