// SPDX-License-Identifier: GPL-3.0-or-later
//
// Automation read into the engine, not yet played (ADR-0159, part two).
//
// An AutomationProgram is compiled OFF the audio thread from the model, the
// tempo map and the session rate, as MidiClips are (ADR-0155). It is
// immutable once built: the audio thread only reads it.
//
//   * One lane per arrangement lane: owner 'track' or 'device', and the
//     stream with no clip. Points are in session samples: time_base 0 through
//     the tempo map, 1 from nanoseconds. Values stay in the lane's
//     value_domain.
//   * A lane with one bad point is refused whole, with a named problem, never
//     kept in part.
//   * valueAt is O(log n); fill writes `count` values at a stride into the
//     caller's buffer. Both are noexcept and allocate nothing.
//   * Before the first point a lane reads its first value; after the last,
//     its last value. A lane with no points reads its default_value.
//
// Emitting parameter events from this is the next mission: user override,
// touch and latch wait on the director's rulings.

#pragma once

#include "adi/store_rows.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace adi::engine {

/// One point, placed. The segment from a point to the next is shaped by the
/// point's curve and tension (SPEC §6.3.2).
struct PlacedPoint {
    std::int64_t sample = 0;
    double value = 0.0;
    double tension = 0.0;
    std::uint8_t curve = 1;
};

struct AutomationLaneProgram {
    std::int64_t laneId = 0;
    std::string ownerKind;
    std::int64_t ownerId = 0;
    std::string paramRef;
    std::string valueDomain;
    bool enabled = true;
    double defaultValue = 0.0;
    std::vector<PlacedPoint> points;   ///< by sample; equal samples are a step
};

/// The value of a lane at a session sample. O(log n), noexcept, no allocation.
[[nodiscard]] double valueAt(const AutomationLaneProgram&, std::int64_t sample) noexcept;

/// `count` values from `first`, `stride` samples apart, into `out`.
/// noexcept and allocation-free; a forward stride walks the points once.
void fill(const AutomationLaneProgram&, std::int64_t first, std::int64_t stride, std::size_t count,
          double* out) noexcept;

class AutomationProgram {
public:
    /// Compiles every arrangement lane. Never throws for a bad lane: it is
    /// refused and named in problems(). `sessionRate` in Hz.
    AutomationProgram(const rows::Model&, double sessionRate);

    [[nodiscard]] const std::vector<AutomationLaneProgram>& lanes() const noexcept { return lanes_; }
    /// The compiled lane, or null if it was refused, not an arrangement lane, or unknown.
    [[nodiscard]] const AutomationLaneProgram* lane(std::int64_t laneId) const noexcept;
    [[nodiscard]] const std::vector<std::string>& problems() const noexcept { return problems_; }
    [[nodiscard]] double sessionRate() const noexcept { return rate_; }

private:
    double rate_ = 0.0;
    std::vector<AutomationLaneProgram> lanes_;   ///< by laneId
    std::vector<std::string> problems_;
};

/// Build a program on the calling thread: the one the message thread hands on.
[[nodiscard]] std::shared_ptr<const AutomationProgram> compileAutomation(const rows::Model&,
                                                                         double sessionRate);

}  // namespace adi::engine
