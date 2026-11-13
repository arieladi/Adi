// SPDX-License-Identifier: AGPL-3.0-or-later
#include "ControlMapping.hpp"
#include <algorithm>
#include <cmath>

namespace adi::mix::gate {
bool effectiveAttackSeconds(float seconds, float &output) noexcept {
    if (!std::isfinite(seconds) || seconds < 0 || seconds > 1)
        return false;
    const float raw = static_cast<float>(std::pow(static_cast<double>(seconds * .1f), .25));
    const double squared = static_cast<double>(raw) * raw;
    output = static_cast<float>(squared * squared) * 10.0f;
    return true;
}
bool effectiveReleaseSeconds(float seconds, float &output) noexcept {
    if (!std::isfinite(seconds) || seconds < 0 || seconds > 5)
        return false;
    const double u = std::max(seconds, .005f) * .1f;
    const float raw = static_cast<float>(u < .5 ? std::pow(2 * u, 1.0 / 3.0) * .9 : .9 + .2 * (u - .5));
    const double a = raw;
    const double mapped = a < .9 ? (a * a) * (a * .6858710562414267) : .5 + 5 * (a - .9);
    output = static_cast<float>(mapped) * 10.0f;
    return true;
}
} // namespace adi::mix::gate
