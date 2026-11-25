// SPDX-License-Identifier: GPL-3.0-or-later
#include "adi/dsp/adi_shaper_audio_trigger.hpp"
#include <algorithm>
#include <bit>
#include <cmath>
#include <limits>
#include <new>

namespace adi::dsp {

void AdiShaperAudioTrigger::prepare(double rate) noexcept {
    noteHit_.fill(false);
    phases_.fill(0);
    if (!std::isfinite(rate) || rate < 1 || rate > 1536000) {
        history_.clear();
        latency_ = 0;
        return;
    }
    rate_ = rate;
    // Accommodates the longest detector, smoothing and both shift endpoints.
    const auto count = std::bit_ceil(static_cast<std::size_t>(std::ceil(.035 * rate)) + 4);
    try {
        history_.assign(count, {});
    } catch (const std::bad_alloc&) {
        history_.clear();
        latency_ = 0;
        return;
    }
    mask_ = count - 1;
    release_ = static_cast<float>(std::exp(-1.0 / (.020 * rate_)));
    separation_ = static_cast<int>(std::ceil(.012 * rate_));
    clear();
    timing();
}

void AdiShaperAudioTrigger::clear() noexcept {
    // Skip the old ring's timestamps. Changing modes on the audio thread is constant work.
    tick_ += history_.size();
    filled_ = 0;
    peak_ = 0;
    hold_ = 0;
    hit_ = false;
    age_ = 0;
}

void AdiShaperAudioTrigger::mode(std::size_t band, int value) noexcept {
    if (modes_[band] == value) return;
    modes_[band] = value;
    if (useNotes_[band]) {
        noteHit_[band] = false;
        phases_[band] = 0;
    }
    timing();
}

void AdiShaperAudioTrigger::useNotes(std::size_t band, bool enabled) noexcept {
    if (useNotes_[band] == enabled) return;
    useNotes_[band] = enabled;
    noteHit_[band] = false;
    phases_[band] = 0;
    timing();
}

void AdiShaperAudioTrigger::restart() noexcept {
    hit_ = true;
    age_ = 0;
    for (std::size_t i = 0; i < phases_.size(); ++i)
        if (!useNotes_[i]) phases_[i] = 0;
}

void AdiShaperAudioTrigger::noteOn() noexcept {
    for (std::size_t i = 0; i < phases_.size(); ++i) {
        if (!useNotes_[i] || modes_[i] == Sync) continue;
        noteHit_[i] = true;
        phases_[i] = 0;
    }
}

void AdiShaperAudioTrigger::endpoint(std::size_t band, double value) noexcept {
    ends_[band] = value >= 1 ? UINT64_MAX : static_cast<std::uint64_t>(value * 0x1p64);
}

void AdiShaperAudioTrigger::detector(int profile, bool smoothing) noexcept {
    profile_ = profile;
    smoothing_ = smoothing;
    timing();
}

void AdiShaperAudioTrigger::timing() noexcept {
    bool audioActive = false;
    notesActive_ = false;
    for (std::size_t i = 0; i < modes_.size(); ++i) {
        if (modes_[i] == Sync) continue;
        if (useNotes_[i]) notesActive_ = true;
        else audioActive = true;
    }
    const int base = static_cast<int>(std::lround((profile_ == 2 ? .024 : .014) * rate_));
    smoothingFrames_ = smoothing_ ? static_cast<int>(rate_ * .006) + 1 : 0;
    const int next = audioActive && !history_.empty() ? base + smoothingFrames_ : 0;
    if (next != latency_) clear();
    latency_ = next;
    lead_ = std::max(0, next + (profile_ == 2 ? 1 : -1)
        + static_cast<int>(std::lround((2.0 * shift_ - 1.0) * .004 * rate_)));
}

void AdiShaperAudioTrigger::threshold(float value) noexcept { threshold_ = value; level(); }
void AdiShaperAudioTrigger::detail(float value) noexcept { detail_ = value; level(); }
void AdiShaperAudioTrigger::shift(float value) noexcept { shift_ = value; timing(); }
void AdiShaperAudioTrigger::level() noexcept {
    onsetLevel_ = detail_ > 0 ? std::max({threshold_, .1f, .05f / detail_}) : std::numeric_limits<float>::infinity();
}

bool AdiShaperAudioTrigger::tick(float& left, float& right) noexcept {
    const float amplitude = std::max(std::abs(left), std::abs(right));
    const float previous = peak_;
    peak_ = std::max(amplitude, release_ * peak_);
    if (hold_ > 0) --hold_;
    if (hold_ == 0 && amplitude > onsetLevel_ && amplitude - previous > onsetLevel_ * .5f) {
        history_[(tick_ + static_cast<unsigned>(lead_)) & mask_].event = tick_ + static_cast<unsigned>(lead_) + 1;
        hold_ = separation_;
    }
    auto& slot = history_[tick_ & mask_];
    const bool event = slot.event == tick_ + 1;
    slot.left = left;
    slot.right = right;
    const auto& delayed = history_[(tick_ - static_cast<unsigned>(latency_)) & mask_];
    if (filled_ >= static_cast<unsigned>(latency_)) {
        left = delayed.left;
        right = delayed.right;
    } else {
        ++filled_;
        left = right = 0;
    }
    ++tick_;
    return event;
}

AdiShaperAudioTrigger::Clock AdiShaperAudioTrigger::clock(std::size_t band, std::uint64_t synced,
                                                          std::uint64_t step) const noexcept {
    const bool hit = useNotes_[band] ? noteHit_[band] : hit_;
    if (modes_[band] == Sync || (!hit && modes_[band] == Loop)) return {synced, 0, UINT64_MAX};
    if (modes_[band] == Loop) return {phases_[band], 0, UINT64_MAX};
    const auto end = ends_[band];
    if (!hit || phases_[band] >= end) return {end, end, 0};
    if (!step) return {phases_[band], end, UINT64_MAX};
    const auto remaining = end - phases_[band];
    const auto endFrame = remaining / step + static_cast<std::uint64_t>(remaining % step != 0);
    return {phases_[band], end, endFrame};
}

void AdiShaperAudioTrigger::advance(std::size_t frames, const std::array<std::uint64_t, 3>& steps) noexcept {
    if (!hit_ && !notesActive_) return;
    for (std::size_t i = 0; i < phases_.size(); ++i)
        if (modes_[i] != Sync && (useNotes_[i] ? noteHit_[i] : hit_))
            phases_[i] = clock(i, phases_[i], steps[i]).at(steps[i], frames);
    if (hit_) age_ += frames;
}

float AdiShaperAudioTrigger::fade(std::size_t band, std::size_t offset) const noexcept {
    if (useNotes_[band] || !hit_ || !smoothing_ || modes_[band] == Sync || age_ + offset >= static_cast<unsigned>(smoothingFrames_))
        return 1;
    return static_cast<float>(age_ + offset) / static_cast<float>(smoothingFrames_);
}

} // namespace adi::dsp
