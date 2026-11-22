// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <span>

namespace adi::dsp {

// A segment of a periodic two-dimensional, uniform cubic B-spline.
// x is in 1/8192-cycle units. Equal-X segments form an instantaneous jump.
// A positive-width span starts at the right-hand value of any such jump.
struct AdiShaperCornerSpan {
    struct XY { double x, y; };
    struct Cubic {
        double a, b, c, d;
        static Cubic through(double p, double q, double r, double s) noexcept {
            return {(p + 4 * q + r) / 6, (r - p) / 2, (p - 2 * q + r) / 2,
                    (s - 3 * r + 3 * q - p) / 6};
        }
        double at(double t) const noexcept { return ((d*t+c)*t+b)*t+a; }
        double slope(double t) const noexcept { return (3*d*t+2*c)*t+b; }
    };
    Cubic x{}, y{};
    double end = 0, inverseWidth = 0, gainDifference = 0;
    bool straight = false, flat = false;

    static AdiShaperCornerSpan make(XY p, XY q, XY r, XY s) noexcept {
        AdiShaperCornerSpan span;
        span.x = Cubic::through(p.x,q.x,r.x,s.x);
        span.y = Cubic::through(p.y,q.y,r.y,s.y);
        span.end = (q.x + 4*r.x + s.x) / 6;
        const double width = span.end - span.x.a;
        if (width <= 0) return span;
        span.inverseWidth = 1 / width;
        span.gainDifference = (q.y + 4*r.y + s.y) / 6 - span.y.a;
        span.flat = p.y == q.y && p.y == r.y && p.y == s.y;
        // Checking the control polygon also catches repeated hard knots and
        // avoids inversion wherever X and Y lie on the same straight line.
        const double dx = s.x-p.x, dy = s.y-p.y;
        span.straight = (q.y-p.y)*dx == (q.x-p.x)*dy && (r.y-p.y)*dx == (r.x-p.x)*dy;
        return span;
    }

    struct Cursor {
        std::size_t span = 0;
        double previousX = 0, previousT = 0, inverseSpeed = 0;
    };
    float at(double phase, Cursor& cursor, bool sameSpan) const noexcept {
        if (flat) return static_cast<float>(y.a);
        double t = std::clamp((phase-x.a)*inverseWidth,0.,1.);
        if (straight) return static_cast<float>(y.a + t*gainDifference);
        if (t == 0 || t == 1) return static_cast<float>(y.at(t));
        // The derivative of X is nonnegative on a sorted control polygon.
        // Bracket each Newton step so coincident controls and nearly vertical
        // edges converge without moving past the selected span.
        // Sequential playback usually stays on this span. Extrapolate its
        // last inverse X instead of starting a new inversion every sample.
        if (sameSpan && cursor.inverseSpeed > 0) {
            const double predicted = cursor.previousT + (phase-cursor.previousX)*cursor.inverseSpeed;
            if (predicted > 0 && predicted < 1) t = predicted;
        }
        double lo = 0, hi = 1;
        for (int i = 0; i < 8; ++i) {
            const double error = x.at(t)-phase;
            const double speed = x.slope(t);
            if (std::abs(error) <= 1.e-10) {
                cursor.previousX = phase;
                cursor.previousT = t;
                cursor.inverseSpeed = speed > 0 ? 1 / speed : 0;
                return static_cast<float>(y.at(t));
            }
            if (error < 0) lo = t; else hi = t;
            const double next = speed > 0 ? t - error/speed : t;
            t = next > lo && next < hi ? next : (lo+hi)*0.5;
        }
        for (int i = 0; i < 18; ++i) {
            if (x.at(t) < phase) lo = t; else hi = t;
            t = (lo+hi)*0.5;
        }
        cursor.inverseSpeed = 0;
        return static_cast<float>(y.at(t));
    }
};
}
