// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>
namespace adi::controllers {
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
