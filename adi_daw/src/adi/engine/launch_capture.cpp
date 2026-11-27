// SPDX-License-Identifier: AGPL-3.0-or-later
#include "adi/engine/launch_capture.hpp"

#include "adi/engine/launcher.hpp"

#include <algorithm>
#include <cmath>
#include <map>
#include <tuple>

namespace adi::engine {
LaunchCaptureBuffer::LaunchCaptureBuffer(std::span<LaunchTransition> storage) noexcept
    : storage_(storage) {}

bool LaunchCaptureBuffer::append(const LaunchTransition& transition) noexcept {
    if (size_ == storage_.size()) {
        overflowed_ = true;
        return false;
    }
    storage_[size_++] = transition;
    return true;
}

std::span<const LaunchTransition> LaunchCaptureBuffer::transitions() const noexcept {
    return std::span<const LaunchTransition>(storage_.data(), size_);
}

LaunchCapturePlan planLaunchCapture(const LaunchCaptureBuffer& trace, LaunchCaptureTime begin,
                                    LaunchCaptureTime end,
                                    std::span<const LaunchCapturedStart> initial) {
    if (trace.overflowed())
        return {LaunchCaptureError::Overflow, {}};
    if (trace.sourceEdited())
        return {LaunchCaptureError::SourceEdited, {}};
    if (begin.sample < 0 || begin.tick < 0 || end.sample <= begin.sample || end.tick < begin.tick)
        return {LaunchCaptureError::InvalidRange, {}};

    struct TrackRun {
        bool arrangement = true;
        std::int64_t clip = 0, sourceTick = 0;
        LaunchCaptureTime begin, last;
        float velocity = 1.0f;
        bool fade = false;
        std::int64_t fadeElapsed = 0;
        std::uint64_t serial = 0;
    };
    std::map<std::int64_t, TrackRun> runs;
    for (const auto& start : initial) {
        if (start.track <= 0 || start.clip < 0 || start.sourceTick < 0 ||
            !std::isfinite(start.velocity) || start.velocity < 0 || start.velocity > 1 ||
            start.fadeElapsed < 0 ||
            !runs.emplace(start.track, TrackRun{false, start.clip, start.sourceTick, begin, begin,
                start.velocity, start.fade, start.fadeElapsed, start.run}).second)
            return {LaunchCaptureError::InconsistentTrack, {}};
    }

    LaunchCapturePlan plan;
    const auto close = [&](std::int64_t track, const TrackRun& run, LaunchCaptureTime at) {
        if (!run.arrangement && at.sample > run.begin.sample)
            plan.spans.push_back({track, run.clip, run.sourceTick, run.begin, at,
                run.velocity, run.fade, run.fadeElapsed});
    };
    for (const auto& transition : trace.transitions()) {
        const LaunchCaptureTime at{transition.sample, transition.tick};
        if (transition.track <= 0 || transition.oldClip < 0 || transition.newClip < 0 ||
            transition.sourceTick < 0 || (transition.arrangement && transition.newClip != 0) ||
            !std::isfinite(transition.velocity) || transition.velocity < 0 || transition.velocity > 1)
            return {LaunchCaptureError::InconsistentTrack, {}};
        if (at.sample < begin.sample || at.sample > end.sample || at.tick < begin.tick || at.tick > end.tick)
            return {LaunchCaptureError::InvalidRange, {}};
        auto& run = runs.try_emplace(transition.track, TrackRun{true, 0, 0, begin, begin}).first->second;
        // Capture and Insert Scene retargets the exact material without a run
        // edge. The original clip still describes the one uninterrupted span.
        const auto hasRunIdentity = run.serial != 0 && transition.oldRun != 0;
        const auto sameRun = hasRunIdentity && run.serial == transition.oldRun;
        const auto sameMaterial = run.clip == transition.oldClip || (sameRun && run.clip > 0 && transition.oldClip > 0);
        if (!sameMaterial || (hasRunIdentity && !sameRun) || at.sample < run.last.sample || at.tick < run.last.tick)
            return {LaunchCaptureError::InconsistentTrack, {}};
        run.last = at;
        if (run.clip == 0 && transition.newClip == 0 && run.arrangement == transition.arrangement)
            continue;
        close(transition.track, run, at);
        run = {transition.arrangement, transition.newClip, transition.sourceTick, at, at,
            transition.velocity, transition.fade, 0, transition.newRun};
    }
    for (const auto& [track, run] : runs)
        close(track, run, end);
    std::sort(plan.spans.begin(), plan.spans.end(), [](const auto& left, const auto& right) {
        return std::tie(left.begin.sample, left.track) < std::tie(right.begin.sample, right.track);
    });
    return plan;
}
} // namespace adi::engine
