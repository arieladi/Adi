// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include <cstdint>
#include <optional>

namespace adi::sync {

struct ClockEstimate {
    double bpm;
    double phaseErrorNanoseconds;
};

/// Second-order delay-locked loop for arrival times of MIDI timing clocks.
/// Feed only F8 arrival timestamps in one monotonic nanosecond domain.
/// One serial owner can use the estimate without allocations or I/O.
class MidiClockFollower {
public:
    void pulse(std::uint64_t timestamp) noexcept;
    [[nodiscard]] std::optional<ClockEstimate> estimate(std::uint64_t now) const noexcept;
    void reset() noexcept;

private:
    std::optional<std::uint64_t> lastPulse_;
    double period_ = 0.0;
    double nextInterval_ = 0.0;
    double phaseError_ = 0.0;
    unsigned intervals_ = 0;
};

} // namespace adi::sync
