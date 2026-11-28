// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include "adi/engine/clip_update_rate.hpp"
#include "adi/engine/launch_gestures.hpp"
#include "adi/engine/launch_quantize.hpp"

#include <memory>
#include <vector>

namespace adi::engine {

struct MidiNotePosition;

/// Owner-prepared launch metadata. Source boundaries use clip-local ADI ticks.
/// An empty slot can override the default Stop button with hasStop = false.
struct LaunchSlot {
    std::int64_t track = 0, scene = 0, clip = 0;
    std::int64_t startTick = 0, endTick = 0, loopStartTick = 0, loopEndTick = 0;
    LaunchMode mode = LaunchMode::Trigger;
    std::optional<LaunchQuantization> quantization; // absent means Global
    float velocityAmount = 0.0f;
    bool legato = false, fade = false, hasStop = true;
    // Absolute pins keep their native extent and laps when the project clock changes.
    std::int64_t physicalStartFrame = -1, physicalFrames = 0;
    std::int64_t physicalLoopBeginFrame = 0, physicalLoopEndFrame = 0;
    // A resident tick-only reader can keep its old flat aperture until its
    // run actually renders on a different clock.
    double residentRawFixedBpm = 0;
};

struct LaunchView {
    std::span<const std::int64_t> tracks; // ascending positive IDs
    std::span<const LaunchSlot> slots;  // ascending (track, scene)
    std::span<const std::int64_t> scenes; // ascending positive IDs
};

struct PreparedSceneLaunch {
    std::int64_t scene = 0, tick = 0;
    float velocity = 1.0f;
    // The producer keeps a stable increasing token across repeated reads.
    std::uint64_t token = 0;
    double tempoBpm = 0;
    std::int32_t signatureNumerator = 0, signatureDenominator = 0;
};

/// One contiguous range on the rendered generation's transport clock. Split
/// at a transport wrap. The clock maps non-negative samples and ticks, with
/// the same projection that placed the Arrangement clips.
struct LaunchBlock {
    std::int64_t sample = 0;
    std::int32_t frames = 0;
    std::uint64_t incarnation = 0;
    LaunchQuantization global = LaunchQuantization::Bar;
    std::span<const rows::TimeSignature> meters;
    const void* clock = nullptr;
    std::int64_t (*tickAtSample)(const void*, std::int64_t) noexcept = nullptr;
    std::int64_t (*sampleAtTick)(const void*, std::int64_t) noexcept = nullptr;
    bool followActionsEnabled = true;
    std::uint64_t followPass = 0;
    // Set only for a natural Arrangement wrap, never for an explicit locate.
    bool arrangementWrap = false;
    std::optional<PreparedSceneLaunch> preparedScene{};
    double sampleRate = 48000.0;
    double (*preciseTickAtSample)(const void*, std::int64_t) noexcept = nullptr;
    bool preparedScenePinned = false; // an absent token then revokes the old published scene
    // Preserve the fractional position separately for note-relative AEXP.
    MidiNotePosition (*notePositionAtSample)(const void*, std::int64_t) noexcept = nullptr;
};

struct LaunchSection {
    std::int64_t track = 0, clip = 0, sourceTick = 0, endSourceTick = 0;
    std::int32_t offset = 0, frames = 0;
    float velocity = 1.0f;
    bool arrangement = true, fade = false;
    std::uint64_t run = 0;
    std::int64_t framesSinceStart = 0;
    std::int64_t timelineTick = 0;
    // Constant-tempo prepared audio advances from the local anchor in frames.
    // The anchor restarts at launches and wraps, independent of block size.
    std::int64_t sourceAnchorTick = 0, framesSinceSourceAnchor = 0;
    bool regionIsLiveEdit = false;
    // Musical anchor of the current run or lap, independent of callback size.
    std::int64_t clockAnchorTick = 0, clockAnchorSourceTick = 0;
    std::int64_t physicalSourceAnchorFrame = -1;
    bool rawProjectAperture = false;
};

struct LaunchTransition {
    std::int64_t track = 0, oldClip = 0, newClip = 0, sourceTick = 0, sample = 0, tick = 0;
    std::int32_t offset = 0;
    bool arrangement = true;
    std::uint64_t oldRun = 0, newRun = 0;
    std::uint64_t followOrdinal = 0;
    float velocity = 1.0f;
    bool fade = false;
    std::int64_t oldSourceAnchorTick = 0, oldFramesSinceSourceAnchor = 0;
    std::int64_t oldFramesSinceStart = 0;
    float oldVelocity = 1.0f;
    bool oldFade = false;
    std::int64_t oldClockAnchorTick = 0, oldClockAnchorSourceTick = 0;
    std::int64_t oldPhysicalSourceAnchorFrame = -1;
    std::int64_t oldFramesUntilMusicalEnd = -1;
    bool oldRawProjectAperture = false;
};

struct LaunchFollowRequest {
    LaunchRequest request;
    std::int64_t tick = 0;
    std::int64_t physicalLaps = 0;
    bool linked = false;
    std::int64_t minimumMusicalLapTicks = 0;
};

struct LaunchSceneTransition {
    std::int64_t scene = 0, sample = 0, tick = 0;
    std::int32_t offset = 0;
    std::uint64_t ordinal = 0;
    bool fromFollow = false;
    std::uint64_t preparedToken = 0;
    double tempoBpm = 0; // filled by the graph when the receipt applies tempo
    std::int32_t signatureNumerator = 0, signatureDenominator = 0;
};

enum class LaunchTrackSource : std::uint8_t { Arrangement, Session, Silence };
enum class LaunchQueuedAction : std::uint8_t { None, Start, Stop, BackToArrangement };

struct LaunchPhysicalRegion {
    std::int64_t startFrame, endFrame, loopBeginFrame = 0, loopEndFrame = 0;
};

struct LaunchSourceCue {
    struct ScheduledRepeat { std::int64_t tick, sourceTick, followingTick; };
    std::int64_t tick = 0, physicalFrame = -1;
    std::optional<ScheduledRepeat> repeat{};
};

struct LaunchQueuedStatus {
    LaunchQueuedAction action = LaunchQueuedAction::None;
    std::int64_t clip = 0, scene = 0, tick = 0;
    std::optional<std::int64_t> legatoSourceTick{};
    std::optional<std::int64_t> legatoSourceFrame{};
    bool operator==(const LaunchQueuedStatus&) const = default;
};

/// A track's completed render state. Clip/scene/run are zero without a Session
/// run. An active clip and a different fired slot can coexist on the same track.
struct LaunchTrackStatus {
    std::int64_t track = 0, clip = 0, scene = 0, sourceTick = 0;
    std::uint64_t run = 0, completedLoops = 0;
    LaunchTrackSource source = LaunchTrackSource::Arrangement;
    LaunchQueuedStatus queued;
    std::int64_t loopStartTick = 0, loopLengthTicks = 0, remainingSamples = 0;
    bool operator==(const LaunchTrackStatus&) const = default;
};

struct LaunchStatusHeader {
    std::uint64_t incarnation = 0, revision = 0;
    std::int64_t endSample = 0, endTick = 0;
    std::size_t trackCount = 0;
    bool transportPlaying = false, backToArrangement = false;
};

enum class LaunchCueAdmission : std::uint8_t { None, Scheduled, Linked, MusicalLinked, ReadyOnly, HeldRepeat };

struct LaunchSink {
    void* context = nullptr;
    void (*section)(void*, const LaunchSection&) noexcept = nullptr;
    void (*transition)(void*, const LaunchTransition&) noexcept = nullptr;
    // A returned same-track action belongs to this run and is discarded when
    // another launch replaces it. Its tick must follow the launch tick.
    std::optional<LaunchFollowRequest> (*followOnLaunch)(void*, const LaunchTransition&) noexcept = nullptr;
    void (*trackStatus)(void*, const LaunchTrackStatus&) noexcept = nullptr;
    void (*scenePerformed)(void*, const LaunchSceneTransition&) noexcept = nullptr;
    std::optional<LaunchFollowRequest> (*followOnSceneLaunch)(void*, const LaunchSceneTransition&) noexcept = nullptr;
    void (*retargeted)(void*,const LaunchSlot& previous,const LaunchSlot& next,std::uint64_t run) noexcept = nullptr;
    // May reserve a prospective cue before the running track is changed.
    bool (*admitLegatoCue)(void*, const LaunchSlot&, const LaunchSourceCue&, std::int64_t launchTick,
                           std::int32_t offset, LaunchCueAdmission) noexcept = nullptr;
    // Driver-only forecast. Linked follows stay out of the user-queued status.
    void (*prefetchLinkedLegato)(void*, const LaunchSlot&, std::int64_t sample, const LaunchSourceCue&) noexcept = nullptr;
    std::int64_t (*residentRawAnchor)(void*, std::int64_t clip, std::int64_t tick) noexcept = nullptr;
};

/// One owner posts gestures and one driver renders. Capacity and all scratch
/// are established off the audio thread. Sections and transitions for each
/// track are chronological, with tracks visited in ascending ID order.
#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable: 4324) // mailbox counters occupy separate cache lines
#endif
class Launcher {
public:
    explicit Launcher(std::size_t maximumTracks);
    [[nodiscard]] bool post(std::span<const LaunchRequest> requests) noexcept {
        return gestures_.push(requests);
    }
    /// Owner only. Queue playback bounds for a single active prepared run.
    [[nodiscard]] bool postRegionEdit(const LaunchClipRegionEdit&, const LaunchSlot* prepared = nullptr) noexcept;
    /// Returns false on an invalid block or a view larger than prepared.
    bool process(const LaunchBlock&, const LaunchView&, LaunchSink = {},
                 std::span<const LaunchFollowRequest> = {}) noexcept;
    /// Driver only. Drain a stopped block's valid gestures. A surviving fresh
    /// launch asks the caller to roll the clock; process() then runs it. Fresh
    /// stops and Back to Arrangement can settle even while the clock is held.
    [[nodiscard]] bool admitWhileStopped(const LaunchBlock&, const LaunchView&, LaunchSink = {}) noexcept;
    /// Driver only. Mark the last completed render state as stopped when the
    /// caller can no longer provide a LaunchView and held transport position.
    void suspendStatus() noexcept;
    /// Copy one coherent, completed frame from a non-driver thread. If storage
    /// is short, fills header with the required trackCount and leaves rows alone.
    /// Returns false before publication, on contention or on short storage.
    [[nodiscard]] bool readStatus(std::span<LaunchTrackStatus>, LaunchStatusHeader&) const noexcept;
    /// Driver only, at a graph boundary before consuming the next gestures.
    [[nodiscard]] bool anyRunningClip(const void*,
        bool (*matches)(const void*, std::int64_t track, std::int64_t clip, std::uint64_t run) noexcept) const noexcept;

private:
    enum class Action : std::uint8_t { Start, Stop, Arrangement };
    enum class RetargetResult : std::uint8_t { Rejected, PendingView, Applied };
    struct Pending {
        Action action;
        std::int64_t tick, scene = 0, clip = 0;
        float velocity = 1.0f;
        LaunchQuantization grid = LaunchQuantization::None;
        bool repeat = false;
        std::int64_t earliestSample = 0;
        bool sceneGesture = false;
        std::uint64_t admission = 0;
        bool prepared = false;
        LaunchCueAdmission linkedAdmission = LaunchCueAdmission::None;
        std::int64_t physicalLaps = 0, minimumMusicalLapTicks = 0;
    };
    struct Run {
        std::int64_t scene = 0, clip = 0, sourceTick = 0, frames = 0;
        float velocity = 1.0f;
        std::uint64_t serial = 0;
        bool arrangement = true;
        std::int64_t anchorTick = 0, anchorSourceTick = 0;
        std::int64_t pcmAnchorTick = 0, pcmAnchorFrame = 0;
        std::uint64_t completedLoops = 0;
        bool fade = false;
        std::optional<LaunchClipRegion> region{};
        std::optional<LaunchPhysicalRegion> physicalRegion{};
        std::int64_t physicalCueFrame = -1;
        std::int64_t residentRawAnchorFrame = -1;
        double residentRawFixedBpm = 0;
        double residentRawFrameRate = 0;
    };
    struct RegionRequest {
        LaunchClipRegionEdit edit;
        std::optional<LaunchPhysicalRegion> physical;
    };
    struct RegionPending {
        LaunchClipRegionEdit edit;
        ClipUpdateBoundary boundary;
        std::optional<LaunchPhysicalRegion> physical;
    };
    struct Track {
        std::int64_t id = 0;
        Run run;
        std::optional<Pending> user, back, gateStop, follow, repeat, sceneFollow, dueScene;
        std::uint64_t followOrdinal = 0;
        std::optional<RegionPending> regionEdit{};
        std::uint64_t stoppedPreparedThrough = 0;
        bool stopSuppressesFollow = false;
    };
    struct ScenePending {
        std::int64_t scene, tick, earliestSample = 0;
        LaunchQuantization grid = LaunchQuantization::None;
        std::uint64_t admission = 0;
        std::uint64_t preparedToken = 0;
        float velocity = 1.0f;
    };

    [[nodiscard]] Track* findTrack(std::int64_t) noexcept;
    void adopt(const LaunchBlock&, const LaunchView&, LaunchSink) noexcept;
    [[nodiscard]] bool valid(const LaunchRequest&, const LaunchBlock&, const LaunchView&) noexcept;
    [[nodiscard]] RetargetResult retarget(std::span<const LaunchRequest>, const LaunchBlock&,
                                          const LaunchView&, LaunchSink) noexcept;
    void drain(const LaunchBlock&, const LaunchView&, std::int64_t, LaunchSink) noexcept;
    void drainRegionEdits(const LaunchBlock&, const LaunchView&) noexcept;
    void queueStop(Track&, const Pending&, std::uint64_t preparedThrough) noexcept;
    [[nodiscard]] bool preparedFollowersCanceled(const Track&, std::uint64_t token) const noexcept;
    void applyUser(const LaunchRequest&, const LaunchBlock&, const LaunchView&, std::int64_t) noexcept;
    void armUserScene(ScenePending, float velocity, const LaunchBlock&, const LaunchView&,
                      std::int64_t now) noexcept;
    void applyFollow(const LaunchFollowRequest&, const LaunchBlock&, const LaunchView&,
                     std::int64_t, std::int64_t earliestSample = 0) noexcept;
    void cancelSceneFollow() noexcept;
    void discardUnpublishedScene(const LaunchBlock&) noexcept;
    void settleScene(const ScenePending&, bool fromFollow, bool rolling, const LaunchBlock&,
                     const LaunchView&, LaunchSink, std::int32_t offset) noexcept;
    void render(Track&, const LaunchBlock&, const LaunchView&, LaunchSink, std::int64_t,
                std::int32_t begin, std::int32_t end) noexcept;
    bool transition(Track&, const LaunchBlock&, const LaunchView&, LaunchSink, const Pending&,
                    std::int32_t, std::int64_t, LaunchCueAdmission = LaunchCueAdmission::None,
                    std::int64_t projectedEnd = -1) noexcept;
    [[nodiscard]] std::optional<LaunchSourceCue> queuedLegatoSourceCue(const Track&, const Pending&,
        const LaunchSlot& target, const LaunchSlot* current, const LaunchBlock&, std::int64_t endSample,
        std::int64_t* due = nullptr) const noexcept;
    void prefetchLinkedLegato(const Track&, const LaunchView&, const LaunchBlock&, LaunchSink,
                              std::int64_t endSample) const noexcept;
    [[nodiscard]] LaunchTrackStatus trackStatus(const Track&, const LaunchBlock&,
                                                const LaunchView&, std::int64_t) const noexcept;
    void publishStatus(const LaunchBlock&, const LaunchView&, LaunchSink, bool, std::int64_t) noexcept;

    // The driver only claims a spare frame. Readers pin a published frame with
    // one bounded atomic reservation while they copy its ordinary row values.
    static constexpr std::uint32_t statusBufferCount = 3;
    struct StatusBuffer {
        mutable std::atomic<std::uint32_t> readers{0};
        std::unique_ptr<LaunchTrackStatus[]> rows;
        LaunchStatusHeader header;
    };
    [[nodiscard]] std::uint32_t claimStatusBuffer() noexcept;
    void finishStatusBuffer(std::uint32_t) noexcept;

    LaunchGestureQueue gestures_;
    std::array<LaunchRequest, LaunchGestureQueue::capacity> batch_{};
    // The owner may enqueue the captured scene while the driver is finishing
    // an older pinned graph. Keep one literal transfer until that scene exists.
    std::array<LaunchRequest, LaunchGestureQueue::capacity> deferredRetarget_{};
    std::size_t deferredRetargetSize_ = 0;
    std::array<RegionRequest, LaunchGestureQueue::capacity> regionMailbox_{};
    alignas(64) std::atomic<std::uint64_t> regionWritten_{0};
    alignas(64) std::atomic<std::uint64_t> regionRead_{0};
    std::vector<Track> tracks_, scratch_;
    Track* activeTracks_ = nullptr;
    Track* spareTracks_ = nullptr;
    std::size_t trackCount_ = 0;
    std::uint64_t incarnation_ = 0, runSerial_ = 0, followPass_ = 0, admission_ = 0;
    std::optional<ScenePending> userScene_, followScene_;
    std::uint64_t sceneOrdinal_ = 0;
    std::uint64_t lastPreparedScene_ = 0, stoppedPreparedAllThrough_ = 0;
    bool stopAllSuppressesSceneFollow_ = false;
    std::int64_t nextSample_ = -1, nextTick_ = 0;
    std::array<StatusBuffer,statusBufferCount> statusBuffers_;
    std::atomic<std::uint32_t> publishedStatus_{statusBufferCount};
    std::uint64_t statusSerial_ = 0;
};
#if defined(_MSC_VER)
#pragma warning(pop)
#endif

} // namespace adi::engine
