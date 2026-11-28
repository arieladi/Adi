// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include <cstdint>
#include <optional>
#include <string_view>

namespace adi::engine {
struct LaunchBlock;

/// The names in the existing record.clipUpdateRate setting. This policy is
/// for audible Clip View edits; launching uses its separate quantization.
enum class ClipUpdateRate : std::uint8_t {
    None, ThirtySecond, Sixteenth, Eighth, Quarter, Half, Bar,
};

struct LaunchClipLoopRegion {
    std::int64_t begin = 0, end = 0;
};

struct LaunchClipRegion {
    std::int64_t start = 0, end = 0;
    std::optional<LaunchClipLoopRegion> loop{};
};

/// Playback geometry relative to the already prepared clip source. Empty
/// loop means one shot. An edit changes only the addressed live run.
struct LaunchClipRegionEdit {
    std::int64_t track = 0, clip = 0;
    std::uint64_t incarnation = 0, run = 0;
    LaunchClipRegion region;
    ClipUpdateRate rate = ClipUpdateRate::Sixteenth;
};

[[nodiscard]] std::optional<ClipUpdateRate> parseClipUpdateRate(std::string_view) noexcept;

struct ClipUpdateBoundary {
    std::int64_t sample = 0, tick = 0;
    bool operator==(const ClipUpdateBoundary&) const = default;
};

/// Choose the first usable boundary on the rendered clock once a clip edit
/// is ready. A stopped or inactive clip can take its prepared edit immediately.
/// The caller owns both prepared clip versions through the handover and
/// reprojects the boundary if the pinned timing generation changes.
[[nodiscard]] std::optional<ClipUpdateBoundary> nextClipUpdateBoundary(
    const LaunchBlock&, std::int64_t readySample, ClipUpdateRate, bool clipRunning) noexcept;

/// Reach the boundary once in a contiguous half-open driver slice.
[[nodiscard]] std::optional<std::int32_t> clipUpdateOffsetIn(
    const LaunchBlock&, const ClipUpdateBoundary&) noexcept;

} // namespace adi::engine
