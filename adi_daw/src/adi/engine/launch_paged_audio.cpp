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
        const auto earliest = entry.primesAllPhases ? 0 : physicalWindowStart(source);
        const auto ready = source.warmPhysicalWindow
            ? pages_.primeTrackWindow(source.sourceTrack,first,static_cast<std::int32_t>(source.endFrame - firstFrame),timeout)
            : pages_.primeTrackAt(source.sourceTrack,first,loop,timeout,
                source.legato ? std::optional{earliest} : std::nullopt);
        if (ready) return false;
        problems_.push_back("Session clips#" + std::to_string(source.clip) + (source.warmPhysicalWindow
            ? ": page preparation did not fill the physical source window"
            : ": page preparation did not fill the start and loop windows"));
        return true;
    });
    for (auto& entry : sources_)
        if (entry.source.legato || entry.source.warmPhysicalWindow) entry.retained = pages_.retainedTrackRange(entry.source.sourceTrack);
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

bool LaunchPagedAudio::retainsRange(std::int64_t clip, std::int64_t first, std::int64_t end) const noexcept {
    const auto* entry = find(clip);
    return entry && entry->retained && first >= entry->retained->first &&
        end >= first && end <= entry->retained->second;
}

void LaunchPagedAudio::prefetch(std::int64_t id, std::int64_t firstFrame) noexcept {
    const auto* found = find(id);
    if (!found || firstFrame < 0) return;
    const auto& source = found->source;
    const bool looping = source.loopEndFrame > source.loopBeginFrame;
    const std::array starts{firstFrame, source.firstFrame, looping ? source.loopBeginFrame : source.firstFrame};
    std::array<Transport, starts.size()> candidates;
    for (std::size_t index = 0; index < starts.size(); ++index) {
        auto& next = candidates[index];
        next.locate(starts[index]);
        next.loop(source.loopBeginFrame, source.loopEndFrame, looping);
        next.play();
    }
    ClipPlayback::prefetchSource(*found->playback, candidates);
}

bool LaunchPagedAudio::reserveCue(std::int64_t id, std::int64_t frame, std::int32_t frames) noexcept {
    const auto* found = find(id);
    if (!found || found->latency != 0 || found->source.loopEndFrame > found->source.loopBeginFrame ||
        frame < found->source.firstFrame || frame >= found->source.endFrame) return false;
    Transport next;
    next.locate(frame);
    next.play();
    return ClipPlayback::reserveSource(*found->playback,next,frames);
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
