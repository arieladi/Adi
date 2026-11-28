// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "adi/engine/curves.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>

namespace adi::engine {

inline constexpr std::size_t kRackMacroCount = 16;
inline constexpr std::array<std::string_view, kRackMacroCount> kRackMacroParams{
    "macro.1", "macro.2", "macro.3", "macro.4", "macro.5", "macro.6", "macro.7", "macro.8",
    "macro.9", "macro.10", "macro.11", "macro.12", "macro.13", "macro.14", "macro.15", "macro.16",
};

/// The stored macros.ord is zero-based. Parameter references are one-based.
[[nodiscard]] constexpr std::optional<std::size_t> rackMacroSlot(std::int64_t ordinal) noexcept {
    if (ordinal < 0 || ordinal >= static_cast<std::int64_t>(kRackMacroCount)) return std::nullopt;
    return static_cast<std::size_t>(ordinal);
}

[[nodiscard]] constexpr std::string_view rackMacroParamRef(std::size_t slot) noexcept {
    if (slot >= kRackMacroCount) return {};
    return kRackMacroParams[slot];
}

[[nodiscard]] constexpr std::optional<std::size_t> parseRackMacroParamRef(std::string_view reference) noexcept {
    for (std::size_t slot = 0; slot < kRackMacroCount; ++slot) {
        if (kRackMacroParams[slot] == reference) return slot;
    }
    return std::nullopt;
}

struct MacroRange {
    double minimum = 0.0;
    double maximum = 1.0;
    std::uint8_t curve = static_cast<std::uint8_t>(CurveShape::Linear);
    bool inverted = false;
};

/// An authored mapping yields one value in the target's domain.
[[nodiscard]] inline std::optional<double> macroContribution(MacroRange range, double position) noexcept {
    if (!std::isfinite(range.minimum) || !std::isfinite(range.maximum) || !std::isfinite(position))
        return std::nullopt;

    // Legacy rows can express inversion with a flag or descending endpoints.
    // The flag on an already descending range is redundant.
    if (range.inverted && range.minimum < range.maximum) std::swap(range.minimum, range.maximum);
    return curveValue(range.minimum, range.maximum, position, range.curve, 0.0);
}

/// Add active mappings in the target's domain. The target cap applies once,
/// after the sum. Callers leave disabled mappings out of this accumulator.
class MacroSum {
public:
    void add(MacroRange range, double position) noexcept {
        if (const auto contribution = macroContribution(range, position)) {
            sum_ += *contribution;
            hasValue_ = true;
        }
    }

    [[nodiscard]] std::optional<double> value(double floor, double ceiling) const noexcept {
        if (!hasValue_ || !std::isfinite(floor) || !std::isfinite(ceiling) ||
            floor > ceiling || std::isnan(sum_)) return std::nullopt;
        return static_cast<double>(std::clamp(sum_, static_cast<long double>(floor),
                                             static_cast<long double>(ceiling)));
    }

private:
    long double sum_ = 0.0;
    bool hasValue_ = false;
};

/// Normalize a linear physical range for ParamValue. VST3 passes [0, 1].
/// Use the native declaration's conversion for a nonlinear built-in.
[[nodiscard]] inline std::optional<double> normalizeMacroValue(
    double value, double minimum, double maximum) noexcept {
    if (!std::isfinite(value) || !std::isfinite(minimum) || !std::isfinite(maximum) ||
        minimum > maximum) return std::nullopt;
    if (minimum == maximum || value <= minimum) return 0.0;
    if (value >= maximum) return 1.0;

    const double width = maximum - minimum;
    if (std::isfinite(width)) return (value - minimum) / width;
    return (value / 2.0 - minimum / 2.0) / (maximum / 2.0 - minimum / 2.0);
}

}  // namespace adi::engine
