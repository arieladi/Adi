// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>
namespace adi::controllers {
// Runtime and op address for the per-channel 'pitchbend' ACTL stream.
// Standard CC numbers retain their ordinary 0..127 addresses.
inline constexpr std::uint16_t kPitchBend = 129;
constexpr bool validCc(std::int64_t controller) noexcept {
    return controller >= 0 && controller < 128;
}
constexpr bool validController(std::int64_t controller) noexcept {
    return validCc(controller) || controller == kPitchBend;
}
// Normalized lane values put the center at .5 on both sides of the
// asymmetric 14-bit MIDI wire range.
constexpr double pitchBendWordExact(double value) noexcept {
    return 8192.0 + (value - .5) * (value < .5 ? 16384.0 : 16382.0);
}
constexpr double pitchBendFromWord(std::uint16_t word) noexcept {
    return .5 + (static_cast<double>(word) - 8192.0) / (word < 8192 ? 16384.0 : 16382.0);
}

struct Point {
    std::uint64_t id = 0;
    std::int64_t timeTicks = 0;
    double value = 0;
    float tension = 0;
    std::uint8_t curve = 0, flags = 0;
    friend bool operator==(const Point &, const Point &) = default;
};
struct Lane {
    std::int64_t id = 0, clipId = 0;
    std::optional<std::int64_t> channel, controller;
    std::vector<Point> points;
    // Retain future header/record/trailer bytes and report invalid rows rather
    // than silently drawing them as an empty/deleted lane.
    std::vector<std::byte> data;
    std::string error;
    friend bool operator==(const Lane &, const Lane &) = default;
};
} // namespace adi::controllers
