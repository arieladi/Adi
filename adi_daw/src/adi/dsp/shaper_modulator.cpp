// SPDX-License-Identifier: GPL-3.0-or-later
#include "adi/dsp/shaper_modulator.hpp"

#include <algorithm>
#include <cmath>

namespace adi::dsp {
namespace {
constexpr std::uint64_t kJitterSalt = 0xD6E8FEB86659FD93ull;

/// A segment's shape at u in [0, 1]: a straight line at curve 0, an
/// exponential of 8 times the curve otherwise.
double curved(double u, double curve) noexcept {
    if (curve == 0) {
        return u;
    }
    const double k = 8 * curve;
    return std::expm1(k * u) / std::expm1(k);
}
}  // namespace

ShaperModulator::ShaperModulator() noexcept {
    for (std::size_t i = 0; i < Count; ++i) {
        p_[i] = ranges[i].initial;
    }
    // ADI's default until reading 11: a triangle through 0, 1 and -1.
    const Point initial[] = {{0, 0, 0}, {0.25, 1, 0}, {0.75, -1, 0}};
    (void)setPoints(initial, 3);
    prepare(48000);
}

void ShaperModulator::prepare(double sampleRate) noexcept {
    rate_ = std::isfinite(sampleRate) && sampleRate > 0 ? sampleRate : 48000;
    clock_.prepare(rate_);
    smoothed_ = 0;
    primed_ = false;
    update();
}

void ShaperModulator::set(Param p, double v) noexcept {
    const int index = static_cast<int>(p);
    if (index < 0 || index >= static_cast<int>(Count) || !std::isfinite(v)) {
        return;
    }
    const auto& r = ranges[static_cast<std::size_t>(index)];
    p_[static_cast<std::size_t>(index)] = std::clamp(v, r.low, r.high);
    update();
}

void ShaperModulator::update() noexcept {
    const double seconds = 0.05 * p_[Smooth];
    smoothing_ = seconds > 0 ? 1 - std::exp(-1 / (seconds * rate_)) : 1;
}

bool ShaperModulator::setPoints(const Point* points, std::size_t count) noexcept {
    std::array<Point, kMaxPoints> next{};
    std::size_t n = 0;
    for (std::size_t i = 0; points != nullptr && i < count && n < kMaxPoints; ++i) {
        const auto& p = points[i];
        if (!std::isfinite(p.time) || !std::isfinite(p.value) || !std::isfinite(p.curve)) {
            continue;
        }
        next[n++] = {std::clamp(p.time, 0.0, std::nextafter(1.0, 0.0)), std::clamp(p.value, -1.0, 1.0),
                     std::clamp(p.curve, -1.0, 1.0)};
    }
    if (n == 0) {
        return false;
    }
    // By time, keeping the given order of points at one time (a step): an
    // insertion sort in place (std::stable_sort may allocate a buffer).
    for (std::size_t i = 1; i < n; ++i) {
        const Point moving = next[i];
        std::size_t j = i;
        while (j > 0 && next[j - 1].time > moving.time) {
            next[j] = next[j - 1];
            --j;
        }
        next[j] = moving;
    }
    points_ = next;
    count_ = n;
    return true;
}

double ShaperModulator::shapeAt(double phase) const noexcept {
    if (count_ == 0) {
        return 0;
    }
    if (count_ == 1) {
        return points_[0].value;
    }
    // The segment that holds the phase: from the last point at or before it
    // to the next point, the last point's running into the next cycle.
    std::size_t from = count_ - 1;
    for (std::size_t i = 0; i < count_; ++i) {
        if (points_[i].time <= phase) {
            from = i;
        } else {
            break;
        }
    }
    const bool wraps = from == count_ - 1 || points_[0].time > phase;
    const Point& a = points_[from];
    const Point& b = wraps ? points_[0] : points_[from + 1];
    double start = a.time, end = b.time, at = phase;
    if (wraps) {
        end += 1;
        if (at < start) {
            at += 1;   // before the first point: still the last point's segment
        }
    }
    const double span = end - start;
    const double u = span > 0 ? std::clamp((at - start) / span, 0.0, 1.0) : 1.0;
    return a.value + (b.value - a.value) * curved(u, a.curve);
}

void ShaperModulator::render(double* out, std::size_t frames, const Position& position) noexcept {
    if (out == nullptr) {
        return;
    }
    const bool synced = p_[Sync] >= 0.5;
    const double offset = p_[Phase] / 360;
    const auto choice = static_cast<std::size_t>(std::clamp<long>(std::lround(p_[Division]), 0, 11));
    const double quarters = kDivisionQuarters[choice];
    for (std::size_t i = 0; i < frames; ++i) {
        double cycles = clock_.cycles(i, synced, quarters, position) + offset;
        auto cycle = static_cast<std::int64_t>(std::floor(cycles));
        if (p_[Jitter] > 0) {
            cycles += 0.5 * p_[Jitter] * ModulatorClock::hash(cycle, kJitterSalt);
            cycle = static_cast<std::int64_t>(std::floor(cycles));
        }
        const double raw = shapeAt(cycles - static_cast<double>(cycle));
        if (!primed_) {
            smoothed_ = raw;
            primed_ = true;
        }
        smoothed_ += (raw - smoothed_) * smoothing_;
        if (std::abs(raw - smoothed_) < 1e-12) {
            smoothed_ = raw;   // settles exactly on its target, never into subnormals
        }
        out[i] = p_[Offset] + p_[Depth] * smoothed_;
        clock_.step(synced, p_[Rate]);
    }
}

}  // namespace adi::dsp
