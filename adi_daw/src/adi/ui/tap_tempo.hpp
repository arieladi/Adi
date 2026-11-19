// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
namespace adi::ui {
// Live's Tap Tempo button (Live 12 manual 9.1.2): "click the Tap Tempo button once every beat to have
// the tempo of the Live Set follow the tempo of your clicking"; "Live will respond to tap tempo
// changes immediately; however, the more you tap, the more accurate the detected tempo result will
// be." From the second tap of a run, the tempo is a minute over the mean of the run's last sixteen
// intervals at most. A pause longer than two seconds starts a new run (PROVISIONAL: Live's is to be
// measured). This only times the taps.
class TapTempo {
  public:
    static constexpr std::int64_t kPauseMs = 2000;
    static constexpr std::size_t kIntervals = 16;
    // A tap at `nowMs`: the tempo the run gives so far, or none on a run's first tap.
    std::optional<double> tap(std::int64_t nowMs) noexcept {
        if (count_ == 0 || nowMs <= times_[newest_] || nowMs - times_[newest_] > kPauseMs) {
            count_ = 1;
            newest_ = 0;
            times_[0] = nowMs;
            return std::nullopt;
        }
        newest_ = (newest_ + 1) % times_.size();
        times_[newest_] = nowMs;
        if (count_ < times_.size())
            ++count_;
        const auto oldest = (newest_ + times_.size() + 1 - count_) % times_.size();
        const auto span = static_cast<double>(nowMs - times_[oldest]);
        return 60000. * static_cast<double>(count_ - 1) / span;
    }
    // The taps in the run so far, at most seventeen.
    std::size_t taps() const noexcept { return count_; }

  private:
    std::array<std::int64_t, kIntervals + 1> times_{};
    std::size_t count_ = 0, newest_ = 0;
};
} // namespace adi::ui
