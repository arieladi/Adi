// SPDX-License-Identifier: GPL-3.0-or-later
#include "adi/dsp/adi_shaper_time.hpp"
#include <algorithm>
#include <cmath>
#include <new>

namespace adi::dsp {
namespace {
float timeSample(float sample) noexcept {
    return std::isfinite(sample) ? sample : 0.0f;
}
} // namespace

bool AdiShaperTime::prepare() noexcept {
    if (!history_) {
        history_.reset(new (std::nothrow) Frame[capacity]{});
        if (!history_) return false;
        // Touch each page before the first audio callback. Zero-initialized
        // heap storage may initially share read-only demand-zero pages.
        volatile float* page = nullptr;
        for (std::size_t n = 0; n < capacity; n += 512) {
            page = &history_[n][0];
            *page = 0.0f;
        }
    }
    reset();
    return true;
}

void AdiShaperTime::reset() noexcept {
    next_ = valid_ = 0;
}

bool AdiShaperTime::processFixedBlock(const float* inL, const float* inR, float* outL, float* outR,
                                     std::size_t frames, double delayFrames) noexcept {
    if (!history_ || !(delayFrames >= 1.0 && delayFrames <= static_cast<double>(maximumDelay))) return false;
    const auto age = static_cast<std::size_t>(delayFrames);
    const auto read = (next_ - age) & (capacity - 1);
    if (age > valid_ || frames > capacity - next_ || frames > capacity - read) return false;
    for (std::size_t i = 0; i < frames; ++i) {
        const Frame past = history_[read + i];
        history_[next_ + i] = {timeSample(inL[i]), timeSample(inR[i])};
        outL[i] = past[0];
        outR[i] = past[1];
    }
    next_ = (next_ + frames) & (capacity - 1);
    valid_ = std::min(capacity, valid_ + frames);
    return true;
}

void AdiShaperTime::processStereo(float& left, float& right, double delayFrames,
                                  bool interpolate) noexcept {
    if (!history_) {
        if (delayFrames != 0.0) left = right = 0.0f;
        return;
    }
    constexpr std::size_t mask = capacity - 1;
    const auto current = next_;
    history_[current] = {timeSample(left), timeSample(right)};
    next_ = (current + 1) & mask;
    valid_ = std::min(valid_ + 1, capacity);
    if (delayFrames == 0.0) return;
    // Also rejects non-finite offsets before the conversion to an index.
    if (!(delayFrames >= 0.0 && delayFrames <= static_cast<double>(maximumDelay))) {
        left = right = 0.0f;
        return;
    }
    const auto whole = static_cast<std::size_t>(delayFrames);
    if (whole >= valid_) {
        left = right = 0.0f;
        return;
    }
    const auto slot = (current - whole) & mask;
    const auto& a = history_[slot];
    const auto fraction = static_cast<float>(delayFrames - static_cast<double>(whole));
    if (!interpolate || fraction == 0.0f) {
        left = a[0];
        right = a[1];
        return;
    }
    const Frame b = whole + 1 < valid_ ? history_[(slot - 1) & mask] : Frame{};
    left = a[0] + fraction * (b[0] - a[0]);
    right = a[1] + fraction * (b[1] - a[1]);
}

} // namespace adi::dsp
