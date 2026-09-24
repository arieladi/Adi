// SPDX-License-Identifier: GPL-3.0-or-later
//
// The curve formulas of AEXP and AAUT points (SPEC §6.3.2, normative;
// ADR-0159). One evaluator, so the engine, the converters and the UI draw the
// same line.
//
// A segment runs from a point (x = 0, value v0) to the next (x = 1, value v1).
// The first point's `curve` and `tension` shape it:
//
//     u = curveShape(curve, x, tension)        u(0) = 0, u(1) = 1 exactly
//     v = v0 + (v1 - v0) * u                   v(0) = v0, v(1) = v1 exactly
//
// Every function here is pure, noexcept and allocation-free: callable on the
// audio thread. Tension is clamped to [-1, 1]; a NaN tension reads as 0.

#pragma once

#include <cstdint>

namespace adi::engine {

/// The shapes, as stored in the `curve` byte. Not `tempo_map.curve`.
enum class CurveShape : std::uint8_t {
    Hold = 0, Linear = 1, Exp = 2, Log = 3, SCurve = 4, Bezier = 5,
};

/// The highest `curve` a reader accepts. A reader refuses a larger value
/// (ADR-0159); the evaluator, to stay total, draws it as linear.
inline constexpr std::uint8_t kMaxCurve = 5;

/// ln(1000): at tension +1 the exponential spans a 1000:1 range, the 60 dB
/// of a classic exponential fade.
inline constexpr double kExpRange = 6.907755278982137;

/// The normalised shape u(x) for x in [0, 1]. x at or below 0 gives 0, at or
/// above 1 gives 1 (except Hold, which is 0 until x reaches 1).
[[nodiscard]] double curveShape(std::uint8_t curve, double x, double tension) noexcept;

/// The value at x of a segment from v0 to v1: v0 at x <= 0, v1 at x >= 1,
/// bit for bit, and v0 + (v1 - v0) * u between.
[[nodiscard]] double curveValue(double v0, double v1, double x, std::uint8_t curve,
                                double tension) noexcept;

}  // namespace adi::engine
