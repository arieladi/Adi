// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include <array>
#include <bitset>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>

namespace adi::engine::mcu {

enum class LcdDevice : std::uint8_t { Main = 0x14, Extender = 0x15 };
inline constexpr std::size_t lcdRows = 2, lcdColumns = 56;
inline constexpr std::size_t lcdStrips = 8, lcdStripPitch = 7;

class LcdFrame {
public:
    LcdFrame() noexcept {
        for (auto &row : rows_)
            row.fill(' ');
    }

    bool setStrip(std::size_t row, std::size_t strip, std::string_view ascii) noexcept {
        if (row >= lcdRows || strip >= lcdStrips)
            return false;
        const auto start = strip * lcdStripPitch;
        for (std::size_t i = 0; i + 1 < lcdStripPitch; ++i)
            rows_[row][start + i] = i < ascii.size() ? printable(ascii[i]) : ' ';
        rows_[row][start + lcdStripPitch - 1] = ' ';
        return true;
    }

    bool setLine(std::size_t row, std::string_view ascii) noexcept {
        if (row >= lcdRows)
            return false;
        for (std::size_t i = 0; i < lcdColumns; ++i)
            rows_[row][i] = i < ascii.size() ? printable(ascii[i]) : ' ';
        return true;
    }

private:
    static std::uint8_t printable(char value) noexcept {
        const auto byte = static_cast<std::uint8_t>(value);
        return byte >= 0x20 && byte <= 0x7e ? byte : '?';
    }

    std::array<std::array<std::uint8_t, lcdColumns>, lcdRows> rows_{};
    friend class LcdFeedback;
};

class LcdPacket {
public:
    [[nodiscard]] std::span<const std::uint8_t> bytes() const noexcept {
        return std::span(bytes_).first(size_);
    }

private:
    LcdPacket() = default;
    std::array<std::uint8_t, lcdColumns + 8> bytes_{};
    std::size_t size_ = 0;
    friend class LcdFeedback;
};

/// One display in the serial MIDI output worker. The worker takes at most
/// one SysEx patch per row in a tick and records only successfully sent
/// packets. Initial and reconnect ticks send a full frame.
class LcdFeedback {
public:
    explicit LcdFeedback(LcdDevice device) noexcept : device_(device) {}

    [[nodiscard]] std::array<std::optional<LcdPacket>, lcdRows>
    prepare(const LcdFrame &frame) const noexcept {
        std::array<std::optional<LcdPacket>, lcdRows> patches;
        for (std::size_t row = 0; row < lcdRows; ++row) {
            const auto &desired = frame.rows_[row];
            auto dirty = [&](std::size_t col) {
                return !known_[row][col] || previous_.rows_[row][col] != desired[col];
            };
            std::size_t first = 0;
            while (first < lcdColumns && !dirty(first))
                ++first;
            if (first == lcdColumns)
                continue;
            std::size_t last = lcdColumns - 1;
            while (!dirty(last))
                --last;
            LcdPacket patch;
            patch.bytes_ = {0xf0, 0x00, 0x00, 0x66, static_cast<std::uint8_t>(device_), 0x12,
                            static_cast<std::uint8_t>(lcdColumns * row + first)};
            for (std::size_t i = first; i <= last; ++i)
                patch.bytes_[7 + i - first] = desired[i];
            patch.size_ = 9 + last - first;
            patch.bytes_[patch.size_ - 1] = 0xf7;
            patches[row] = patch;
        }
        return patches;
    }

    void sent(const LcdPacket &packet) noexcept {
        if (packet.bytes_[4] != static_cast<std::uint8_t>(device_))
            return;
        const auto position = static_cast<std::size_t>(packet.bytes_[6]);
        const auto row = position / lcdColumns;
        const auto first = position % lcdColumns;
        for (std::size_t i = 0; i < packet.size_ - 8; ++i) {
            previous_.rows_[row][first + i] = packet.bytes_[7 + i];
            known_[row][first + i] = true;
        }
    }

    void reset() noexcept { known_ = {}; }

private:
    LcdDevice device_;
    LcdFrame previous_;
    std::array<std::bitset<lcdColumns>, lcdRows> known_;
};

} // namespace adi::engine::mcu
