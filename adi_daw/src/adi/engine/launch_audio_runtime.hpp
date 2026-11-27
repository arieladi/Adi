// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include "adi/engine/launch_audio.hpp"
#include "adi/engine/launch_gain_edits.hpp"
#include "adi/engine/launcher.hpp"

#include <memory>

namespace adi::engine {

struct NodeIo;
class Transport;
class LaunchFollowPlayer;

struct LaunchSceneClock {
    std::int64_t scene = 0;
    std::optional<double> bpm;
    std::optional<rows::TimeSignature> signature;
};

/// An immutable resident audio project prepared at one session tempo. Track,
/// scene and slot ordering follow LaunchView. Each occupied slot has its PCM.
class LaunchAudioProject {
public:
    LaunchAudioProject(std::uint64_t incarnation, double bpm, std::vector<std::int64_t> tracks,
                       std::vector<LaunchSlot> slots, std::vector<std::int64_t> scenes,
                       std::vector<LaunchAudioClip> clips, LaunchQuantization global = LaunchQuantization::Bar,
                       std::shared_ptr<const LaunchFollowPlayer> follow = {},
                       std::uint64_t followSeed = 0, std::vector<std::int64_t> midiTracks = {},
                       bool stored = false, std::vector<LaunchSceneClock> sceneClocks = {});
    LaunchAudioProject(const LaunchAudioProject&) = delete;
    LaunchAudioProject& operator=(const LaunchAudioProject&) = delete;

    [[nodiscard]] std::uint64_t incarnation() const noexcept { return incarnation_; }
    [[nodiscard]] std::uint64_t pcmRevision() const noexcept { return pcmRevision_; }
    [[nodiscard]] double bpm() const noexcept { return bpm_; }
    [[nodiscard]] LaunchQuantization globalQuantization() const noexcept { return global_; }
    [[nodiscard]] LaunchView view() const noexcept { return {tracks_, slots_, scenes_}; }
    [[nodiscard]] bool supportsRate(double rate) const noexcept;
    [[nodiscard]] std::uint64_t residentBytes() const noexcept;
    [[nodiscard]] const LaunchAudioClip* clip(std::int64_t id) const noexcept;
    [[nodiscard]] const LaunchFollowPlayer* followPlayer() const noexcept { return follow_.get(); }
    [[nodiscard]] std::uint64_t followSeed() const noexcept { return followSeed_; }
    [[nodiscard]] bool isMidiTrack(std::int64_t track) const noexcept;
    [[nodiscard]] bool stored() const noexcept { return stored_; }
    [[nodiscard]] const LaunchSceneClock* sceneClock(std::int64_t) const noexcept;

private:
    std::uint64_t incarnation_, pcmRevision_;
    double bpm_;
    std::vector<std::int64_t> tracks_, scenes_;
    std::vector<LaunchSlot> slots_;
    std::vector<LaunchAudioClip> clips_;
    LaunchQuantization global_;
    std::shared_ptr<const LaunchFollowPlayer> follow_;
    std::uint64_t followSeed_;
    std::vector<std::int64_t> midiTracks_;
    bool stored_;
    std::vector<LaunchSceneClock> sceneClocks_;
};

struct LaunchRenderSlice {
    LaunchSection section;
    LaunchBlock block;
    std::int32_t baseOffset = 0; // Arrangement-loop slice origin within the logical callback
};

/// Session-owned state that survives graph replacement. The gesture and gain
/// mailboxes carry live commands from the owner to the driver.
class LaunchAudioDriver {
public:
    explicit LaunchAudioDriver(std::size_t tracks) : launcher_(tracks), gains_(tracks), tails_(tracks * 2) {}
    [[nodiscard]] bool post(std::span<const LaunchRequest> requests) noexcept { return launcher_.post(requests); }
    [[nodiscard]] bool readStatus(std::span<LaunchTrackStatus> tracks, LaunchStatusHeader& header) const noexcept {
        return launcher_.readStatus(tracks, header);
    }
    void suspend() noexcept {
        if (wasPlaying_) launcher_.suspendStatus();
        wasPlaying_ = false;
        tailCount_ = 0;
        nextTimelineSample_ = -1;
    }
    [[nodiscard]] std::uint64_t performedSceneToken() const noexcept {
        return performedScene_.load(std::memory_order_acquire);
    }

private:
    friend class LaunchAudioRuntime;
    struct Tail {
        std::int64_t track, clip, sourceFrame, elapsed, fadeInAtExit;
        float velocity;
        std::uint64_t media;
    };
    Launcher launcher_;
    LaunchClipGainEdits gains_;
    std::vector<Tail> tails_;
    std::size_t tailCount_ = 0;
    std::uint64_t tailIncarnation_ = 0;
    std::uint64_t pass_ = 0;
    bool wasPlaying_ = false;
    std::int64_t nextTimelineSample_ = -1, loopBegin_ = -1, loopEnd_ = -1;
    std::uint64_t transportRevision_ = 0;
    static_assert(std::atomic<std::uint64_t>::is_always_lock_free);
    std::atomic<std::uint64_t> performedScene_{0};
};

/// Scratch belonging to one published graph. The driver fills the current
/// slice before rendering, then each track source reads its own ranges.
class LaunchAudioRuntime {
public:
    LaunchAudioRuntime(std::shared_ptr<const LaunchAudioProject>, std::vector<std::int64_t> tracks,
                       std::int32_t maxFrames,
                       std::shared_ptr<const LaunchAudioProject> beforeClock = {},
                       std::int64_t clockSample = 0);

    void startIfQueued(LaunchAudioDriver&, LaunchBlock, Transport&) const noexcept;
    [[nodiscard]] LaunchQuantization globalQuantization() const noexcept {
        return project_->globalQuantization();
    }
    void begin(LaunchAudioDriver&, LaunchBlock, const Transport&, std::int64_t streamSample,
               void* observerContext,
               void (*observer)(void*, const LaunchTransition&, std::int64_t) noexcept,
               void* sceneObserverContext = nullptr,
               void (*sceneObserver)(void*, const LaunchSceneTransition&, std::int64_t) noexcept = nullptr) noexcept;
    void finish() noexcept { active_ = false; }
    void overlay(std::int64_t track, const NodeIo&) const noexcept;
    [[nodiscard]] std::uint64_t incarnation() const noexcept { return project_->incarnation(); }
    [[nodiscard]] const LaunchSceneClock* sceneClock(std::int64_t id) const noexcept { return project_->sceneClock(id); }
    [[nodiscard]] bool hasScene(std::int64_t scene) const noexcept;
    [[nodiscard]] bool postGainEdit(LaunchAudioDriver&, const LaunchClipGainEdit&) const noexcept;
    [[nodiscard]] bool postRegionEdit(LaunchAudioDriver&, const LaunchClipRegionEdit&, std::int64_t sample) const noexcept;
    [[nodiscard]] const std::shared_ptr<const LaunchAudioProject>& mediaAt(std::int64_t sample) const noexcept;
    [[nodiscard]] bool hasTrack(std::int64_t track) const noexcept;
    [[nodiscard]] std::span<const LaunchRenderSlice> sectionsFor(std::int64_t track) const noexcept;

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
        std::vector<LaunchRenderSlice> midi;
        struct TailRange {
            Range range;
            std::int64_t fadeInAtExit;
        };
        std::vector<TailRange> tails;
        std::size_t size = 0;
        std::size_t tailCount = 0;
    };
    [[nodiscard]] bool addTail(LaunchAudioDriver::Tail&, std::int32_t offset,
                               std::int32_t frames) noexcept;
    void fadeOut(LaunchAudioDriver&, const LaunchAudioProject&, const LaunchTransition&, std::int32_t offset,
                 std::int32_t blockFrames) noexcept;
    void add(LaunchClipGainEdits&, const LaunchAudioProject&, const LaunchSection&, const LaunchBlock&,
             std::int32_t blockOffset) noexcept;
    [[nodiscard]] std::uint64_t gainRevision() const noexcept;

    std::shared_ptr<const LaunchAudioProject> project_;
    std::shared_ptr<const LaunchAudioProject> beforeClock_;
    std::int64_t clockSample_;
    std::vector<std::int64_t> tracks_;
    std::vector<TrackRanges> ranges_;
    std::int32_t maxFrames_;
    bool active_ = false;
};

} // namespace adi::engine
