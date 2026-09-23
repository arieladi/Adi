// SPDX-License-Identifier: GPL-3.0-or-later
#include "play_quantize.hpp"
#include <algorithm>
#include <cmath>
#include <limits>

namespace adi::engine {
namespace {
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
            auto sample = static_cast<long double>(tempo[i].sample) + (tick - phase) / speed;
            // Suppress roundoff at integral sample boundaries, not real fractional
            // samples. Cap tolerance well below a sample even at long timelines.
            const auto nearest = std::round(sample);
            const auto tolerance = std::min(0.000001L,
                16 * std::numeric_limits<long double>::epsilon() * std::max(1.0L, std::abs(sample)));
            if (std::abs(sample - nearest) <= tolerance) sample = nearest;
            sample = std::ceil(sample);
            if (!std::isfinite(sample) || sample < 0 ||
                sample >= static_cast<long double>(std::numeric_limits<std::int64_t>::max()))
                return std::nullopt;
            return static_cast<std::int64_t>(sample);
        }
        phase = end;
    }
    return std::nullopt;
}
}
std::optional<std::int64_t> playQuantizeRelease(std::int64_t eventSample,
    std::span<const PlayTempoPoint> tempo, std::uint32_t sampleRate,
    double gridTicks, std::int64_t forgivenessSamples) noexcept {
    if (eventSample < 0 || forgivenessSamples < 0 || sampleRate == 0 ||
        !std::isfinite(gridTicks) || gridTicks <= 0 || tempo.empty() || tempo[0].sample != 0)
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
    const auto line = std::floor(phase / gridTicks);
    if (!std::isfinite(line)) return std::nullopt;
    const auto previous = sampleForTick(line * gridTicks, tempo, sampleRate);
    if (!previous || *previous > eventSample) return std::nullopt;
    if (eventSample - *previous <= forgivenessSamples) return eventSample;
    const auto next = sampleForTick((line + 1) * gridTicks, tempo, sampleRate);
    if (!next) return std::nullopt;
    // A grid line rounded upward to this sample is also "now".
    return std::max(eventSample, *next);
}
} // namespace adi::engine
