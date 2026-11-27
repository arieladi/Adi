// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include "adi/engine/launch_audio.hpp"
#include "adi/engine/launcher.hpp"

#include <memory>

namespace adi::engine {

struct NodeIo;
class Transport;
class LaunchFollowPlayer;

/// An immutable resident audio project prepared at one session tempo. Track,
/// scene and slot ordering follow LaunchView. Each occupied slot has its PCM.
class LaunchAudioProject {
public:
    LaunchAudioProject(std::uint64_t incarnation, double bpm, std::vector<std::int64_t> tracks,
                       std::vector<LaunchSlot> slots, std::vector<std::int64_t> scenes,
                       std::vector<LaunchAudioClip> clips, LaunchQuantization global = LaunchQuantization::Bar,
                       std::shared_ptr<const LaunchFollowPlayer> follow = {},
                       std::uint64_t followSeed = 0);
    LaunchAudioProject(const LaunchAudioProject&) = delete;
    LaunchAudioProject& operator=(const LaunchAudioProject&) = delete;

    [[nodiscard]] std::uint64_t incarnation() const noexcept { return incarnation_; }
    [[nodiscard]] double bpm() const noexcept { return bpm_; }
    [[nodiscard]] LaunchQuantization globalQuantization() const noexcept { return global_; }
    [[nodiscard]] LaunchView view() const noexcept { return {tracks_, slots_, scenes_}; }
    [[nodiscard]] bool supportsRate(double rate) const noexcept;
    [[nodiscard]] const LaunchAudioClip* clip(std::int64_t id) const noexcept;
    [[nodiscard]] const LaunchFollowPlayer* followPlayer() const noexcept { return follow_.get(); }
    [[nodiscard]] std::uint64_t followSeed() const noexcept { return followSeed_; }

private:
    std::uint64_t incarnation_;
    double bpm_;
    std::vector<std::int64_t> tracks_, scenes_;
    std::vector<LaunchSlot> slots_;
    std::vector<LaunchAudioClip> clips_;
    LaunchQuantization global_;
    std::shared_ptr<const LaunchFollowPlayer> follow_;
    std::uint64_t followSeed_;
};

/// Session-owned state that survives graph replacement. Only the gesture
/// mailbox is shared between the owner and driver.
class LaunchAudioDriver {
public:
    explicit LaunchAudioDriver(std::size_t tracks) : launcher_(tracks) {}
    [[nodiscard]] bool post(std::span<const LaunchRequest> requests) noexcept { return launcher_.post(requests); }
    void suspend() noexcept { wasPlaying_ = false; }

private:
    friend class LaunchAudioRuntime;
    Launcher launcher_;
    std::uint64_t pass_ = 0;
    bool wasPlaying_ = false;
};

/// Scratch belonging to one published graph. The driver fills the current
/// slice before rendering, then each track source reads its own ranges.
class LaunchAudioRuntime {
public:
    LaunchAudioRuntime(std::shared_ptr<const LaunchAudioProject>, std::vector<std::int64_t> tracks,
                       std::int32_t maxFrames);

    void begin(LaunchAudioDriver&, LaunchBlock, const Transport&, std::int64_t streamSample,
               void* observerContext,
               void (*observer)(void*, const LaunchTransition&, std::int64_t) noexcept) noexcept;
    void finish() noexcept { active_ = false; }
    void overlay(std::int64_t track, const NodeIo&) const noexcept;

private:
    struct Range {
        std::int32_t offset, frames;
        const LaunchAudioClip* pcm;
        std::int64_t sourceFrame;
        float velocity;
        std::int64_t fadeElapsed;
    };
    struct TrackRanges {
        std::vector<Range> items;
        std::size_t size = 0;
    };
    void add(const LaunchSection&, std::int32_t blockOffset) noexcept;

    std::shared_ptr<const LaunchAudioProject> project_;
    std::vector<std::int64_t> tracks_;
    std::vector<TrackRanges> ranges_;
    std::int32_t maxFrames_;
    bool active_ = false;
};

} // namespace adi::engine
