// SPDX-License-Identifier: GPL-3.0-or-later
#include "adi/engine/transport.hpp"
#include <algorithm>
#include <limits>
namespace adi::engine {
void Transport::locate(std::int64_t sample) noexcept { position_ = std::max<std::int64_t>(0, sample); streamed_=0;wrapped_=false;++revision_; }
void Transport::loop(std::int64_t begin, std::int64_t end, bool enabled) noexcept {
    ++revision_;
    streamed_=0;wrapped_=false;
    begin_ = std::max<std::int64_t>(0, begin); end_ = end;
    looping_ = enabled && end_ > begin_;
}
std::int64_t Transport::delayedSample(std::int32_t offset,std::int32_t delay) const noexcept {
    const auto behind=static_cast<std::int64_t>(std::max(0,delay))-std::max(0,offset);
    if(behind<=0)return sampleAt(-behind);
    if(streamed_<behind)return -1;
    if(looping_ && wrapped_) {
        const auto length=end_-begin_,phase=sampleAt(0)-begin_,back=behind%length;
        return begin_+(phase>=back?phase-back:length-(back-phase));
    }
    return position_>=behind?position_-behind:-1;
}
std::int64_t Transport::sampleAt(std::int64_t offset) const noexcept {
    offset = std::max<std::int64_t>(0, offset);
    if (looping_) {
        const auto length = end_ - begin_;
        if (position_ >= end_) {
            const auto a = (position_ - begin_) % length, b = offset % length;
            return begin_ + (a >= length-b ? a-(length-b) : a+b);
        }
        const auto until = end_ - position_;
        if (offset >= until) return begin_ + (offset - until) % length;
    }
    return position_ + std::min(offset, std::numeric_limits<std::int64_t>::max() - position_);
}
std::int64_t Transport::contiguousFrames(std::int64_t offset, std::int64_t count) const noexcept {
    count = std::max<std::int64_t>(0, count);
    return looping_ ? std::min(count, end_ - sampleAt(offset)) : count;
}
void Transport::advance(std::int32_t frames) noexcept {
    if (playing_ && frames > 0) {
        if(looping_ && (position_>=end_ || frames>=end_-position_))wrapped_=true;
        position_ = sampleAt(frames);
        streamed_+=std::min<std::int64_t>(frames,std::numeric_limits<std::int64_t>::max()-streamed_);
    }
}
}
