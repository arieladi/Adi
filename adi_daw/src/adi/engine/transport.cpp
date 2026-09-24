// SPDX-License-Identifier: GPL-3.0-or-later
#include "adi/engine/transport.hpp"
#include <algorithm>
#include <limits>
namespace adi::engine {
void Transport::locate(std::int64_t sample) noexcept { position_ = std::max<std::int64_t>(0, sample); }
void Transport::loop(std::int64_t begin, std::int64_t end, bool enabled) noexcept {
    begin_ = std::max<std::int64_t>(0, begin); end_ = end;
    looping_ = enabled && end_ > begin_;
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
    if (playing_ && frames > 0) position_ = sampleAt(frames);
}
}
