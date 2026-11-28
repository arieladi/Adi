// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include "adi/engine/snapshot.hpp"
#include "adi/textproj.hpp"

namespace adi::engine {

struct MidiNotePosition {
    std::int64_t wholeTick = 0;
    double fraction = 0.0;

    // Add the fractional tick only after subtracting the authored note origin.
    [[nodiscard]] double relativeTo(std::int64_t origin, std::int64_t noteStart = 0) const noexcept {
        const double elapsed = (wholeTick < 0) == (origin < 0) ? static_cast<double>(wholeTick - origin)
            : static_cast<double>(wholeTick) - static_cast<double>(origin);
        return (elapsed - static_cast<double>(noteStart)) + fraction;
    }
};

[[nodiscard]] inline MidiNotePosition midiNotePosition(const auto& tempo, double seconds) noexcept {
    constexpr double ppq = static_cast<double>(textproj::kPPQ);
    const auto tick = tempo.secondsToTicks(seconds);
    return {tick, (seconds - tempo.ticksToSeconds(tick)) * tempo.bpmAt(tick) * ppq / 60.0};
}

} // namespace adi::engine
