// SPDX-License-Identifier: AGPL-3.0-or-later
#include "ControlMapping.hpp"
#include <array>
#include <cmath>
#include <limits>
#include <numbers>

namespace adi::mix::deesser {
namespace {
bool unit(float v) noexcept { return std::isfinite(v) && v >= 0 && v <= 1; }
float thresholdPhysical(float v) noexcept {
    constexpr std::array<float, 5> x{0, .02f, .1f, .2f, 1};
    constexpr std::array<float, 5> y{0, .5f, .6f, .7333333492279053f, 1};
    float mapped = 0;
    if (v > 0) {
        for (std::size_t i = 1; i < x.size(); ++i) {
            if (v <= x[i]) {
                const float position = (v - x[i - 1]) / (x[i] - x[i - 1]);
                mapped = position * (y[i] - y[i - 1]) + y[i - 1];
                break;
            }
        }
    }
    return mapped * 180.0f - 180.0f;
}
float gain(float raw) noexcept {
    const float t = (raw + 1.0f) * .5f;
    if (t < .2f)
        return (t * t) * 2.07940936088562f;
    const float db = (t - .2f) * 72.0f - 21.600000381469727f;
    return static_cast<float>(std::exp(static_cast<double>(db) * .1151292546497023));
}
} // namespace
std::optional<float> thresholdFromNormalized(float v) noexcept {
    if (!unit(v))
        return {};
    return thresholdPhysical(v);
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
    return gain(v * 2.0f - 1.0f);
}
std::optional<float> panFromNormalized(float v) noexcept {
    if (!unit(v))
        return {};
    return v * 2.0f - 1.0f;
}
ScalarControlState ScalarControlState::threshold() noexcept { return ScalarControlState(Kind::Threshold); }
ScalarControlState ScalarControlState::range() noexcept { return ScalarControlState(Kind::Range); }
ScalarControlState ScalarControlState::level() noexcept { return ScalarControlState(Kind::Level); }
ScalarControlState ScalarControlState::pan() noexcept { return ScalarControlState(Kind::Pan); }
void ScalarControlState::reset() noexcept {
    raw_ = kind_ == Kind::Threshold ? .4f : kind_ == Kind::Range ? 6.0f : 0.0f;
}
Admission ScalarControlState::admitNormalized(float v) noexcept {
    if (!unit(v))
        return Admission::Invalid;
    const float next = kind_ == Kind::Threshold ? v : kind_ == Kind::Range ? v * 24.0f : v * 2.0f - 1.0f;
    if (std::abs(next - raw_) <= std::numeric_limits<float>::epsilon())
        return Admission::Unchanged;
    raw_ = next;
    return Admission::Changed;
}
float ScalarControlState::normalizedValue() const noexcept {
    return kind_ == Kind::Threshold ? raw_ : kind_ == Kind::Range ? raw_ / 24.0f : (raw_ + 1.0f) / 2.0f;
}
float ScalarControlState::physicalValue() const noexcept {
    if (kind_ == Kind::Threshold)
        return thresholdPhysical(raw_);
    return kind_ == Kind::Level ? gain(raw_) : raw_;
}
} // namespace adi::mix::deesser
