// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include "adi/engine/launch_gestures.hpp"
#include "adi/engine/launch_quantize.hpp"

#include <memory>
#include <vector>

namespace adi::engine {

/// Owner-prepared launch metadata. Source boundaries use clip-local ADI ticks.
/// An empty slot can override the default Stop button with hasStop = false.
struct LaunchSlot {
    std::int64_t track = 0, scene = 0, clip = 0;
    std::int64_t startTick = 0, endTick = 0, loopStartTick = 0, loopEndTick = 0;
    LaunchMode mode = LaunchMode::Trigger;
    std::optional<LaunchQuantization> quantization; // absent means Global
    float velocityAmount = 0.0f;
    bool legato = false, fade = false, hasStop = true;
};

struct LaunchView {
    std::span<const std::int64_t> tracks; // ascending positive IDs
    std::span<const LaunchSlot> slots;  // ascending (track, scene)
    std::span<const std::int64_t> scenes; // ascending positive IDs
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
};

struct LaunchFollowRequest {
    LaunchRequest request;
    std::int64_t tick = 0;
};

enum class LaunchTrackSource : std::uint8_t { Arrangement, Session, Silence };
enum class LaunchQueuedAction : std::uint8_t { None, Start, Stop, BackToArrangement };

struct LaunchQueuedStatus {
    LaunchQueuedAction action = LaunchQueuedAction::None;
    std::int64_t clip = 0, scene = 0, tick = 0;
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

struct LaunchSink {
    void* context = nullptr;
    void (*section)(void*, const LaunchSection&) noexcept = nullptr;
    void (*transition)(void*, const LaunchTransition&) noexcept = nullptr;
    // A returned same-track action belongs to this run and is discarded when
    // another launch replaces it. Its tick must follow the launch tick.
    std::optional<LaunchFollowRequest> (*followOnLaunch)(void*, const LaunchTransition&) noexcept = nullptr;
    void (*trackStatus)(void*, const LaunchTrackStatus&) noexcept = nullptr;
};

/// One owner posts gestures and one driver renders. Capacity and all scratch
/// are established off the audio thread. Sections and transitions for each
/// track are chronological, with tracks visited in ascending ID order.
class Launcher {
public:
    explicit Launcher(std::size_t maximumTracks);
    [[nodiscard]] bool post(std::span<const LaunchRequest> requests) noexcept {
        return gestures_.push(requests);
    }
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

private:
    enum class Action : std::uint8_t { Start, Stop, Arrangement };
    struct Pending {
        Action action;
        std::int64_t tick, scene = 0, clip = 0;
        float velocity = 1.0f;
        LaunchQuantization grid = LaunchQuantization::None;
        bool repeat = false;
        std::int64_t earliestSample = 0;
        bool sceneGesture = false;
        std::uint64_t admission = 0;
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
    };
    struct Track {
        std::int64_t id = 0;
        Run run;
        std::optional<Pending> user, back, gateStop, follow, repeat;
        std::uint64_t followOrdinal = 0;
    };

    [[nodiscard]] Track* findTrack(std::int64_t) noexcept;
    void adopt(const LaunchBlock&, const LaunchView&, LaunchSink) noexcept;
    [[nodiscard]] bool valid(const LaunchRequest&, const LaunchBlock&, const LaunchView&) noexcept;
    void drain(const LaunchBlock&, const LaunchView&, std::int64_t) noexcept;
    void applyUser(const LaunchRequest&, const LaunchBlock&, const LaunchView&, std::int64_t) noexcept;
    void applyFollow(const LaunchFollowRequest&, const LaunchBlock&, const LaunchView&,
                     std::int64_t) noexcept;
    void render(Track&, const LaunchBlock&, const LaunchView&, LaunchSink, std::int64_t) noexcept;
    void transition(Track&, const LaunchBlock&, const LaunchView&, LaunchSink, const Pending&,
                    std::int32_t, std::int64_t) noexcept;
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
    std::vector<Track> tracks_, scratch_;
    Track* activeTracks_ = nullptr;
    Track* spareTracks_ = nullptr;
    std::size_t trackCount_ = 0;
    std::uint64_t incarnation_ = 0, runSerial_ = 0, followPass_ = 0, admission_ = 0;
    std::int64_t nextSample_ = -1, nextTick_ = 0;
    std::array<StatusBuffer,statusBufferCount> statusBuffers_;
    std::atomic<std::uint32_t> publishedStatus_{statusBufferCount};
    std::uint64_t statusSerial_ = 0;
};

} // namespace adi::engine
