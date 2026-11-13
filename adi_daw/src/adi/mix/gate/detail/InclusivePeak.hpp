// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include <cstddef>
#include <cstdint>
#include <memory>

namespace adi::mix::gate::detail {
// Inclusive current + preceding window samples, newest equal peak retained.
// Expiry happens before insertion: at most window+1 entries are ever needed.
// Unsigned ages also preserve bounded expiry across counter wrap.
class InclusivePeak final {
  public:
    void prepare(std::uint32_t window) {
        auto replacement = std::make_unique<Entry[]>(static_cast<std::size_t>(window) + 1);
        entries_.swap(replacement);
        window_ = window;
        capacity_ = static_cast<std::size_t>(window) + 1;
        reset();
    }
    void reset() noexcept {
        head_ = size_ = 0;
        position_ = 0;
    }
    float push(float value) noexcept {
        while (size_ && position_ - entries_[head_].position > window_) {
            head_ = next(head_);
            --size_;
        }
        while (size_) {
            auto index = head_ + size_ - 1;
            if (index >= capacity_)
                index -= capacity_;
            if (entries_[index].value > value)
                break;
            --size_;
        }
        auto index = head_ + size_;
        if (index >= capacity_)
            index -= capacity_;
        entries_[index] = {position_++, value};
        ++size_;
        return entries_[head_].value;
    }

  private:
    struct Entry {
        std::uint64_t position;
        float value;
    };
    std::size_t next(std::size_t i) const noexcept { return ++i == capacity_ ? 0 : i; }
    std::unique_ptr<Entry[]> entries_;
    std::size_t head_ = 0, size_ = 0, capacity_ = 0;
    std::uint64_t position_ = 0;
    std::uint32_t window_ = 0;
};
} // namespace adi::mix::gate::detail
