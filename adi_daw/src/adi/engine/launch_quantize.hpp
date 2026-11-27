// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include "adi/launch_quantization.hpp"
#include "adi/store_rows.hpp"

#include <optional>
#include <span>

namespace adi::engine::launch_quantize {

/// Inclusive in ADI_PPQ. Meters are ordered by their authored tick position.
[[nodiscard]] std::optional<std::int64_t> nextGridTick(
    std::int64_t now, LaunchQuantization code, std::span<const rows::TimeSignature> meters) noexcept;

/// Link measures its quantum in quarters. None leaves the Link cycle intact.
[[nodiscard]] std::optional<double> quantumBeats(
    std::int64_t now, LaunchQuantization code, std::span<const rows::TimeSignature> meters) noexcept;

}  // namespace adi::engine::launch_quantize
