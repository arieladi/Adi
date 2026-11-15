// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include "palettes/data.hpp"
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace adi::ui::palettes {
inline constexpr std::size_t count = data::ids.size();
inline constexpr std::size_t defaultIndex = 0;
constexpr std::string_view id(std::size_t index) noexcept {
    return data::ids[index < count ? index : defaultIndex];
}
constexpr std::size_t find(std::string_view name) noexcept {
    for (std::size_t i = 0; i < count; ++i) if (data::ids[i] == name) return i;
    return defaultIndex;
}
// Index zero is the application's existing palette, supplied by presentation.
constexpr std::uint32_t rgb(std::size_t index, std::uint8_t sample, bool reverse = false) noexcept {
    if (index == 0 || index >= count) return 0;
    return data::rgb[index - 1][reverse ? 255u - sample : sample];
}
} // namespace adi::ui::palettes
