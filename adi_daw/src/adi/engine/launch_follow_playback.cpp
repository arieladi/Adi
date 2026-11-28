// SPDX-License-Identifier: AGPL-3.0-or-later
#include "adi/engine/launch_follow_playback.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <map>

namespace adi::engine {
namespace {
constexpr auto maxTick = std::numeric_limits<std::int64_t>::max();
// Live 12 §16.7 documents Next wrapping, while these boundary cases await N6.
constexpr bool previousWrapsAtFirst = false;
constexpr bool otherRelaunchesSingleton = false;
// The manual describes Stop for clips. The scope of a scene Follow Action
// Stop remains a Live measurement; leave the current scene running for now.
constexpr bool sceneFollowStopStopsAll = false;
// The manual allows choosing a slot by number. N6 still needs to confirm a
// Jump across any intervening empty slot and to an empty destination.
constexpr bool jumpCanCrossGroup = true;
constexpr FollowResult emptyJumpPolicy = FollowResult::Unavailable;

std::optional<std::int64_t> after(std::int64_t tick, std::int64_t duration) noexcept {
    if (tick < 0 || duration <= 0 || duration > maxTick - tick)
        return {};
    return tick + duration;
}

constexpr std::uint64_t mix(std::uint64_t bits) noexcept {
    bits = (bits ^ (bits >> 30)) * UINT64_C(0xbf58476d1ce4e5b9);
    bits = (bits ^ (bits >> 27)) * UINT64_C(0x94d049bb133111eb);
    return bits ^ (bits >> 31);
}

std::size_t uniformRank(std::uint64_t draw, std::size_t count) noexcept {
    // Discard the leading incomplete bucket before reducing to the group size.
    // This permutation retries without retaining a generator or mutable state.
    const auto bound = static_cast<std::uint64_t>(count);
    const auto threshold = (std::uint64_t{0} - bound) % bound;
    auto lane = draw;
    auto bits = mix(lane);
    while (bits < threshold) {
        lane += UINT64_C(0x9e3779b97f4a7c15);
        bits = mix(lane);
    }
    return static_cast<std::size_t>(bits % bound);
}
} // namespace

std::optional<std::int64_t> sceneFollowDueTick(const launch::FollowSettings& settings,
                                               std::int64_t launchTick,
                                               bool globallyEnabled) noexcept {
    if (!globallyEnabled || !settings.enabled)
        return {};
    return after(launchTick, settings.durationTicks);
}

std::optional<std::int64_t> clipFollowDueTick(const launch::FollowSettings& settings,
                                              const FollowClipClock& clock,
                                              bool globallyEnabled) noexcept {
    if (!globallyEnabled || !settings.enabled)
        return {};
    if (!settings.linked)
        return after(clock.launchTick, settings.durationTicks);
    if (clock.sourceTick < 0 || clock.sourceEnd <= clock.sourceTick)
        return {};
    if (!clock.loop)
        return after(clock.launchTick, clock.sourceEnd - clock.sourceTick);

    const auto [begin, end] = *clock.loop;
    if (begin < 0 || end <= begin || end > clock.sourceEnd || end <= clock.sourceTick ||
        settings.loopCount < 1)
        return {};
    const auto first = end - clock.sourceTick;
    const auto period = end - begin;
    if (settings.loopCount - 1 > (maxTick - first) / period)
        return {};
    return after(clock.launchTick, first + (settings.loopCount - 1) * period);
}

std::uint64_t followPassSeed(std::uint64_t sessionSeed, std::uint64_t passOrdinal) noexcept {
    return mix(sessionSeed + UINT64_C(0x9e3779b97f4a7c15) * passOrdinal);
}

FollowDecision resolveFollowPlaybackTarget(const launch::FollowSettings& settings,
                                           std::span<const std::int64_t> sceneIds,
                                           std::span<const std::optional<std::int64_t>> clips,
                                           std::size_t playing, FollowDrawKey key) noexcept {
    if (!settings.enabled)
        return {FollowResult::Unchanged, {}};
    const auto chance = settings.secondChancePercent;
    if (!std::isfinite(chance) || chance < 0.0 || chance > 100.0)
        return {};
    const auto draw = mix(key.passSeed + mix(key.launchOrdinal) +
                          mix(static_cast<std::uint64_t>(key.track) ^ UINT64_C(0x94d049bb133111eb)));
    constexpr auto count = 0x1p53;
    const auto chanceDraw = static_cast<double>(draw >> 11) / count;
    const auto& selected = chanceDraw < chance / 100.0 ? settings.second : settings.first;
    FollowChoice choice;
    if (selected.action == FollowAction::Any || selected.action == FollowAction::Other) {
        const auto group = followGroup(clips, playing);
        if (!group)
            return {};
        const auto available = group->end - group->begin - (selected.action == FollowAction::Other ? 1u : 0u);
        if (available > 0)
            choice.rank = uniformRank(draw ^ UINT64_C(0xd1b54a32d192ed03), available);
    } else if (selected.action == FollowAction::Jump && selected.jumpSceneId &&
               sceneIds.size() == clips.size()) {
        const auto found = std::find(sceneIds.begin(), sceneIds.end(), *selected.jumpSceneId);
        if (found != sceneIds.end()) {
            const auto index = static_cast<std::size_t>(found - sceneIds.begin());
            if constexpr (jumpCanCrossGroup) {
                choice.jump = index;
            } else {
                const auto group = followGroup(clips, playing);
                if (group && index >= group->begin && index < group->end)
                    choice.jump = index;
            }
        }
    }

    const auto decision = resolveFollowTarget(clips, playing, selected.action, choice);
    if (selected.action == FollowAction::Jump && decision.result == FollowResult::Unavailable)
        return {emptyJumpPolicy, {}};
    if (decision.result != FollowResult::GroupBoundary)
        return decision;
    if (selected.action == FollowAction::Next)
        return resolveFollowTarget(clips, playing, FollowAction::First);
    if (selected.action == FollowAction::Previous && previousWrapsAtFirst)
        return resolveFollowTarget(clips, playing, FollowAction::Last);
    if (selected.action == FollowAction::Other && otherRelaunchesSingleton)
        return resolveFollowTarget(clips, playing, FollowAction::Again);
    return {FollowResult::Unchanged, {}};
}

LaunchFollowPlayer::LaunchFollowPlayer(const launch::LaunchData& data) {
    const auto scenes = data.scenes();
    if (std::ranges::any_of(scenes, [](const auto& scene) { return scene.follow.enabled; })) {
        sceneSequence_.sceneIds.reserve(scenes.size());
        sceneSequence_.clipIds.reserve(scenes.size());
        for (const auto& scene : scenes) {
            if (scene.follow.enabled)
                sceneEntries_.push_back({scene.id, sceneSequence_.sceneIds.size(), scene.follow});
            sceneSequence_.sceneIds.push_back(scene.id);
            sceneSequence_.clipIds.push_back(scene.id);
        }
        std::ranges::sort(sceneEntries_, {}, &SceneEntry::scene);
    }
    struct Slot {
        std::size_t order;
        std::int64_t scene, clip;
    };
    std::map<std::int64_t, std::vector<Slot>> tracks;
    for (const auto& row : data.settingsRows()) {
        if (row.follow.enabled) {
            const auto* slot = data.findSlotForClip(row.clipId);
            tracks.try_emplace(slot->key.trackId);
        }
    }
    if (tracks.empty())
        return;

    std::map<std::int64_t, std::size_t> sceneOrder;
    for (std::size_t i = 0; i < scenes.size(); ++i)
        sceneOrder.emplace(scenes[i].id, i);
    for (const auto& slot : data.slots()) {
        const auto track = tracks.find(slot.key.trackId);
        if (track != tracks.end() && slot.clipId)
            track->second.push_back({sceneOrder.at(slot.key.sceneId), slot.key.sceneId, *slot.clipId});
    }

    tracks_.reserve(tracks.size());
    for (auto& track : tracks) {
        auto& slots = track.second;
        std::ranges::sort(slots, {}, &Slot::order);
        const auto trackIndex = tracks_.size();
        auto& prepared = tracks_.emplace_back();
        std::optional<std::size_t> previous;
        for (const auto& slot : slots) {
            if (previous && slot.order - *previous > 1) {
                prepared.sceneIds.push_back(0);
                prepared.clipIds.push_back(std::nullopt);
            }
            const auto settings = data.settings(slot.clip);
            if (settings && settings->follow.enabled)
                entries_.push_back({slot.clip, slot.scene, trackIndex, prepared.sceneIds.size(),
                                    settings->follow});
            prepared.sceneIds.push_back(slot.scene);
            prepared.clipIds.push_back(slot.clip);
            previous = slot.order;
        }
    }
    std::ranges::sort(entries_, {}, &Entry::clip);
}

std::optional<LaunchFollowRequest> LaunchFollowPlayer::followOnLaunch(
    const LaunchTransition& transition, const LaunchView& view, std::uint64_t incarnation,
    bool globallyEnabled, std::uint64_t passSeed) const noexcept {
    if (!globallyEnabled || transition.newClip <= 0)
        return {};
    const auto entries = std::span<const Entry>(entries_.data(), entries_.size());
    const auto entry = std::ranges::lower_bound(entries, transition.newClip, {}, &Entry::clip);
    if (entry == entries.end() || entry->clip != transition.newClip)
        return {};
    const auto address = std::pair{transition.track, entry->scene};
    const auto slotAddress = [](const LaunchSlot& slot) { return std::pair{slot.track, slot.scene}; };
    const auto slot = std::ranges::lower_bound(view.slots, address, {}, slotAddress);
    if (slot == view.slots.end() || slotAddress(*slot) != address || slot->clip != entry->clip)
        return {};

    FollowClipClock clock{transition.tick, transition.sourceTick, slot->endTick, {}};
    if (slot->loopEndTick > slot->loopStartTick)
        clock.loop = FollowSourceLoop{slot->loopStartTick, slot->loopEndTick};
    const auto due = clipFollowDueTick(entry->settings, clock, globallyEnabled);
    if (!due)
        return {};
    const auto& track = tracks_[entry->trackIndex];
    const auto decision = resolveFollowPlaybackTarget(entry->settings, track.sceneIds, track.clipIds,
        entry->sceneIndex, {passSeed, transition.followOrdinal, transition.track});
    const auto laps = entry->settings.linked && slot->physicalFrames > 0
        ? (slot->physicalLoopEndFrame ? entry->settings.loopCount : 1) : 0;
    if (decision.result == FollowResult::Stop) {
        return LaunchFollowRequest{
            {LaunchRequestKind::StopTrack, transition.track, {}, {}, incarnation}, *due, laps};
    }
    if (decision.target) {
        const auto [index, clip] = *decision.target;
        return LaunchFollowRequest{
            {LaunchRequestKind::SlotPress, transition.track, track.sceneIds[index], clip, incarnation}, *due, laps};
    }
    return {};
}

std::optional<LaunchFollowRequest> LaunchFollowPlayer::followOnSceneLaunch(
    std::int64_t scene, std::int64_t launchTick, std::uint64_t sceneLaunchOrdinal,
    std::uint64_t incarnation, bool globallyEnabled, std::uint64_t passSeed) const noexcept {
    if (!globallyEnabled)
        return {};
    const auto entries = std::span<const SceneEntry>(sceneEntries_.data(), sceneEntries_.size());
    const auto source = std::ranges::lower_bound(entries, scene, {}, &SceneEntry::scene);
    if (source == entries.end() || source->scene != scene)
        return {};
    const auto due = sceneFollowDueTick(source->settings, launchTick, globallyEnabled);
    if (!due)
        return {};
    const auto decision = resolveFollowPlaybackTarget(source->settings, sceneSequence_.sceneIds,
        sceneSequence_.clipIds, source->sceneIndex, {passSeed, sceneLaunchOrdinal, 0});
    if (decision.result == FollowResult::Stop && sceneFollowStopStopsAll) {
        return LaunchFollowRequest{
            {LaunchRequestKind::StopAll, {}, {}, {}, incarnation}, *due};
    }
    if (decision.result == FollowResult::Launch && decision.target) {
        return LaunchFollowRequest{
            {LaunchRequestKind::ScenePress, {}, decision.target->clip, {}, incarnation}, *due};
    }
    return {};
}
} // namespace adi::engine
