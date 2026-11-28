// SPDX-License-Identifier: GPL-3.0-or-later
#include "play_quantize.hpp"
#include "snapshot.hpp"
#include "warp_arrangement_clock.hpp"
#include <algorithm>
#include <cmath>
#include <limits>

namespace adi::engine {
namespace {
bool validReleaseInput(std::int64_t eventSample, double gridTicks, std::int64_t forgivenessSamples) noexcept {
    return eventSample >= 0 && forgivenessSamples >= 0 && std::isfinite(gridTicks) && gridTicks > 0;
}

std::optional<std::int64_t> ceilingSample(long double sample, long double epsilon) noexcept {
    // Ignore representational noise around a whole sample.
    const auto nearest = std::round(sample);
    const auto tolerance = std::min(0.000001L, 16 * epsilon * std::max(1.0L, std::abs(sample)));
    if (std::abs(sample - nearest) <= tolerance) sample = nearest;
    sample = std::ceil(sample);
    if (!std::isfinite(sample) || sample < 0 ||
        sample >= static_cast<long double>(std::numeric_limits<std::int64_t>::max()))
        return std::nullopt;
    return static_cast<std::int64_t>(sample);
}

long double ticksPerSample(const PlayTempoPoint& point, std::uint32_t rate) noexcept {
    return static_cast<long double>(point.bpm) * playQuantizePpq / (60.0L * rate);
}
std::optional<std::int64_t> sampleForTick(long double tick,
    std::span<const PlayTempoPoint> tempo, std::uint32_t rate) noexcept {
    long double phase = 0;
    for (std::size_t i = 0; i < tempo.size(); ++i) {
        const auto speed = ticksPerSample(tempo[i], rate);
        const auto end = i + 1 < tempo.size()
            ? phase + static_cast<long double>(tempo[i+1].sample - tempo[i].sample) * speed
            : std::numeric_limits<long double>::infinity();
        if (tick <= end) {
            const auto sample = static_cast<long double>(tempo[i].sample) + (tick - phase) / speed;
            return ceilingSample(sample, std::numeric_limits<long double>::epsilon());
        }
        phase = end;
    }
    return std::nullopt;
}

template <class SampleAt>
std::optional<std::int64_t> releaseAtPhase(long double phase, long double gridTicks, std::int64_t eventSample,
                                          std::int64_t forgivenessSamples, SampleAt sampleAt) noexcept {
    const auto line = std::floor(phase / gridTicks);
    if (!std::isfinite(line)) return std::nullopt;
    const auto previous = sampleAt(line * gridTicks);
    if (!previous || *previous > eventSample) return std::nullopt;
    if (eventSample - *previous <= forgivenessSamples) return eventSample;
    const auto next = sampleAt((line + 1) * gridTicks);
    if (!next) return std::nullopt;
    return std::max(eventSample, *next);
}

template <class FrameAt>
std::optional<std::int64_t> releaseOnProjectClock(long double phase, double gridTicks, std::int64_t eventSample,
                                                std::int64_t forgivenessSamples, FrameAt frameAt) noexcept {
    constexpr long double scale = static_cast<long double>(textproj::kPPQ) / playQuantizePpq;
    return releaseAtPhase(phase, gridTicks * scale, eventSample, forgivenessSamples,
        [&](long double tick) {
            if (tick >= static_cast<long double>(std::numeric_limits<std::int64_t>::max()))
                return std::optional<std::int64_t>{};
            return ceilingSample(frameAt(tick), std::numeric_limits<double>::epsilon());
        });
}
}
std::optional<std::int64_t> playQuantizeRelease(std::int64_t eventSample,
    std::span<const PlayTempoPoint> tempo, std::uint32_t sampleRate,
    double gridTicks, std::int64_t forgivenessSamples) noexcept {
    if (!validReleaseInput(eventSample, gridTicks, forgivenessSamples) ||
        sampleRate == 0 || tempo.empty() || tempo[0].sample != 0)
        return std::nullopt;
    for (std::size_t i = 0; i < tempo.size(); ++i) {
        if (tempo[i].sample < 0 || !std::isfinite(tempo[i].bpm) || tempo[i].bpm <= 0 ||
            (i && tempo[i].sample <= tempo[i-1].sample)) return std::nullopt;
        const auto speed = ticksPerSample(tempo[i], sampleRate);
        if (!std::isfinite(speed) || speed <= 0) return std::nullopt;
    }
    long double phase = 0;
    for (std::size_t i = 0; i < tempo.size() && tempo[i].sample < eventSample; ++i) {
        const auto end = i + 1 < tempo.size()
            ? std::min(eventSample, tempo[i+1].sample) : eventSample;
        phase += static_cast<long double>(end - tempo[i].sample) * ticksPerSample(tempo[i], sampleRate);
    }
    return releaseAtPhase(phase, gridTicks, eventSample, forgivenessSamples,
        [&](long double tick) { return sampleForTick(tick, tempo, sampleRate); });
}

std::optional<std::int64_t> playQuantizeReleaseOnTempoMap(std::int64_t eventSample, const TempoMap& tempo,
    std::uint32_t sampleRate, double gridTicks, std::int64_t forgivenessSamples) noexcept {
    if (!validReleaseInput(eventSample, gridTicks, forgivenessSamples) ||
        sampleRate == 0 || tempo.events.empty() || tempo.events.front().posTicks != 0)
        return std::nullopt;
    const auto phase = tempo.positionAtSeconds(static_cast<long double>(eventSample) / sampleRate);
    return releaseOnProjectClock(phase, gridTicks, eventSample, forgivenessSamples,
        [&](long double tick) { return tempo.secondsAtPosition(tick) * sampleRate; });
}

std::optional<std::int64_t> playQuantizeReleaseOnClock(std::int64_t eventSample,
    const WarpArrangementClock& clock, double gridTicks, std::int64_t forgivenessSamples) noexcept {
    if (!validReleaseInput(eventSample, gridTicks, forgivenessSamples)) return std::nullopt;
    const auto phase = clock.tickAtFrame(static_cast<double>(eventSample));
    return releaseOnProjectClock(phase, gridTicks, eventSample, forgivenessSamples,
        [&](long double tick) { return clock.frameAtTick(static_cast<double>(tick)); });
}
} // namespace adi::engine
