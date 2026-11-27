// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include "adi/store_rows.hpp"
#include "adi/textproj.hpp"
#include <algorithm>
#include <cmath>
#include <limits>
#include <span>
#include <vector>

namespace adi::engine {

// Compiled owner-side clock for a streaming Warp source. The inverse retains
// fractional ticks, so resampling follows the integrated tempo at each frame.
// Linear tempo means BPM linear in musical position, not in elapsed seconds.
class WarpArrangementClock {
public:
    WarpArrangementClock(std::span<const rows::TempoEvent> events, double sampleRate)
        : framesPerTickAtOneBpm_(sampleRate * 60.0 / textproj::kPPQ) {
        segments_.reserve(events.size() + 1);
        if (events.empty() || events.front().posTicks > 0) {
            const auto bpm = events.empty() ? 120.0 : events.front().bpm;
            segments_.push_back({0, 0, std::isfinite(bpm) && bpm > 0 ? bpm : 120.0, 0});
        }
        for (std::size_t i=0; i<events.size(); ++i) {
            const auto& event = events[i];
            if (event.posTicks < 0 || !std::isfinite(event.bpm) || event.bpm <= 0)
                continue;
            const double tick = static_cast<double>(event.posTicks);
            const double frame = segments_.empty() ? 0 : frameAtTick(tick);
            double slope = 0;
            if (event.curve == 1 && i+1 < events.size() && events[i+1].posTicks > event.posTicks &&
                std::isfinite(events[i+1].bpm) && events[i+1].bpm > 0) {
                slope = (events[i+1].bpm - event.bpm) /
                    static_cast<double>(events[i+1].posTicks - event.posTicks);
                hasLinearRamp_ = hasLinearRamp_ || slope != 0;
            }
            if (!segments_.empty() && segments_.back().tick == tick)
                segments_.back() = {tick, frame, event.bpm, slope};
            else
                segments_.push_back({tick, frame, event.bpm, slope});
        }
        if (segments_.empty()) segments_.push_back({0, 0, 120, 0});
        for (const auto& segment : segments_) {
            minimumBpm_ = std::min(minimumBpm_, segment.bpm);
            maximumBpm_ = std::max(maximumBpm_, segment.bpm);
        }
    }

    [[nodiscard]] double frameAtTick(double tick) const noexcept {
        const auto* first = segments_.data();
        const auto* next = std::upper_bound(first, first + segments_.size(), tick,
            [](double value, const Segment& segment) { return value < segment.tick; });
        const auto& s = next == first ? *first : *(next-1);
        const double offset = tick - s.tick;
        if (s.slope == 0 || offset <= 0)
            return s.frame + framesPerTickAtOneBpm_ * offset / s.bpm;
        return s.frame + framesPerTickAtOneBpm_ / s.slope * std::log1p(s.slope * offset / s.bpm);
    }

    [[nodiscard]] double tickAtFrame(double frame) const noexcept {
        const auto* first = segments_.data();
        const auto* next = std::upper_bound(first, first + segments_.size(), frame,
            [](double value, const Segment& segment) { return value < segment.frame; });
        const auto& s = next == first ? *first : *(next-1);
        const double offset = frame - s.frame;
        if (s.slope == 0 || offset <= 0)
            return s.tick + s.bpm * offset / framesPerTickAtOneBpm_;
        return s.tick + s.bpm / s.slope * std::expm1(s.slope * offset / framesPerTickAtOneBpm_);
    }

    [[nodiscard]] double constantFramesPerTick() const noexcept {
        return segments_.size() == 1 ? framesPerTickAtOneBpm_ / segments_.front().bpm : 0;
    }

    [[nodiscard]] double maxTicksPerFrame() const noexcept {
        return maximumBpm_ / framesPerTickAtOneBpm_;
    }

    [[nodiscard]] double minTicksPerFrame() const noexcept {
        return minimumBpm_ / framesPerTickAtOneBpm_;
    }

    [[nodiscard]] bool hasLinearRamp() const noexcept { return hasLinearRamp_; }

    [[nodiscard]] double framesPerTickAt(double tick) const noexcept {
        const auto* first = segments_.data();
        const auto* next = std::upper_bound(first, first + segments_.size(), tick,
            [](double value, const Segment& segment) { return value < segment.tick; });
        const auto& s = next == first ? *first : *(next-1);
        return framesPerTickAtOneBpm_ / (s.bpm + s.slope * std::max(0.0, tick-s.tick));
    }

private:
    struct Segment { double tick, frame, bpm, slope; };
    double framesPerTickAtOneBpm_;
    double minimumBpm_ = std::numeric_limits<double>::infinity();
    double maximumBpm_ = 0;
    bool hasLinearRamp_ = false;
    std::vector<Segment> segments_;
};

} // namespace adi::engine
