// SPDX-License-Identifier: GPL-3.0-or-later
//
// A control's glide (DEV-22's cores): a one-pole move to the set value over
// about `seconds` (to within 1 % in that time), so a gain, a level or a mix
// moved while audio runs does not step. Set before any audio (just after
// prepare()), the value is simply set: a device loading its state starts
// where it was left. Within 1e-12 of its target the value snaps to it, so a
// gain that arrives at 1 or a mix that arrives at 0 is exact.
//
// Not thread-safe: set() and next() are called from the same thread (the
// audio thread, as every core's set() and process() are).

#pragma once

#include <cmath>

namespace adi::dsp {

struct Glide {
    double value = 0, target = 0, coefficient = 0;
    bool fresh = true;

    /// The glide's speed at a rate, and a fresh start: the next set() lands.
    void prepare(double rate, double seconds = 0.010) noexcept {
        coefficient = rate > 0 && seconds > 0 ? std::exp(-1 / (seconds / 4.6 * rate)) : 0.0;
        value = target;
        fresh = true;
    }
    void set(double v) noexcept {
        target = v;
        if (fresh) {
            value = v;
        }
    }
    /// The value for the next sample.
    double next() noexcept {
        fresh = false;
        value = target + coefficient * (value - target);
        if (std::abs(value - target) < 1e-12 * (1 + std::abs(target))) {
            value = target;
        }
        return value;
    }
};

}  // namespace adi::dsp
