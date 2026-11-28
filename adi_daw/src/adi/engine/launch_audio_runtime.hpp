// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include "adi/engine/launch_audio.hpp"
#include "adi/engine/launch_gain_edits.hpp"
#include "adi/engine/launcher.hpp"

#include <atomic>
#include <functional>
#include <map>
#include <memory>
#include <utility>

namespace adi::engine {

struct NodeIo;
class Transport;
class LaunchFollowPlayer;
class LaunchCaptureBuffer;
class LaunchAudioRuntime;

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
    [[nodiscard]] bool hasPagedAudio() const noexcept;
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

// Graph catalogs and the owner registry share these descriptors. The audio
// driver only increments/decrements the independent raw lease count.
struct LaunchPinnedSource {
    explicit LaunchPinnedSource(LaunchAudioClip source) : pcm(std::move(source)) {}
    LaunchAudioClip pcm;
    static_assert(std::atomic<std::uint32_t>::is_always_lock_free);
    std::atomic<std::uint32_t> leases{0};
};

struct LaunchRenderSlice {
    LaunchSection section;
    LaunchBlock block;
    std::int32_t baseOffset = 0; // Arrangement-loop slice origin within the logical callback
};

/// A prepared resident source revision for one still-running clip.
struct LaunchClipSourceEdit {
    std::int64_t track = 0, clip = 0;
    std::uint64_t incarnation = 0, run = 0;
    ClipUpdateRate rate = ClipUpdateRate::Sixteenth;
};

namespace detail {
/// An owner may stage a buffer while idle. Its exact rolling token must
/// still match when it publishes. A stop closes both roles in one update.
class LaunchSourceEditLease {
public:
    [[nodiscard]] std::optional<std::uint64_t> idleToken() const noexcept {
        const auto state = state_.load(std::memory_order_acquire);
        return (state & flags) == rolling ? std::optional{state} : std::nullopt;
    }
    [[nodiscard]] bool publish(std::uint64_t token) noexcept {
        return (token & flags) == rolling && state_.compare_exchange_strong(token,token | reserved,
            std::memory_order_release,std::memory_order_relaxed);
    }
    [[nodiscard]] bool held() const noexcept {
        return (state_.load(std::memory_order_acquire) & reserved) != 0;
    }
    void resume() noexcept {
        const auto state = state_.load(std::memory_order_relaxed);
        if ((state & rolling) == 0) state_.store((state + generationStep) | rolling,std::memory_order_release);
    }
    void suspend() noexcept {
        if ((state_.load(std::memory_order_relaxed) & rolling) != 0)
            state_.fetch_and(~flags,std::memory_order_release);
    }
    void release() noexcept { state_.fetch_and(~reserved,std::memory_order_release); }

private:
    static constexpr std::uint64_t rolling = 1, reserved = 2, flags = rolling | reserved, generationStep = 4;
    static_assert(std::atomic<std::uint64_t>::is_always_lock_free);
    std::atomic<std::uint64_t> state_{0};
};
} // namespace detail

/// Session-owned state that survives graph replacement. The gesture and gain
/// mailboxes carry live commands from the owner to the driver.
class LaunchAudioDriver {
public:
    explicit LaunchAudioDriver(std::size_t tracks) : launcher_(tracks), gains_(tracks), tails_(tracks * 2),
        retained_(tracks), retiring_(tracks * 4) {}
    [[nodiscard]] bool post(std::span<const LaunchRequest> requests) noexcept { return launcher_.post(requests); }
    [[nodiscard]] bool readStatus(std::span<LaunchTrackStatus> tracks, LaunchStatusHeader& header) const noexcept {
        return launcher_.readStatus(tracks, header);
    }
    [[nodiscard]] bool dropsRunningProtectedAudio(const LaunchAudioRuntime& before, const LaunchAudioRuntime* after,
                                              std::int64_t sample, bool externalTempo, bool pinnedPages) const noexcept;
    void suspend() noexcept;
    void retainPinnedRuns(const LaunchAudioRuntime& before, const LaunchAudioRuntime& after,
                          std::int64_t sample) noexcept;
    void discardIncompatiblePinnedRuns(const LaunchAudioRuntime*, std::int64_t sample) noexcept;
    void collectPinnedSourceOwners() noexcept; // owner only
    [[nodiscard]] bool regionEditsSettled(const LaunchStatusHeader& header,
                                          std::span<const std::int64_t> tracks) const noexcept {
        return launcher_.regionEditsSettled(header,tracks);
    }
    [[nodiscard]] std::uint64_t performedSceneToken() const noexcept {
        return performedScene_.load(std::memory_order_acquire);
    }
    [[nodiscard]] bool sourceEditReserved() const noexcept {
        return sourceLease_.held();
    }
    /// Driver or quiescent owner. Close source admission after the last callback.
    void suspendSourceEdits() noexcept;
    /// Owner only. Reclaim an edited buffer whose last callback has ended.
    void collectSourceEdit() noexcept;

private:
    friend class LaunchAudioRuntime;
    enum class SourceEditPhase : std::uint8_t { Idle, Pending, Applied, Retiring };
    struct SourceEdit {
        LaunchClipSourceEdit address;
        std::uint64_t baseline = 0;
        std::unique_ptr<LaunchAudioClip> clip;
    };
    struct SourceRange {
        std::int32_t beforeFrames;
        const LaunchAudioClip* replacement = nullptr;
    };
    [[nodiscard]] bool postSource(const LaunchClipSourceEdit&, std::uint64_t baseline,
                                  std::unique_ptr<LaunchAudioClip>&) noexcept;
    void releaseSourceEdit() noexcept;
    void beginSourceCallback() noexcept;
    void beginSourceSlice(const LaunchBlock&, const LaunchAudioProject&, std::span<const std::int64_t>) noexcept;
    void trackSourceStatus(const LaunchTrackStatus&) noexcept;
    [[nodiscard]] SourceRange sourceRange(const LaunchBlock&, const LaunchSection&, std::uint64_t baseline) noexcept;
    [[nodiscard]] bool hasSourceEdit() const noexcept;

    struct Tail {
        std::int64_t track, clip, sourceFrame, elapsed, fadeInAtExit;
        float velocity;
        std::uint64_t media;
        LaunchWarpPosition warp;
        LaunchPinnedSource* pinned = nullptr;
    };
    struct Retained {
        std::int64_t track = 0, clip = 0;
        std::uint64_t run = 0, incarnation = 0, gainRevision = 0;
        LaunchPinnedSource* source = nullptr;
        LaunchSlot slot;
    };
    [[nodiscard]] Retained* findRetained(std::int64_t track, std::size_t count) noexcept;
    [[nodiscard]] const Retained* retainedFor(std::int64_t, std::int64_t, std::uint64_t) const noexcept;
    void retireRun(std::int64_t, std::uint64_t) noexcept;
    void retargetPinnedRun(const LaunchSlot& previous,const LaunchSlot& next,std::uint64_t run) noexcept;
    void retire(LaunchPinnedSource*) noexcept;
    void finishRanges() noexcept;
    [[nodiscard]] std::shared_ptr<LaunchPinnedSource> ownerFor(const LaunchAudioClip&);
    Launcher launcher_;
    LaunchClipGainEdits gains_;
    // The owner writes and reclaims the prepared clip only while available.
    // The driver keeps the lease until the callback after the last raw use.
    detail::LaunchSourceEditLease sourceLease_;
    SourceEdit sourceEdit_;
    SourceEditPhase sourcePhase_ = SourceEditPhase::Idle;
    std::optional<ClipUpdateBoundary> sourceBoundary_;
    std::vector<Tail> tails_;
    std::size_t tailCount_ = 0;
    std::vector<Retained> retained_;
    std::size_t retainedCount_ = 0;
    bool endedRetainedRun_ = false;
    std::vector<LaunchPinnedSource*> retiring_;
    std::size_t retiringCount_ = 0;
    struct SourceKey {
        const LaunchPagedAudio* reader;
        std::int64_t clip;
        bool operator<(const SourceKey& other) const noexcept {
            return reader == other.reader ? clip < other.clip : std::less<const LaunchPagedAudio*>{}(reader,other.reader);
        }
    };
    std::map<SourceKey,std::shared_ptr<LaunchPinnedSource>> sourceOwners_; // owner thread only
    std::uint64_t tailIncarnation_ = 0;
    std::uint64_t pass_ = 0;
    bool wasPlaying_ = false;
    std::int64_t nextTimelineSample_ = -1, loopBegin_ = -1, loopEnd_ = -1;
    std::uint64_t transportRevision_ = 0;
    double lastMediaBpm_ = 0.0;
    std::uint64_t lastMediaRevision_ = 0;
    static_assert(std::atomic<std::uint64_t>::is_always_lock_free);
    std::atomic<std::uint64_t> performedScene_{0};
};

struct LaunchTempoBank {
    std::int64_t startSample;
    std::shared_ptr<const LaunchAudioProject> media;
};

/// Scratch belonging to one published graph. The driver fills the current
/// slice before rendering, then each track source reads its own ranges.
class LaunchAudioRuntime {
public:
    LaunchAudioRuntime(std::shared_ptr<const LaunchAudioProject>, std::vector<std::int64_t> tracks,
                       std::int32_t maxFrames,
                       std::shared_ptr<const LaunchAudioProject> beforeClock = {},
                       std::int64_t clockSample = 0, std::uint64_t compatibleGainRevision = 0,
                       LaunchAudioDriver* owner = nullptr);
    LaunchAudioRuntime(std::shared_ptr<const LaunchAudioProject>, std::vector<std::int64_t> tracks,
                       std::int32_t maxFrames, std::vector<LaunchTempoBank>,
                       std::uint64_t compatibleGainRevision = 0, LaunchAudioDriver* owner = nullptr);

    void startIfQueued(LaunchAudioDriver&, LaunchBlock, Transport&) const noexcept;
    [[nodiscard]] LaunchQuantization globalQuantization() const noexcept {
        return project_->globalQuantization();
    }
    void begin(LaunchAudioDriver&, LaunchBlock, const Transport&, std::int64_t streamSample,
               void* observerContext,
               void (*observer)(void*, const LaunchTransition&, std::int64_t) noexcept,
               void* sceneObserverContext = nullptr,
               void (*sceneObserver)(void*, const LaunchSceneTransition&, std::int64_t) noexcept = nullptr,
               LaunchCaptureBuffer* capture = nullptr) noexcept;
    void finish() noexcept;
    void finish(LaunchAudioDriver& driver) noexcept { finish(); driver.finishRanges(); }
    void overlay(std::int64_t track, const NodeIo&, std::span<std::uint8_t> replaces = {}) const noexcept;
    // Owner only. The source declares this latency before graph realization.
    [[nodiscard]] std::int32_t latencyForTrack(std::int64_t track) const noexcept;
    [[nodiscard]] std::uint64_t incarnation() const noexcept { return project_->incarnation(); }
    [[nodiscard]] std::uint64_t gainRevision() const noexcept;
    [[nodiscard]] const LaunchSceneClock* sceneClock(std::int64_t id) const noexcept { return project_->sceneClock(id); }
    [[nodiscard]] bool hasScene(std::int64_t scene) const noexcept;
    [[nodiscard]] bool postGainEdit(LaunchAudioDriver&, const LaunchClipGainEdit&) const noexcept;
    [[nodiscard]] bool postRegionEdit(LaunchAudioDriver&, const LaunchClipRegionEdit&, std::int64_t sample) const noexcept;
    /// One prepared replacement can be pending or active. The replacement
    /// belongs to the exact addressed run and current unchanging source bank.
    /// A rejected post leaves ownership with the caller.
    [[nodiscard]] bool postSourceEdit(LaunchAudioDriver&, const LaunchClipSourceEdit&,
                                      std::unique_ptr<LaunchAudioClip>&) const noexcept;
    [[nodiscard]] const std::shared_ptr<const LaunchAudioProject>& mediaAt(std::int64_t sample) const noexcept;
    [[nodiscard]] std::shared_ptr<const LaunchAudioProject> bankAtTempo(double bpm) const noexcept;
    [[nodiscard]] std::size_t tempoBankCount() const noexcept { return tempoBanks_.size(); }
    [[nodiscard]] bool hasTrack(std::int64_t track) const noexcept;
    [[nodiscard]] bool dropsWorkerWarp(const LaunchAudioRuntime* after, std::int64_t sample,
                                       std::int64_t track, std::int64_t clip) const noexcept;
    [[nodiscard]] bool dropsPinnedPages(const LaunchAudioRuntime* after, std::int64_t sample,
                                        std::int64_t track, std::int64_t clip, bool retainedOnly = false) const noexcept;
    [[nodiscard]] std::span<const LaunchRenderSlice> sectionsFor(std::int64_t track) const noexcept;

private:
    friend class LaunchAudioDriver;
    struct Candidate {
        LaunchSlot slot;
        std::shared_ptr<LaunchPinnedSource> source;
    };
    [[nodiscard]] const Candidate* pinnedCandidate(std::int64_t sample, std::int64_t track,
                                                   std::int64_t clip) const noexcept;
    [[nodiscard]] const Candidate* pinnedCandidate(const LaunchAudioProject&, std::int64_t track,
                                                   std::int64_t clip) const noexcept;
    struct Range {
        std::int32_t offset, frames;
        const LaunchAudioClip* pcm;
        std::int64_t sourceFrame;
        float velocity;
        bool useAuthoredLoop = true;
        std::int64_t fadeElapsed;
        LaunchWarpPosition warp;
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
        LaunchPagedAudio* cuePages = nullptr;
        std::int64_t cueClip = 0;
    };
    [[nodiscard]] TrackRanges* trackRanges(std::int64_t track) noexcept;
    [[nodiscard]] bool addTail(LaunchAudioDriver::Tail&, std::int32_t offset,
                               std::int32_t frames, LaunchCaptureBuffer*) noexcept;
    void fadeOut(LaunchAudioDriver&, const LaunchAudioProject&, const LaunchTransition&, std::int32_t offset,
                 std::int32_t blockFrames, LaunchCaptureBuffer*) noexcept;
    void add(LaunchAudioDriver&, const LaunchAudioProject&, const LaunchSection&, const LaunchBlock&,
             std::int32_t blockOffset, LaunchCaptureBuffer*) noexcept;
    [[nodiscard]] const LaunchAudioProject* mediaByRevision(std::uint64_t) const noexcept;

    std::shared_ptr<const LaunchAudioProject> project_;
    std::vector<LaunchTempoBank> tempoBanks_;
    std::vector<const LaunchAudioProject*> bankRevisions_;
    struct PinnedBank {
        const LaunchAudioProject* media;
        std::vector<Candidate> candidates;
    };
    std::vector<PinnedBank> pinnedBanks_;
    std::vector<std::int64_t> tracks_;
    std::vector<TrackRanges> ranges_;
    std::int32_t maxFrames_;
    bool active_ = false;
    std::uint64_t compatibleGainRevision_;
};

} // namespace adi::engine
