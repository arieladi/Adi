// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include "adi/engine/launch_follow.hpp"
#include "adi/engine/launch_data.hpp"
#include "adi/engine/launcher.hpp"

#include <vector>

namespace adi::engine {

struct FollowSourceLoop {
    std::int64_t begin = 0, end = 0;
};

struct FollowClipClock {
    std::int64_t launchTick = 0;
    std::int64_t sourceTick = 0;
    std::int64_t sourceEnd = 0;
    std::optional<FollowSourceLoop> loop;
};

/// All boundaries use project ticks. Linked clips count their first partial
/// loop from the actual launch position (including Legato).
[[nodiscard]] std::optional<std::int64_t> clipFollowDueTick(
    const launch::FollowSettings&, const FollowClipClock&, bool globallyEnabled) noexcept;

[[nodiscard]] std::optional<std::int64_t> sceneFollowDueTick(
    const launch::FollowSettings&, std::int64_t launchTick, bool globallyEnabled) noexcept;

/// A new performance uses a fresh ordinal under its owner-supplied session
/// seed. A render reuses the captured pass seed and launch ordinal.
[[nodiscard]] std::uint64_t followPassSeed(std::uint64_t sessionSeed,
                                           std::uint64_t passOrdinal) noexcept;

struct FollowDrawKey {
    std::uint64_t passSeed = 0;
    std::uint64_t launchOrdinal = 0;
    std::int64_t track = 0; // zero is reserved for scene Follow Actions
};

/// IDs are in scene order. Empty clip slots break groups. For a scene Follow
/// Action, pass the same scene IDs as occupied members of the scene sequence.
/// The returned target is literal so a later reorder cannot retarget the action.
[[nodiscard]] FollowDecision resolveFollowPlaybackTarget(
    const launch::FollowSettings&, std::span<const std::int64_t> sceneIds,
    std::span<const std::optional<std::int64_t>> clips, std::size_t playing,
    FollowDrawKey) noexcept;

/// Owner-prepared bindings for the driver's Follow Action hook. Occupied
/// slots on tracks with enabled actions are retained in scene order. Gaps
/// take one entry regardless of how many empty scenes separate two clips.
class LaunchFollowPlayer {
public:
    explicit LaunchFollowPlayer(const launch::LaunchData&);

    [[nodiscard]] std::optional<LaunchFollowRequest> followOnLaunch(
        const LaunchTransition&, const LaunchView&, std::uint64_t incarnation,
        bool globallyEnabled, std::uint64_t passSeed) const noexcept;

    /// A caller uses the performed scene boundary and the scene's ordinal
    /// within the current pass. The target is a single scene gesture.
    [[nodiscard]] std::optional<LaunchFollowRequest> followOnSceneLaunch(
        std::int64_t scene, std::int64_t launchTick, std::uint64_t sceneLaunchOrdinal,
        std::uint64_t incarnation, bool globallyEnabled, std::uint64_t passSeed) const noexcept;

private:
    struct TrackSlots {
        std::vector<std::int64_t> sceneIds;
        std::vector<std::optional<std::int64_t>> clipIds;
    };
    struct Entry {
        std::int64_t clip = 0, scene = 0;
        std::size_t trackIndex = 0, sceneIndex = 0;
        launch::FollowSettings settings;
    };
    struct SceneEntry {
        std::int64_t scene = 0;
        std::size_t sceneIndex = 0;
        launch::FollowSettings settings;
    };
    std::vector<TrackSlots> tracks_;
    std::vector<Entry> entries_;
    TrackSlots sceneSequence_;
    std::vector<SceneEntry> sceneEntries_;
};

} // namespace adi::engine
