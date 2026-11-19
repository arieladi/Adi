// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "adi/render/queue.hpp"
#include <cstddef>
#include <string>
#include <string_view>

namespace adi::render {
inline constexpr std::size_t kMaximumJobJsonBytes = 1024 * 1024;
// EX-09: writes complete version-2 values, including collision. Legacy version 1
// must omit collision and imports as Fail. UTF-8 paths, closed required objects.
// Throws std::invalid_argument for malformed/unsupported inputs, including
// duplicate keys, invalid enums, narrowing integers and runtime-only services.
// No filesystem access, queue action, plug-in load or project snapshot occurs.
// A decoder pointer or live external tempo is not a persistent render option.
[[nodiscard]] std::string jobToJson(const Job&);
[[nodiscard]] Job jobFromJson(std::string_view);
} // namespace adi::render
