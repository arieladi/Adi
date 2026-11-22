// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include "adi/time_base.hpp"

#include <array>
#include <cstdint>
#include <optional>
#include <span>

namespace adi::sync {

inline constexpr std::int64_t ticksPerClock = 240'240;
inline constexpr std::int64_t ticksPerSongPosition = 6 * ticksPerClock;
inline constexpr std::uint16_t maximumSongPosition = 0x3fff;

enum class ClockKind : std::uint8_t {
    SongPosition = 0xf2, Clock = 0xf8, Start = 0xfa, Continue = 0xfb, Stop = 0xfc
};

struct ClockEvent {
    ClockKind kind;
    // Only SongPosition carries an absolute ADI tick.
    std::int64_t tick = 0;
    bool operator==(const ClockEvent &) const = default;
};

[[nodiscard]] std::optional<ClockEvent> decodeClock(std::span<const std::uint8_t>) noexcept;
[[nodiscard]] std::optional<std::array<std::uint8_t, 3>>
encodeSongPosition(std::int64_t absoluteTick) noexcept;

enum class MtcDirection { Forward, Reverse };
enum class MtcRate { Fps24, Fps25, Drop2997, Fps30 };

struct MtcPosition {
    time_base::Timecode label;
    MtcRate rate;
    MtcDirection direction;
    // The transmitted label's frame within its day, before host chase compensation.
    std::int64_t frame;
};

/// One serial owner for an input port. Clock messages can interleave with
/// MTC. The caller resets on disconnect or a timestamped receive timeout.
class QuarterFrameDecoder {
public:
    [[nodiscard]] std::optional<MtcPosition> receive(std::span<const std::uint8_t>) noexcept;
    [[nodiscard]] bool locked() const noexcept { return locked_; }
    void reset() noexcept;

private:
    std::array<std::uint8_t, 8> nibbles_{};
    MtcDirection direction_ = MtcDirection::Forward;
    unsigned received_ = 0;
    bool locked_ = false;
};

} // namespace adi::sync
