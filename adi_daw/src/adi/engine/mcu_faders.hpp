// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

namespace adi::engine::mcu {

inline constexpr std::size_t faderCount = 9; // eight channels and the Main fader
inline constexpr std::uint16_t maximumFader = 0x3fff;
using MidiPacket = std::array<std::uint8_t, 3>;
enum class FaderEventKind : std::uint8_t { Move, Touch, Release };
struct FaderEvent {
    FaderEventKind kind = FaderEventKind::Move;
    std::size_t strip = 0;
    std::uint16_t position = 0;
    bool operator==(const FaderEvent &) const = default;
};

[[nodiscard]] constexpr std::optional<MidiPacket>
encodeFader(std::size_t strip, std::uint16_t position) noexcept {
    if (strip >= faderCount || position > maximumFader)
        return {};
    return MidiPacket{static_cast<std::uint8_t>(0xe0 + strip),
                      static_cast<std::uint8_t>(position & 0x7f),
                      static_cast<std::uint8_t>(position >> 7)};
}

/// Complete MIDI messages on an MCU port. Handshakes, buttons and other
/// controller messages are handled by the host's protocol dispatcher.
[[nodiscard]] constexpr std::optional<FaderEvent>
decodeFader(std::span<const std::uint8_t> bytes) noexcept {
    if (bytes.size() != 3 || bytes[1] > 127 || bytes[2] > 127)
        return {};
    const auto status = bytes[0];
    if (status >= 0xe0 && status < 0xe0 + faderCount) {
        return FaderEvent{FaderEventKind::Move, static_cast<std::size_t>(status - 0xe0),
                          static_cast<std::uint16_t>(bytes[1] | (bytes[2] << 7))};
    }
    constexpr std::uint8_t firstTouch = 0x68;
    if ((status == 0x80 || status == 0x90) && bytes[1] >= firstTouch &&
        bytes[1] < firstTouch + faderCount) {
        const bool down = status == 0x90 && bytes[2] != 0;
        return FaderEvent{down ? FaderEventKind::Touch : FaderEventKind::Release,
                          static_cast<std::size_t>(bytes[1] - firstTouch)};
    }
    return {};
}

/// One serial surface-port owner. Input and feedback share the same
/// per-fader state. No MIDI I/O or host parameter mutation occurs here.
class FaderFeedback {
public:
    [[nodiscard]] std::optional<FaderEvent> receive(std::span<const std::uint8_t> packet) noexcept {
        const auto event = decodeFader(packet);
        if (!event)
            return {};
        auto &state = states_[event->strip];
        if (event->kind != FaderEventKind::Move) {
            const bool touched = event->kind == FaderEventKind::Touch;
            if (touched == state.touched)
                return {};
            state.touched = touched;
            state.commanded.reset();
            state.outstanding = 0;
            return event;
        }
        if (!state.touched) {
            for (std::size_t i = 0; i < state.outstanding; ++i) {
                if (state.echoes[i] != event->position)
                    continue;
                // Older commands can echo after a newer feedback update.
                // Retire only the matching value.
                for (std::size_t j = i + 1; j < state.outstanding; ++j)
                    state.echoes[j - 1] = state.echoes[j];
                --state.outstanding;
                if (state.commanded == event->position)
                    state.observed = event->position;
                return {};
            }
        }
        state.observed = event->position;
        state.commanded.reset();
        state.outstanding = 0;
        return event;
    }

    [[nodiscard]] std::optional<MidiPacket> motor(std::size_t strip, std::uint16_t position) noexcept {
        const auto wire = encodeFader(strip, position);
        if (!wire)
            return {};
        auto &state = states_[strip];
        if (state.touched || (state.commanded ? state.commanded : state.observed) == position)
            return {};
        if (state.outstanding == state.echoes.size()) {
            for (std::size_t i = 1; i < state.echoes.size(); ++i)
                state.echoes[i - 1] = state.echoes[i];
            --state.outstanding;
        }
        state.echoes[state.outstanding++] = position;
        state.commanded = position;
        return wire;
    }

private:
    struct State {
        std::optional<std::uint16_t> observed, commanded;
        std::array<std::uint16_t, 8> echoes{};
        std::size_t outstanding = 0;
        bool touched = false;
    };
    std::array<State, faderCount> states_{};
};

} // namespace adi::engine::mcu
