// SPDX-License-Identifier: AGPL-3.0-or-later
#include "adi/library/sample_descriptors.hpp"
#include "adi/audio/decode.hpp"
#include "adi/audio/io_audit.hpp"
#include "adi/dsp/masking.hpp"
#include "adi/dsp/spectrum.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <complex>
#include <limits>
#include <numeric>
#include <vector>

namespace adi::library {
namespace {
constexpr double lowestHz = 20.0;
constexpr double highestHz = 20000.0;
constexpr double silencePower = 1e-20;
constexpr std::array<double, 7> spectralEdges{125, 250, 500, 1000, 2000, 4000, 8000};

struct SpectralBin {
    std::uint8_t spectralBand;
    std::uint8_t maskingBand;
};

template <std::size_t size>
void normalize(const std::array<double, size>& energy, std::array<float, size>& shares, double total) noexcept {
    if (total > 0.0) {
        for (std::size_t band = 0; band < size; ++band)
            shares[band] = static_cast<float>(energy[band] / total);
    }
}
} // namespace

struct SampleDescriptorAnalyzer::State {
    State(std::uint32_t rate, std::uint32_t channelCount)
        : sampleRate(rate), channels(channelCount),
          fftSize(std::bit_ceil(std::max<std::uint32_t>(32, rate / 24))), hop(fftSize / 2),
          fft(static_cast<std::int32_t>(fftSize)), window(fftSize), input(fftSize),
          planar(fftSize * channels), spectrum(hop + 1), filled(hop) {
        dsp::fillWindow(dsp::Window::Hann, window);
        const double binHz = static_cast<double>(sampleRate) / static_cast<double>(fftSize);
        firstBin = static_cast<std::size_t>(std::ceil(lowestHz / binHz));
        const auto lastBin = std::min(hop, static_cast<std::size_t>(highestHz / binHz));
        if (lastBin < firstBin) return;
        bins.reserve(lastBin - firstBin + 1);
        const double firstErb = dsp::erbRate(lowestHz);
        for (std::size_t bin = firstBin; bin <= lastBin; ++bin) {
            const double hz = static_cast<double>(bin) * binHz;
            const auto edge = std::upper_bound(spectralEdges.begin(), spectralEdges.end(), hz);
            const auto erb = static_cast<std::size_t>(dsp::erbRate(hz) - firstErb);
            bins.push_back({static_cast<std::uint8_t>(edge - spectralEdges.begin()),
                            static_cast<std::uint8_t>(std::min(erb, sampleMaskingBandCount - 1))});
        }
    }

    void appendFrame(std::span<const float> samples) noexcept {
        double frameEnergy = 0.0;
        for (std::size_t channel = 0; channel < channels; ++channel) {
            // Bound even damaged float PCM so the FFT cannot overflow. The
            // limit is over 240 dBFS and outside any ordinary source level.
            const float sample = std::isfinite(samples[channel])
                                     ? std::clamp(samples[channel], -1e12f, 1e12f) : 0.0f;
            frameEnergy += static_cast<double>(sample) * sample;
            planar[fftSize * channel + filled] = sample;
        }
        ++frames;
        levelEnergy += frameEnergy;
        currentHopEnergy += frameEnergy;
        envelopeBlockEnergy += frameEnergy;
        ++envelopeFill;
        if (envelopeFill == envelopeBlockSize) {
            envelope[envelopeBlocks++] = envelopeBlockEnergy;
            envelopeFill = 0;
            envelopeBlockEnergy = 0.0;
            if (envelopeBlocks == envelope.size()) {
                for (std::size_t block = 0; block < envelope.size() / 2; ++block)
                    envelope[block] = envelope[block * 2] + envelope[block * 2 + 1];
                envelopeBlocks /= 2;
                envelopeBlockSize *= 2;
            }
        }
        if (++filled == fftSize) {
            if (previousHopEnergy + currentHopEnergy > 0.0) measureWindow(filled);
            previousHopEnergy = currentHopEnergy;
            currentHopEnergy = 0.0;
            filled = hop;
        }
    }

    void measureWindow(std::size_t available) noexcept {
        if (bins.empty()) return;
        // Per-channel power preserves energy in opposite-polarity stereo.
        // A mean subtraction removes local DC before the taper spreads it.
        for (std::size_t channel = 0; channel < channels; ++channel) {
            auto samples = std::span(planar).subspan(fftSize * channel, fftSize);
            const auto present = samples.first(available);
            const auto mean = std::accumulate(present.begin(), present.end(), 0.0) / static_cast<double>(available);
            for (std::size_t i = 0; i < available; ++i)
                input[i] = static_cast<float>(present[i] - mean) * window[i];
            const auto padding = std::span(input).subspan(available);
            std::fill(padding.begin(), padding.end(), 0.0f);
            if (fft.forward(input, spectrum)) {
                for (std::size_t index = 0; index < bins.size(); ++index) {
                    const auto bin = firstBin + index;
                    const auto& info = bins[index];
                    const double real = spectrum[bin].real();
                    const double imaginary = spectrum[bin].imag();
                    // Nyquist has no negative-frequency partner.
                    const auto power = (real * real + imaginary * imaginary) * (bin == hop ? 0.5 : 1.0);
                    spectralEnergy[info.spectralBand] += power;
                    maskingEnergy[info.maskingBand] += power;
                    weightedBin += power * static_cast<double>(bin);
                }
            }
            std::copy(samples.last(hop).begin(), samples.last(hop).end(), samples.begin());
        }
    }

    std::array<double, 4> timeQuarters() const noexcept {
        std::array<double, 4> quarters{};
        const double quarterFrames = static_cast<double>(frames) / static_cast<double>(quarters.size());
        for (std::size_t block = 0; block <= envelopeBlocks; ++block) {
            const bool partial = block == envelopeBlocks;
            const auto count = partial ? envelopeFill : envelopeBlockSize;
            if (count == 0) continue;
            const double start = static_cast<double>(block * envelopeBlockSize);
            const double end = start + static_cast<double>(count);
            const double energy = partial ? envelopeBlockEnergy : envelope[block];
            for (std::size_t quarter = 0; quarter < quarters.size(); ++quarter) {
                const double low = static_cast<double>(quarter) * quarterFrames;
                const double high = static_cast<double>(quarter + 1) * quarterFrames;
                const double overlap = std::max(0.0, std::min(end, high) - std::max(start, low));
                quarters[quarter] += energy * (overlap / static_cast<double>(count));
            }
        }
        return quarters;
    }

    const std::uint32_t sampleRate;
    const std::size_t channels;
    const std::size_t fftSize, hop;
    dsp::RealFft fft;
    std::vector<float> window, input, planar;
    std::vector<std::complex<float>> spectrum;
    std::vector<SpectralBin> bins;
    std::size_t firstBin = 0;
    std::size_t filled;
    std::uint64_t frames = 0;
    bool finished = false;
    std::array<double, 8> spectralEnergy{};
    std::array<double, sampleMaskingBandCount> maskingEnergy{};
    double weightedBin = 0.0;
    double levelEnergy = 0.0;
    double previousHopEnergy = 0.0, currentHopEnergy = 0.0;
    // Merge adjacent buckets on demand. This permits exact source duration
    // to arrive at EOF, with any approximated split confined to one bucket.
    std::array<double, 1024> envelope{};
    std::size_t envelopeBlocks = 0;
    std::uint64_t envelopeBlockSize = 1, envelopeFill = 0;
    double envelopeBlockEnergy = 0.0;
};

SampleDescriptorAnalyzer::SampleDescriptorAnalyzer(std::uint32_t rate, std::uint32_t channels) {
    if (audio::onAudioThread || rate == 0 || rate > audio::kMaxDecodeRate ||
        channels == 0 || channels > audio::kMaxDecodeChannels)
        return;
    state_ = std::make_unique<State>(rate, channels);
    if (!state_->fft.valid()) state_.reset();
}

SampleDescriptorAnalyzer::~SampleDescriptorAnalyzer() = default;

bool SampleDescriptorAnalyzer::append(std::span<const float> interleaved) noexcept {
    if (audio::onAudioThread || !state_ || state_->finished) return false;
    auto& state = *state_;
    if (interleaved.size() % state.channels != 0 ||
        interleaved.size() / state.channels > std::numeric_limits<std::uint64_t>::max() - state.frames)
        return false;
    for (std::size_t frame = 0; frame < interleaved.size(); frame += state.channels)
        state.appendFrame(interleaved.subspan(frame, state.channels));
    return true;
}

std::optional<SampleDescriptors> SampleDescriptorAnalyzer::finish(std::span<const std::uint64_t> onsets) noexcept {
    if (audio::onAudioThread || !state_ || state_->finished || state_->frames == 0) return {};
    auto& state = *state_;
    SampleDescriptors result;
    std::array<double, 8> hits{};
    std::optional<std::uint64_t> previous;
    for (const auto onset : onsets) {
        if (onset >= state.frames || (previous && onset <= *previous)) return {};
        const auto fraction = static_cast<double>(onset) / static_cast<double>(state.frames);
        const auto bin = static_cast<std::size_t>(fraction * static_cast<double>(hits.size()));
        ++hits[std::min(bin, hits.size() - 1)];
        previous = onset;
    }
    if (state.previousHopEnergy + state.currentHopEnergy > 0.0) state.measureWindow(state.filled);
    // The last nonempty half-window also contributes after the next hop.
    if (state.currentHopEnergy > 0.0) state.measureWindow(state.filled - state.hop);
    state.finished = true;
    const double spectralTotal = std::accumulate(state.spectralEnergy.begin(), state.spectralEnergy.end(), 0.0);
    normalize(state.spectralEnergy, result.spectralEnergy, spectralTotal);
    normalize(state.maskingEnergy, result.maskingEnergy, spectralTotal);
    normalize(state.timeQuarters(), result.envelopeEnergy, state.levelEnergy);
    normalize(hits, result.onsetDensity, static_cast<double>(onsets.size()));
    if (spectralTotal > 0.0)
        result.brightnessHz = static_cast<float>(state.weightedBin * state.sampleRate /
                                                 (spectralTotal * static_cast<double>(state.fftSize)));
    const auto frames = static_cast<double>(state.frames);
    result.hitDensityHz = static_cast<float>(static_cast<double>(onsets.size()) * state.sampleRate / frames);
    const auto power = state.levelEnergy / (frames * static_cast<double>(state.channels));
    result.rmsDbfs = static_cast<float>(10.0 * std::log10(std::max(power, silencePower)));
    return result;
}

} // namespace adi::library
