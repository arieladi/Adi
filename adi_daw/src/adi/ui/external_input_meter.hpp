// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include "adi/engine/process.hpp"
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>

namespace adi::ui {

// Device peaks use the callback's packed input order. The picker requests
// updates while its menu is open.
class ExternalInputMeter {
  public:
    void beginReading() noexcept { readers_.fetch_add(1, std::memory_order_relaxed); }
    void endReading() noexcept { readers_.fetch_sub(1, std::memory_order_relaxed); }
    void clear() noexcept {
        for (auto &peak : peaks_)
            peak.store(0.f, std::memory_order_relaxed);
    }

    void measure(const engine::AudioIo &io) noexcept {
        if (readers_.load(std::memory_order_relaxed) == 0 || !io.in || io.frames <= 0)
            return;
        const auto channels = std::min(io.numIn, static_cast<std::int32_t>(peaks_.size()));
        for (std::int32_t index = 0; index < channels; ++index) {
            float peak = 0.f;
            if (const auto *input = io.in[index])
                for (std::int32_t frame = 0; frame < io.frames; ++frame)
                    peak = std::max(peak, std::abs(input[frame]));
            peaks_[static_cast<std::size_t>(index)].store(peak, std::memory_order_relaxed);
        }
    }

    float peak(std::uint32_t channel) const noexcept {
        return channel < peaks_.size() ? peaks_[channel].load(std::memory_order_relaxed) : 0.f;
    }

  private:
    static_assert(std::atomic<float>::is_always_lock_free);
    static_assert(std::atomic<std::uint32_t>::is_always_lock_free);
    std::atomic<std::uint32_t> readers_{0};
    std::array<std::atomic<float>, 64> peaks_{};
};

} // namespace adi::ui
