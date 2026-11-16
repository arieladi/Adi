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

// The handles on a selection of lane values (ADR-0241 d2; Cubase's controller event editor): what a
// drag of `by` lane units on a handle does to a value that stood at `value`, at `t` from 0 (the
// selection's first event) to 1 (its last). `mean` and `highest` are the selection's own.
//   TiltLeft and TiltRight raise or lower one end and leave the other where it is.
//   Scale multiplies every value so that the highest follows the pointer.
//   Compress draws every value towards the selection's mean (a drag down) or away from it (up).
enum class LaneHandle { None, TiltLeft, TiltRight, Scale, Compress };
inline double handleValue(LaneHandle handle, double value, double t, double by, double mean, double highest) noexcept {
    switch (handle) {
    case LaneHandle::TiltLeft:
        return value + by * (1. - std::clamp(t, 0., 1.));
    case LaneHandle::TiltRight:
        return value + by * std::clamp(t, 0., 1.);
    case LaneHandle::Scale:
        return highest <= 0 ? value : value * std::max(0., (highest + by) / highest);
    case LaneHandle::Compress: {
        const double spread = std::max(1., highest - mean);
        return mean + (value - mean) * std::max(0., (spread + by) / spread);
    }
    case LaneHandle::None:
        break;
    }
    return value;
}
} // namespace adi::ui
