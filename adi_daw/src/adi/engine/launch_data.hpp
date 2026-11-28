// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include "adi/engine/launch_follow.hpp"
#include "adi/engine/launch_gestures.hpp"
#include "adi/launch_quantization.hpp"
#include "adi/store_rows.hpp"

#include <compare>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace adi::engine::launch {

// Live's unlinked Follow Action Time defaults to one bar. Tick-only producers
// use this 4/4 fallback until they can supply the authored meter.
inline constexpr std::int64_t defaultFollowDurationTicks = 4 * 5765760;
inline constexpr double defaultDormantSceneTempo = 120.0;
inline constexpr std::int64_t defaultDormantSceneSignature = 4;
inline constexpr bool defaultEmptySlotStop = true;
inline constexpr double defaultVelocityAmount = 0.0;
inline constexpr bool defaultClipRamMode = false;
// ADI's Create Fades on Clip Edges preference defaults to off. Authoring
// with the preference enabled supplies its chosen value on the clip row.
inline constexpr bool defaultClipFade = false;

struct FollowChoice {
    FollowAction action = FollowAction::None;
    std::optional<std::int64_t> jumpSceneId;
    bool operator==(const FollowChoice&) const = default;
};

struct FollowSettings {
    bool enabled = false;
    bool linked = true;
    std::int64_t loopCount = 1;
    std::int64_t durationTicks = defaultFollowDurationTicks;
    FollowChoice first{}, second{};
    double secondChancePercent = 0.0;
    bool operator==(const FollowSettings&) const = default;
};

struct Scene {
    std::int64_t id = 0;
    std::int64_t order = 0;
    std::string name;
    std::optional<std::int64_t> color;
    double tempoBpm = defaultDormantSceneTempo;
    bool tempoEnabled = false;
    std::int64_t signatureNumerator = defaultDormantSceneSignature;
    std::int64_t signatureDenominator = defaultDormantSceneSignature;
    bool signatureEnabled = false;
    FollowSettings follow{.linked = false};
    bool operator==(const Scene&) const = default;
};

struct SlotKey {
    std::int64_t trackId = 0;
    std::int64_t sceneId = 0;
    auto operator<=>(const SlotKey&) const = default;
};

struct Slot {
    SlotKey key;
    std::optional<std::int64_t> clipId;
    bool hasStop = defaultEmptySlotStop;
    bool operator==(const Slot&) const = default;
};

struct ClipLaunchSettings {
    std::int64_t clipId = 0;
    LaunchMode mode = LaunchMode::Trigger;
    std::int64_t quantization = 0; // Clip code 0: Global. Explicit Set codes are offset by one.
    bool legato = false;
    double velocityAmount = defaultVelocityAmount;
    FollowSettings follow{};
    bool clipFade = defaultClipFade;
    bool ramMode = defaultClipRamMode;
    bool operator==(const ClipLaunchSettings&) const = default;
};

[[nodiscard]] constexpr std::optional<LaunchQuantization> resolveClipQuantization(
    std::int64_t code, LaunchQuantization global) noexcept {
    if (code == 0)
        return isLaunchQuantizationCode(static_cast<std::int64_t>(global))
            ? std::optional{global} : std::nullopt;
    if (code < 0 || !isLaunchQuantizationCode(code - 1))
        return std::nullopt;
    return static_cast<LaunchQuantization>(code - 1);
}

enum class LaunchDataError {
    None,
    InvalidTrack,
    DuplicateTrack,
    InvalidClip,
    DuplicateClip,
    InvalidScene,
    DuplicateScene,
    DuplicateSceneOrder,
    UnknownFollowScene,
    DuplicateSlot,
    DuplicateSlotClip,
    UnknownSlotTrack,
    UnknownSlotScene,
    UnknownSlotClip,
    InvalidSlotClip,
    DuplicateLaunchSettings,
    UnslottedLaunchSettings,
    InvalidLaunchSettings,
};

// Off-driver preparation of a coherent Session data image. Ownership stays
// with the caller. Queries on an admitted image allocate nothing and take
// no locks. Scene order is authored order. Slots and settings are sparse.
class LaunchData {
public:
    [[nodiscard]] static std::optional<LaunchData> prepare(
        std::span<const std::int64_t> trackIds, std::span<const rows::Clip> clips,
        std::span<const Scene> scenes, std::span<const Slot> slots,
        std::span<const ClipLaunchSettings> settings, LaunchDataError* error = nullptr);

    [[nodiscard]] std::span<const std::int64_t> tracks() const noexcept { return {tracks_.data(), tracks_.size()}; }
    [[nodiscard]] std::span<const Scene> scenes() const noexcept { return {scenes_.data(), scenes_.size()}; }
    [[nodiscard]] std::span<const Slot> slots() const noexcept { return {slots_.data(), slots_.size()}; }
    [[nodiscard]] std::span<const ClipLaunchSettings> settingsRows() const noexcept {
        return {settings_.data(), settings_.size()};
    }
    [[nodiscard]] bool hasTrack(std::int64_t id) const noexcept;
    [[nodiscard]] const Scene* findScene(std::int64_t id) const noexcept;
    [[nodiscard]] const Slot* findSlotForClip(std::int64_t clipId) const noexcept;

    // A missing row in an existing scene and track is an empty cell with Stop.
    // An unknown address cannot affect playback.
    [[nodiscard]] Slot slot(SlotKey key) const noexcept;
    // A slotted clip without an explicit settings row inherits the defaults.
    [[nodiscard]] std::optional<ClipLaunchSettings> settings(std::int64_t clipId) const noexcept;

private:
    using IndexEntry = std::pair<std::int64_t, std::size_t>;
    std::vector<std::int64_t> tracks_;
    std::vector<Scene> scenes_;
    std::vector<Slot> slots_;
    std::vector<ClipLaunchSettings> settings_;
    std::vector<IndexEntry> sceneIndex_, slotClipIndex_;
};

} // namespace adi::engine::launch
