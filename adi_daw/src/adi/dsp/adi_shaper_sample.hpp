// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <bit>
#include <cstdint>
#include <limits>

namespace adi::dsp::shaper {
// Replace IEEE-754 infinities and NaNs without changing the bits of finite audio.
inline float finiteSample(float sample) noexcept {
    static_assert(std::numeric_limits<float>::is_iec559 && sizeof(float) == sizeof(std::uint32_t));
    constexpr std::uint32_t exponent = 0x7f800000U;
    return (std::bit_cast<std::uint32_t>(sample) & exponent) == exponent ? 0.f : sample;
}
} // namespace adi::dsp::shaper
