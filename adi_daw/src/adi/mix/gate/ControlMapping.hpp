// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

namespace adi::mix::gate {
// Stateless physical-control conversions. Invalid inputs return false and
// leave output unchanged. No controller admission, smoothing or factory state.
// Requires the same round-to-nearest environment as Core preparation.
[[nodiscard]] bool effectiveAttackSeconds(float seconds, float &output) noexcept;
[[nodiscard]] bool effectiveReleaseSeconds(float seconds, float &output) noexcept;
} // namespace adi::mix::gate
