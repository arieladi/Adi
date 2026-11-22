// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include <optional>

namespace adi::mix::deesser {
// Invalid/nonfinite unit controls return no value. Helpers do not clamp.
// Threshold covers only [0.2,1), the closed -48..0 dB branch. The lower
// piecewise/mute region and zero-dB endpoint are not extrapolated.
// These are physical conversion helpers, without previous controller state or
// a same-target deadband. They do not replay a host parameter setter lifecycle.
[[nodiscard]] std::optional<float> thresholdFromNormalized(float) noexcept;
[[nodiscard]] std::optional<float> rangeFromNormalized(float) noexcept;
[[nodiscard]] std::optional<float> frequencyFromNormalized(float) noexcept;
[[nodiscard]] std::optional<float> gainFromNormalized(float) noexcept;
[[nodiscard]] std::optional<float> panFromNormalized(float) noexcept;
} // namespace adi::mix::deesser
