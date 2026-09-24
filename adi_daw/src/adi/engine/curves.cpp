// SPDX-License-Identifier: GPL-3.0-or-later
#include "adi/engine/curves.hpp"

#include <cmath>
#include <limits>

namespace adi::engine {
namespace {

double clampTension(double t) noexcept {
    if (!(t == t)) return 0.0;   // NaN
    return t > 1.0 ? 1.0 : (t < -1.0 ? -1.0 : t);
}

// The exponential family, for x strictly inside (0, 1) and t != 0:
//   u = (e^(k x) - 1) / (e^k - 1),  k = t ln 1000.
// Positive t is convex (slow start), negative t concave.
double expShape(double x, double t) noexcept {
    const double k = t * kExpRange;
    return std::expm1(k * x) / std::expm1(k);
}

// The quadratic bezier from (0,0) to (1,1) whose control point is
// (0.5 + t/2, 0.5 - t/2): on the anti-diagonal, the centre at t = 0 (a line),
// the corner (1,0) at t = +1 (convex) and (0,1) at t = -1 (concave).
// x(s) = (1+t) s - t s^2 is solved for s, then y(s) = (1-t) s (1-s) + s^2.
double bezierShape(double x, double t) noexcept {
    const double b = 1.0 + t;
    const double disc = b * b - 4.0 * t * x;   // >= 0 for t, x in range
    const double root = std::sqrt(disc > 0.0 ? disc : 0.0);
    const double s = 2.0 * x / (b + root);     // the stable root; b + root > 0 for x > 0
    return (1.0 - t) * s * (1.0 - s) + s * s;
}

// Guard against rounding stepping a shape a hair outside [0, 1].
double unit(double u) noexcept { return u < 0.0 ? 0.0 : (u > 1.0 ? 1.0 : u); }

}  // namespace

double curveShape(std::uint8_t curve, double x, double tension) noexcept {
    if (curve == static_cast<std::uint8_t>(CurveShape::Hold)) return x >= 1.0 ? 1.0 : 0.0;
    if (!(x > 0.0)) return 0.0;   // NaN reads as the start
    if (x >= 1.0) return 1.0;
    const double t = clampTension(tension);
    if (t == 0.0 || curve == static_cast<std::uint8_t>(CurveShape::Linear) || curve > kMaxCurve)
        return x;
    switch (static_cast<CurveShape>(curve)) {
        case CurveShape::Exp: return unit(expShape(x, t));
        case CurveShape::Log: return unit(1.0 - expShape(1.0 - x, t));
        case CurveShape::SCurve:
            // Two exponential halves, mirrored through the centre.
            return x <= 0.5 ? unit(0.5 * expShape(2.0 * x, t))
                            : unit(1.0 - 0.5 * expShape(2.0 - 2.0 * x, t));
        case CurveShape::Bezier: return unit(bezierShape(x, t));
        case CurveShape::Hold:
        case CurveShape::Linear: break;
    }
    return x;
}

double curveValue(double v0, double v1, double x, std::uint8_t curve, double tension) noexcept {
    const bool hold = curve == static_cast<std::uint8_t>(CurveShape::Hold);
    if (hold ? !(x >= 1.0) : !(x > 0.0)) return v0;
    if (x >= 1.0) return v1;
    const double u = curveShape(curve, x, tension);
    const double d = v1 - v0;
    if (std::isfinite(d)) return v0 + d * u;
    // v1 - v0 overflows only for values of opposite sign near the double
    // range; blend instead, which stays finite.
    return v0 * (1.0 - u) + v1 * u;
}

}  // namespace adi::engine
