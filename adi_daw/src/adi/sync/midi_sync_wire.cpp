// SPDX-License-Identifier: AGPL-3.0-or-later
#include "adi/sync/midi_sync_wire.hpp"

namespace adi::sync {

std::optional<ClockEvent> decodeClock(std::span<const std::uint8_t> bytes) noexcept {
    if (bytes.empty())
        return {};
    const auto kind = static_cast<ClockKind>(bytes.front());
    switch (kind) {
    case ClockKind::SongPosition: {
        if (bytes.size() != 3 || bytes[1] >= 0x80 || bytes[2] >= 0x80)
            return {};
        const auto position = (bytes[2] << 7) | bytes[1];
        return ClockEvent{kind, position * ticksPerSongPosition};
    }
    case ClockKind::Clock:
    case ClockKind::Start:
    case ClockKind::Continue:
    case ClockKind::Stop:
        if (bytes.size() == 1)
            return ClockEvent{kind};
        break;
    }
    return {};
}

std::optional<std::array<std::uint8_t, 3>> encodeSongPosition(std::int64_t tick) noexcept {
    if (tick < 0 || tick % ticksPerSongPosition != 0)
        return {};
    const auto position = tick / ticksPerSongPosition;
    if (position > maximumSongPosition)
        return {};
    return std::array<std::uint8_t, 3>{0xf2, static_cast<std::uint8_t>(position & 0x7f),
                                       static_cast<std::uint8_t>(position >> 7)};
}

std::optional<MtcPosition> QuarterFrameDecoder::receive(std::span<const std::uint8_t> packet) noexcept {
    if (packet.empty() || packet.front() != 0xf1)
        return {};
    if (packet.size() != 2 || packet[1] >= 0x80) {
        reset();
        return {};
    }
    constexpr std::array<std::uint8_t, 8> masks{0x0f, 0x01, 0x0f, 0x03, 0x0f, 0x03, 0x0f, 0x07};
    const auto piece = static_cast<unsigned>(packet[1] >> 4);
    const auto nibble = static_cast<std::uint8_t>(packet[1] & 0x0f);
    if ((nibble & ~masks[piece]) != 0) {
        reset();
        return {};
    }
    const auto expected = direction_ == MtcDirection::Forward ? received_ : 7 - received_;
    if ((received_ != 0 || locked_) && piece != expected)
        reset();
    if (received_ == 0) {
        if (piece != 0 && piece != 7)
            return {};
        direction_ = piece == 0 ? MtcDirection::Forward : MtcDirection::Reverse;
    }
    nibbles_[piece] = nibble;
    ++received_;
    if (received_ < 8)
        return {};
    received_ = 0;

    const time_base::Timecode label{((nibbles_[7] & 1) << 4) | nibbles_[6],
                                    (nibbles_[5] << 4) | nibbles_[4],
                                    (nibbles_[3] << 4) | nibbles_[2],
                                    (nibbles_[1] << 4) | nibbles_[0]};
    const auto rateIndex = static_cast<std::size_t>(nibbles_[7] >> 1);
    constexpr std::array<time_base::Rate, 4> rates{{{24, 1}, {25, 1}, {30000, 1001}, {30, 1}}};
    const auto rate = static_cast<MtcRate>(rateIndex);
    const auto frame = time_base::timecodeToFrame(label, rates[rateIndex], rate == MtcRate::Drop2997);
    locked_ = frame.has_value();
    if (!frame)
        return {};
    return MtcPosition{label, rate, direction_, *frame};
}

void QuarterFrameDecoder::reset() noexcept {
    received_ = 0;
    locked_ = false;
}

} // namespace adi::sync
