// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

namespace adi::engine::mackie {

enum class VpotProtocol { Mcu, Hui };
enum class VpotMode : std::uint8_t { Dot, BoostCut, Wrap, Spread };
inline constexpr std::size_t vpotCount = 8;
using VpotPacket = std::array<std::uint8_t, 3>;

struct VpotTurn {
    std::size_t strip;
    int ticks;
    bool operator==(const VpotTurn &) const = default;
};

struct VpotRing {
    VpotMode mode = VpotMode::Dot;
    std::uint8_t segment = 0;
    bool centerLed = false;
};

/// Standard MCU and HUI strip rotation are channel-one CCs, with opposite
/// direction bits. The device's raw magnitude is retained for the host.
[[nodiscard]] constexpr std::optional<VpotTurn>
decodeVpotTurn(VpotProtocol protocol, std::span<const std::uint8_t> bytes) noexcept {
    constexpr std::uint8_t controlChange = 0xb0;
    const auto first = protocol == VpotProtocol::Mcu ? 0x10U : 0x40U;
    if (bytes.size() != 3 || bytes[0] != controlChange || bytes[1] < first ||
        bytes[1] >= first + vpotCount || bytes[2] > 0x7f)
        return {};
    const int magnitude = bytes[2] & 0x3f;
    if (magnitude == 0)
        return {};
    const bool positive = ((bytes[2] & 0x40) != 0) == (protocol == VpotProtocol::Hui);
    return VpotTurn{bytes[1] - first, positive ? magnitude : -magnitude};
}

/// Logical segments are the existing wire values (zero turns the ring off).
/// MCU Spread has six widths and a separate center LED; HUI uses its own CC.
[[nodiscard]] constexpr std::optional<VpotPacket>
encodeVpotRing(VpotProtocol protocol, std::size_t strip, VpotRing ring) noexcept {
    const auto mode = static_cast<unsigned>(ring.mode);
    const bool mcu = protocol == VpotProtocol::Mcu;
    const auto limit = mcu && ring.mode == VpotMode::Spread ? 6U : 11U;
    if (strip >= vpotCount || mode > static_cast<unsigned>(VpotMode::Spread) || ring.segment > limit ||
        (!mcu && ring.centerLed))
        return {};
    // Off uses no segment. The mode bits remain useful on MCU when only its
    // center LED is lit.
    const auto value = ring.segment == 0 && !ring.centerLed ? 0U
                         : (mode << 4) | ring.segment | (ring.centerLed ? 0x40U : 0U);
    return VpotPacket{0xb0, static_cast<std::uint8_t>((mcu ? 0x30U : 0x10U) + strip),
                      static_cast<std::uint8_t>(value)};
}

/// Serial output worker. An encoded wire value becomes the feedback baseline
/// only after the containing CC was sent to the surface.
class VpotFeedback {
public:
    explicit VpotFeedback(VpotProtocol protocol) noexcept : protocol_(protocol) {}

    [[nodiscard]] std::optional<VpotPacket> prepare(std::size_t strip, VpotRing ring) const noexcept {
        const auto wire = encodeVpotRing(protocol_, strip, ring);
        if (!wire || sent_[strip] == (*wire)[2])
            return {};
        return wire;
    }

    void sent(VpotPacket packet) noexcept {
        const auto first = protocol_ == VpotProtocol::Mcu ? 0x30U : 0x10U;
        if (packet[0] == 0xb0 && packet[1] >= first && packet[1] < first + vpotCount && packet[2] < 0x80)
            sent_[packet[1] - first] = packet[2];
    }

    void reset() noexcept { sent_ = {}; }

private:
    VpotProtocol protocol_;
    std::array<std::optional<std::uint8_t>, vpotCount> sent_{};
};

} // namespace adi::engine::mackie
