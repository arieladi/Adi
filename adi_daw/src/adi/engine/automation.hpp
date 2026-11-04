// SPDX-License-Identifier: GPL-3.0-or-later
//
// Immutable arrangement automation and absolute clip-relative envelopes.
//
// An AutomationProgram is compiled OFF the audio thread from the model, the
// tempo map and the session rate, as MidiClips are (ADR-0155). It is
// immutable once built: the audio thread only reads it.
//
//   * One lane per arrangement lane: owner 'track' or 'device', and the
//     arrangement stream plus clip overlays. Arrangement points are in samples: time_base 0 through
//     the tempo map, 1 from nanoseconds. Values stay in the lane's
//     value_domain.
//   * An arrangement with a bad point refuses its whole lane; a bad clip stream
//     refuses that overlay. Both report named problems and keep no partial stream.
//   * valueAt is O(log n); fill writes `count` values at a stride into the
//     caller's buffer. Both are noexcept and allocate nothing.
//   * Before the first point a lane reads its first value; after the last,
//     its last value. A lane with no points reads its default_value.
//
// Strip and parameter bindings read these immutable programs; clips override
// the arrangement only while their half-open placement is active.

#pragma once

#include "adi/store_rows.hpp"
#include "adi/blob.hpp"

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

struct AutomationClock;
struct ClipEnvelope {
    std::int64_t clipId = 0, begin = 0, end = 0;
    long double originTick = 0, originSeconds = 0, offsetTicks = 0;
    long double loopStart = 0, loopLength = 0;
    bool nanos = false, loop = false;
    std::vector<AutomationPoint> points; // original clip-relative units/curves
};
struct ClipAutomationRegion {
    std::int64_t begin = 0, end = 0;
    std::size_t envelope = 0;
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
    std::shared_ptr<const AutomationClock> clipClock;
    std::vector<ClipEnvelope> clipEnvelopes;
    std::vector<ClipAutomationRegion> clipRegions; // disjoint, by begin
    std::vector<std::int64_t> changes; // arrangement + clip/point/wrap boundaries
};

/// The value of a lane at a session sample. O(log n), noexcept, no allocation.
[[nodiscard]] double valueAt(const AutomationLaneProgram&, std::int64_t sample) noexcept;
/// First exact change boundary strictly after sample; INT64_MAX if none.
[[nodiscard]] std::int64_t automationBoundaryAfter(const AutomationLaneProgram&, std::int64_t sample) noexcept;

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
