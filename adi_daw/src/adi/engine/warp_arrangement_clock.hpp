// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include <cstdint>
#include <span>
#include <utility>
#include <vector>

namespace adi { namespace rows { struct TempoEvent; } }

namespace adi::engine {
struct TempoMap;

// A prepared sample-clock index over the effective TempoMap. It shares tempo
// curve math with TempoMap, with bounded per-frame lookup and inversion.
class WarpArrangementClock {
public:
    WarpArrangementClock(const TempoMap&, double sampleRate);
    WarpArrangementClock(std::span<const rows::TempoEvent>, double sampleRate);
    ~WarpArrangementClock();

    [[nodiscard]] double frameAtTick(double tick) const noexcept;
    [[nodiscard]] double tickAtFrame(double frame) const noexcept;
    /// Preserve TempoMap's seconds projection where an existing caller
    /// makes its own sample-rounding decision.
    [[nodiscard]] double ticksToSeconds(std::int64_t tick) const noexcept;
    [[nodiscard]] std::int64_t secondsToTicks(double seconds) const noexcept;
    [[nodiscard]] double bpmAt(std::int64_t tick) const noexcept;
    [[nodiscard]] long double secondsAtPosition(long double tick) const noexcept;
    [[nodiscard]] long double positionAtSeconds(long double seconds) const noexcept;
    [[nodiscard]] double framesPerTickAt(double tick) const noexcept;
    /// Minimum and maximum over a half-open interval, including ramp limits.
    /// Used during owner-side source preparation.
    [[nodiscard]] std::pair<double, double> framesPerTickRange(double firstTick, double endTick) const noexcept;
    [[nodiscard]] double constantFramesPerTick() const noexcept;
    [[nodiscard]] double maxTicksPerFrame() const noexcept;

private:
    struct Segment;
    [[nodiscard]] const Segment& segmentAtTick(double tick) const noexcept;
    [[nodiscard]] const Segment& segmentAtFrame(double frame) const noexcept;
    [[nodiscard]] const Segment& segmentAtExactTick(std::int64_t tick) const noexcept;

    double sampleRate_;
    double leadInBpm_;
    double minimumBpm_ = 0;
    double maximumBpm_ = 0;
    std::vector<Segment> segments_;
};

} // namespace adi::engine
