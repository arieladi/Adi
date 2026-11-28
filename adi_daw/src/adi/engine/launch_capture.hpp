// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <vector>

namespace adi::engine {
struct LaunchTransition;

/// A performed half-open timeline range with one forward source frame per
/// output sample. The first frame is relative to prepared clip PCM frame
/// zero at the driver rate. Exact raw replay checks that this is a straight,
/// same-rate reader and uses the frozen clip's authored source-file origin.
struct LaunchCapturedPcmRange {
    std::int64_t track = 0, clip = 0;
    std::uint64_t run = 0;
    std::int64_t beginSample = 0, endSample = 0, firstSourceFrame = 0;
    bool liveRegionEdit = false;
    float playbackGain = 1.0f;
    bool operator==(const LaunchCapturedPcmRange&) const = default;
};

/// The owner supplies strictly ascending positive track IDs. The capture
/// resets and uses lastRange internally while the driver owns the storage.
struct LaunchPcmTrackCursor {
    std::int64_t track = 0;
    std::size_t lastRange = std::numeric_limits<std::size_t>::max();
};

/// The owner supplies storage before attaching the capture to the driver. Once
/// detached, the owner can read the trace and prepare the entire Arrangement
/// edit. An overflow, unreplayable source edit, or Fade tail invalidates the trace.
class LaunchCaptureBuffer {
public:
    explicit LaunchCaptureBuffer(std::span<LaunchTransition> storage) noexcept;
    LaunchCaptureBuffer(std::span<LaunchTransition> storage, std::span<LaunchCapturedPcmRange> pcmStorage,
                        std::span<LaunchPcmTrackCursor> pcmTracks) noexcept;
    bool append(const LaunchTransition&) noexcept;
    /// Driver only. Disabled tracing accepts a no-op. Contiguous source
    /// frames merge with the last range for the same track, clip and run.
    bool appendPcmRange(const LaunchCapturedPcmRange&) noexcept;
    /// Every contiguous driver slice, including slices without Session PCM.
    void notePcmTimeline(std::int64_t firstSample, std::int64_t endSample) noexcept;
    void markPcmDiscontinuity() noexcept { pcmInvalid_ = capturesPcm_; }
    void markSourceEdit() noexcept { sourceEdited_ = true; }
    /// Driver only. A tail may sound after its original run left Session.
    void markFadeTail() noexcept { fadeTail_ = true; }
    [[nodiscard]] bool overflowed() const noexcept { return overflowed_ || pcmOverflowed_; }
    [[nodiscard]] bool sourceEdited() const noexcept { return sourceEdited_; }
    [[nodiscard]] bool hasFadeTail() const noexcept { return fadeTail_; }
    [[nodiscard]] bool capturesPcm() const noexcept { return capturesPcm_; }
    [[nodiscard]] bool pcmOverflowed() const noexcept { return pcmOverflowed_; }
    [[nodiscard]] bool pcmInvalid() const noexcept { return pcmInvalid_; }
    [[nodiscard]] std::span<const LaunchTransition> transitions() const noexcept;
    [[nodiscard]] std::span<const LaunchCapturedPcmRange> pcmRanges() const noexcept;

private:
    std::span<LaunchTransition> storage_;
    std::size_t size_ = 0;
    bool overflowed_ = false;
    bool sourceEdited_ = false, fadeTail_ = false;
    std::span<LaunchCapturedPcmRange> pcmStorage_{};
    std::span<LaunchPcmTrackCursor> pcmTracks_{};
    std::size_t pcmSize_ = 0;
    std::int64_t pcmTimelineEnd_ = -1;
    bool capturesPcm_ = false, pcmOverflowed_ = false, pcmInvalid_ = false;
};

struct LaunchCaptureTime {
    std::int64_t sample = 0, tick = 0;
    bool operator==(const LaunchCaptureTime&) const = default;
};

/// Initially Arrangement-owned tracks can be omitted. Clip zero is an
/// explicitly stopped Session track, which still covers its Arrangement.
struct LaunchCapturedStart {
    std::int64_t track = 0, clip = 0, sourceTick = 0;
    float velocity = 1.0f;
    bool fade = false;
    std::int64_t fadeElapsed = 0;
    std::uint64_t run = 0; // stable driver identity when capture starts mid-run
    bool operator==(const LaunchCapturedStart&) const = default;
};

struct LaunchCapturedSpan {
    std::int64_t track = 0, clip = 0, sourceTick = 0;
    LaunchCaptureTime begin, end;
    float velocity = 1.0f;
    bool fade = false;
    std::int64_t fadeElapsed = 0;
    std::uint64_t run = 0; // the literal driver run when PCM tracing was enabled
    bool operator==(const LaunchCapturedSpan&) const = default;
};

enum class LaunchCaptureError {
    None, Overflow, InvalidRange, InconsistentTrack, SourceEdited, PcmRangeOverflow, InvalidPcmRange, FadeTail
};

struct LaunchCapturePlan {
    LaunchCaptureError error = LaunchCaptureError::None;
    /// Chronological spans; clip zero removes existing Arrangement material
    /// from the recorded interval, while omitted intervals leave it playing.
    std::vector<LaunchCapturedSpan> spans;
    /// Chronological by (beginSample, track). Gaps may correspond to material
    /// outside the producer's eligibility. Replay requires covering the
    /// complete selected span before relying on these ranges.
    std::vector<LaunchCapturedPcmRange> pcmRanges{};
    bool pcmTraceEnabled = false;
};

/// Owner only. The input trace can be track-interleaved, with consecutive
/// transitions on a single track ordered by the driver. Retaining the
/// performed samples and ticks lets an Arrangement adapter check its clock.
[[nodiscard]] LaunchCapturePlan planLaunchCapture(
    const LaunchCaptureBuffer&, LaunchCaptureTime begin, LaunchCaptureTime end,
    std::span<const LaunchCapturedStart> initial);

} // namespace adi::engine
