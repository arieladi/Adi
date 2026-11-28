// SPDX-License-Identifier: AGPL-3.0-or-later
#include "adi/engine/launch_data.hpp"

#include <algorithm>

namespace adi::engine::launch {
namespace {

constexpr auto scenePaletteSize = 70; // ADR-0211: Live palette indices
constexpr auto minimumSceneTempo = 20.0;
constexpr auto maximumSceneTempo = 999.0;
constexpr auto maximumSceneNumerator = 99;
constexpr auto maximumSceneDenominator = 16;

bool validFollowChoice(const FollowChoice& choice) {
    if (choice.action > FollowAction::Jump)
        return false;
    return (choice.action == FollowAction::Jump) == choice.jumpSceneId.has_value();
}

bool validFollow(const FollowSettings& follow) {
    return follow.loopCount > 0 && follow.durationTicks > 0 &&
        follow.secondChancePercent >= 0.0 && follow.secondChancePercent <= 100.0 &&
        validFollowChoice(follow.first) && validFollowChoice(follow.second);
}

bool validScene(const Scene& scene) {
    const auto denominator = scene.signatureDenominator;
    return scene.id > 0 && scene.order >= 0 &&
        (!scene.color || (*scene.color >= 0 && *scene.color < scenePaletteSize)) &&
        scene.tempoBpm >= minimumSceneTempo && scene.tempoBpm <= maximumSceneTempo &&
        scene.signatureNumerator >= 1 && scene.signatureNumerator <= maximumSceneNumerator &&
        denominator >= 1 && denominator <= maximumSceneDenominator &&
        (denominator & (denominator - 1)) == 0 &&
        !scene.follow.linked && validFollow(scene.follow);
}

bool validSlotClip(const rows::Clip& clip, SlotKey key) {
    if (clip.trackId != key.trackId || clip.laneId ||
        (clip.kind != "audio" && clip.kind != "midi"))
        return false;
    return (clip.timeBase == 0 && clip.posTicks == 0 && !clip.posNs) ||
        (clip.timeBase == 1 && clip.posNs == 0 && (!clip.posTicks || clip.posTicks == 0));
}

bool validClipSettings(const ClipLaunchSettings& settings) {
    return settings.mode <= LaunchMode::Repeat &&
        resolveClipQuantization(settings.quantization, LaunchQuantization::Bar).has_value() &&
        settings.velocityAmount >= 0.0 && settings.velocityAmount <= 1.0 &&
        validFollow(settings.follow);
}

bool knownFollowScenes(const FollowSettings& follow, const LaunchData& data) {
    return (!follow.first.jumpSceneId || data.findScene(*follow.first.jumpSceneId)) &&
        (!follow.second.jumpSceneId || data.findScene(*follow.second.jumpSceneId));
}

} // namespace

std::optional<LaunchData> LaunchData::prepare(
    std::span<const std::int64_t> trackIds, std::span<const rows::Clip> clips,
    std::span<const Scene> scenes, std::span<const Slot> slots,
    std::span<const ClipLaunchSettings> settings, LaunchDataError* error) {
    const auto reject = [&](LaunchDataError reason) -> std::optional<LaunchData> {
        if (error)
            *error = reason;
        return std::nullopt;
    };

    LaunchData data;
    data.tracks_.assign(trackIds.begin(), trackIds.end());
    std::ranges::sort(data.tracks_);
    if (!data.tracks_.empty() && data.tracks_.front() <= 0)
        return reject(LaunchDataError::InvalidTrack);
    if (std::ranges::adjacent_find(data.tracks_) != data.tracks_.end())
        return reject(LaunchDataError::DuplicateTrack);

    data.scenes_.assign(scenes.begin(), scenes.end());
    std::ranges::sort(data.scenes_, {}, &Scene::order);
    if (std::ranges::adjacent_find(data.scenes_, {}, &Scene::order) != data.scenes_.end())
        return reject(LaunchDataError::DuplicateSceneOrder);
    data.sceneIndex_.reserve(scenes.size());
    for (std::size_t i = 0; i < data.scenes_.size(); ++i) {
        const auto& scene = data.scenes_[i];
        if (!validScene(scene))
            return reject(LaunchDataError::InvalidScene);
        data.sceneIndex_.emplace_back(scene.id, i);
    }
    std::ranges::sort(data.sceneIndex_);
    if (std::ranges::adjacent_find(data.sceneIndex_, {}, &IndexEntry::first) != data.sceneIndex_.end())
        return reject(LaunchDataError::DuplicateScene);
    for (const auto& scene : data.scenes_) {
        if (!knownFollowScenes(scene.follow, data))
            return reject(LaunchDataError::UnknownFollowScene);
    }

    std::vector<const rows::Clip*> clipsById;
    clipsById.reserve(clips.size());
    for (const auto& clip : clips) {
        if (clip.id <= 0)
            return reject(LaunchDataError::InvalidClip);
        clipsById.push_back(&clip);
    }
    std::ranges::sort(clipsById, {}, &rows::Clip::id);
    if (std::ranges::adjacent_find(clipsById, {}, &rows::Clip::id) != clipsById.end())
        return reject(LaunchDataError::DuplicateClip);

    data.slots_.assign(slots.begin(), slots.end());
    std::ranges::sort(data.slots_, {}, &Slot::key);
    if (std::ranges::adjacent_find(data.slots_, {}, &Slot::key) != data.slots_.end())
        return reject(LaunchDataError::DuplicateSlot);
    data.slotClipIndex_.reserve(slots.size());
    for (std::size_t i = 0; i < data.slots_.size(); ++i) {
        const auto& slot = data.slots_[i];
        if (!data.hasTrack(slot.key.trackId))
            return reject(LaunchDataError::UnknownSlotTrack);
        if (!data.findScene(slot.key.sceneId))
            return reject(LaunchDataError::UnknownSlotScene);
        if (!slot.clipId)
            continue;
        const auto clip = std::ranges::lower_bound(clipsById, *slot.clipId, {}, &rows::Clip::id);
        if (clip == clipsById.end() || (*clip)->id != *slot.clipId)
            return reject(LaunchDataError::UnknownSlotClip);
        if (!validSlotClip(**clip, slot.key))
            return reject(LaunchDataError::InvalidSlotClip);
        data.slotClipIndex_.emplace_back(*slot.clipId, i);
    }
    std::ranges::sort(data.slotClipIndex_);
    if (std::ranges::adjacent_find(data.slotClipIndex_, {}, &IndexEntry::first) != data.slotClipIndex_.end())
        return reject(LaunchDataError::DuplicateSlotClip);

    data.settings_.assign(settings.begin(), settings.end());
    std::ranges::sort(data.settings_, {}, &ClipLaunchSettings::clipId);
    if (std::ranges::adjacent_find(data.settings_, {}, &ClipLaunchSettings::clipId) != data.settings_.end())
        return reject(LaunchDataError::DuplicateLaunchSettings);
    for (const auto& launch : data.settings_) {
        if (!data.findSlotForClip(launch.clipId))
            return reject(LaunchDataError::UnslottedLaunchSettings);
        if (!validClipSettings(launch))
            return reject(LaunchDataError::InvalidLaunchSettings);
        if (!knownFollowScenes(launch.follow, data))
            return reject(LaunchDataError::UnknownFollowScene);
    }
    if (error)
        *error = LaunchDataError::None;
    return data;
}

bool LaunchData::hasTrack(std::int64_t id) const noexcept {
    return std::ranges::binary_search(tracks(), id);
}

// Use spans for driver-side iteration. MSVC's checked vector iterators can
// acquire a lock in Debug builds when an iterator attaches to its container.
const Scene* LaunchData::findScene(std::int64_t id) const noexcept {
    const std::span<const IndexEntry> index{sceneIndex_.data(), sceneIndex_.size()};
    const auto found = std::ranges::lower_bound(index, id, {}, &IndexEntry::first);
    return found != index.end() && found->first == id ? &scenes()[found->second] : nullptr;
}

const Slot* LaunchData::findSlotForClip(std::int64_t clipId) const noexcept {
    const std::span<const IndexEntry> index{slotClipIndex_.data(), slotClipIndex_.size()};
    const auto found = std::ranges::lower_bound(index, clipId, {}, &IndexEntry::first);
    return found != index.end() && found->first == clipId ? &slots()[found->second] : nullptr;
}

Slot LaunchData::slot(SlotKey key) const noexcept {
    const auto rows = slots();
    const auto found = std::ranges::lower_bound(rows, key, {}, &Slot::key);
    if (found != rows.end() && found->key == key)
        return *found;
    return {key, std::nullopt, defaultEmptySlotStop && hasTrack(key.trackId) && findScene(key.sceneId)};
}

std::optional<ClipLaunchSettings> LaunchData::settings(std::int64_t clipId) const noexcept {
    const auto rows = settingsRows();
    const auto found = std::ranges::lower_bound(rows, clipId, {}, &ClipLaunchSettings::clipId);
    if (found != rows.end() && found->clipId == clipId)
        return *found;
    if (findSlotForClip(clipId))
        return ClipLaunchSettings{.clipId = clipId};
    return std::nullopt;
}

} // namespace adi::engine::launch
