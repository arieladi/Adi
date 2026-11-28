// SPDX-License-Identifier: AGPL-3.0-or-later
#include "adi/audio/warp_timing_cache.hpp"
#include "adi/audio/io_audit.hpp"
#include <algorithm>
#include <bit>
#include <cmath>
#include <functional>
#include <limits>
#include <stdexcept>

namespace adi::audio {
namespace {
const std::string tag = "AWTM";
constexpr std::uint32_t version = 2; // Distinguish a detected pulse from repeatable bar evidence.
constexpr std::size_t maxPoints = 1000000;
constexpr std::size_t headerSize = 56;

bool goodBpm(double value) {
    return std::isfinite(value) && value > 0 && value < 1000;
}

std::optional<std::uint32_t> configuration(WarpTimingSettings settings) {
    if (settings.numerator < 2 || settings.numerator > 16 || settings.sensitivity > 100 ||
        !std::has_single_bit(settings.denominator) || settings.denominator > 32 ||
        (settings.quarterBpm && !goodBpm(*settings.quarterBpm))) return {};
    return static_cast<std::uint32_t>((settings.numerator << 16) | (settings.denominator << 8) | settings.sensitivity);
}

std::uint64_t canonicalFrames(const media::AnalysisIdentity& identity) {
    if (identity.sampleRate < 8000 || identity.sampleRate > 768000 || identity.channels == 0 ||
        identity.channels > 64 || identity.frames == 0 ||
        identity.frames > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) return 0;
    const auto seconds = identity.frames / identity.sampleRate;
    if (seconds >= std::numeric_limits<std::uint64_t>::max() / kWarpAnalysisRate) return 0;
    return seconds * kWarpAnalysisRate + identity.frames % identity.sampleRate * kWarpAnalysisRate / identity.sampleRate;
}

template<class T, class Position>
bool orderedPositions(const T& array, std::uint64_t limit, Position position) {
    if (array.size() > maxPoints) return false;
    std::optional<std::uint64_t> previous;
    for (const auto& entry : array) {
        const auto current = static_cast<std::uint64_t>(std::invoke(position, entry));
        if (current >= limit || (previous && current <= *previous)) return false;
        previous = current;
    }
    return true;
}
bool valid(const WarpTimingAnalysis& value, std::uint64_t frames) {
    if (frames == 0 || value.ticksPerPulse <= 0 || value.ticksPerPulse > textproj::kWhole ||
        (value.medianBpm && (!goodBpm(*value.medianBpm) || value.beats.size() < 2)))
        return false;
    if (!orderedPositions(value.onsets, frames, &SpectralOnset::analysisFrame) ||
        !orderedPositions(value.beats, frames, [](auto at) { return at; }) ||
        !orderedPositions(value.downbeats, value.beats.size(), [](auto at) { return at; }) ||
        !orderedPositions(value.tempoRegions, value.beats.empty() ? 0 : value.beats.size() - 1,
                          &WarpTempoRegion::firstBeat))
        return false;
    return (value.tempoRegions.empty() || value.tempoRegions.front().firstBeat == 0) &&
        std::ranges::all_of(value.tempoRegions, [](const auto& region) { return goodBpm(region.bpm); }) &&
        std::ranges::all_of(value.onsets, [](const auto& onset) {
            return std::isfinite(onset.strength) && onset.strength >= 0 && onset.strength <= 1 &&
                std::isfinite(onset.peakDbfs);
        });
}

std::optional<WarpTimingAnalysis> decodeTiming(const media::SampleAnalysis& record,
    WarpTimingSettings settings, std::uint32_t config, std::stop_token stop) {
    const auto& identity = record.identity;
    const auto section = record.sections.find(tag);
    if (section == record.sections.end() || section->second.version != version ||
        section->second.configuration != config || section->second.bytes.size() < headerSize) return {};
    const auto& data = section->second.bytes;
    std::size_t at = 0;
    const auto get = [&](auto zero) {
        using T = decltype(zero);
        if (sizeof(T) > data.size() - at) throw std::runtime_error("short timing analysis");
        std::uint64_t value = 0;
        for (std::size_t byte = 0; byte < sizeof(T); ++byte)
            value |= std::to_integer<std::uint64_t>(data[at++]) << (byte * 8);
        return static_cast<T>(value);
    };
    try {
        if (get(std::uint32_t{}) != identity.sampleRate || get(std::uint32_t{}) != identity.channels ||
            get(std::uint64_t{}) != identity.frames) return {};
        const auto onsetCount = get(std::uint32_t{});
        const auto beatCount = get(std::uint32_t{});
        const auto downbeatCount = get(std::uint32_t{});
        const auto regionCount = get(std::uint32_t{});
        const auto bpm = std::bit_cast<double>(get(std::uint64_t{}));
        if (get(std::uint64_t{}) != std::bit_cast<std::uint64_t>(settings.quarterBpm.value_or(0))) return {};
        const auto ticks = get(std::uint64_t{});
        if (ticks > static_cast<std::uint64_t>(textproj::kWhole)) return {};
        const auto bytes = std::uint64_t{headerSize} + std::uint64_t{onsetCount} * 16 +
            std::uint64_t{beatCount} * 8 + std::uint64_t{downbeatCount} * 4 + std::uint64_t{regionCount} * 12;
        if (bytes != data.size() || onsetCount > maxPoints || beatCount > maxPoints ||
            downbeatCount > beatCount || regionCount > beatCount) return {};
        WarpTimingAnalysis decoded;
        decoded.ticksPerPulse = static_cast<std::int64_t>(ticks);
        if (bpm != 0) decoded.medianBpm = bpm;
        decoded.onsets.reserve(onsetCount);
        decoded.beats.reserve(beatCount);
        decoded.downbeats.reserve(downbeatCount);
        decoded.tempoRegions.reserve(regionCount);
        for (unsigned int i = 0; i < onsetCount; ++i)
            decoded.onsets.push_back({get(std::uint64_t{}), std::bit_cast<float>(get(std::uint32_t{})),
                std::bit_cast<float>(get(std::uint32_t{}))});
        for (unsigned int i = 0; i < beatCount; ++i) decoded.beats.push_back(get(std::uint64_t{}));
        for (unsigned int i = 0; i < downbeatCount; ++i) decoded.downbeats.push_back(get(std::uint32_t{}));
        for (unsigned int i = 0; i < regionCount; ++i)
            decoded.tempoRegions.push_back({get(std::uint32_t{}), std::bit_cast<double>(get(std::uint64_t{}))});
        if (!stop.stop_requested() && valid(decoded, canonicalFrames(identity))) return decoded;
    } catch (const std::exception&) {}
    return {};
}
} // namespace

std::optional<WarpTimingAnalysis> readWarpTiming(const media::AnalysisCache& cache,
    const media::AnalysisIdentity& identity, WarpTimingSettings settings, std::stop_token stop) {
    if (onAudioThread || stop.stop_requested()) return {};
    const auto config = configuration(settings);
    if (!config) return {};
    const auto result = cache.read(identity, stop);
    return result ? decodeTiming(*result.data, settings, *config, stop) : std::nullopt;
}

std::optional<WarpTimingAnalysis> readWarpTimingByHash(const media::AnalysisCache& cache,
    const std::string& contentHash, WarpTimingSettings settings, std::stop_token stop) {
    if (onAudioThread || stop.stop_requested()) return {};
    const auto config = configuration(settings);
    if (!config) return {};
    const auto result = cache.readByHash(contentHash, stop);
    return result ? decodeTiming(*result.data, settings, *config, stop) : std::nullopt;
}

media::AnalysisCacheProblem storeWarpTiming(media::AnalysisCache& cache,
    const media::AnalysisIdentity& identity, WarpTimingSettings settings, const WarpTimingAnalysis& value,
    std::string& error, std::stop_token stop) {
    using Problem = media::AnalysisCacheProblem;
    if (onAudioThread) return Problem::Realtime;
    if (stop.stop_requested()) return Problem::Cancelled;
    const auto config = configuration(settings);
    if (!config || !valid(value, canonicalFrames(identity))) return Problem::Invalid;
    const auto bytes = headerSize + value.onsets.size() * 16 + value.beats.size() * 8 +
        value.downbeats.size() * 4 + value.tempoRegions.size() * 12;
    if (bytes > 32 * 1024 * 1024 - 184) return Problem::Resource;
    try {
        media::AnalysisSection section{version, *config, {}};
        section.bytes.reserve(bytes);
        const auto put = [&](auto bits) {
            for (std::size_t byte = 0; byte < sizeof(bits); ++byte)
                section.bytes.push_back(static_cast<std::byte>((bits >> (byte * 8)) & 255u));
        };
        put(identity.sampleRate);
        put(identity.channels);
        put(identity.frames);
        put(static_cast<std::uint32_t>(value.onsets.size()));
        put(static_cast<std::uint32_t>(value.beats.size()));
        put(static_cast<std::uint32_t>(value.downbeats.size()));
        put(static_cast<std::uint32_t>(value.tempoRegions.size()));
        put(std::bit_cast<std::uint64_t>(value.medianBpm.value_or(0)));
        put(std::bit_cast<std::uint64_t>(settings.quarterBpm.value_or(0)));
        put(static_cast<std::uint64_t>(value.ticksPerPulse));
        for (const auto& item : value.onsets) {
            put(item.analysisFrame);
            put(std::bit_cast<std::uint32_t>(item.strength));
            put(std::bit_cast<std::uint32_t>(item.peakDbfs));
        }
        for (const auto beat : value.beats) put(beat);
        for (const auto index : value.downbeats) put(static_cast<std::uint32_t>(index));
        for (const auto& region : value.tempoRegions) {
            put(static_cast<std::uint32_t>(region.firstBeat));
            put(std::bit_cast<std::uint64_t>(region.bpm));
        }
        return cache.update(identity, tag, section, error, stop);
    } catch (const std::bad_alloc&) {
        return Problem::Resource;
    }
}
}
