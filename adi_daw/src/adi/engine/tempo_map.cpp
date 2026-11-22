// SPDX-License-Identifier: AGPL-3.0-or-later
#include "adi/engine/snapshot.hpp"

#include "adi/engine/curves.hpp"
#include "adi/store_rows.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace adi::engine {
namespace {
constexpr double ticksPerQuarter = static_cast<double>(textproj::kPPQ);
constexpr auto highestTick = std::numeric_limits<std::int64_t>::max();

TempoRampSpan spanFor(const TempoEvent& left, const TempoEvent& right) noexcept {
    return left.rampSpan.value_or(TempoRampSpan{left.posTicks, right.posTicks, left.bpm, right.bpm});
}

bool isRamp(const TempoEvent& left, const TempoEvent& right) noexcept {
    const auto span = spanFor(left, right);
    return (left.curve == 1 || left.curve == 2) && span.firstBpm != span.lastBpm;
}

long double fraction(const TempoRampSpan& span, std::int64_t tick) noexcept {
    return static_cast<long double>(tick - span.firstTick) /
           static_cast<long double>(span.lastTick - span.firstTick);
}

double tensionOf(const TempoEvent& event) noexcept {
    return event.curve == 2 && std::isfinite(event.tension) ? std::clamp(event.tension, -1.0, 1.0) : 0.0;
}

long double integral(long double start, long double change, long double shape,
                     long double x) noexcept {
    if (x <= 0) return 0;
    if (shape == 0) return std::log1p(change * x / start) / change;

    // The same quadratic Bezier as curveShape. Solve x(s) for its parameter.
    const auto edge = 1 + shape;
    const auto discriminant = std::max(0.0L, edge * edge - 4 * shape * x);
    const auto s = 2 * x / (edge + std::sqrt(discriminant));

    // P(s) is BPM. x'(s)/P(s) equals 2/P(s) - P'(s)/(change*P(s)).
    // Use the definite rational primitive of 1/P(s), with its root terms
    // combined before evaluation to preserve precision for short intervals.
    const auto b = change * (1 - shape);
    const auto c = change * shape;
    const auto determinant = 4 * start * c - b * b;
    const auto centre = 2 * start + b * s;
    long double reciprocal = 0;
    if (determinant > 0) {
        const auto root = std::sqrt(determinant);
        reciprocal = 2 * std::atan2(s * root, centre) / root;
    } else if (determinant < 0) {
        const auto root = std::sqrt(-determinant);
        reciprocal = std::log1p(2 * s * root / (centre - s * root)) / root;
    } else {
        reciprocal = 2 * s / centre;
    }
    const auto delta = s * (b + c * s) / start;
    return 2 * reciprocal - std::log1p(delta) / change;
}

double segmentSeconds(const TempoEvent& left, const TempoEvent& right, std::int64_t tick) noexcept {
    if (tick <= left.posTicks) return 0.0;
    if (!isRamp(left, right))
        return static_cast<double>(tick - left.posTicks) / ticksPerQuarter * 60.0 / left.bpm;
    const auto span = spanFor(left, right);
    const auto quarters = static_cast<long double>(span.lastTick - span.firstTick) / ticksPerQuarter;
    const long double change = static_cast<long double>(span.lastBpm) - span.firstBpm;
    const auto tension = tensionOf(left);
    const auto begin = integral(span.firstBpm, change, tension, fraction(span, left.posTicks));
    const auto end = integral(span.firstBpm, change, tension, fraction(span, tick));
    return static_cast<double>(60 * quarters * (end - begin));
}

std::int64_t stepTick(const TempoEvent& left, double elapsed, double seconds) noexcept {
    const double offset = (seconds - elapsed) * left.bpm / 60.0 * ticksPerQuarter;
    if (!(offset > 0.0)) return left.posTicks;
    if (offset >= static_cast<double>(highestTick - left.posTicks)) return highestTick;
    const auto whole = static_cast<std::int64_t>(offset);
    // Absorb the representational noise from subtraction at segment boundaries.
    const double noise = std::min(0.125, 4.0 * std::numeric_limits<double>::epsilon() *
                                           seconds * left.bpm / 60.0 * ticksPerQuarter);
    const auto next = whole + 1;
    return left.posTicks + (static_cast<double>(next) - offset <= noise ? next : whole);
}
}  // namespace

TempoMap TempoMap::fromRows(std::span<const rows::TempoEvent> rows) {
    TempoMap result;
    result.events.reserve(rows.size() + 1);
    if (rows.empty() || rows.front().posTicks != 0) result.events.push_back({});
    for (const auto& row : rows) {
        result.events.push_back({row.posTicks, row.bpm, static_cast<int>(row.curve), row.tension, row.rampSpan});
    }
    return result;
}

double TempoMap::bpmAt(std::int64_t ticks) const noexcept {
    if (events.empty()) return 120.0;
    auto left = TempoEvent{0, events.front().bpm, 0, 0};
    for (const auto& right : events) {
        if (right.posTicks > ticks) {
            if (!isRamp(left, right)) return left.bpm;
            const auto span = spanFor(left, right);
            const auto x = static_cast<double>(fraction(span, ticks));
            return curveValue(span.firstBpm, span.lastBpm, x, static_cast<std::uint8_t>(CurveShape::Bezier),
                              tensionOf(left));
        }
        left = right;
    }
    return left.bpm;
}

double TempoMap::ticksToSeconds(std::int64_t ticks) const noexcept {
    if (ticks <= 0 || events.empty()) return 0.0;
    auto left = TempoEvent{0, events.front().bpm, 0, 0};
    double seconds = 0.0;
    for (const auto& right : events) {
        if (right.posTicks >= ticks) return seconds + segmentSeconds(left, right, ticks);
        seconds += segmentSeconds(left, right, right.posTicks);
        left = right;
    }
    return seconds + static_cast<double>(ticks - left.posTicks) / ticksPerQuarter * 60.0 / left.bpm;
}

std::int64_t TempoMap::secondsToTicks(double seconds) const noexcept {
    if (!(seconds > 0.0) || !std::isfinite(seconds) || events.empty()) return 0;
    auto left = TempoEvent{0, events.front().bpm, 0, 0};
    double elapsed = 0.0;
    for (const auto& right : events) {
        const auto duration = segmentSeconds(left, right, right.posTicks);
        if (elapsed + duration >= seconds) {
            if (!isRamp(left, right)) return std::min(right.posTicks, stepTick(left, elapsed, seconds));
            const auto noise = std::min(4 * std::numeric_limits<double>::epsilon() * seconds,
                0.125 * 60.0 / (std::max(left.bpm, right.bpm) * ticksPerQuarter));
            auto low = left.posTicks;
            auto high = right.posTicks;
            while (low < high) {
                const auto middle = high - (high - low) / 2;
                if (elapsed + segmentSeconds(left, right, middle) <= seconds + noise) low = middle;
                else high = middle - 1;
            }
            return low;
        }
        elapsed += duration;
        left = right;
    }
    return stepTick(left, elapsed, seconds);
}

std::int64_t TempoMap::storedNanosecondsToTicks(std::int64_t nanos) const noexcept {
    if (nanos < 0 || events.empty()) return 0;
    const auto floor = secondsToTicks(static_cast<double>(nanos) * 1e-9);
    if (floor == highestTick) return floor;
    const auto following = static_cast<long double>(ticksToSeconds(floor + 1)) * 1e9L;
    return following >= nanos - 0.5L && following < nanos + 0.5L ? floor + 1 : floor;
}

long double TempoMap::secondsAtPosition(long double ticks) const noexcept {
    if (ticks < 0) return ticks * 60 / (ticksPerQuarter * bpmAt(0));
    if (!std::isfinite(ticks) || ticks >= static_cast<long double>(highestTick))
        return ticksToSeconds(highestTick);
    const auto anchor = static_cast<std::int64_t>(ticks);
    const auto start = static_cast<long double>(ticksToSeconds(anchor));
    if (ticks == anchor) return start;
    const auto finish = static_cast<long double>(ticksToSeconds(anchor + 1));
    return std::lerp(start, finish, ticks - anchor);
}

long double TempoMap::positionAtSeconds(long double seconds) const noexcept {
    if (seconds < 0) return seconds * (ticksPerQuarter * bpmAt(0)) / 60;
    const auto anchor = secondsToTicks(static_cast<double>(seconds));
    if (anchor == highestTick) return anchor;
    const auto start = static_cast<long double>(ticksToSeconds(anchor));
    const auto finish = static_cast<long double>(ticksToSeconds(anchor + 1));
    if (finish <= start) return anchor;
    return static_cast<long double>(anchor) + std::clamp((seconds - start) / (finish - start), 0.0L, 1.0L);
}
}  // namespace adi::engine
