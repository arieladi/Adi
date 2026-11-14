// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <stdexcept>

namespace adi::mix::compressor::detail {

// Inclusive maximum over the current sample and the preceding window samples.
// prepare may allocate; reset/push do not. The serialized caller supplies finite
// samples. Equal peaks keep the newest endpoint. Unsigned ages survive counter
// wrap because every retained entry is within the bounded prepared window.
// Work is amortized constant; one push may retire the bounded retained queue.
class HoldPeak final {
  public:
    void prepare(std::uint32_t window) {
        const auto n = static_cast<std::size_t>(window);
        if (n == std::numeric_limits<std::size_t>::max())
            throw std::length_error("Hold window too large");
        auto replacement = std::make_unique<Entry[]>(n + 1);
        entries_.swap(replacement);
        capacity_ = n + 1;
        window_ = window;
        reset();
    }
    void reset() noexcept {
        head_ = size_ = 0;
        position_ = 0;
    }
    float push(float value) noexcept {
        if (!entries_)
            return value;
        const auto capacity = capacity_;
        while (size_ != 0 && position_ - entries_[head_].position > window_) {
            if (++head_ == capacity)
                head_ = 0;
            --size_;
        }
        while (size_ != 0) {
            auto back = head_ + size_ - 1;
            if (back >= capacity)
                back -= capacity;
            if (entries_[back].value > value)
                break;
            --size_;
        }
        auto tail = head_ + size_;
        if (tail >= capacity)
            tail -= capacity;
        entries_[tail] = {position_, value};
        ++size_;
        ++position_;
        return entries_[head_].value;
    }

  private:
    struct Entry {
        std::uint64_t position = 0;
        float value = 0;
    };
    std::unique_ptr<Entry[]> entries_;
    std::uint64_t position_ = 0;
    std::uint32_t window_ = 0;
    std::size_t head_ = 0, size_ = 0, capacity_ = 0;
};

} // namespace adi::mix::compressor::detail
