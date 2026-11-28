// SPDX-License-Identifier: AGPL-3.0-or-later
#include "adi/audio/pitch_compare.hpp"

#include "adi/audio/io_audit.hpp"

#include <algorithm>
#include <cmath>
#include <utility>

namespace adi::audio {

static_assert(std::atomic<bool>::is_always_lock_free);

PitchComparePcm::PitchComparePcm(std::uint32_t rate, std::uint16_t channels, std::vector<float>&& original,
                                 std::vector<float>&& corrected)
    : rate_(rate), channels_(channels), original_(std::move(original)), corrected_(std::move(corrected)) {}

std::shared_ptr<const PitchComparePcm> PitchComparePcm::prepare(
    std::uint32_t rate, std::uint16_t channels, std::vector<float>&& original, std::vector<float>&& corrected,
    std::stop_token stop) {
    if (onAudioThread || stop.stop_requested() || rate == 0 || channels == 0 || channels > 64 || original.empty() ||
        original.size() != corrected.size() || original.size() % channels != 0 ||
        original.capacity() > kPitchCompareMaxResidentSamples ||
        corrected.capacity() > kPitchCompareMaxResidentSamples - original.capacity()) return {};
    for (std::size_t sample = 0; sample < original.size(); ++sample) {
        if (sample % 4096 == 0 && stop.stop_requested()) return {};
        if (!std::isfinite(original[sample])) original[sample] = 0.0f;
        if (!std::isfinite(corrected[sample])) corrected[sample] = 0.0f;
    }
    if (stop.stop_requested()) return {};
    return std::shared_ptr<const PitchComparePcm>(new PitchComparePcm(rate, channels, std::move(original), std::move(corrected)));
}

PitchComparePlayback::PitchComparePlayback(std::shared_ptr<const PitchComparePcm> pcm, std::uint32_t crossfadeFrames) noexcept
    : pcm_(std::move(pcm)), crossfadeFrames_(std::min(crossfadeFrames, kPitchCompareMaxCrossfadeFrames)) {}

std::uint16_t PitchComparePlayback::channels() const noexcept { return pcm_ ? pcm_->channels() : 0; }
std::uint64_t PitchComparePlayback::frames() const noexcept { return pcm_ ? pcm_->frames() : 0; }
std::uint32_t PitchComparePlayback::rate() const noexcept { return pcm_ ? pcm_->rate() : 0; }

void PitchComparePlayback::select(PitchCompareSide side) noexcept {
    requestedCorrected_.store(side == PitchCompareSide::Corrected, std::memory_order_relaxed);
}

std::span<const float> PitchComparePlayback::source(PitchCompareSide side) const noexcept {
    if (!pcm_) return {};
    return side == PitchCompareSide::Corrected ? pcm_->corrected_ : pcm_->original_;
}

float PitchComparePlayback::nextMix() noexcept {
    return advanceCrossfade(beginCrossfade());
}

bool PitchComparePlayback::beginCrossfade() noexcept {
    const bool selected = requestedCorrected_.load(std::memory_order_relaxed);
    const float target = selected ? 1.0f : 0.0f;
    if (!started_ || crossfadeFrames_ == 0) {
        mix_ = target;
        remaining_ = 0;
    } else if (selected != latchedCorrected_) {
        remaining_ = crossfadeFrames_;
        step_ = (target - mix_) / static_cast<float>(crossfadeFrames_);
    }
    started_ = true;
    latchedCorrected_ = selected;
    return selected;
}

float PitchComparePlayback::advanceCrossfade(bool corrected) noexcept {
    const float previous = mix_;
    if (remaining_ != 0) {
        --remaining_;
        mix_ = remaining_ == 0 ? (corrected ? 1.0f : 0.0f) : std::clamp(mix_ + step_, 0.0f, 1.0f);
    }
    return previous;
}

bool PitchComparePlayback::readAt(std::uint64_t firstFrame, std::span<float> interleaved) noexcept {
    if (!pcm_) return false;
    const auto channels = pcm_->channels();
    const auto count = interleaved.size() / channels;
    if (interleaved.size() % channels != 0 || firstFrame > pcm_->frames() || count > pcm_->frames() - firstFrame)
        return false;
    if (count == 0) return true;

    const bool selected = beginCrossfade();
    const auto firstSample = static_cast<std::size_t>(firstFrame) * channels;
    const auto* original = pcm_->original_.data() + firstSample;
    const auto* corrected = pcm_->corrected_.data() + firstSample;
    auto* destination = interleaved.data();
    std::size_t frame = 0;
    for (; frame < count && remaining_ != 0; ++frame) {
        const auto amount = advanceCrossfade(selected);
        for (std::size_t channel = 0; channel < channels; ++channel) {
            const auto sample = frame * channels + channel;
            destination[sample] = std::lerp(original[sample], corrected[sample], amount);
        }
    }
    const auto firstPureSample = frame * channels;
    if (firstPureSample < interleaved.size())
        std::copy_n((selected ? corrected : original) + firstPureSample,
                    interleaved.size() - firstPureSample, destination + firstPureSample);
    return true;
}

} // namespace adi::audio
