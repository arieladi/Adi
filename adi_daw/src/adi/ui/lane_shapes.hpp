// SPDX-License-Identifier: AGPL-3.0-or-later
//
// The line tools of a value lane (ADR-0241 d2; Cubase Pro 15 manual, the Key Editor's Line tool),
// headless: ADI's addition to Live's Velocity and controller lanes. A stroke across the lane gives
// every event under it the value of a shape at that point.
//
// A stroke runs from where the mouse went down to where it is. Line and Parabola run from the
// first value to the last. Sine, Triangle and Square swing around the first value by the difference
// to the last, `cycles` times along the stroke (the window gives one cycle per four grid steps).
#pragma once

#include <algorithm>
#include <array>
#include <cmath>

namespace adi::ui {
enum class LaneShape { Pointer, Line, Parabola, Sine, Triangle, Square, Paint };
inline constexpr std::array<const char *, 7> kLaneShapeNames{"Pointer", "Line", "Parabola", "Sine",
                                                             "Triangle", "Square",  "Paint"};
// The shape's value at `t`, from 0 at the stroke's start to 1 at its end.
inline double shapeValue(LaneShape shape, double t, double first, double last, double cycles) noexcept {
    const double at = std::clamp(t, 0., 1.);
    const double swing = last - first;
    const double phase = at * cycles - std::floor(at * cycles); // 0 to 1 within a cycle
    switch (shape) {
    case LaneShape::Line:
        return first + swing * at;
    case LaneShape::Parabola:
        return first + swing * at * at;
    case LaneShape::Sine:
        return first + swing * std::sin(2. * 3.14159265358979323846 * phase);
    case LaneShape::Triangle:
        // Up to the peak at a quarter, down through the centre to the trough at three quarters, back.
        return first + swing * (phase < .25 ? 4. * phase : phase < .75 ? 2. - 4. * phase : 4. * phase - 4.);
    case LaneShape::Square:
        return phase < .5 ? first + swing : first - swing;
    case LaneShape::Pointer:
    case LaneShape::Paint:
        break;
    }
    return last;
}
} // namespace adi::ui
