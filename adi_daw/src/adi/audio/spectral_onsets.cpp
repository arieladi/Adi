// SPDX-License-Identifier: AGPL-3.0-or-later
#include "adi/audio/spectral_onsets.hpp"
#include "adi/audio/io_audit.hpp"
#include "adi/dsp/spectrum.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
#include <limits>
#include <new>
#include <utility>

namespace adi::audio {
namespace {
constexpr std::int64_t fftSize = 1024;
constexpr std::int64_t halfWindow = fftSize / 2;
constexpr std::int64_t hop = 128;
constexpr std::int64_t attackWindow = 32;
constexpr std::int64_t releaseCheckWindow = 176;
constexpr std::int64_t refractory = kWarpAnalysisRate * 30 / 1000;
constexpr std::size_t bandCount = 32;
constexpr std::size_t ringSize = 4096;
constexpr std::size_t maximumOnsets = 1000000;
constexpr std::int64_t maximumFrames = std::numeric_limits<std::int64_t>::max() - 2 * fftSize;
constexpr double firstFrequency = 40.0;
constexpr double lastFrequency = 18000.0;
constexpr std::size_t lastBin = static_cast<std::size_t>(lastFrequency * fftSize / kWarpAnalysisRate);
static_assert(fftSize + hop == kSpectralOnsetLookaheadFrames);

struct FluxFrame {
    std::int64_t center = 0;
    double flux = 0;
    double threshold = 0;
};

} // namespace

struct SpectralOnsetAnalyzer::State {
    State(std::uint32_t rate, std::uint32_t channels, int sensitivity)
        : lanes(channels > 1 ? 2u : 1u),
          minimumFlux(0.036 - 0.035 * sensitivity / 100.0),
          noiseRatio(4.0 - 2.6 * sensitivity / 100.0),
          minimumPeakPower(std::pow(10.0, (-44.0 - 36.0 * sensitivity / 100.0) / 10.0)),
          fft(static_cast<std::int32_t>(fftSize)),
          pcm(rate, channels, [this](std::span<const float> block) { return feed(block); }, WarpAnalysisLayout::MidSide) {
        dsp::fillWindow(dsp::Window::Hann, window);
        const double scale = static_cast<double>(bandCount) / std::log(lastFrequency / firstFrequency);
        for (std::size_t bin = 1; bin <= lastBin; ++bin) {
            const double hz = static_cast<double>(bin) * kWarpAnalysisRate / fftSize;
            binBand[bin] = std::min(bandCount - 1, static_cast<std::size_t>(std::log(hz / firstFrequency) * scale));
        }
    }

    bool feed(std::span<const float> samples) {
        if (samples.size() / lanes > static_cast<std::uint64_t>(maximumFrames - frames)) {
            result.error = AnalysisError::InvalidSource;
            return false;
        }
        for (std::size_t offset = 0; offset < samples.size(); offset += lanes) {
            const auto pos = static_cast<std::size_t>(frames++) % ringSize;
            for (std::size_t lane = 0; lane < lanes; ++lane) ring[lane][pos] = samples[offset + lane];
            if (frames == nextCenter + halfWindow && !scan()) return false;
        }
        return true;
    }

    float sampleAt(std::size_t lane, std::int64_t frame) const noexcept {
        if (frame < 0 || frame >= frames) return 0;
        return ring[lane][static_cast<std::size_t>(frame) % ringSize];
    }

    double powerAt(std::int64_t frame) const noexcept {
        const double mid = sampleAt(0, frame);
        const double side = lanes == 2 ? sampleAt(1, frame) : 0.0;
        return mid * mid + side * side;
    }

    double peakPowerAt(std::int64_t frame) const noexcept {
        // In stereo, max(|L|, |R|) = |mid| + |side|.
        const double value = std::abs(sampleAt(0, frame)) + (lanes == 2 ? std::abs(sampleAt(1, frame)) : 0.0);
        return value * value;
    }

    double spectralFlux(std::size_t lane) noexcept {
        const auto first = nextCenter - halfWindow;
        for (std::size_t i = 0; i < input.size(); ++i)
            input[i] = window[i] * sampleAt(lane, first + static_cast<std::int64_t>(i));
        if (!fft.forward(input, spectrum)) return 0.0;
        std::array<double, bandCount> magnitudes{};
        for (std::size_t bin = 1; bin <= lastBin; ++bin) {
            const double real = spectrum[bin].real();
            const double imag = spectrum[bin].imag();
            magnitudes[binBand[bin]] += real * real + imag * imag;
        }
        for (double& band : magnitudes) band = std::log1p(48.0 * std::sqrt(band) / fftSize);

        double flux = 0.0;
        for (std::size_t band = 0; band < bandCount; ++band) {
            // A previous adjacent band can account for pitch motion across a
            // boundary, but only positive novelty contributes to the flux.
            const auto left = band == 0 ? 0 : band - 1;
            const auto right = std::min(band + 1, bandCount - 1);
            const auto prior = std::max({previousBands[lane][left], previousBands[lane][band], previousBands[lane][right]});
            flux += std::max(0.0, magnitudes[band] - prior);
        }
        previousBands[lane] = magnitudes;
        return flux;
    }

    bool scan() {
        double flux = 0.0;
        for (std::size_t lane = 0; lane < lanes; ++lane) flux = std::max(flux, spectralFlux(lane));
        if (previous.flux > olderFlux && previous.flux >= flux && previous.flux >= previous.threshold) {
            if (!addOnset(previous)) return false;
        }
        olderFlux = previous.flux;
        previous = {nextCenter, flux, std::max(minimumFlux, noiseRatio * meanFlux)};
        meanFlux += 0.04 * (flux - meanFlux);
        nextCenter += hop;
        return true;
    }

    bool addOnset(const FluxFrame& candidate) {
        const auto begin = std::max<std::int64_t>(0, candidate.center - halfWindow);
        const auto end = std::min(frames, candidate.center + halfWindow);
        std::int64_t attack = begin;
        double preceding = 0.0, following = 0.0;
        for (auto i = begin - attackWindow; i < begin; ++i) preceding += powerAt(i);
        for (auto i = begin; i < begin + attackWindow; ++i) following += powerAt(i);
        double peakPower = 0.0;
        double highestRise = 0.0;
        for (auto i = begin; i < end; ++i) {
            const auto power = powerAt(i);
            peakPower = std::max(peakPower, peakPowerAt(i));
            const auto rise = following - preceding;
            if (rise > highestRise) {
                highestRise = rise;
                attack = i;
            }
            preceding += power - powerAt(i - attackWindow);
            following += powerAt(i + attackWindow) - power;
        }
        if (peakPower < minimumPeakPower || highestRise <= 0) return true;

        const auto threshold = 0.0025 * peakPower;
        while (attack > begin && peakPowerAt(attack - 1) > threshold) --attack;
        while (attack + 1 < end && peakPowerAt(attack) <= threshold) ++attack;
        if (candidate.center + halfWindow > frames && !risingAtTail(attack)) return true;
        const auto frame = static_cast<std::uint64_t>(attack);
        if (!result.onsets.empty() && frame < result.onsets.back().analysisFrame + refractory) return true;
        if (result.onsets.size() == maximumOnsets) {
            result.error = AnalysisError::TooManyOnsets;
            return false;
        }
        const auto strength = static_cast<float>(candidate.flux / (1.0 + candidate.flux));
        const auto peakDbfs = static_cast<float>(10.0 * std::log10(peakPower));
        try {
            result.onsets.push_back({frame, strength, peakDbfs});
        } catch (const std::bad_alloc&) {
            result.error = AnalysisError::Io;
            return false;
        }
        return true;
    }

    bool risingAtTail(std::int64_t attack) const noexcept {
        // A final taper extending into padding can see positive spectral
        // flux from a release. Look for a rise in the real frames around it.
        const auto count = std::min(releaseCheckWindow, frames - attack);
        double before = 0.0, after = 0.0;
        for (auto i = attack - count; i < attack; ++i) before += powerAt(i);
        for (auto i = attack; i < attack + count; ++i) after += powerAt(i);
        return after > 1.25 * before;
    }

    SpectralOnsetResult complete(std::stop_token stop) {
        if (finished) return {AnalysisError::InvalidSource, {}};
        finished = true;
        if (!pcm.finish(stop)) return {result ? pcm.error() : result.error, {}};
        if (frames == 0) return {AnalysisError::InvalidSource, {}};
        while (nextCenter <= frames + hop) {
            if (stop.stop_requested()) return {AnalysisError::Cancelled, {}};
            if (!scan()) return {result.error, {}};
        }
        return std::move(result);
    }

    const std::size_t lanes;
    const double minimumFlux, noiseRatio, minimumPeakPower;
    dsp::RealFft fft;
    WarpAnalysisPcm pcm;
    std::array<std::array<float, ringSize>, 2> ring{};
    std::array<std::array<double, bandCount>, 2> previousBands{};
    std::array<float, fftSize> window{}, input{};
    std::array<std::complex<float>, halfWindow + 1> spectrum{};
    std::array<std::size_t, lastBin + 1> binBand{};
    SpectralOnsetResult result;
    std::int64_t frames = 0, nextCenter = 0;
    FluxFrame previous;
    double meanFlux = 0.0, olderFlux = 0.0;
    bool finished = false;
};

SpectralOnsetAnalyzer::SpectralOnsetAnalyzer(std::uint32_t rate, std::uint32_t channels, int sensitivity) {
    if (onAudioThread) {
        initialError_ = AnalysisError::Realtime;
        return;
    }
    if (rate >= 8000 && rate <= 768000 && channels > 0 && channels <= 64 && sensitivity >= 0 && sensitivity <= 100) {
        state_ = std::make_unique<State>(rate, channels, sensitivity);
        if (!state_->fft.valid()) {
            state_.reset();
            initialError_ = AnalysisError::Io;
        }
    }
}

SpectralOnsetAnalyzer::~SpectralOnsetAnalyzer() = default;

bool SpectralOnsetAnalyzer::append(std::span<const float> samples, std::stop_token stop) {
    return !onAudioThread && state_ && !state_->finished && state_->pcm.append(samples, stop);
}

SpectralOnsetResult SpectralOnsetAnalyzer::finish(std::stop_token stop) {
    if (onAudioThread) return {AnalysisError::Realtime, {}};
    return state_ ? state_->complete(stop) : SpectralOnsetResult{initialError_, {}};
}

} // namespace adi::audio
