// SPDX-License-Identifier: AGPL-3.0-or-later
#include "adi/engine/launch_capture.hpp"

#include "adi/engine/launcher.hpp"

#include <algorithm>
#include <cmath>
#include <map>
#include <tuple>
#include <utility>

namespace adi::engine {
namespace {
constexpr auto noPcmRange = std::numeric_limits<std::size_t>::max();

bool validPcmRange(const LaunchCapturedPcmRange& range) noexcept {
    return range.track > 0 && range.clip > 0 && range.run != 0 &&
        range.beginSample >= 0 && range.endSample > range.beginSample && range.firstSourceFrame >= 0 &&
        std::isfinite(range.playbackGain) && range.playbackGain >= 0 &&
        range.firstSourceFrame <= std::numeric_limits<std::int64_t>::max() - (range.endSample - range.beginSample);
}
} // namespace

LaunchCaptureBuffer::LaunchCaptureBuffer(std::span<LaunchTransition> storage) noexcept
    : storage_(storage) {}

LaunchCaptureBuffer::LaunchCaptureBuffer(std::span<LaunchTransition> storage,
                                         std::span<LaunchCapturedPcmRange> pcmStorage,
                                         std::span<LaunchPcmTrackCursor> pcmTracks) noexcept
    : storage_(storage), pcmStorage_(pcmStorage), pcmTracks_(pcmTracks), capturesPcm_(true) {
    auto last = std::int64_t{0};
    for (auto& track : pcmTracks_) {
        if (track.track <= last) pcmInvalid_ = true;
        last = track.track;
        track.lastRange = noPcmRange;
    }
}

bool LaunchCaptureBuffer::append(const LaunchTransition& transition) noexcept {
    if (size_ == storage_.size()) {
        overflowed_ = true;
        return false;
    }
    storage_[size_++] = transition;
    return true;
}

void LaunchCaptureBuffer::notePcmTimeline(std::int64_t firstSample, std::int64_t endSample) noexcept {
    if (!capturesPcm_ || pcmInvalid_) return;
    if (firstSample < 0 || endSample <= firstSample || (pcmTimelineEnd_ >= 0 && firstSample != pcmTimelineEnd_)) {
        markPcmDiscontinuity();
        return;
    }
    pcmTimelineEnd_ = endSample;
}

bool LaunchCaptureBuffer::appendPcmRange(const LaunchCapturedPcmRange& range) noexcept {
    if (!capturesPcm_) return true;
    if (pcmInvalid_ || pcmOverflowed_) return false;
    const auto cursor = std::ranges::lower_bound(pcmTracks_,range.track,{},&LaunchPcmTrackCursor::track);
    if (!validPcmRange(range) || cursor == pcmTracks_.end() || cursor->track != range.track) {
        pcmInvalid_ = true;
        return false;
    }
    if (cursor->lastRange != noPcmRange) {
        auto& previous = pcmStorage_[cursor->lastRange];
        if (range.beginSample < previous.endSample) {
            pcmInvalid_ = true;
            return false;
        }
        const auto next = previous.firstSourceFrame + (previous.endSample - previous.beginSample);
        if (previous.clip == range.clip && previous.run == range.run &&
            previous.endSample == range.beginSample && next == range.firstSourceFrame &&
            previous.playbackGain == range.playbackGain) {
            previous.endSample = range.endSample;
            previous.liveRegionEdit |= range.liveRegionEdit;
            return true;
        }
    }
    if (pcmSize_ == pcmStorage_.size()) {
        pcmOverflowed_ = true;
        return false;
    }
    cursor->lastRange = pcmSize_;
    pcmStorage_[pcmSize_++] = range;
    return true;
}

std::span<const LaunchTransition> LaunchCaptureBuffer::transitions() const noexcept {
    return std::span<const LaunchTransition>(storage_.data(), size_);
}

std::span<const LaunchCapturedPcmRange> LaunchCaptureBuffer::pcmRanges() const noexcept {
    return pcmStorage_.first(pcmSize_);
}

LaunchCapturePlan planLaunchCapture(const LaunchCaptureBuffer& trace, LaunchCaptureTime begin,
                                    LaunchCaptureTime end,
                                    std::span<const LaunchCapturedStart> initial) {
    if (trace.pcmOverflowed())
        return {LaunchCaptureError::PcmRangeOverflow, {}};
    if (trace.overflowed())
        return {LaunchCaptureError::Overflow, {}};
    if (trace.pcmInvalid())
        return {LaunchCaptureError::InvalidPcmRange, {}};
    if (trace.hasFadeTail())
        return {LaunchCaptureError::FadeTail, {}};
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
    plan.pcmTraceEnabled = trace.capturesPcm();
    const auto close = [&](std::int64_t track, const TrackRun& run, LaunchCaptureTime at) {
        if (!run.arrangement && at.sample > run.begin.sample)
            plan.spans.push_back({track, run.clip, run.sourceTick, run.begin, at,
                run.velocity, run.fade, run.fadeElapsed, plan.pcmTraceEnabled ? run.serial : 0});
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
    if (!trace.pcmRanges().empty()) {
        // Ranges are track-interleaved, so find the most recent span on
        // their track at the first sample and verify it contains the range.
        std::vector<const LaunchCapturedSpan*> byTrack;
        byTrack.reserve(plan.spans.size());
        for (const auto& span : plan.spans)
            byTrack.push_back(&span);
        std::ranges::sort(byTrack,{},[](const auto* span) { return std::pair{span->track,span->begin.sample}; });
        for (const auto& range : trace.pcmRanges()) {
            const auto key = std::pair{range.track,range.beginSample};
            const auto upper = std::ranges::upper_bound(byTrack,key,{},[](const auto* span) {
                return std::pair{span->track,span->begin.sample};
            });
            if (!validPcmRange(range) || upper == byTrack.begin())
                return {LaunchCaptureError::InvalidPcmRange, {}};
            const auto& span = **(upper - 1);
            if (span.track != range.track || span.clip != range.clip || span.run != range.run ||
                range.endSample > span.end.sample)
                return {LaunchCaptureError::InvalidPcmRange, {}};
        }
        plan.pcmRanges.assign(trace.pcmRanges().begin(),trace.pcmRanges().end());
        std::sort(plan.pcmRanges.begin(),plan.pcmRanges.end(),[](const auto& left, const auto& right) {
            return std::tie(left.beginSample,left.track) < std::tie(right.beginSample,right.track);
        });
    }
    return plan;
}
} // namespace adi::engine
