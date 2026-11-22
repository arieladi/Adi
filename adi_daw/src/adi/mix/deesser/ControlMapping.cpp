// SPDX-License-Identifier: AGPL-3.0-or-later
#include "ControlMapping.hpp"
#include <cmath>
#include <numbers>

namespace adi::mix::deesser {
namespace {
bool unit(float v) noexcept { return std::isfinite(v) && v >= 0 && v <= 1; }
} // namespace
std::optional<float> thresholdFromNormalized(float v) noexcept {
    if (!unit(v) || v < .2f || v == 1)
        return {};
    return v * 60.0f - 60.0f;
}
std::optional<float> rangeFromNormalized(float v) noexcept {
    if (!unit(v))
        return {};
    return v * 24.0f;
}
std::optional<float> frequencyFromNormalized(float v) noexcept {
    if (!unit(v))
        return {};
    constexpr float low = 10.965784072875977f, high = 14.287712097167969f;
    const float logarithm = low + v * (high - low);
    return static_cast<float>(std::exp(static_cast<double>(logarithm) * std::numbers::ln2));
}
std::optional<float> gainFromNormalized(float v) noexcept {
    if (!unit(v))
        return {};
    const float t = ((v * 2.0f - 1.0f) + 1.0f) * .5f;
    if (t < .2f)
        return (t * t) * 2.07940936088562f;
    const float db = (t - .2f) * 72.0f - 21.600000381469727f;
    return static_cast<float>(std::exp(static_cast<double>(db) * .1151292546497023));
}
std::optional<float> panFromNormalized(float v) noexcept {
    if (!unit(v))
        return {};
    return v * 2.0f - 1.0f;
}
} // namespace adi::mix::deesser
