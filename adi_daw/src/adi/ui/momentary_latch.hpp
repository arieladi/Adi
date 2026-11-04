// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include <cstdint>
#include <optional>
namespace adi::ui {
// Live's momentary latching (manual §42.25): "Some shortcuts can be momentarily latched… hold down
// the shortcut key and briefly toggle the shortcut action. After releasing the key, Live's UI will
// return to its previous state. Momentary latching becomes available after holding down a shortcut
// key for about 500 ms." ADI's latching keys: S (Solo Selected), F1…F8 (the Track Activators) and Z
// (Zoom to Selection); Live's A, B and Tab wait on Automation Mode, Draw Mode and the Session View.
// This only times the key and keeps what to go back from: a first press acts, a repeat while the key
// is held does nothing, and the release after a hold of 500 ms or more says to go back.
class MomentaryLatch {
  public:
    static constexpr std::int64_t kHoldMs = 500;
    // What a long hold goes back from: the command the press ran, and the undo step it made (0 when
    // it made none, as a zoom makes none).
    struct Held {
        int key = 0, command = 0;
        std::int64_t step = 0;
    };
    // A key goes down for a command. True on its first press, which acts; false for a repeat while
    // it is held. Another key pressed while one is held takes over: the first stays as it is.
    bool press(int key, int command, std::int64_t nowMs) noexcept {
        if (held_.key == key)
            return false;
        held_ = {key, command, 0};
        pressedAt_ = nowMs;
        return true;
    }
    // The step the press made.
    void made(std::int64_t step) noexcept { held_.step = step; }
    // The key comes up: what to go back from, after a hold of 500 ms or more; a tap stays.
    std::optional<Held> release(int key, std::int64_t nowMs) noexcept {
        if (held_.key == 0 || held_.key != key)
            return std::nullopt;
        const auto was = held_;
        held_ = {};
        if (nowMs - pressedAt_ < kHoldMs)
            return std::nullopt;
        return was;
    }
    // The key was let go unseen: it stays as it is, and nothing goes back.
    void letGo() noexcept { held_ = {}; }
    bool held() const noexcept { return held_.key != 0; }
    int key() const noexcept { return held_.key; }

  private:
    Held held_;
    std::int64_t pressedAt_ = 0;
};
} // namespace adi::ui
