// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include "adi/engine/events.hpp"
#include <algorithm>
#include <array>
#include <cstdint>

namespace adi::ui {
// The owner records each held key's original pitch and ID. Octave changes
// affect new notes, including when two keys overlap on the same MIDI pitch.
class ComputerMidiKeyboard {
  public:
    static bool recognizes(int key) noexcept {
        const auto letter = lower(key);
        return letter >= 'a' && letter <= 'z' &&
               (pitches[static_cast<std::size_t>(letter - 'a')] >= 0 || letter == 'z' || letter == 'x' || letter == 'c' || letter == 'v');
    }

    template <class Send>
    bool press(int key, Send &&send) {
        if (!recognizes(key))
            return false;
        const auto letter = lower(key);
        const auto index = static_cast<std::size_t>(letter - 'a');
        const auto offset = pitches[index];
        auto &held = held_[index];
        if (held.down)
            return true;
        held.down = true;
        switch (letter) {
        case 'z': base_ = std::max(0, base_ - 12); break;
        case 'x': base_ = std::min(108, base_ + 12); break;
        case 'c': velocity_ = velocity_ == 127 ? 120 : std::max(20, velocity_ - 20); break;
        case 'v': velocity_ = std::min(127, velocity_ + 20); break;
        default:
            held.pitch = static_cast<std::uint16_t>(base_ + offset);
            held.id = (std::uint64_t{1} << 48) | ++serial_;
            send(engine::Event{0, engine::EventType::NoteOn, 0, held.pitch, held.id, 0,
                               static_cast<double>(velocity_) / 127.0});
        }
        return true;
    }

    template <class Down, class Send>
    bool releaseUp(Down &&down, Send &&send) {
        bool released = false;
        for (std::size_t i = 0; i < held_.size(); ++i) {
            auto &held = held_[i];
            if (!held.down || down(static_cast<int>(i) + 'a'))
                continue;
            if (held.id != 0)
                send(engine::Event{0, engine::EventType::NoteOff, 0, held.pitch, held.id, 0, 0.0});
            held = {};
            released = true;
        }
        return released;
    }

    template <class Send>
    void releaseAll(Send &&send) {
        releaseUp([](int) { return false; }, send);
    }

  private:
    static int lower(int key) noexcept { return key >= 'A' && key <= 'Z' ? key + ('a' - 'A') : key; }
    struct Held {
        bool down = false;
        std::uint16_t pitch = 0;
        std::uint64_t id = 0;
    };
    // A S D F G H J K and W E T Y U are the white and black keys.
    static constexpr std::array<int, 26> pitches{
        0, -1, -1, 4, 3, 5, 7, 9, -1, 11, 12, -1, -1, -1, -1, -1, -1, -1, 2, 6, 10, -1, 1, -1, 8, -1,
    };
    static constexpr int defaultVelocity = 100;
    std::array<Held, 26> held_{};
    std::uint64_t serial_ = 0;
    int base_ = 60;
    int velocity_ = defaultVelocity;
};
} // namespace adi::ui
