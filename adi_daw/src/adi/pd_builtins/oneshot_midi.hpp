// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "adi/engine/mpe_output.hpp"
#include <array>
#include <cmath>

namespace adi::device {
// The real-Pd fallback sees MIDI wire values, not ADI's original note ids and
// doubles. One id per channel/key is the recoverable identity; overlapping
// same-key notes on one channel cannot be reconstructed from MIDI 1.0. A new
// note explicitly releases the previous identity before replacing it, including
// when OneShot's Retrigger control is off, so its loop cannot become orphaned.
// Events are ordered exactly as Pd delivers them and apply at the next tick.
class OneShotPdMidi {
  public:
    static constexpr std::size_t capacity = 512;
    void reset() noexcept {
        ids_ = {};
        bends_ = {};
        count_ = 0;
        serial_ = 0;
        dropped_ = 0;
        panic_ = false;
    }
    void note(float key, float velocity, float channel) noexcept {
        if (!integer(key, 0, 127) || !integer(velocity, 0, 127) ||
            !integer(channel, 1, 16)) return;
        const auto c = static_cast<std::size_t>(channel - 1);
        const auto k = static_cast<std::size_t>(key);
        if (panic_) { ++dropped_; return; }
        auto& id = ids_[c][k];
        if (velocity > 0) {
            if (id != 0 && !push({0, engine::EventType::NoteOff,
                    static_cast<std::uint8_t>(c), static_cast<std::uint16_t>(k), id, 0, 0}))
                return;
            if (++serial_ == 0) ++serial_;
            id = serial_;
            engine::Event on{0, engine::EventType::NoteOn, static_cast<std::uint8_t>(c),
                             static_cast<std::uint16_t>(k), id, 0,
                             static_cast<double>(velocity) / 127.};
            if (push(on) && bends_[c] != 0) expression(c, id);
        } else if (id != 0) {
            const auto released = id;
            id = 0;
            push({0, engine::EventType::NoteOff, static_cast<std::uint8_t>(c),
                  static_cast<std::uint16_t>(k), released, 0, 0});
        }
    }
    void bend(float value, float channel) noexcept {
        if (!integer(value, 0, 16383) || !integer(channel, 1, 16)) return;
        const auto c = static_cast<std::size_t>(channel - 1);
        // The host MPE router declares this +/-48-semitone range. Keep its
        // asymmetric positive/negative MIDI endpoints (8191 versus 8192).
        const double centered = static_cast<double>(value) - 8192.;
        bends_[c] = engine::kMpeOutBendSemitones * centered /
                    (centered < 0 ? 8192. : 8191.);
        for (const auto id : ids_[c]) if (id != 0) expression(c, id);
    }
    [[nodiscard]] engine::EventSpan events() const noexcept {
        return {events_.data(), static_cast<std::int32_t>(count_)};
    }
    [[nodiscard]] bool needsVoiceReset() const noexcept { return panic_; }
    [[nodiscard]] std::uint64_t dropped() const noexcept { return dropped_; }
    void consumed() noexcept { count_ = 0; panic_ = false; }

  private:
    static bool integer(float x, float lo, float hi) noexcept {
        return std::isfinite(x) && x >= lo && x <= hi && std::floor(x) == x;
    }
    bool push(const engine::Event& e) noexcept {
        if (panic_ || count_ == events_.size()) {
            ++dropped_;
            // A dropped release must not leave a voice sounding indefinitely.
            // Discard this tick and request a bounded reset in perform.
            panic_ = true;
            count_ = 0;
            ids_ = {};
            return false;
        }
        events_[count_++] = e;
        return true;
    }
    void expression(std::size_t channel, std::uint64_t id) noexcept {
        push({0, engine::EventType::NoteExpression, static_cast<std::uint8_t>(channel),
              0, id, 0, bends_[channel]});
    }
    std::array<engine::Event, capacity> events_{};
    std::array<std::array<std::uint64_t, 128>, 16> ids_{};
    std::array<double, 16> bends_{};
    std::size_t count_ = 0;
    std::uint64_t serial_ = 0, dropped_ = 0;
    bool panic_ = false;
};
} // namespace adi::device
