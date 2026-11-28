// SPDX-License-Identifier: AGPL-3.0-or-later
#include "adi/audio/warp_source_analysis.hpp"
#include "adi/audio/io_audit.hpp"
#include "adi/audio/warp_beat_candidate.hpp"
#include <algorithm>
#include <bit>
#include <cmath>
#include <condition_variable>
#include <limits>
#include <map>
#include <mutex>
#include <thread>
#include <utility>

namespace adi::audio {
namespace {
bool validTimingInput(std::uint32_t rate, std::uint32_t channels, WarpTimingSettings settings) {
    return rate >= 8000 && rate <= 768000 && channels > 0 && channels <= 64 &&
        settings.numerator >= 2 && settings.numerator <= 16 && settings.sensitivity <= 100;
}

struct TimingFlightKey {
    std::filesystem::path folder;
    std::string hash;
    unsigned int numerator, denominator, sensitivity;
    std::uint64_t quarterBpmBits;
    auto operator<=>(const TimingFlightKey&) const = default;
};

struct TimingFlights {
    std::mutex mutex;
    std::condition_variable_any wake;
    std::map<TimingFlightKey, std::thread::id> owners;
};

TimingFlights& timingFlights() {
    static TimingFlights flights;
    return flights;
}
} // namespace

struct WarpTimingFlight::State {
    explicit State(TimingFlightKey value) : key(std::move(value)) {}
    ~State() {
        if (!acquired) return;
        auto& flights = timingFlights();
        {
            std::lock_guard lock(flights.mutex);
            flights.owners.erase(key);
        }
        flights.wake.notify_all();
    }

    TimingFlightKey key;
    bool acquired = false;
};

WarpTimingFlight::WarpTimingFlight(std::unique_ptr<State> state) noexcept : state_(std::move(state)) {}
WarpTimingFlight::WarpTimingFlight(WarpTimingFlight&&) noexcept = default;
WarpTimingFlight& WarpTimingFlight::operator=(WarpTimingFlight&&) noexcept = default;
WarpTimingFlight::~WarpTimingFlight() = default;

std::optional<WarpTimingFlight> acquireWarpTimingFlight(const media::AnalysisCache& cache,
    std::string_view contentHash, WarpTimingSettings settings, std::stop_token stop) {
    if (onAudioThread || stop.stop_requested()) return {};
    if (!cache.folder().is_absolute() || contentHash.empty()) return WarpTimingFlight(nullptr);

    auto folder = cache.folder().lexically_normal();
    if (folder.has_relative_path() && !folder.has_filename()) folder = folder.parent_path();
    auto state = std::make_unique<WarpTimingFlight::State>(TimingFlightKey{std::move(folder),
        std::string(contentHash), settings.numerator, settings.denominator, settings.sensitivity,
        std::bit_cast<std::uint64_t>(settings.quarterBpm.value_or(0.0))});
    auto& flights = timingFlights();
    const auto thread = std::this_thread::get_id();
    std::unique_lock lock(flights.mutex);
    if (!flights.wake.wait(lock, stop, [&] {
        const auto holder = flights.owners.find(state->key);
        return holder == flights.owners.end() || holder->second == thread;
    }) || stop.stop_requested()) return {};
    // A same-thread request satisfies the wait predicate instead of waiting
    // for its own lease, but must leave that existing registration intact.
    if (!flights.owners.emplace(state->key, thread).second) return {};
    state->acquired = true;
    return WarpTimingFlight(std::move(state));
}

struct WarpTimingAnalyzer::State {
    State(std::uint32_t rate, std::uint32_t channels, WarpTimingSettings settings)
        : transients(rate, channels, static_cast<int>(settings.sensitivity)),
          beats(WarpBeatMeter{settings.numerator, settings.denominator, settings.quarterBpm}),
          pcm(rate, channels, [this](std::span<const float> mono) { return beats.append(mono, stop); }) {}

    SpectralOnsetAnalyzer transients;
    WarpBeatCandidateAnalyzer beats;
    std::stop_token stop;
    WarpAnalysisPcm pcm;
    bool failed = false;
};

WarpTimingAnalyzer::WarpTimingAnalyzer(std::uint32_t rate, std::uint32_t channels, WarpTimingSettings settings) {
    if (!onAudioThread && validTimingInput(rate, channels, settings))
        state_ = std::make_unique<State>(rate, channels, settings);
}

WarpTimingAnalyzer::~WarpTimingAnalyzer() = default;

bool WarpTimingAnalyzer::append(std::span<const float> interleaved, std::stop_token stop) {
    if (onAudioThread || !state_ || state_->failed) return false;
    state_->stop = stop;
    state_->failed = !state_->transients.append(interleaved, stop) || !state_->pcm.append(interleaved, stop);
    return !state_->failed;
}

std::optional<WarpTimingAnalysis> WarpTimingAnalyzer::finish(std::stop_token stop) {
    if (onAudioThread || !state_) return {};
    auto state = std::move(state_);
    if (state->failed || stop.stop_requested()) return {};
    state->stop = stop;
    if (!state->pcm.finish(stop)) return {};
    auto onsets = state->transients.finish(stop);
    auto grid = state->beats.finish(stop);
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
    return result;
}

std::optional<WarpTimingAnalysis> analyzeWarpTiming(WavReader& source,
    const std::string& contentHash, WarpTimingSettings settings, std::stop_token stop,
    media::AnalysisCache* cache) {
    if (onAudioThread || stop.stop_requested() || source.frames() == 0 ||
        !validTimingInput(source.sampleRate(), source.channels(), settings)) return {};
    const media::AnalysisIdentity identity{contentHash, source.sampleRate(), source.channels(), source.frames()};
    auto& store = cache ? *cache : media::defaultAnalysisCache();
    const auto flight = acquireWarpTimingFlight(store, contentHash, settings, stop);
    if (!flight) return {};
    if (auto previous = readWarpTiming(store, identity, settings, stop)) return previous;
    if (stop.stop_requested()) return {};

    WarpTimingAnalyzer analyzer(identity.sampleRate, identity.channels, settings);
    constexpr std::uint32_t blockFrames = 4096;
    std::vector<float> input(static_cast<std::size_t>(blockFrames) * identity.channels);
    source.seek(0);
    for (auto left = identity.frames; left != 0;) {
        if (stop.stop_requested()) return {};
        const auto frames = static_cast<std::uint32_t>(std::min<std::uint64_t>(left, blockFrames));
        if (source.read(input.data(), frames) != frames) return {};
        const auto block = std::span(input).first(static_cast<std::size_t>(frames) * identity.channels);
        if (!analyzer.append(block, stop)) return {};
        left -= frames;
    }
    auto result = analyzer.finish(stop);
    if (!result) return {};
    std::string diagnostic;
    (void)storeWarpTiming(store, identity, settings, *result, diagnostic, stop);
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
            const bool useDownbeat = options.fromFirstBeat && !grid.downbeats.empty();
            if (auto map = buildWarpBeatCandidateMap(grid, frames, source.sampleRate(), useDownbeat, stop)) {
                AutoWarpResult result;
                result.estimatedBpm = grid.medianBpm;
                result.trimFrames = map->trimFrames;
                result.markers = std::move(map->markers);
                result.variableTempo = std::ranges::any_of(grid.tempoRegions, [&](const auto& region) {
                    return std::abs(region.bpm - grid.tempoRegions.front().bpm) > 0.001;
                });
                return result;
            }
            // A cached onset is an attack, not a bar boundary. If the timing
            // grid supplied no usable map, the onset fit keeps any pickup.
            auto keepSourceStart = options;
            keepSourceStart.fromFirstBeat = false;
            return autoWarpFromOnsets(warpOnsetsInWindow(*timing, source.sampleRate(), start, frames),
                frames, source.sampleRate(), keepSourceStart, stop);
        }
    }

    const auto onsets = detectTransients(source, start, frames, options.transientSensitivity, stop);
    if (!onsets)
        return {stop.stop_requested() ? AutoWarpError::Cancelled : AutoWarpError::InvalidSource, 0, 0, 0, false, {}};
    return autoWarpFromOnsets(onsets.frames, frames, source.sampleRate(), options, stop);
}
}
