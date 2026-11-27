// SPDX-License-Identifier: AGPL-3.0-or-later
#include "adi/audio/warp_beat_candidate.hpp"

#include "adi/audio/io_audit.hpp"

#include <dsp/onsets/DetectionFunction.h>
#include <dsp/tempotracking/DownBeat.h>
#include <dsp/tempotracking/TempoTrackV2.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <functional>
#include <limits>
#include <numbers>
#include <numeric>
#include <utility>

namespace adi::audio {
namespace {

constexpr int kHop = 512;
constexpr int kWindow = 2 * kHop;
constexpr int kDownbeatDecimation = 16;
constexpr int kInitialDetectionFrames = 2;
constexpr std::size_t kTrackerFrameCount = 128;
constexpr std::uint64_t kMaxSourceFrames = 0x7fffffff;

DFConfig detectionConfig() {
    DFConfig config{};
    config.DFType = DF_COMPLEXSD;
    config.stepSize = kHop;
    config.frameLength = kWindow;
    config.dbRise = 3;
    config.adaptiveWhitening = false;
    config.whiteningRelaxCoeff = -1;
    config.whiteningFloor = -1;
    return config;
}

double bpmForPeriod(double period) {
    return 60.0 * kWarpAnalysisRate / period;
}

template <class Number>
double median(std::vector<Number>& values) {
    const auto middle = values.begin() + static_cast<std::ptrdiff_t>(values.size() / 2);
    std::nth_element(values.begin(), middle, values.end());
    const auto center = static_cast<double>(*middle);
    return values.size() % 2 != 0 ? center
        : (center + static_cast<double>(*std::max_element(values.begin(), middle))) / 2.0;
}

bool validMeter(const WarpBeatMeter& meter) {
    constexpr unsigned int maximumDenominator = 32;
    return meter.numerator >= 2 && meter.numerator <= 16 && meter.denominator != 0 &&
           meter.denominator <= maximumDenominator && (meter.denominator & (meter.denominator - 1)) == 0 &&
           (!meter.quarterBpm || (std::isfinite(*meter.quarterBpm) && *meter.quarterBpm > 0 && *meter.quarterBpm < 1000));
}

struct MeterPulse {
    unsigned int pulsesPerBar;
    std::int64_t ticksPerPulse;
    bool compound;
};

struct PulseSignature {
    float energy = 0;
    float difference = 0;
};

PulseSignature pulseSignature(std::span<const float> samples, float precedingSample) {
    PulseSignature result;
    for (const auto sample : samples) {
        result.energy += sample * sample;
        const auto difference = sample - precedingSample;
        result.difference += difference * difference;
        precedingSample = sample;
    }
    return result;
}

bool uniformPulse(std::span<const PulseSignature> hops, std::span<const std::uint64_t> beats,
                  std::size_t pulsesPerBar, std::stop_token stop) {
    constexpr std::size_t minimumCompleteBars = 3;
    if (beats.size() < minimumCompleteBars * pulsesPerBar + 2) return false;
    PulseSignature minimum{std::numeric_limits<float>::max(), std::numeric_limits<float>::max()};
    PulseSignature maximum;
    for (std::size_t i = 1; i + 1 < beats.size(); ++i) {
        if (stop.stop_requested()) return false;
        // Compare complete beat interiors. Fixed windows near the tracked
        // beat can mistake hop-size position jitter for a musical accent.
        const auto start = static_cast<std::size_t>((beats[i - 1] + beats[i] + kHop) / (2 * kHop));
        const auto end = static_cast<std::size_t>((beats[i] + beats[i + 1] + kHop) / (2 * kHop));
        if (end <= start || end > hops.size()) return false;
        PulseSignature beat;
        for (const auto& hop : hops.subspan(start, end - start)) {
            beat.energy += hop.energy;
            beat.difference += hop.difference;
        }
        minimum.energy = std::min(minimum.energy, beat.energy);
        minimum.difference = std::min(minimum.difference, beat.difference);
        maximum.energy = std::max(maximum.energy, beat.energy);
        maximum.difference = std::max(maximum.difference, beat.difference);
    }
    constexpr float minimumUniformFraction = 0.96f;
    return maximum.energy > 0 && minimum.energy >= maximum.energy * minimumUniformFraction &&
           minimum.difference >= maximum.difference * minimumUniformFraction;
}

std::optional<MeterPulse> resolveMeter(const WarpBeatMeter& meter, double detectedPulseBpm) {
    const bool compound = meter.numerator >= 6 && meter.numerator % 3 == 0 && meter.denominator >= 8;
    const auto noteTicks = textproj::kWhole / meter.denominator;
    auto notesPerPulse = compound ? 3u : 1u;
    if (meter.quarterBpm) {
        const auto unit = static_cast<double>(noteTicks) / textproj::kPPQ;
        const auto observed = *meter.quarterBpm / (detectedPulseBpm * unit);
        const auto maximumNotesPerPulse = compound ? 3u : 2u;
        constexpr double kMaximumRateError = 0.10;
        double best = kMaximumRateError;
        notesPerPulse = 0;
        for (unsigned int notes = 1; notes <= maximumNotesPerPulse; ++notes) {
            if (meter.numerator % notes != 0 || noteTicks * notes > textproj::kWhole) continue;
            const double error = std::abs(observed / notes - 1.0);
            if (error <= best) {
                notesPerPulse = notes;
                best = error;
            }
        }
    }
    if (notesPerPulse == 0) return {};
    return MeterPulse{meter.numerator / notesPerPulse, noteTicks * notesPerPulse, compound};
}

double lowBandAttack(std::span<const float> pcm, std::uint64_t frame) {
    constexpr double rate = static_cast<double>(kWarpAnalysisRate) / kDownbeatDecimation;
    constexpr double lowBandHz = 160.0;
    constexpr auto before = static_cast<std::size_t>(0.030 * rate);
    constexpr auto after = static_cast<std::size_t>(0.085 * rate);
    const auto center = static_cast<std::size_t>(frame / kDownbeatDecimation);
    const auto first = center > before ? center - before : 0;
    const auto last = std::min(pcm.size(), center + after);
    if (first >= last) return 0.0;
    const auto alpha = 1.0 - std::exp(-2.0 * std::numbers::pi * lowBandHz / rate);
    double stage1 = 0.0, stage2 = 0.0, energy = 0.0;
    for (auto index = first; index < last; ++index) {
        stage1 = std::lerp(stage1, static_cast<double>(pcm[index]), alpha);
        stage2 = std::lerp(stage2, stage1, alpha);
        energy += stage2 * stage2;
    }
    return energy / static_cast<double>(last - first);
}

std::optional<std::size_t> accentedBarPhase(std::span<const float> pcm,
    std::span<const std::uint64_t> beats, std::size_t pulsesPerBar, std::stop_token stop) {
    if (beats.size() < 3 * pulsesPerBar) return {};
    std::vector<double> strengths(pulsesPerBar);
    for (std::size_t phase = 0; phase < pulsesPerBar; ++phase) {
        std::vector<double> attacks;
        for (auto index = phase; index < beats.size(); index += pulsesPerBar) {
            if (stop.stop_requested()) return {};
            attacks.push_back(lowBandAttack(pcm, beats[index]));
        }
        strengths[phase] = median(attacks);
    }
    const auto best = std::ranges::max_element(strengths);
    const auto index = static_cast<std::size_t>(best - strengths.begin());
    const auto energy = *best;
    *best = 0.0;
    // Let the original spectral-change detector settle the phase if the
    // repeated bass accents do not separate the two main compound pulses.
    constexpr double kMinimumLowBandEnergy = 1e-7;
    constexpr double kMinimumAccentRatio = 1.25;
    if (energy < kMinimumLowBandEnergy || energy <= kMinimumAccentRatio * std::ranges::max(strengths)) return {};
    return index;
}

bool scaleMusicalTicks(std::span<WarpMarker> markers, std::int64_t ticksPerPulse, std::stop_token stop) {
    if (ticksPerPulse == textproj::kPPQ) return true;
    if (ticksPerPulse <= 0 || ticksPerPulse > textproj::kWhole) return false;
    const auto divisor = std::gcd(textproj::kPPQ, ticksPerPulse);
    const auto numerator = ticksPerPulse / divisor;
    const auto denominator = textproj::kPPQ / divisor;
    std::int64_t previous = -1;
    for (auto& marker : markers) {
        if (stop.stop_requested()) return false;
        const auto whole = marker.tick / denominator;
        const auto fraction = (marker.tick % denominator * numerator + denominator / 2) / denominator;
        if (whole > (std::numeric_limits<std::int64_t>::max() - fraction) / numerator) return false;
        marker.tick = whole * numerator + fraction;
        if (marker.tick <= previous) return false;
        previous = marker.tick;
    }
    return true;
}

} // namespace

struct WarpBeatCandidateAnalyzer::State {
    explicit State(unsigned int beatsInBar, std::optional<WarpBeatMeter> explicitMeter = {})
        : beatsPerBar(beatsInBar), meter(explicitMeter) {}

    void processWindow(bool appendDownbeatAudio) {
        std::copy(previous.begin(), previous.end(), spectrumInput.begin());
        std::copy(pending.begin(), pending.end(), spectrumInput.begin() + kHop);
        detection.push_back(onset.processTimeDomain(spectrumInput.data()));
        if (appendDownbeatAudio) {
            downbeat.pushAudioBlock(pending.data());
            pulseSignatures.push_back(pulseSignature(std::span(pending).first(pendingFrames), previous.back()));
        }
        previous.swap(pending);
        pendingFrames = 0;
    }

    DetectionFunction onset{detectionConfig()};
    DownBeat downbeat{static_cast<float>(kWarpAnalysisRate), kDownbeatDecimation, kHop};
    unsigned int beatsPerBar;
    std::optional<WarpBeatMeter> meter;
    std::uint64_t frames = 0;
    std::size_t pendingFrames = 0;
    std::array<float, kHop> previous{}, pending{};
    std::array<double, kWindow> spectrumInput{};
    std::vector<double> detection;
    std::vector<PulseSignature> pulseSignatures;
};

WarpBeatCandidateAnalyzer::WarpBeatCandidateAnalyzer(unsigned int beatsPerBar) {
    if (onAudioThread) error_ = WarpBeatCandidateError::Realtime;
    else if (beatsPerBar < 2 || beatsPerBar > 16) error_ = WarpBeatCandidateError::InvalidSource;
    else state_ = std::make_unique<State>(beatsPerBar);
}

WarpBeatCandidateAnalyzer::WarpBeatCandidateAnalyzer(WarpBeatMeter meter) {
    if (onAudioThread) error_ = WarpBeatCandidateError::Realtime;
    else if (!validMeter(meter)) error_ = WarpBeatCandidateError::InvalidSource;
    else state_ = std::make_unique<State>(meter.numerator, meter);
}

WarpBeatCandidateAnalyzer::~WarpBeatCandidateAnalyzer() = default;

bool WarpBeatCandidateAnalyzer::append(std::span<const float> mono, std::stop_token stop) {
    if (onAudioThread || !state_ || error_ != WarpBeatCandidateError::None) return false;
    if (stop.stop_requested()) {
        error_ = WarpBeatCandidateError::Cancelled;
        return false;
    }
    if (mono.size() > kMaxSourceFrames - state_->frames) {
        error_ = WarpBeatCandidateError::InvalidSource;
        return false;
    }
    state_->frames += mono.size();
    while (!mono.empty()) {
        if (stop.stop_requested()) {
            error_ = WarpBeatCandidateError::Cancelled;
            return false;
        }
        const auto count = std::min(mono.size(), state_->pending.size() - state_->pendingFrames);
        auto output = std::span(state_->pending).subspan(state_->pendingFrames, count);
        std::ranges::transform(mono.first(count), output.begin(), [](float value) {
            return std::isfinite(value) ? std::clamp(value, -1.0f, 1.0f) : 0.0f;
        });
        state_->pendingFrames += count;
        if (state_->pendingFrames == state_->pending.size()) state_->processWindow(true);
        mono = mono.subspan(count);
    }
    return true;
}

WarpBeatCandidateResult WarpBeatCandidateAnalyzer::finish(std::stop_token stop) {
    using Error = WarpBeatCandidateError;
    if (onAudioThread) return {Error::Realtime, {}, {}, {}, 0};
    if (stop.stop_requested()) return {Error::Cancelled, {}, {}, {}, 0};
    if (error_ != Error::None) return {error_, {}, {}, {}, 0};
    if (!state_) return {Error::InvalidSource, {}, {}, {}, 0};
    auto state = std::move(state_);
    std::ranges::fill(std::span(state->pending).subspan(state->pendingFrames), 0.0f);
    state->processWindow(state->pendingFrames != 0);
    auto& detection = state->detection;
    while (!detection.empty() && detection.back() <= 0.0) detection.pop_back();
    if (detection.size() < kInitialDetectionFrames + kTrackerFrameCount)
        return {Error::NoBeat, {}, {}, {}, 0};
    detection.erase(detection.begin(), detection.begin() + kInitialDetectionFrames);

    TempoTrackV2 tracker(static_cast<float>(kWarpAnalysisRate), kHop);
    std::vector<int> beatPeriods(detection.size() / kTrackerFrameCount + 1);
    tracker.calculateBeatPeriod(detection, beatPeriods);
    if (stop.stop_requested()) return {Error::Cancelled, {}, {}, {}, 0};
    std::vector<double> beatWindows;
    tracker.calculateBeats(detection, beatPeriods, beatWindows);
    if (stop.stop_requested()) return {Error::Cancelled, {}, {}, {}, 0};

    WarpBeatCandidateResult result;
    std::erase_if(beatWindows, [&](double window) {
        return !std::isfinite(window) || window < 0.0 ||
            window * kHop + kHop / 2 >= static_cast<double>(state->frames);
    });
    for (const auto position : beatWindows)
        result.beats.push_back(static_cast<std::uint64_t>(std::llround(position * kHop + kHop / 2)));
    if (result.beats.size() < 2 || std::adjacent_find(result.beats.begin(), result.beats.end(),
        std::greater_equal<>()) != result.beats.end())
        return {Error::NoBeat, {}, {}, {}, 0};

    std::vector<std::uint64_t> intervals;
    for (std::size_t index = 1; index < result.beats.size(); ++index)
        intervals.push_back(result.beats[index] - result.beats[index - 1]);
    const auto rawPulseBpm = bpmForPeriod(median(intervals));
    const auto grid = state->meter ? resolveMeter(*state->meter, rawPulseBpm)
        : std::optional<MeterPulse>{{state->beatsPerBar, textproj::kPPQ, false}};
    if (!grid) return {Error::NoBeat, {}, {}, {}, 0};
    if (grid->pulsesPerBar == 1) {
        result.downbeats.resize(result.beats.size());
        std::iota(result.downbeats.begin(), result.downbeats.end(), std::size_t{0});
    } else if (result.beats.size() >= grid->pulsesPerBar + 2 &&
               !uniformPulse(state->pulseSignatures, result.beats, grid->pulsesPerBar, stop)) {
        std::size_t decimatedFrames = 0;
        const auto* pcm = state->downbeat.getBufferedAudio(decimatedFrames);
        const auto accent = grid->compound ? accentedBarPhase({pcm, decimatedFrames}, result.beats, grid->pulsesPerBar, stop)
                                           : std::nullopt;
        if (accent) {
            for (auto index = *accent; index < result.beats.size(); index += grid->pulsesPerBar)
                result.downbeats.push_back(index);
        } else {
            state->downbeat.setBeatsPerBar(static_cast<int>(grid->pulsesPerBar));
            std::vector<int> downbeats;
            state->downbeat.findDownBeats(pcm, decimatedFrames, beatWindows, downbeats);
            for (const auto index : downbeats) {
                if (index >= 0 && static_cast<std::size_t>(index) < result.beats.size())
                    result.downbeats.push_back(static_cast<std::size_t>(index));
            }
        }
    }
    if (stop.stop_requested()) return {Error::Cancelled, {}, {}, {}, 0};
    result.ticksPerPulse = grid->ticksPerPulse;
    const auto quarterScale = static_cast<double>(result.ticksPerPulse) / textproj::kPPQ;
    result.medianBpm = rawPulseBpm * quarterScale;
    result.tempoRegions = warpTempoRegions(result.beats, kWarpAnalysisRate, stop);
    for (auto& region : result.tempoRegions) region.bpm *= quarterScale;
    if (stop.stop_requested()) return {Error::Cancelled, {}, {}, {}, 0};
    return result;
}

std::optional<WarpBeatCandidateMap> buildWarpBeatCandidateMap(const WarpBeatCandidateResult& timing,
    std::uint64_t sourceFrames, std::uint32_t sourceRate, bool fromFirstDownbeat, std::stop_token stop) {
    if (!timing) return std::nullopt;
    auto map = buildWarpTimingMap(timing.beats, timing.downbeats, timing.tempoRegions, sourceFrames,
                                 sourceRate, fromFirstDownbeat, stop);
    if (map && !scaleMusicalTicks(map->markers, timing.ticksPerPulse, stop)) return {};
    return map;
}

} // namespace adi::audio
