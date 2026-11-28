// SPDX-License-Identifier: GPL-3.0-or-later
#include "adi/engine/transport.hpp"
#include <algorithm>
#include <limits>
namespace adi::engine {
void Transport::locate(std::int64_t sample) noexcept {
    countInLeft_ = 0;
    position_ = std::max<std::int64_t>(0, sample);
    streamed_ = 0;
    firstWrap_ = -1;
    retimedLeadInWrap_ = -1;
    ++revision_;
}
void Transport::loop(std::int64_t begin, std::int64_t end, bool enabled) noexcept {
    ++revision_;
    streamed_ = 0;
    firstWrap_ = -1;
    retimedLeadInWrap_ = -1;
    begin_ = std::max<std::int64_t>(0, begin); end_ = end;
    loopEnabled_ = enabled && end_ > begin_;
}
void Transport::retimeLoop(std::int64_t begin, std::int64_t end, bool enabled) noexcept {
    const bool wasLooping = playing_ && looping();
    begin_ = std::max<std::int64_t>(0, begin);
    end_ = end;
    loopEnabled_ = enabled && end_ > begin_;
    const bool approachingNewWrap = retimedLeadInWrap_ > streamed_;
    if (wasLooping && loopEnabled_ && position_ >= end_) {
        // A shortened loop can fold an already-rendered region. Its first
        // crossing lies at the new End, before the current folded phase.
        const auto crossing = streamed_ - std::min(streamed_, position_ - end_);
        if (firstWrap_ < 0) firstWrap_ = crossing;
        if (approachingNewWrap) retimedLeadInWrap_ = crossing;
        position_ = begin_ + (position_ - begin_) % (end_ - begin_);
    } else if (looping() && (approachingNewWrap || (firstWrap_ >= 0 && position_ < begin_))) {
        // Moving Begin ahead leaves a linear run through the new window.
        // Crossing Begin alone must not turn delayed lead-in into a lap.
        retimedLeadInWrap_ = streamed_ + std::min(end_ - position_,
                            std::numeric_limits<std::int64_t>::max() - streamed_);
    }
}
std::int64_t Transport::delayedSample(std::int32_t offset,std::int32_t delay) const noexcept {
    const auto behind = static_cast<std::int64_t>(std::max(0, delay)) - std::max(0, offset);
    if (behind > streamed_) return -1;
    return std::max<std::int64_t>(-1, sampleBehind(behind));
}
std::int64_t Transport::inputSample(std::int32_t offset, std::int32_t arrival) const noexcept {
    return sampleBehind(static_cast<std::int64_t>(arrival) - (playing_ ? offset : 0));
}
std::int64_t Transport::sampleBehind(std::int64_t behind) const noexcept {
    if (behind <= 0)
        return sampleAt(-behind);
    const auto current = sampleAt(0);
    const auto first = retimedLeadInWrap_ >= 0 ? retimedLeadInWrap_ : firstWrap_;
    if (!looping() || first < 0 || first > streamed_ || current < begin_)
        return current - behind;

    const auto sinceFirstWrap = streamed_ - first;
    if (behind > sinceFirstWrap)
        return end_ - (behind - sinceFirstWrap);
    const auto length = end_ - begin_;
    const auto phase = current - begin_;
    const auto back = behind % length;
    return begin_ + (phase >= back ? phase - back : length - (back - phase));
}
std::int32_t Transport::inputContiguousFrames(std::int64_t sample, std::int32_t count) const noexcept {
    if (!playing_ || !looping() || sample < end_ - count)
        return count;
    return static_cast<std::int32_t>(end_ - sample);
}
std::int64_t Transport::sampleAt(std::int64_t offset) const noexcept {
    offset = std::max<std::int64_t>(0, offset);
    if (looping()) {
        const auto until = end_ - position_;
        if (offset >= until) return begin_ + (offset - until) % (end_ - begin_);
    }
    return position_ + std::min(offset, std::numeric_limits<std::int64_t>::max() - position_);
}
std::int64_t Transport::contiguousFrames(std::int64_t offset, std::int64_t count) const noexcept {
    count = std::max<std::int64_t>(0, count);
    return looping() ? std::min(count, end_ - sampleAt(offset)) : count;
}
void Transport::countIn(std::int64_t frames) noexcept {
    countInTotal_ = countInLeft_ = std::max<std::int64_t>(0, frames);
    playing_ = countInLeft_ == 0;
    streamed_ = 0;
    firstWrap_ = -1;
    retimedLeadInWrap_ = -1;
    ++revision_;
}
void Transport::advance(std::int32_t frames) noexcept {
    if (frames > 0 && countInLeft_ > 0) {
        const auto consumed = std::min<std::int64_t>(frames, countInLeft_);
        countInLeft_ -= consumed;
        frames -= static_cast<std::int32_t>(consumed);
        if (countInLeft_ > 0)
            return;
        playing_ = true;
        streamed_ = 0;
        firstWrap_ = -1;
        retimedLeadInWrap_ = -1;
        ++revision_;
    }
    if (playing_ && frames > 0) {
        if (looping() && firstWrap_ < 0) {
            const auto toWrap = end_ - position_;
            if (frames >= toWrap)
                firstWrap_ = streamed_ + std::min(toWrap,
                                                std::numeric_limits<std::int64_t>::max() - streamed_);
        }
        position_ = sampleAt(frames);
        streamed_+=std::min<std::int64_t>(frames,std::numeric_limits<std::int64_t>::max()-streamed_);
    }
}
}
