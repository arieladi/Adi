// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include "adi/engine/curves.hpp"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <optional>

namespace adi::engine {
/// The same envelope for playback and the window's fade handle. Absence keeps
/// legacy files' exact arithmetic at their original width; explicit tension,
/// including zero, selects the shared continuous curve evaluator.
template<class T>
[[nodiscard]] T clipFadeShape(T x,std::int64_t curve,const std::optional<double>& tension={}) noexcept {
    x=std::clamp(x,T{0},T{1});
    if(tension)return static_cast<T>(curveShape(static_cast<std::uint8_t>(curve),static_cast<double>(x),*tension));
    switch(curve) {
        case 0:return x>=T{1}?T{1}:T{0};
        case 2:return x*x;
        case 3:return std::sqrt(x);
        case 4:return x*x*(T{3}-T{2}*x);
        default:return x;
    }
}
} // namespace adi::engine
