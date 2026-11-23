// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

namespace adi::engine::hui {

inline constexpr std::size_t faderCount = 8;
inline constexpr std::uint16_t maximumFader = 0x3fff;
using MidiPacket = std::array<std::uint8_t, 3>;
using FaderPackets = std::array<MidiPacket, 2>;

enum class FaderEventKind : std::uint8_t { Move, Touch, Release };
struct FaderEvent {
    FaderEventKind kind;
    std::size_t strip;
    std::uint16_t position = 0;
    bool operator==(const FaderEvent &) const = default;
};

[[nodiscard]] constexpr std::optional<FaderPackets>
encodeFader(std::size_t strip, std::uint16_t position) noexcept {
    if (strip >= faderCount || position > maximumFader)
        return {};
    return FaderPackets{MidiPacket{0xb0, static_cast<std::uint8_t>(strip),
                                  static_cast<std::uint8_t>(position >> 7)},
                        MidiPacket{0xb0, static_cast<std::uint8_t>(0x20 + strip),
                                  static_cast<std::uint8_t>(position & 0x7f)}};
}

/// One standard eight-strip HUI connection on MIDI channel 1.
class FaderDecoder {
public:
    [[nodiscard]] std::optional<FaderEvent>
    receive(std::span<const std::uint8_t> packet) noexcept {
        if (packet.size() != 3 || packet[0] != 0xb0 || packet[1] > 127 || packet[2] > 127)
            return {};
        const auto controller = packet[1], value = packet[2];
        if (controller < faderCount) {
            coarse_[controller] = value;
            return {};
        }
        if (controller >= 0x20 && controller < 0x20 + faderCount) {
            const auto strip = static_cast<std::size_t>(controller - 0x20);
            auto &coarse = coarse_[strip];
            if (!coarse)
                return {};
            const auto position = static_cast<std::uint16_t>((*coarse << 7) | value);
            coarse.reset();
            return FaderEvent{FaderEventKind::Move, strip, position};
        }
        if (controller == 0x0f)
            zone_ = value;
        if (controller == 0x2f && zone_ && *zone_ < faderCount) {
            if (value == 0x40)
                return FaderEvent{FaderEventKind::Touch, *zone_};
            if (value == 0x00)
                return FaderEvent{FaderEventKind::Release, *zone_};
        }
        return {};
    }

    void reset() noexcept {
        coarse_ = {};
        zone_.reset();
    }

private:
    std::array<std::optional<std::uint8_t>, faderCount> coarse_{};
    std::optional<std::uint8_t> zone_;
};

/// The host polls in monotonic milliseconds and sends a returned packet.
/// Late polls produce a single ping. Reconnection starts a fresh cadence.
class Heartbeat {
public:
    [[nodiscard]] std::optional<MidiPacket> poll(std::uint64_t nowMs) noexcept {
        if (lastPing_ && nowMs >= *lastPing_ && nowMs - *lastPing_ < 1000)
            return {};
        lastPing_ = nowMs;
        return MidiPacket{0x90, 0x00, 0x00};
    }

    void reset() noexcept { lastPing_.reset(); }

private:
    std::optional<std::uint64_t> lastPing_;
};

} // namespace adi::engine::hui
