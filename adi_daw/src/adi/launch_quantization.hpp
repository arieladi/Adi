// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include <cstdint>

namespace adi {

enum class LaunchQuantization : std::int32_t {
    None = 0, EightBars, FourBars, TwoBars, Bar, Half, HalfTriplet,
    Quarter, QuarterTriplet, Eighth, EighthTriplet, Sixteenth, SixteenthTriplet, ThirtySecond,
};

[[nodiscard]] constexpr bool isLaunchQuantizationCode(std::int64_t code) noexcept {
    return code >= 0 && code <= static_cast<std::int64_t>(LaunchQuantization::ThirtySecond);
}

} // namespace adi
