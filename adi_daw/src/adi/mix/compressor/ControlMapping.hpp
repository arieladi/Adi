// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include <optional>

namespace adi::mix::compressor {

struct ManualTimes {
    double attackSeconds = 0.0;
    double releaseSeconds = 0.0;
};

// Invalid/nonfinite unit controls return no value. These helpers do not clamp an
// invalid setting into a different accepted setting, and perform no allocation.
[[nodiscard]] std::optional<float> ratioFromNormalized(float) noexcept;
[[nodiscard]] std::optional<ManualTimes> manualTimes(float attack, float release) noexcept;

} // namespace adi::mix::compressor
