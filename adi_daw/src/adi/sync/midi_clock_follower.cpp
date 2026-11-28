// SPDX-License-Identifier: AGPL-3.0-or-later
#include "adi/sync/midi_clock_follower.hpp"

#include <algorithm>
#include <numbers>

namespace adi::sync {
namespace {
constexpr double nsPerMinutePerClock = 60'000'000'000.0 / 24.0;
constexpr double minPeriod = nsPerMinutePerClock / 999.0;
constexpr double maxPeriod = nsPerMinutePerClock / 20.0;
constexpr unsigned lockIntervals = 6; // one MIDI song-position unit
constexpr double dropoutRatio = 1.75;
} // namespace

void MidiClockFollower::pulse(std::uint64_t timestamp) noexcept {
    if (!lastPulse_) {
        lastPulse_ = timestamp;
        return;
    }
    if (timestamp <= *lastPulse_)
        return;
    const auto elapsed = static_cast<double>(timestamp - *lastPulse_);
    if (elapsed < minPeriod * 0.5)
        return;
    lastPulse_ = timestamp;

    // A lost pulse or a different source must warm up again. No phase or
    // pulse count is synthesized for the interval the sender missed.
    if (elapsed > maxPeriod * dropoutRatio ||
        (period_ != 0.0 && (elapsed > period_ * dropoutRatio || elapsed < period_ * 0.5))) {
        period_ = nextInterval_ = phaseError_ = 0.0;
        intervals_ = 0;
        return;
    }
    if (intervals_ == 0) {
        period_ = nextInterval_ = std::clamp(elapsed, minPeriod, maxPeriod);
        intervals_ = 1;
        return;
    }

    // Approximate a one-Hz loop bandwidth, reducing it at very slow
    // tempos to keep the discrete-time gains in their well-damped range.
    const auto omega = std::min(0.35, 2.0 * std::numbers::pi * period_ / 1'000'000'000.0);
    const auto phaseGain = std::numbers::sqrt2 * omega;
    const auto periodGain = omega * omega;
    phaseError_ = elapsed - nextInterval_;
    period_ = std::clamp(period_ + periodGain * phaseError_, minPeriod, maxPeriod);
    // Rebase the prediction on this observed pulse. Working in intervals
    // keeps floating-point precision independent of the host's uptime.
    nextInterval_ = period_ + (phaseGain - 1.0) * phaseError_;
    if (intervals_ < lockIntervals)
        ++intervals_;
}

std::optional<ClockEstimate> MidiClockFollower::estimate(std::uint64_t now) const noexcept {
    if (!lastPulse_ || now < *lastPulse_ || intervals_ < lockIntervals ||
        static_cast<double>(now - *lastPulse_) > period_ * dropoutRatio)
        return {};
    return ClockEstimate{nsPerMinutePerClock / period_, phaseError_};
}

void MidiClockFollower::reset() noexcept {
    *this = MidiClockFollower{};
}

} // namespace adi::sync
