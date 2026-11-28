// SPDX-License-Identifier: AGPL-3.0-or-later
#include "adi/engine/launch_paged_audio.hpp"
#include "adi/audio/io_audit.hpp"

#include <algorithm>
#include <array>
#include <stdexcept>
#include <utility>

namespace adi::engine {

namespace {
std::int64_t physicalWindowStart(const LaunchPagedSource& source) noexcept {
    return source.loopEndFrame > source.loopBeginFrame ? std::min(source.firstFrame,source.loopBeginFrame)
                                                       : source.firstFrame;
}
}

LaunchPagedAudio::LaunchPagedAudio(std::shared_ptr<const ClipProject> project,
                                  std::span<const LaunchPagedSource> sources, std::uint32_t sampleRate,
                                  std::int32_t channels, std::int32_t maxFrames)
    : pages_(std::move(project), clock_, sampleRate, channels, maxFrames, nullptr, false,
             ClipPlayback::WarpPlayback::AlignedR3), channels_(channels),
      blockFrames_(maxFrames), sampleRate_(sampleRate) {
    if (channels < 1 || channels > 64 || maxFrames < 1 || maxFrames > 4096)
        throw std::invalid_argument("invalid Session paged playback format");
    output_.resize(static_cast<std::size_t>(channels) * static_cast<std::size_t>(maxFrames));
    for (const auto& source : sources) {
        if (source.clip <= 0 || source.sourceTrack <= 0 || source.firstFrame < 0 || source.endFrame < 0 ||
            source.loopBeginFrame < 0 || source.loopEndFrame < source.loopBeginFrame)
            throw std::invalid_argument("invalid Session paged source");
        const auto window = source.endFrame - physicalWindowStart(source);
        if (source.warmPhysicalWindow && (window <= 0 || window > 3 * ClipPlayback::readAheadFrames(sampleRate)))
            throw std::invalid_argument("invalid unwarped Session page window");
        const auto nodes = pages_.sourcesFor(source.sourceTrack);
        if (nodes.size() == 1) {
            if (source.keepPhysicalStartWarm) ClipPlayback::keepSourceStartWarm(*nodes.front(), source.firstFrame);
            const auto latency = pages_.warpLatencyFor(source.sourceTrack);
            const bool primesAll = latency > 0 && source.endFrame > 0 &&
                source.endFrame <= ClipPlayback::readAheadFrames(sampleRate);
            sources_.push_back({source, nodes.front(), latency, primesAll});
        }
    }
    std::ranges::sort(sources_, {}, [](const Entry& entry) { return entry.source.clip; });
    if (std::ranges::adjacent_find(sources_, {}, [](const Entry& entry) { return entry.source.clip; }) != sources_.end())
        throw std::invalid_argument("duplicate Session paged source");
    const auto fatal = pages_.fatalProblems();
    problems_.assign(fatal.begin(), fatal.end());
}

bool LaunchPagedAudio::prime(std::chrono::milliseconds timeout) {
    if (audio::onAudioThread) return false;
    std::erase_if(sources_, [&](const Entry& entry) {
        const auto& source = entry.source;
        Transport first, loop;
        const bool looping = source.loopEndFrame > source.loopBeginFrame;
        // A small worker R3 slot can admit every phase in its unrolled
        // window. Keep that request at launch preparation, including phases
        // skipped by the slot's normal start point or loop head.
        // Physical Legato can enter any frame in the loop, including a
        // loop head before the authored Start. Prime the unrolled window.
        const auto firstFrame = source.warmPhysicalWindow ? physicalWindowStart(source) : source.firstFrame;
        first.loop(source.loopBeginFrame, source.loopEndFrame,
                   looping && !entry.primesAllPhases && !source.warmPhysicalWindow);
        loop.loop(source.loopBeginFrame, source.loopEndFrame, looping);
        first.locate(entry.primesAllPhases ? 0 : firstFrame);
        first.play();
        loop.locate(looping ? source.loopBeginFrame : source.firstFrame);
        loop.play();
        const auto ready = source.warmPhysicalWindow
            ? pages_.primeTrackWindow(source.sourceTrack,first,static_cast<std::int32_t>(source.endFrame - firstFrame),timeout)
            : pages_.primeTrackAt(source.sourceTrack,first,loop,timeout);
        if (ready) return false;
        problems_.push_back("Session clips#" + std::to_string(source.clip) + (source.warmPhysicalWindow
            ? ": page preparation did not fill the physical source window"
            : ": page preparation did not fill the start and loop windows"));
        return true;
    });
    return !sources_.empty();
}

const LaunchPagedAudio::Entry* LaunchPagedAudio::find(std::int64_t clip) const noexcept {
    if (sources_.empty()) return nullptr;
    const auto* end = sources_.data() + sources_.size();
    const auto* found = std::lower_bound(sources_.data(), end, clip,
        [](const Entry& entry, std::int64_t key) { return entry.source.clip < key; });
    return found != end && found->source.clip == clip ? found : nullptr;
}

bool LaunchPagedAudio::hasClip(std::int64_t clip) const noexcept {
    return find(clip) != nullptr;
}

std::int64_t LaunchPagedAudio::firstFrame(std::int64_t clip) const noexcept {
    const auto* found = find(clip);
    return found ? found->source.firstFrame : -1;
}

std::int32_t LaunchPagedAudio::latencySamples(std::int64_t clip) const noexcept {
    const auto* source = find(clip);
    return source ? source->latency : 0;
}

bool LaunchPagedAudio::allWorkerPhasesPrimed(std::int64_t clip) const noexcept {
    const auto* source = find(clip);
    return source && source->primesAllPhases;
}

bool LaunchPagedAudio::allPhysicalPhasesPrimed(std::int64_t clip) const noexcept {
    const auto* source = find(clip);
    return source && source->source.warmPhysicalWindow;
}

void LaunchPagedAudio::prefetch(std::int64_t id, std::int64_t firstFrame) noexcept {
    const auto* found = find(id);
    if (!found || firstFrame < 0) return;
    const auto& source = found->source;
    const bool looping = source.loopEndFrame > source.loopBeginFrame;
    const auto head = looping ? source.loopBeginFrame : source.firstFrame;
    const bool worker = found->latency > 0;
    // A worker cue and its possible musical wrap are mandatory windows.
    // Keep saved Start behind both when the page cap is tight.
    const auto starts = worker ? std::array{firstFrame,head,source.firstFrame}
                               : std::array{firstFrame,source.firstFrame,head};
    std::array<Transport, starts.size()> candidates;
    for (std::size_t index = 0; index < starts.size(); ++index) {
        auto& next = candidates[index];
        next.locate(starts[index]);
        next.loop(source.loopBeginFrame, source.loopEndFrame, looping && !worker);
        next.play();
    }
    ClipPlayback::prefetchSource(*found->playback, candidates);
}

bool LaunchPagedAudio::reserveCue(std::int64_t id, std::int64_t frame, std::int32_t frames,
                                 bool requestAhead, bool repeatAtEnd) noexcept {
    const auto* found = find(id);
    if (!found || found->latency != 0) return false;
    const auto& source = found->source;
    const bool looping = source.loopEndFrame > source.loopBeginFrame;
    const auto end = looping ? source.loopEndFrame : source.endFrame;
    if (frame < physicalWindowStart(source) || frame >= end) return false;
    Transport next;
    next.locate(frame);
    next.loop(source.loopBeginFrame,source.loopEndFrame,looping);
    next.play();
    if (!repeatAtEnd || end - frame >= frames)
        return ClipPlayback::reserveSource(*found->playback,next,frames,requestAhead);
    // A Release in a later driver slice can select the normal loop head
    // instead of this held Restart. The source reserves both possibilities.
    auto restart = next;
    restart.loop(source.firstFrame,end,true);
    return ClipPlayback::reserveSource(*found->playback,next,frames,requestAhead,{&restart,1});
}

bool LaunchPagedAudio::reserveWorkerCue(std::int64_t id, std::int64_t frame, std::int32_t frames,
                                       bool includeLoopHead, std::optional<std::int64_t> restartFrame,
                                       bool restartInBlock, std::optional<WorkerLoopCue> loopCue) noexcept {
    const auto* found = find(id);
    if (!found || found->latency <= 0 || frame < 0 || frame >= found->source.endFrame) return false;
    const auto& source = found->source;
    const auto exactLoop = [&] {
        if (!includeLoopHead || restartFrame || !loopCue || loopCue->wrapOffsets.empty() ||
            loopCue->sourceHead != source.loopBeginFrame || frame >= source.loopEndFrame)
            return false;
        const auto lap = source.loopEndFrame - source.loopBeginFrame;
        if (lap <= 0) return false;
        auto nextWrap = source.loopEndFrame - frame;
        for (std::size_t i = 0; i < loopCue->wrapOffsets.size(); ++i) {
            if (nextWrap >= frames || loopCue->wrapOffsets.data()[i] != nextWrap) return false;
            nextWrap += std::min(lap,frames - nextWrap);
        }
        return nextWrap == frames;
    }();
    Transport next;
    next.locate(frame);
    next.loop(source.loopBeginFrame,source.loopEndFrame,exactLoop);
    next.play();
    // Unless every reanchor matches the reader's physical loop, reserve
    // the unrolled windows required by a rounded wrap, restart or fade.
    std::array<Transport,2> alternates{next,next};
    auto count = std::size_t{0};
    if (includeLoopHead && !exactLoop) alternates[count++].locate(source.loopBeginFrame);
    if (restartFrame && (*restartFrame < 0 || *restartFrame >= found->source.endFrame)) return false;
    if (restartInBlock && restartFrame) alternates[count++].locate(*restartFrame);
    if (!ClipPlayback::reserveSource(*found->playback,next,frames,false,{alternates.data(),count})) return false;
    if (restartFrame) ClipPlayback::keepSourceStartWarm(*found->playback,*restartFrame);
    return true;
}

void LaunchPagedAudio::releaseCue(std::int64_t id) noexcept {
    if (const auto* found = find(id)) ClipPlayback::releaseSource(*found->playback);
}

bool LaunchPagedAudio::read(std::int64_t id, std::int64_t firstFrame, std::int32_t frames, bool allowLoop) noexcept {
    readFrames_ = 0;
    if (frames <= 0 || frames > blockFrames_) return false;
    const auto* found = find(id);
    if (!found) return false;

    readFrames_ = frames;
    const auto prefix = firstFrame < 0 ? static_cast<std::int32_t>(-std::max<std::int64_t>(-frames, firstFrame)) : 0;
    std::array<float*, 64> out;
    for (std::int32_t channel = 0; channel < channels_; ++channel) {
        auto* samples = output_.data() + static_cast<std::size_t>(channel) * static_cast<std::size_t>(blockFrames_);
        std::fill_n(samples, prefix, 0.0f);
        out[static_cast<std::size_t>(channel)] = samples + prefix;
    }
    if (prefix == frames) return true;
    clock_.locate(std::max<std::int64_t>(0, firstFrame));
    clock_.loop(found->source.loopBeginFrame, found->source.loopEndFrame, allowLoop);
    clock_.play();
    NodeIo io;
    io.out = out.data();
    io.channels = channels_;
    io.frames = frames - prefix;
    io.sampleRate = sampleRate_;
    found->playback->process(io);
    return true;
}

std::span<const float> LaunchPagedAudio::channel(std::int32_t index) const noexcept {
    if (index < 0 || index >= channels_) return {};
    return {output_.data() + static_cast<std::size_t>(index) * static_cast<std::size_t>(blockFrames_),
            static_cast<std::size_t>(readFrames_)};
}

std::uint32_t LaunchPagedAudio::underrunSamples() const noexcept { return pages_.underrunSamples(); }
const std::vector<std::string>& LaunchPagedAudio::problems() const noexcept { return problems_; }

} // namespace adi::engine
