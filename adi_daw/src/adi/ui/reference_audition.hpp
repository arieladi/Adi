// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include <cstdint>
namespace adi::ui {
// The ★ Reference's [REF] button and its key, Shift+R (ADR-0209 d9): a tap under 200 ms latches the
// reference on, or off when it was latched; a hold auditions it for as long as it is held. The
// 200 ms is a fixed value, no setting (ADR-0209 Q9). What sounds is the engine's: the reference on
// the cue path, the Main bypassed (window log, N23); this only decides whether it should.
class ReferenceAudition {
  public:
    static constexpr std::int64_t kTapMs = 200;
    // The key or the button goes down; a repeat while it is down changes nothing.
    void press(std::int64_t nowMs) noexcept {
        if (down_)
            return;
        down_ = true;
        pressedAt_ = nowMs;
        wasLatched_ = latched_;
        latched_ = false;
    }
    // It comes up: a tap latches, or unlatches what was latched; a hold lets go.
    void release(std::int64_t nowMs) noexcept {
        if (!down_)
            return;
        down_ = false;
        latched_ = nowMs - pressedAt_ < kTapMs && !wasLatched_;
    }
    bool on() const noexcept { return down_ || latched_; }
    bool held() const noexcept { return down_; }
    bool latched() const noexcept { return latched_; }

  private:
    bool down_ = false, latched_ = false, wasLatched_ = false;
    std::int64_t pressedAt_ = 0;
};
} // namespace adi::ui
