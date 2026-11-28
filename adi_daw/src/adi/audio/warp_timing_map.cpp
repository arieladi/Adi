// SPDX-License-Identifier: AGPL-3.0-or-later
#include "adi/audio/warp_timing_map.hpp"
#include "adi/audio/io_audit.hpp"
#include "adi/textproj.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <functional>
#include <limits>

namespace adi::audio {
namespace {
constexpr std::size_t maximumMarkers = 1000000;
constexpr std::uint64_t maximumSourceFrames = 0x7fffffff;
constexpr std::uint32_t minimumWarpSourceRate = 8000;
constexpr std::uint32_t maximumWarpSourceRate = 768000;
constexpr std::uint64_t maximumAnalysisFrame = maximumSourceFrames * kWarpAnalysisRate / minimumWarpSourceRate;

// Phase limits and BPM preferences from Mixxx BeatUtils (GPL-2.0-or-later),
// at 902cfc2543d7ef65bda1678e6f3c3e98be4aa39d, src/track/beatutils.cpp.
// This implementation grows regions with constant-size phase summaries to
// retain bounded work on long, variable-tempo recordings.
constexpr double maxPhaseSeconds = 0.025;
constexpr double maxPhaseSumSeconds = 0.1;
constexpr std::size_t correctionLookaheadBeats = 4;

bool orderedBeats(std::span<const std::uint64_t> beats) {
    return beats.size() >= 2 && beats.size() <= maximumMarkers && beats.back() <= maximumAnalysisFrame &&
           std::adjacent_find(beats.begin(), beats.end(), std::greater_equal<>()) == beats.end();
}

double meanPeriod(std::span<const std::uint64_t> beats, std::size_t begin, std::size_t end) {
    return static_cast<double>(beats[end] - beats[begin]) / static_cast<double>(end - begin);
}

template<class Compare>
void addBound(std::array<double, 2>& limits, double next, Compare stronger) {
    if (stronger(next, limits.front())) {
        limits.back() = limits.front();
        limits.front() = next;
    } else if (stronger(next, limits.back())) {
        limits.back() = next;
    }
}

struct PhaseFit {
    explicit PhaseFit(std::uint32_t rate)
        : maxPhase(maxPhaseSeconds * rate), maxSum(maxPhaseSumSeconds * rate) {}

    void observe(std::size_t beat, double elapsed) {
        const auto count = static_cast<double>(beat);
        addBound(lower, (elapsed - maxPhase) / count, std::greater<>());
        addBound(upper, (elapsed + maxPhase) / count, std::less<>());
        if (beat == 1) firstPeriod = elapsed;
        elapsedSum += elapsed;
    }

    bool accepts(std::size_t beats, double mean, double lastPeriod) const {
        if (std::abs(firstPeriod - mean) > maxPhase || mean < lower.back() || mean > upper.back())
            return false;
        // The two tightest bounds in each direction admit one outlier. A
        // period cannot be above and below the same beat's allowed range.
        if (mean < lower.front() && mean > upper.front())
            return false;
        const auto count = static_cast<double>(beats);
        const auto signedPhaseSum = mean * count * (count + 1) / 2 - elapsedSum;
        if (std::abs(signedPhaseSum) > maxSum) return false;
        return beats < 3 || std::abs(firstPeriod + lastPeriod - 2 * mean) < maxPhase / 2;
    }

    const double maxPhase, maxSum;
    double firstPeriod = 0, elapsedSum = 0;
    static constexpr double infinity = std::numeric_limits<double>::infinity();
    std::array<double, 2> lower{-infinity, -infinity};
    std::array<double, 2> upper{infinity, infinity};
};

double snappedBpm(double period, std::size_t intervals, std::uint32_t rate) {
    const double framesPerMinute = 60.0 * rate;
    const auto tolerance = maxPhaseSeconds * rate / static_cast<double>(intervals);
    const auto bpm = framesPerMinute / period;
    const auto min = framesPerMinute / (period + tolerance);
    const auto max = period > tolerance ? framesPerMinute / (period - tolerance) : PhaseFit::infinity;
    // Prefer whole beats per minute, then the customary half/double and
    // 3:2 families, then the finest supported musical fraction.
    const std::array factors{1.0, bpm < 85.0 ? 2.0 : 1.0, bpm > 127.0 ? 2.0 / 3.0 : 1.0, 3.0, 12.0};
    for (const double factor : factors) {
        const auto candidate = std::round(bpm * factor) / factor;
        if (candidate > min && candidate < max) return candidate;
    }
    return bpm;
}

bool validDownbeats(std::span<const std::size_t> indices, std::size_t beatCount) {
    return indices.empty() || (indices.size() <= beatCount && indices.back() < beatCount &&
        std::adjacent_find(indices.begin(), indices.end(), std::greater_equal<>()) == indices.end());
}

bool validRegions(std::span<const WarpTempoRegion> regions, std::size_t lastBeat) {
    if (regions.empty()) return true;
    return regions.size() <= lastBeat && regions.front().firstBeat == 0 && regions.back().firstBeat < lastBeat &&
        std::ranges::all_of(regions, [](const auto& region) { return std::isfinite(region.bpm) && region.bpm > 0.0; }) &&
        std::adjacent_find(regions.begin(), regions.end(), [](const auto& a, const auto& b) {
            return a.firstBeat >= b.firstBeat;
        }) == regions.end();
}

std::uint64_t toSourceFrame(std::uint64_t canonical, std::uint32_t sourceRate) {
    return (canonical * sourceRate + kWarpAnalysisRate / 2) / kWarpAnalysisRate;
}

std::uint64_t ironedBeat(std::span<const std::uint64_t> beats, std::size_t index, std::size_t begin, std::size_t end) {
    // Evaluate the endpoint slope as a rational so all source rates receive
    // the same canonical position. Boundaries keep their original frames.
    if (index == begin || index == end) return beats[index];
    const auto intervals = static_cast<std::uint64_t>(end - begin);
    const auto elapsed = (beats[end] - beats[begin]) * static_cast<std::uint64_t>(index - begin);
    return beats[begin] + (elapsed + intervals / 2) / intervals;
}

std::int64_t elapsedTicks(long double canonicalFrames, std::span<const std::uint64_t> beats,
                          std::size_t begin, std::size_t end) {
    // The input bounds keep this below INT64_MAX even for a one-frame beat
    // interval at an extreme source rate.
    const auto span = static_cast<long double>(beats[end] - beats[begin]);
    const auto intervals = static_cast<long double>(end - begin);
    return std::llround(canonicalFrames * intervals * textproj::kPPQ / span);
}

} // namespace

std::vector<WarpTempoRegion> warpTempoRegions(std::span<const std::uint64_t> beats, std::uint32_t rate,
                                            std::stop_token stop) {
    if (onAudioThread || stop.stop_requested() || rate == 0 || rate > maximumWarpSourceRate || !orderedBeats(beats))
        return {};
    std::vector<WarpTempoRegion> result;
    for (std::size_t start = 0; start + 1 < beats.size();) {
        if (stop.stop_requested()) return {};
        PhaseFit fit(rate);
        std::size_t accepted = start + 1;
        for (auto end = start + 1; end < beats.size(); ++end) {
            if (end % 256 == 0 && stop.stop_requested()) return {};
            const auto elapsed = static_cast<double>(beats[end] - beats[start]);
            const auto count = end - start;
            fit.observe(count, elapsed);
            if (fit.accepts(count, elapsed / static_cast<double>(count), static_cast<double>(beats[end] - beats[end - 1])))
                accepted = end;
            // A single correction beat can temporarily bend the endpoint
            // estimate. Look past it without restarting the region search.
            if (end - accepted == correctionLookaheadBeats) break;
        }
        // Equal snapped tempi can still have a measured phase shift between
        // them, so keep each boundary for the marker builder.
        const auto bpm = snappedBpm(meanPeriod(beats, start, accepted), accepted - start, rate);
        result.push_back({start, bpm});
        start = accepted;
    }
    return result;
}

std::optional<WarpTimingMap> buildWarpTimingMap(
    std::span<const std::uint64_t> beats, std::span<const std::size_t> downbeats,
    std::span<const WarpTempoRegion> regions, std::uint64_t sourceFrames,
    std::uint32_t sourceRate, bool fromFirstDownbeat, std::stop_token stop) {
    if (onAudioThread || stop.stop_requested() || sourceFrames == 0 || sourceFrames > maximumSourceFrames ||
        sourceRate < minimumWarpSourceRate || sourceRate > maximumWarpSourceRate || !orderedBeats(beats) ||
        !validDownbeats(downbeats, beats.size()) || !validRegions(regions, beats.size() - 1) ||
        (fromFirstDownbeat && downbeats.empty())) return {};

    const auto scaledEnd = sourceFrames * kWarpAnalysisRate;
    const auto scaledLast = beats.back() * sourceRate;
    const auto firstBeat = fromFirstDownbeat ? downbeats.front() : 0;
    const auto lastBeat = beats.size() - 1;
    if (firstBeat >= lastBeat || beats.back() >= scaledEnd / sourceRate ||
        toSourceFrame(beats.back(), sourceRate) >= sourceFrames) return {};

    WarpTimingMap map;
    map.trimFrames = fromFirstDownbeat ? toSourceFrame(beats[firstBeat], sourceRate) : 0;
    map.markers.reserve(std::min(maximumMarkers, downbeats.size() + regions.size() + 3));
    map.markers.push_back({0, 0});
    const auto firstRegionEnd = regions.size() > 1 ? regions[1].firstBeat : lastBeat;
    const auto leadTicks = fromFirstDownbeat ? 0 : elapsedTicks(static_cast<long double>(beats.front()), beats, 0, firstRegionEnd);
    std::size_t activeRegion = 0, nextDownbeat = 0;
    for (std::size_t index = 0; index < beats.size(); ++index) {
        if (index % 256 == 0 && stop.stop_requested()) return {};
        const bool downbeat = nextDownbeat < downbeats.size() && downbeats[nextDownbeat] == index;
        if (downbeat) ++nextDownbeat;
        const bool regionChange = activeRegion + 1 < regions.size() && regions[activeRegion + 1].firstBeat == index;
        if (regionChange) ++activeRegion;
        if (index < firstBeat || (!downbeat && !regionChange && index != firstBeat)) continue;

        const auto regionStart = regions.empty() ? 0 : regions[activeRegion].firstBeat;
        const auto regionEnd = activeRegion + 1 < regions.size() ? regions[activeRegion + 1].firstBeat : lastBeat;
        const auto canonical = ironedBeat(beats, index, std::max(firstBeat, regionStart), regionEnd);
        const auto source = toSourceFrame(canonical, sourceRate) - map.trimFrames;
        const auto tick = leadTicks + static_cast<std::int64_t>(index - firstBeat) * textproj::kPPQ;
        if (source == 0 && tick == 0) continue;
        if (source <= static_cast<std::uint64_t>(map.markers.back().sourceFrame) || tick <= map.markers.back().tick ||
            map.markers.size() >= maximumMarkers - 1) return {};
        map.markers.push_back({static_cast<std::int64_t>(source), tick});
    }

    const auto tailRegionStart = std::max(firstBeat, regions.empty() ? 0 : regions.back().firstBeat);
    const auto tailCanonicalFrames = static_cast<long double>(scaledEnd - scaledLast) / sourceRate;
    const auto tailTicks = elapsedTicks(tailCanonicalFrames, beats, tailRegionStart, lastBeat);
    const auto endTick = leadTicks + static_cast<std::int64_t>(lastBeat - firstBeat) * textproj::kPPQ + tailTicks;
    if (endTick <= map.markers.back().tick) return {};
    map.markers.push_back({static_cast<std::int64_t>(sourceFrames - map.trimFrames), endTick});
    return map;
}

} // namespace adi::audio
