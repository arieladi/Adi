// SPDX-License-Identifier: AGPL-3.0-or-later
#include "adi/audio/warp_source_analysis.hpp"
#include "adi/audio/io_audit.hpp"
#include "adi/audio/warp_beat_candidate.hpp"
#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

namespace adi::audio {
std::optional<WarpTimingAnalysis> analyzeWarpTiming(WavReader& source,
    const std::string& contentHash, WarpTimingSettings settings, std::stop_token stop,
    media::AnalysisCache* cache) {
    if (onAudioThread || stop.stop_requested() || source.frames() == 0 || source.sampleRate() < 8000 ||
        source.sampleRate() > 768000 || source.channels() > 64 || settings.numerator < 2 ||
        settings.numerator > 16 || settings.sensitivity > 100) return {};
    const media::AnalysisIdentity identity{contentHash, source.sampleRate(), source.channels(), source.frames()};
    auto& store = cache ? *cache : media::defaultAnalysisCache();
    if (auto previous = readWarpTiming(store, identity, settings, stop)) return previous;
    if (stop.stop_requested()) return {};

    SpectralOnsetAnalyzer transients(identity.sampleRate, identity.channels, static_cast<int>(settings.sensitivity));
    WarpBeatCandidateAnalyzer beats(WarpBeatMeter{settings.numerator, settings.denominator, settings.quarterBpm});
    WarpAnalysisPcm pcm(identity.sampleRate, identity.channels, [&](std::span<const float> mono) {
        return beats.append(mono, stop);
    });
    constexpr std::uint32_t blockFrames = 4096;
    std::vector<float> input(static_cast<std::size_t>(blockFrames) * identity.channels);
    source.seek(0);
    for (auto left = identity.frames; left != 0;) {
        if (stop.stop_requested()) return {};
        const auto frames = static_cast<std::uint32_t>(std::min<std::uint64_t>(left, blockFrames));
        if (source.read(input.data(), frames) != frames) return {};
        const auto block = std::span(input).first(static_cast<std::size_t>(frames) * identity.channels);
        if (!transients.append(block, stop) || !pcm.append(block, stop)) return {};
        left -= frames;
    }
    if (!pcm.finish(stop)) return {};
    auto onsets = transients.finish(stop);
    auto grid = beats.finish(stop);
    if (!onsets || stop.stop_requested() || (!grid && grid.error != WarpBeatCandidateError::NoBeat)) return {};

    WarpTimingAnalysis result;
    result.onsets = std::move(onsets.onsets);
    if (grid) {
        result.beats = std::move(grid.beats);
        result.downbeats = std::move(grid.downbeats);
        result.tempoRegions = std::move(grid.tempoRegions);
        result.ticksPerPulse = grid.ticksPerPulse;
        if (grid.medianBpm > 0) result.medianBpm = grid.medianBpm;
    }
    std::string diagnostic;
    (void)storeWarpTiming(store, identity, settings, result, diagnostic, stop);
    if (stop.stop_requested()) return {};
    return result;
}

std::vector<std::uint64_t> warpOnsetsInWindow(const WarpTimingAnalysis& data,
    std::uint32_t sourceRate, std::uint64_t start, std::uint64_t frames) {
    if (onAudioThread || sourceRate == 0 || sourceRate > 768000 ||
        start > std::numeric_limits<std::uint64_t>::max() - frames) return {};
    std::vector<std::uint64_t> result;
    for (const auto& onset : data.onsets) {
        const auto seconds = onset.analysisFrame / kWarpAnalysisRate;
        if (seconds >= std::numeric_limits<std::uint64_t>::max() / sourceRate) break;
        const auto frame = seconds * sourceRate +
            (onset.analysisFrame % kWarpAnalysisRate * sourceRate + kWarpAnalysisRate / 2) / kWarpAnalysisRate;
        if (frame >= start + frames) break;
        if (frame >= start && (result.empty() || result.back() != frame - start))
            result.push_back(frame - start);
    }
    return result;
}

AutoWarpResult analyzeWarpSource(WavReader& source, const std::string& contentHash,
    std::uint64_t start, std::uint64_t frames, const AutoWarpOptions& options, std::stop_token stop,
    media::AnalysisCache* cache) {
    if (onAudioThread) return {AutoWarpError::Realtime, 0, 0, 0, false, {}};
    if (stop.stop_requested()) return {AutoWarpError::Cancelled, 0, 0, 0, false, {}};
    if (options.transientSensitivity < 0 || options.transientSensitivity > 100)
        return {AutoWarpError::InvalidOptions, 0, 0, 0, false, {}};

    if (options.useCachedBeatGrid && !options.sourceBpm && options.detectTempoChanges &&
        start == 0 && frames == source.frames() && options.numerator >= 2 && options.numerator <= 16 &&
        options.denominator >= 1 && options.denominator <= 32) {
        const WarpTimingSettings settings{static_cast<unsigned>(options.numerator),
            static_cast<unsigned>(options.transientSensitivity), static_cast<unsigned>(options.denominator),
            options.sourceMeterBpm};
        if (auto timing = analyzeWarpTiming(source, contentHash, settings, stop, cache)) {
            WarpBeatCandidateResult grid;
            grid.beats = std::move(timing->beats);
            grid.downbeats = std::move(timing->downbeats);
            grid.tempoRegions = std::move(timing->tempoRegions);
            grid.medianBpm = timing->medianBpm.value_or(0);
            grid.ticksPerPulse = timing->ticksPerPulse;
            if (auto map = buildWarpBeatCandidateMap(grid, frames, source.sampleRate(), options.fromFirstBeat, stop)) {
                AutoWarpResult result;
                result.estimatedBpm = grid.medianBpm;
                result.trimFrames = map->trimFrames;
                result.markers = std::move(map->markers);
                result.variableTempo = std::ranges::any_of(grid.tempoRegions, [&](const auto& region) {
                    return std::abs(region.bpm - grid.tempoRegions.front().bpm) > 0.001;
                });
                return result;
            }
            return autoWarpFromOnsets(warpOnsetsInWindow(*timing, source.sampleRate(), start, frames),
                frames, source.sampleRate(), options, stop);
        }
    }

    const auto onsets = detectTransients(source, start, frames, options.transientSensitivity, stop);
    if (!onsets)
        return {stop.stop_requested() ? AutoWarpError::Cancelled : AutoWarpError::InvalidSource, 0, 0, 0, false, {}};
    return autoWarpFromOnsets(onsets.frames, frames, source.sampleRate(), options, stop);
}
}
