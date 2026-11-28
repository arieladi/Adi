// SPDX-License-Identifier: AGPL-3.0-or-later
#include "adi/engine/snapshot.hpp"

#include "adi/engine/curves.hpp"
#include "adi/engine/warp_arrangement_clock.hpp"
#include "adi/store_rows.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <numeric>

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

long double fraction(const TempoRampSpan& span, long double tick) noexcept {
    return (tick - static_cast<long double>(span.firstTick)) /
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

long double rampSeconds(const TempoEvent& left, const TempoEvent& right, long double tick) noexcept {
    const auto span = spanFor(left, right);
    const auto quarters = static_cast<long double>(span.lastTick - span.firstTick) / ticksPerQuarter;
    const long double change = static_cast<long double>(span.lastBpm) - span.firstBpm;
    const auto tension = tensionOf(left);
    const auto begin = integral(span.firstBpm, change, tension,
                                fraction(span, static_cast<long double>(left.posTicks)));
    const auto end = integral(span.firstBpm, change, tension, fraction(span, tick));
    return 60 * quarters * (end - begin);
}

double segmentSeconds(const TempoEvent& left, const TempoEvent& right, std::int64_t tick) noexcept {
    if (tick <= left.posTicks) return 0.0;
    if (!isRamp(left, right))
        return static_cast<double>(tick - left.posTicks) / ticksPerQuarter * 60.0 / left.bpm;
    return static_cast<double>(rampSeconds(left, right, static_cast<long double>(tick)));
}

double segmentBpm(const TempoEvent& left, const TempoEvent& right, long double tick) noexcept {
    if (!isRamp(left, right)) return left.bpm;
    const auto span = spanFor(left, right);
    const auto x = static_cast<double>(fraction(span, tick));
    return curveValue(span.firstBpm, span.lastBpm, x, static_cast<std::uint8_t>(CurveShape::Bezier),
                      tensionOf(left));
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

std::int64_t segmentTick(const TempoEvent& left, const TempoEvent& right,
                         double elapsed, double seconds) noexcept {
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

std::int64_t recoverStoredTick(std::int64_t nanos, const auto& clock) noexcept {
    if (nanos < 0) return 0;
    const auto floor = clock.secondsToTicks(static_cast<double>(nanos) * 1e-9);
    if (floor == highestTick) return floor;
    const auto following = static_cast<long double>(clock.ticksToSeconds(floor + 1)) * 1e9L;
    const auto requested = static_cast<long double>(nanos);
    return following >= requested - 0.5L && following < requested + 0.5L ? floor + 1 : floor;
}

long double interpolateTickSeconds(long double ticks, const auto& secondsAtTick) noexcept {
    if (!std::isfinite(ticks) || ticks >= static_cast<long double>(highestTick))
        return secondsAtTick(highestTick);
    const auto anchor = static_cast<std::int64_t>(ticks);
    const auto start = static_cast<long double>(secondsAtTick(anchor));
    if (ticks == static_cast<long double>(anchor)) return start;
    const auto finish = static_cast<long double>(secondsAtTick(anchor + 1));
    return std::lerp(start, finish, ticks - static_cast<long double>(anchor));
}

long double interpolateSecondsPosition(long double seconds, const auto& ticksAtSeconds,
                                       const auto& secondsAtTick) noexcept {
    const auto anchor = ticksAtSeconds(static_cast<double>(seconds));
    if (anchor == highestTick) return static_cast<long double>(anchor);
    const auto start = static_cast<long double>(secondsAtTick(anchor));
    const auto finish = static_cast<long double>(secondsAtTick(anchor + 1));
    if (finish <= start) return static_cast<long double>(anchor);
    return static_cast<long double>(anchor) + std::clamp((seconds - start) / (finish - start), 0.0L, 1.0L);
}
}  // namespace

TempoMap TempoMap::fromRows(std::span<const rows::TempoEvent> rows) {
    TempoMap result;
    result.events.reserve(rows.size() + 1);
    if (rows.empty()) result.events.push_back({});
    else if (rows.front().posTicks != 0) {
        // Keep the playback lead-in for old projects without a tick-zero row.
        // The first event's outgoing curve still begins at its authored tick.
        result.events.push_back({0, rows.front().bpm, 0, 0});
    }
    for (const auto& row : rows) {
        result.events.push_back({row.posTicks, row.bpm, static_cast<int>(row.curve), row.tension, row.rampSpan});
    }
    return result;
}

double TempoMap::bpmAt(std::int64_t ticks) const noexcept {
    if (events.empty()) return 120.0;
    auto left = TempoEvent{0, events.front().bpm, 0, 0};
    // The driver reads these conversions. MSVC Debug vector iterators lock
    // their ownership proxy, so traverse the immutable events by index.
    for (std::size_t i = 0; i < events.size(); ++i) {
        const auto& right = events[i];
        if (right.posTicks > ticks) return segmentBpm(left, right, static_cast<long double>(ticks));
        left = right;
    }
    return left.bpm;
}

double TempoMap::ticksToSeconds(std::int64_t ticks) const noexcept {
    if (ticks <= 0 || events.empty()) return 0.0;
    auto left = TempoEvent{0, events.front().bpm, 0, 0};
    double seconds = 0.0;
    for (std::size_t i = 0; i < events.size(); ++i) {
        const auto& right = events[i];
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
    for (std::size_t i = 0; i < events.size(); ++i) {
        const auto& right = events[i];
        const auto duration = segmentSeconds(left, right, right.posTicks);
        if (elapsed + duration >= seconds) return segmentTick(left,right,elapsed,seconds);
        elapsed += duration;
        left = right;
    }
    return stepTick(left, elapsed, seconds);
}

std::int64_t TempoMap::storedNanosecondsToTicks(std::int64_t nanos) const noexcept {
    return events.empty() ? 0 : recoverStoredTick(nanos,*this);
}

long double TempoMap::secondsAtPosition(long double ticks) const noexcept {
    if (ticks < 0) return ticks * 60 / (ticksPerQuarter * bpmAt(0));
    return interpolateTickSeconds(ticks,[this](std::int64_t tick) { return ticksToSeconds(tick); });
}

long double TempoMap::positionAtSeconds(long double seconds) const noexcept {
    if (seconds < 0) return seconds * (ticksPerQuarter * bpmAt(0)) / 60;
    return interpolateSecondsPosition(seconds,[this](double value) { return secondsToTicks(value); },
        [this](std::int64_t tick) { return ticksToSeconds(tick); });
}

struct WarpArrangementClock::Segment {
    TempoEvent left, right;
    double startSeconds, startFrame, endFrame;

    [[nodiscard]] long double secondsAtTick(long double tick) const noexcept {
        if (isRamp(left, right)) return rampSeconds(left, right, tick);
        return (tick - static_cast<long double>(left.posTicks)) * 60 / (ticksPerQuarter * left.bpm);
    }
};

WarpArrangementClock::WarpArrangementClock(std::span<const rows::TempoEvent> rows, double sampleRate)
    : WarpArrangementClock(TempoMap::fromRows(rows), sampleRate) {}

WarpArrangementClock::WarpArrangementClock(const TempoMap& tempo, double sampleRate)
    : sampleRate_(sampleRate), leadInBpm_(tempo.events.empty() ? 120.0 : tempo.events.front().bpm),
      minimumBpm_(tempo.bpmAt(0)), maximumBpm_(minimumBpm_) {
    segments_.reserve(tempo.events.size() + 1);
    auto left = TempoEvent{0, leadInBpm_, 0, 0};
    double seconds = 0;
    for (const auto& right : tempo.events) {
        if (right.posTicks > left.posTicks) {
            const double duration = segmentSeconds(left, right, right.posTicks);
            const double first = segmentBpm(left, right, static_cast<long double>(left.posTicks));
            const double last = segmentBpm(left, right, static_cast<long double>(right.posTicks));
            minimumBpm_ = std::min({minimumBpm_, first, last});
            maximumBpm_ = std::max({maximumBpm_, first, last});
            segments_.push_back({left, right, seconds, seconds * sampleRate_, (seconds + duration) * sampleRate_});
            seconds += duration;
        }
        left = right;
    }
    minimumBpm_ = std::min(minimumBpm_, left.bpm);
    maximumBpm_ = std::max(maximumBpm_, left.bpm);
    // The final point holds its BPM. Discard any performance-only curve span
    // on that point since it has no outgoing destination.
    left.curve = 0;
    segments_.push_back({left, left, seconds, seconds * sampleRate_, std::numeric_limits<double>::infinity()});
}

WarpArrangementClock::~WarpArrangementClock() = default;

const WarpArrangementClock::Segment& WarpArrangementClock::segmentAtTick(double tick) const noexcept {
    const auto* first = segments_.data();
    const auto* next = std::upper_bound(first, first + segments_.size(), tick,
        [](double value, const Segment& segment) { return value < static_cast<double>(segment.left.posTicks); });
    return next == first ? *first : *(next - 1);
}

const WarpArrangementClock::Segment& WarpArrangementClock::segmentAtFrame(double frame) const noexcept {
    const auto* first = segments_.data();
    const auto* next = std::upper_bound(first, first + segments_.size(), frame,
        [](double value, const Segment& segment) { return value < segment.startFrame; });
    return next == first ? *first : *(next - 1);
}

const WarpArrangementClock::Segment& WarpArrangementClock::segmentAtExactTick(std::int64_t tick) const noexcept {
    const auto* first = segments_.data();
    const auto* next = std::upper_bound(first,first + segments_.size(),tick,
        [](std::int64_t value, const Segment& segment) { return value < segment.left.posTicks; });
    return next == first ? *first : *(next - 1);
}

double WarpArrangementClock::ticksToSeconds(std::int64_t tick) const noexcept {
    if (tick <= 0) return 0;
    const auto& segment = segmentAtExactTick(tick);
    return segment.startSeconds + segmentSeconds(segment.left,segment.right,tick);
}

std::int64_t WarpArrangementClock::secondsToTicks(double seconds) const noexcept {
    if (!(seconds > 0) || !std::isfinite(seconds)) return 0;
    const auto* first = segments_.data();
    const auto* end = first + segments_.size();
    const auto* boundary = std::lower_bound(first+1,end,seconds,
        [](const Segment& segment, double value) { return segment.startSeconds < value; });
    const auto& segment = *(boundary - 1);
    if (boundary == end) return stepTick(segment.left,segment.startSeconds,seconds);
    return segmentTick(segment.left,segment.right,segment.startSeconds,seconds);
}

std::int64_t WarpArrangementClock::storedNanosecondsToTicks(std::int64_t nanos) const noexcept {
    return recoverStoredTick(nanos,*this);
}

double WarpArrangementClock::bpmAt(std::int64_t tick) const noexcept {
    if (tick < 0) return leadInBpm_;
    const auto& segment = segmentAtExactTick(tick);
    return segmentBpm(segment.left,segment.right,static_cast<long double>(tick));
}

long double WarpArrangementClock::secondsAtPosition(long double ticks) const noexcept {
    if (ticks < 0) return ticks * 60 / (ticksPerQuarter * bpmAt(0));
    return interpolateTickSeconds(ticks,[this](std::int64_t tick) { return ticksToSeconds(tick); });
}

long double WarpArrangementClock::positionAtSeconds(long double seconds) const noexcept {
    if (seconds < 0) return seconds * (ticksPerQuarter * bpmAt(0)) / 60;
    return interpolateSecondsPosition(seconds,[this](double value) { return secondsToTicks(value); },
        [this](std::int64_t tick) { return ticksToSeconds(tick); });
}

double WarpArrangementClock::frameAtTick(double tick) const noexcept {
    if (tick < 0) return tick * framesPerTickAt(0);
    const auto& segment = segmentAtTick(tick);
    const long double frame = segment.startFrame + segment.secondsAtTick(tick) * sampleRate_;
    return static_cast<double>(frame);
}

double WarpArrangementClock::tickAtFrame(double frame) const noexcept {
    if (frame < 0) return frame / framesPerTickAt(0);
    const auto& segment = segmentAtFrame(frame);
    const auto& left = segment.left;
    const auto& right = segment.right;
    const double startTick = static_cast<double>(left.posTicks);
    const double elapsed = (frame - segment.startFrame) / sampleRate_;
    if (!isRamp(left, right)) return startTick + elapsed * left.bpm * ticksPerQuarter / 60;

    const auto span = spanFor(left, right);
    const double tension = tensionOf(left);
    const double firstBpm = segmentBpm(left, right, static_cast<long double>(left.posTicks));
    if (tension == 0) {
        const double slope = (span.lastBpm - span.firstBpm) / static_cast<double>(span.lastTick - span.firstTick);
        return startTick + firstBpm / slope * std::expm1(slope * elapsed * ticksPerQuarter / 60);
    }

    // Newton steps use the derivative of the same integral as placement.
    // Keep a bracket so even the steep endpoint of a full-tension curve
    // converges without unbounded callback work or per-sample state.
    double low = startTick;
    double high = static_cast<double>(right.posTicks);
    double tick = std::lerp(low, high, (frame - segment.startFrame) / (segment.endFrame - segment.startFrame));
    for (int i = 0; i < 48; ++i) {
        const double error = static_cast<double>(segment.secondsAtTick(tick)) - elapsed;
        if (std::abs(error * sampleRate_) < 1.0e-8) break;
        if (error > 0) high = tick;
        else low = tick;
        const double bpm = segmentBpm(left, right, tick);
        const double next = tick - error * bpm * ticksPerQuarter / 60;
        const auto candidate = next > low && next < high ? next : std::midpoint(low, high);
        if (candidate == tick) break;
        tick = candidate;
    }
    return tick;
}

double WarpArrangementClock::framesPerTickAt(double tick) const noexcept {
    const auto& segment = segmentAtTick(tick);
    const double bpm = segmentBpm(segment.left, segment.right, tick);
    return sampleRate_ * 60 / (ticksPerQuarter * bpm);
}

std::pair<double, double> WarpArrangementClock::framesPerTickRange(double firstTick, double endTick) const noexcept {
    double minimum = framesPerTickAt(firstTick), maximum = minimum;
    const auto* end = segments_.data() + segments_.size();
    for (const auto* segment = &segmentAtTick(firstTick); segment != end; ++segment) {
        const auto first = std::max(firstTick, static_cast<double>(segment->left.posTicks));
        if (first >= endTick) break;
        const auto next = segment + 1;
        const auto last = next == end ? endTick : std::min(endTick, static_cast<double>(next->left.posTicks));
        // The shared linear and Bezier tempo curves are monotone on each
        // segment, so the clipped endpoints bound every intermediate rate.
        for (const auto tick : {first, last}) {
            const auto bpm = segmentBpm(segment->left, segment->right, tick);
            const auto framesPerTick = sampleRate_ * 60 / (ticksPerQuarter * bpm);
            minimum = std::min(minimum, framesPerTick);
            maximum = std::max(maximum, framesPerTick);
        }
    }
    return {minimum, maximum};
}

double WarpArrangementClock::constantFramesPerTick() const noexcept {
    return minimumBpm_ == maximumBpm_ ? framesPerTickAt(0) : 0;
}

double WarpArrangementClock::maxTicksPerFrame() const noexcept {
    return maximumBpm_ * ticksPerQuarter / (sampleRate_ * 60);
}

double WarpArrangementClock::minTicksPerFrame() const noexcept {
    return minimumBpm_ * ticksPerQuarter / (sampleRate_ * 60);
}

}  // namespace adi::engine
