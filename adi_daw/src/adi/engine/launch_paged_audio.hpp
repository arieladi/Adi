// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include "adi/engine/clip_playback.hpp"

#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <vector>

namespace adi::engine {

struct LaunchPagedSource {
    std::int64_t clip = 0, sourceTrack = 0;
    std::int64_t firstFrame = 0, loopBeginFrame = 0, loopEndFrame = 0;
    std::int64_t endFrame = 0;
    bool warmPhysicalWindow = false, keepPhysicalStartWarm = false;
};

/// One paged player for a published collection of unrolled slot sources. A
/// source has a private track ID unique to its clip. The existing ClipPlayback
/// worker group serves every slot; the driver changes only a sample cursor and
/// a preallocated planar buffer. The graph's serial source path owns read().
class LaunchPagedAudio {
public:
    LaunchPagedAudio(std::shared_ptr<const ClipProject>, std::span<const LaunchPagedSource>,
                     std::uint32_t sampleRate, std::int32_t channels, std::int32_t maxFrames);
    LaunchPagedAudio(const LaunchPagedAudio&) = delete;
    LaunchPagedAudio& operator=(const LaunchPagedAudio&) = delete;

    /// Owner/worker before publication. Each first/loop phase must be warm at
    /// admission. No Store or future file callback survives into the driver.
    [[nodiscard]] bool prime(std::chrono::milliseconds timeout = std::chrono::seconds(10));
    [[nodiscard]] bool matchesFormat(std::uint32_t rate, std::int32_t channels, std::int32_t blockFrames) const noexcept {
        return sampleRate_ == rate && channels_ == channels && blockFrames_ == blockFrames;
    }
    [[nodiscard]] bool hasClip(std::int64_t clip) const noexcept;
    [[nodiscard]] std::int64_t firstFrame(std::int64_t clip) const noexcept;
    [[nodiscard]] std::int32_t latencySamples(std::int64_t clip) const noexcept;
    /// Owner query after prime for a newly published worker-backed bank.
    [[nodiscard]] bool allWorkerPhasesPrimed(std::int64_t clip) const noexcept;
    /// A published raw slot that admitted its entire physical window.
    [[nodiscard]] bool allPhysicalPhasesPrimed(std::int64_t clip) const noexcept;
    /// Driver only. Queue an idle clip's upcoming source phase while keeping
    /// its ordinary start and loop head in the same bounded worker request.
    void prefetch(std::int64_t clip, std::int64_t firstFrame) noexcept;

    /// Driver only. A successful reservation pins the initial physical cue
    /// and any first wrap across scheduling and overlay until releaseCue().
    /// By default a cold cue queues the worker. A ready-only check does not
    /// request pages, and a refused gesture leaves the outgoing run intact.
    /// A held Repeat also protects a return to saved Start at the first lap.
    [[nodiscard]] bool reserveCue(std::int64_t clip, std::int64_t frame, std::int32_t frames,
                                  bool requestAhead = true, bool repeatAtEnd = false) noexcept;
    /// Driver only. Pin the already requested aligned PCM and, if this
    /// callback can wrap, the loop head. Prepare a held Repeat's saved Start
    /// as read-ahead, and lease it too if the retrigger is in this callback.
    [[nodiscard]] bool reserveWorkerCue(std::int64_t clip, std::int64_t frame, std::int32_t frames,
                                        bool includeLoopHead, std::optional<std::int64_t> restartFrame = {},
                                        bool restartInBlock = false) noexcept;
    void releaseCue(std::int64_t clip) noexcept;

    /// Driver only. Missing pages emit silence and report the normal reader
    /// underrun count. Planar channels remain valid until the next read.
    [[nodiscard]] bool read(std::int64_t clip, std::int64_t firstFrame,
                            std::int32_t frames, bool allowLoop = true) noexcept;
    [[nodiscard]] std::span<const float> channel(std::int32_t channel) const noexcept;
    [[nodiscard]] std::uint32_t underrunSamples() const noexcept;
    [[nodiscard]] std::uint64_t pageBytesCeiling() const noexcept { return pages_.pageBytesCeiling(); }
    [[nodiscard]] const std::vector<std::string>& problems() const noexcept;
    void setRequestSnapshotProbeForTest(const ClipPlayback::RequestSnapshotProbeForTest* probe) noexcept {
        pages_.setRequestSnapshotProbeForTest(probe);
    }

private:
    struct Entry { LaunchPagedSource source; Node* playback; std::int32_t latency; bool primesAllPhases; };
    [[nodiscard]] const Entry* find(std::int64_t clip) const noexcept;
    Transport clock_;
    ClipPlayback pages_;
    std::vector<Entry> sources_;
    std::vector<float> output_;
    std::vector<std::string> problems_;
    std::int32_t channels_, blockFrames_, readFrames_ = 0;
    std::uint32_t sampleRate_;
};

} // namespace adi::engine
