// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "adi/dsp/adi_shaper.hpp"
#include "adi/textproj.hpp"
#include <cmath>

namespace adi::device {
// The stock Pd transport carries one-based bar/beat and the fractional part
// of the absolute quarter note. The built-in uses the exact segment position
// whenever the graph supplies it, including across meter-map edits.
inline void applyShaperTransport(dsp::AdiShaper& core, bool playing, double bpm,
                                 double numerator, double denominator, double bar,
                                 double beat, double ticks) noexcept {
    if (!std::isfinite(numerator) || !std::isfinite(denominator) ||
        !std::isfinite(bar) || !std::isfinite(beat) || !std::isfinite(ticks) ||
        numerator <= 0 || denominator <= 0 || ticks < 0 || ticks >= textproj::kPPQ) return;
    core.tempo(bpm);
    const double beatStart = ((bar - 1.0) * numerator + beat - 1.0) * 4.0 / denominator;
    core.sync(std::floor(beatStart) + ticks / textproj::kPPQ, playing);
}
} // namespace adi::device
