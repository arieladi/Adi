// SPDX-License-Identifier: AGPL-3.0-or-later
#include "adi/library/sample_tonality.hpp"

#include "adi/audio/io_audit.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <numeric>

namespace adi::library {
namespace {
constexpr std::uint32_t targetAnalysisRate = 11025;
constexpr std::uint32_t maximumRate = 768000;
constexpr std::uint32_t maximumChannels = 64;
constexpr double minimumFrequency = 45;
constexpr double maximumFrequency = 3000;

// Tonic, third and fifth carry the most evidence. The minor profile allows
// both sevenths because a minor cadence commonly has a major dominant.
constexpr std::array<double, 12> majorProfile{1, -.4, .25, -.5, .7, .5, -.5, .8, -.5, .3, -.35, .3};
constexpr std::array<double, 12> minorProfile{1, -.4, .25, .7, -.5, .5, -.5, .8, .3, -.5, .2, .2};
} // namespace

SampleKeyAnalyzer::SampleKeyAnalyzer(std::uint32_t sampleRate, std::uint32_t channels)
    : channels_(channels), reduction_(std::max(1u, (std::min(sampleRate, maximumRate) + targetAnalysisRate / 2) / targetAnalysisRate)),
      analysisRate_(static_cast<double>(sampleRate) / reduction_),
      fft_(sampleRate > 0 && sampleRate <= maximumRate && channels > 0 && channels <= maximumChannels
           && !audio::onAudioThread ? static_cast<std::int32_t>(windowSize) : 0) {
    if (fft_.valid()) dsp::fillWindow(dsp::Window::Hann, window_);
}

bool SampleKeyAnalyzer::append(std::span<const float> interleaved) {
    if (audio::onAudioThread || finished_ || !fft_.valid() || interleaved.size() % channels_ != 0) return false;
    for (std::size_t offset = 0; offset < interleaved.size(); offset += channels_) {
        double sum = 0;
        const auto sample = [&](std::size_t channel) {
            const auto value = interleaved[offset + channel];
            return std::isfinite(value) ? std::clamp(value, -16.f, 16.f) : 0.f;
        };
        if (channels_ > 1) {
            const auto left = sample(0);
            const auto right = sample(1);
            sum = left + right;
            reductionSideSum_ += .5 * (left - right);
            for (std::size_t channel = 2; channel < channels_; ++channel) sum += sample(channel);
        } else {
            sum = sample(0);
        }
        reductionSum_ += sum / channels_;
        if (++reductionFrames_ == reduction_) {
            addFrame(static_cast<float>(reductionSum_ / reductionFrames_),
                     static_cast<float>(reductionSideSum_ / reductionFrames_));
            reductionSum_ = 0;
            reductionSideSum_ = 0;
            reductionFrames_ = 0;
        }
    }
    return true;
}

void SampleKeyAnalyzer::addFrame(float mid, float side) {
    frame_[position_] = mid;
    sideFrame_[position_++] = side;
    if (position_ < windowSize) return;
    accumulate();
    std::move(frame_.begin() + static_cast<std::ptrdiff_t>(hopSize), frame_.end(), frame_.begin());
    if (channels_ > 1)
        std::move(sideFrame_.begin() + static_cast<std::ptrdiff_t>(hopSize), sideFrame_.end(), sideFrame_.begin());
    position_ = hopSize;
    hasCompleteWindow_ = true;
}

void SampleKeyAnalyzer::accumulate() {
    // A strongly out-of-phase pair is audible in stereo even when its mono
    // sum cancels. Keep the ordinary fold unless the side dominates it.
    bool useSide = false;
    if (channels_ > 1) {
        const auto midEnergy = std::inner_product(frame_.begin(), frame_.end(), frame_.begin(), 0.);
        const auto sideEnergy = std::inner_product(sideFrame_.begin(), sideFrame_.end(), sideFrame_.begin(), 0.);
        useSide = sideEnergy > 4 * midEnergy;
    }
    const auto& source = useSide ? sideFrame_ : frame_;
    for (std::size_t i = 0; i < windowSize; ++i) input_[i] = window_[i] * source[i];
    if (!fft_.forward(input_, spectrum_)) return;

    const double binHz = analysisRate_ / windowSize;
    const auto firstBin = static_cast<std::size_t>(std::ceil(minimumFrequency / binHz));
    const auto lastBin = std::min(spectrum_.size() - 2, static_cast<std::size_t>(maximumFrequency / binHz));
    std::array<double, 12> windowChroma{};
    double totalEnergy = 0;
    double peakEnergy = 0;
    double highestPeak = 0;
    for (std::size_t bin = firstBin; bin <= lastBin; ++bin)
        highestPeak = std::max(highestPeak, static_cast<double>(std::norm(spectrum_[bin])));
    if (highestPeak < .0001) return;

    for (std::size_t bin = firstBin; bin <= lastBin; ++bin) {
        const double energy = std::norm(spectrum_[bin]);
        totalEnergy += energy;
        const double before = std::norm(spectrum_[bin - 1]);
        const double after = std::norm(spectrum_[bin + 1]);
        if (energy < .0025 * highestPeak || energy < before || energy <= after) continue;
        peakEnergy += energy;

        // Interpolating a Hann peak reduces pitch error at low frequencies.
        const double left = std::log(std::max(before, 1.e-30));
        const double center = std::log(energy);
        const double right = std::log(std::max(after, 1.e-30));
        const double delta = .5 * (left - right) / (left - 2 * center + right);
        const double frequency = (static_cast<double>(bin) + delta) * binHz;
        const double pitch = 69. + 12. * std::log2(frequency / 440.);
        const auto nearest = std::lround(pitch);
        const double closeness = 1. - std::abs(pitch - static_cast<double>(nearest));
        const auto note = static_cast<std::size_t>((nearest % 12 + 12) % 12);
        windowChroma[note] += std::sqrt(energy * 220. / frequency) * closeness;
    }

    // Broadband percussive windows have little energy in stable peaks.
    if (peakEnergy < .38 * totalEnergy) return;
    for (std::size_t i = 0; i < chroma_.size(); ++i) chroma_[i] += windowChroma[i];
}

std::optional<DetectedSampleKey> SampleKeyAnalyzer::finish() {
    if (audio::onAudioThread || !fft_.valid()) return {};
    if (finished_) return result_;
    if (reductionFrames_ > 0)
        addFrame(static_cast<float>(reductionSum_ / reductionFrames_),
                 static_cast<float>(reductionSideSum_ / reductionFrames_));
    if ((!hasCompleteWindow_ && position_ >= hopSize / 2) || position_ > hopSize + hopSize / 2) {
        std::fill(frame_.begin() + static_cast<std::ptrdiff_t>(position_), frame_.end(), 0.f);
        if (channels_ > 1)
            std::fill(sideFrame_.begin() + static_cast<std::ptrdiff_t>(position_), sideFrame_.end(), 0.f);
        accumulate();
    }
    finished_ = true;

    const auto strongest = *std::max_element(chroma_.begin(), chroma_.end());
    if (strongest == 0) return {};
    const auto supportedNotes = std::count_if(chroma_.begin(), chroma_.end(), [strongest](double energy) {
        return energy > .15 * strongest;
    });
    if (supportedNotes < 3) return {};

    const double total = std::accumulate(chroma_.begin(), chroma_.end(), 0.);
    double highest = -std::numeric_limits<double>::infinity();
    double next = highest;
    SampleKey key;
    for (const bool minor : {false, true}) {
        const auto& profile = minor ? minorProfile : majorProfile;
        for (std::size_t root = 0; root < chroma_.size(); ++root) {
            double score = 0;
            for (std::size_t degree = 0; degree < profile.size(); ++degree)
                score += profile[degree] * chroma_[(root + degree) % chroma_.size()];
            if (score > highest) {
                next = highest;
                highest = score;
                key = {static_cast<std::uint8_t>(root), minor};
            } else {
                next = std::max(next, score);
            }
        }
    }
    const auto strength = static_cast<float>(std::clamp((highest - next) / total, 0., 1.));
    if (strength >= .035f) result_ = DetectedSampleKey{key, strength};
    return result_;
}

} // namespace adi::library
