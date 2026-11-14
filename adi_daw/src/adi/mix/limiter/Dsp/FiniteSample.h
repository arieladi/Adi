// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include <bit>
#include <cstdint>
#include <limits>

namespace adi::mix::limiter
{
inline float finiteSample(float input) noexcept
{
    static_assert(sizeof(float) == sizeof(std::uint32_t) && std::numeric_limits<float>::is_iec559);
    return (std::bit_cast<std::uint32_t>(input) & 0x7f800000u) == 0x7f800000u ? 0.0f : input;
}
}
