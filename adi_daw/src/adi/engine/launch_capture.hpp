// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include <cstdint>
#include <span>
#include <vector>

namespace adi::engine {
struct LaunchTransition;

/// The owner supplies storage before attaching the capture to the driver. Once
/// detached, the owner can read the trace and prepare the entire Arrangement
/// edit. An overflow invalidates the whole trace.
class LaunchCaptureBuffer {
public:
    explicit LaunchCaptureBuffer(std::span<LaunchTransition> storage) noexcept;
    bool append(const LaunchTransition&) noexcept;
    [[nodiscard]] bool overflowed() const noexcept { return overflowed_; }
    [[nodiscard]] std::span<const LaunchTransition> transitions() const noexcept;

private:
    std::span<LaunchTransition> storage_;
    std::size_t size_ = 0;
    bool overflowed_ = false;
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
    bool operator==(const LaunchCapturedSpan&) const = default;
};

enum class LaunchCaptureError { None, Overflow, InvalidRange, InconsistentTrack };

struct LaunchCapturePlan {
    LaunchCaptureError error = LaunchCaptureError::None;
    /// Chronological spans; clip zero removes existing Arrangement material
    /// from the recorded interval, while omitted intervals leave it playing.
    std::vector<LaunchCapturedSpan> spans;
};

/// Owner only. The input trace can be track-interleaved, with consecutive
/// transitions on a single track ordered by the driver. Retaining the
/// performed samples and ticks lets an Arrangement adapter check its clock.
[[nodiscard]] LaunchCapturePlan planLaunchCapture(
    const LaunchCaptureBuffer&, LaunchCaptureTime begin, LaunchCaptureTime end,
    std::span<const LaunchCapturedStart> initial);

} // namespace adi::engine
