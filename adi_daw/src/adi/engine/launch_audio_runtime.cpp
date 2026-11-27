// SPDX-License-Identifier: AGPL-3.0-or-later
#include "adi/engine/launch_audio_runtime.hpp"

#include "adi/engine/graph.hpp"
#include "adi/engine/launch_follow_playback.hpp"
#include "adi/engine/transport.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <unordered_set>
#include <utility>

namespace adi::engine {
namespace {
std::atomic<std::uint64_t> nextPcmRevision{1};

bool positiveAscending(const std::vector<std::int64_t>& ids) {
    return (ids.empty() || ids.front() > 0) &&
        std::is_sorted(ids.begin(), ids.end()) &&
        std::adjacent_find(ids.begin(), ids.end()) == ids.end();
}

} // namespace

LaunchAudioClip LaunchAudioClip::fromSamples(std::int64_t clip, std::uint32_t rate, std::uint16_t channels,
                                             std::vector<float> pcm) {
    if (clip <= 0 || rate == 0 || channels == 0 || pcm.empty() || pcm.size() % channels != 0)
        throw std::invalid_argument("invalid prepared Session clip audio");
    return {clip, 0, rate, channels, std::move(pcm)};
}

LaunchAudioProject::LaunchAudioProject(std::uint64_t incarnation, double bpm, std::vector<std::int64_t> tracks,
                                       std::vector<LaunchSlot> slots, std::vector<std::int64_t> scenes,
                                       std::vector<LaunchAudioClip> clips, LaunchQuantization global,
                                       std::shared_ptr<const LaunchFollowPlayer> follow, std::uint64_t followSeed,
                                       std::vector<std::int64_t> midiTracks, bool stored,
                                       std::vector<LaunchSceneClock> sceneClocks)
    : incarnation_(incarnation), pcmRevision_(nextPcmRevision.fetch_add(1, std::memory_order_relaxed)),
      bpm_(bpm), tracks_(std::move(tracks)), scenes_(std::move(scenes)),
      slots_(std::move(slots)), clips_(std::move(clips)), global_(global), follow_(std::move(follow)),
      followSeed_(followSeed), midiTracks_(std::move(midiTracks)), stored_(stored), sceneClocks_(std::move(sceneClocks)) {
    if (incarnation_ == 0 || !std::isfinite(bpm_) || bpm_ < 20.0 || bpm_ > 999.0 ||
        !isLaunchQuantizationCode(static_cast<std::int64_t>(global_)) ||
        !positiveAscending(tracks_) || !positiveAscending(scenes_) || !positiveAscending(midiTracks_) ||
        !std::includes(tracks_.begin(), tracks_.end(), midiTracks_.begin(), midiTracks_.end()))
        throw std::invalid_argument("invalid Session audio project clock, tracks or scenes");

    std::ranges::sort(sceneClocks_, {}, &LaunchSceneClock::scene);
    std::int64_t priorScene = 0;
    for (const auto& item : sceneClocks_) {
        if (item.scene <= priorScene || !std::ranges::binary_search(scenes_, item.scene) ||
            (!item.bpm && !item.signature))
            throw std::invalid_argument("invalid Session scene clock");
        priorScene = item.scene;
    }

    const auto lessClip = [](const LaunchAudioClip& a, const LaunchAudioClip& b) {
        return a.clip() < b.clip();
    };
    std::sort(clips_.begin(), clips_.end(), lessClip);
    if (!stored_ && std::ranges::any_of(clips_, [](const LaunchAudioClip& clip) { return clip.pageReader() != nullptr; }))
        throw std::invalid_argument("a shared injected project cannot own Session page playback state");
    if (std::adjacent_find(clips_.begin(), clips_.end(), [](const auto& a, const auto& b) {
            return a.clip() == b.clip();
        }) != clips_.end())
        throw std::invalid_argument("duplicate Session clip PCM");

    auto previous = std::pair<std::int64_t, std::int64_t>{0, 0};
    std::unordered_set<std::int64_t> occupied;
    for (const auto& slot : slots_) {
        const auto key = std::pair{slot.track, slot.scene};
        const bool invalidPlayback = slot.startTick < 0 || slot.endTick <= slot.startTick ||
            slot.loopStartTick < 0 || slot.loopEndTick < slot.loopStartTick ||
            slot.mode > LaunchMode::Repeat || !std::isfinite(slot.velocityAmount) ||
            slot.velocityAmount < 0.0f || slot.velocityAmount > 1.0f ||
            (slot.quantization && !isLaunchQuantizationCode(static_cast<std::int64_t>(*slot.quantization)));
        if (key <= previous || !std::binary_search(tracks_.begin(), tracks_.end(), slot.track) ||
            !std::binary_search(scenes_.begin(), scenes_.end(), slot.scene) || slot.clip < 0 ||
            (slot.clip > 0 && (invalidPlayback || (!stored_ && !isMidiTrack(slot.track) && !clip(slot.clip)) ||
                              !occupied.insert(slot.clip).second)))
            throw std::invalid_argument("invalid Session audio slot or missing prepared PCM");
        previous = key;
    }
}

const LaunchSceneClock* LaunchAudioProject::sceneClock(std::int64_t scene) const noexcept {
    if (sceneClocks_.empty()) return nullptr;
    const auto* start = sceneClocks_.data();
    const auto* finish = start + sceneClocks_.size();
    const auto* match = std::lower_bound(start, finish, scene,
        [](const LaunchSceneClock& item, std::int64_t id) { return item.scene < id; });
    return match != finish && match->scene == scene ? match : nullptr;
}

bool LaunchAudioProject::isMidiTrack(std::int64_t track) const noexcept {
    if (midiTracks_.empty()) return false;
    const auto* begin = midiTracks_.data();
    return std::binary_search(begin, begin + midiTracks_.size(), track);
}

bool LaunchAudioProject::supportsRate(double rate) const noexcept {
    if (!std::isfinite(rate) || rate <= 0 || rate != std::round(rate))
        return false;
    return std::all_of(clips_.begin(), clips_.end(), [rate](const LaunchAudioClip& pcm) {
        return pcm.rate() == rate;
    });
}

const LaunchAudioClip* LaunchAudioProject::clip(std::int64_t id) const noexcept {
    if (id <= 0 || clips_.empty())
        return nullptr;
    const auto* end = clips_.data() + clips_.size();
    const auto* pcm = std::lower_bound(clips_.data(), end, id,
        [](const LaunchAudioClip& candidate, std::int64_t target) { return candidate.clip() < target; });
    return pcm != end && pcm->clip() == id ? pcm : nullptr;
}

bool LaunchAudioRuntime::postGainEdit(LaunchAudioDriver& driver, const LaunchClipGainEdit& edit) const noexcept {
    return edit.incarnation == project_->incarnation() && hasTrack(edit.track) && project_->clip(edit.clip) &&
        driver.gains_.post(edit, project_->pcmRevision());
}

LaunchAudioRuntime::LaunchAudioRuntime(std::shared_ptr<const LaunchAudioProject> project,
                                       std::vector<std::int64_t> tracks, std::int32_t maxFrames)
    : project_(std::move(project)), tracks_(std::move(tracks)), ranges_(tracks_.size()), maxFrames_(maxFrames) {
    if (!project_ || maxFrames <= 0)
        throw std::invalid_argument("invalid Session launch audio runtime format");
    const auto known = project_->view().tracks;
    if (!positiveAscending(tracks_) || !std::includes(known.begin(), known.end(), tracks_.begin(), tracks_.end()))
        throw std::invalid_argument("Session audio runtime needs known tracks in ascending order");
    // A one-frame loop can produce one range per frame per track. Reserve
    // that bound so a callback never drops or allocates a range.
    for (std::size_t index = 0; index < tracks_.size(); ++index) {
        auto& track = ranges_[index];
        if (project_->isMidiTrack(tracks_[index])) track.midi.resize(static_cast<std::size_t>(maxFrames));
        track.items.resize(static_cast<std::size_t>(maxFrames));
        track.tails.resize(static_cast<std::size_t>(maxFrames) + 2);
    }
}

void LaunchAudioRuntime::startIfQueued(LaunchAudioDriver& driver, LaunchBlock block, Transport& transport) const noexcept {
    if (!project_->stored() || transport.playing() || transport.countInRemaining() != 0) return;
    block.sample = transport.sampleAt(0);
    block.incarnation = project_->incarnation();
    block.followPass = driver.pass_;
    auto view = project_->view();
    view.tracks = tracks_;
    if ((block.preparedScene && block.preparedScene->token > driver.performedSceneToken()) ||
        driver.launcher_.admitWhileStopped(block, view)) transport.play();
}

void LaunchAudioRuntime::begin(LaunchAudioDriver& driver, LaunchBlock block,
                               const Transport& transport, std::int64_t streamSample,
                               void* observerContext,
                               void (*observer)(void*, const LaunchTransition&, std::int64_t) noexcept,
                               void* sceneObserverContext,
                               void (*sceneObserver)(void*, const LaunchSceneTransition&, std::int64_t) noexcept) noexcept {
    for (std::size_t index = 0; index < ranges_.size(); ++index) {
        auto& ranges = ranges_.data()[index];
        ranges.size = 0;
        ranges.tailCount = 0;
    }
    const auto frames = block.frames;
    active_ = frames > 0 && frames <= maxFrames_ && transport.playing();
    if (!active_) {
        driver.suspend();
        return;
    }
    if (!driver.wasPlaying_ || transport.streamedFrames() == 0) ++driver.pass_;
    driver.wasPlaying_ = true;
    block.incarnation = project_->incarnation();
    if (driver.tailIncarnation_ != block.incarnation) {
        driver.tailCount_ = 0;
        driver.tailIncarnation_ = block.incarnation;
    }
    std::size_t remainingTails = 0;
    for (std::size_t index = 0; index < driver.tailCount_; ++index) {
        auto tail = driver.tails_.data()[index];
        if (addTail(tail, 0, block.frames))
            driver.tails_.data()[remainingTails++] = tail;
    }
    driver.tailCount_ = remainingTails;
    block.followPass = driver.pass_;
    const auto* follow = project_->followPlayer();
    block.followActionsEnabled &= follow != nullptr;
    auto view = project_->view();
    view.tracks = tracks_;
    struct Context {
        LaunchAudioRuntime& runtime;
        LaunchAudioDriver& driver;
        LaunchView view;
        const LaunchBlock& block;
        const LaunchFollowPlayer* follow;
        std::uint64_t incarnation, seed;
        std::int64_t streamSample;
        std::int32_t frames;
        void* observerContext;
        void (*observer)(void*, const LaunchTransition&, std::int64_t) noexcept;
        void* sceneObserverContext;
        void (*sceneObserver)(void*, const LaunchSceneTransition&, std::int64_t) noexcept;
        std::int32_t offset = 0;
    };
    if (streamSample < 0 || streamSample > std::numeric_limits<std::int64_t>::max() - frames) {
        observer = nullptr;
        sceneObserver = nullptr;
    }
    Context context{*this, driver, view, block, follow, block.incarnation,
        followPassSeed(project_->followSeed(), driver.pass_), streamSample, frames, observerContext, observer,
        sceneObserverContext, sceneObserver};
    const LaunchSink sink{&context, [](void* state, const LaunchSection& section) noexcept {
        auto& call = *static_cast<Context*>(state);
        call.runtime.add(call.driver.gains_, section, call.block, call.offset);
    }, [](void* state, const LaunchTransition& edge) noexcept {
        const auto& call = *static_cast<const Context*>(state);
        call.runtime.fadeOut(call.driver, edge, call.offset + edge.offset, call.frames);
        if (call.observer) {
            auto logicalEdge = edge;
            logicalEdge.offset += call.offset;
            call.observer(call.observerContext, logicalEdge, call.streamSample + logicalEdge.offset);
        }
    }, [](void* state, const LaunchTransition& edge) noexcept -> std::optional<LaunchFollowRequest> {
        const auto& call = *static_cast<const Context*>(state);
        if (!call.follow) return std::nullopt;
        return call.follow->followOnLaunch(edge, call.view, call.incarnation, call.block.followActionsEnabled, call.seed);
    }, nullptr, [](void* state, const LaunchSceneTransition& edge) noexcept {
        const auto& call = *static_cast<const Context*>(state);
        if (edge.preparedToken != 0)
            call.driver.performedScene_.store(edge.preparedToken, std::memory_order_release);
        if (call.sceneObserver) {
            auto logicalEdge = edge;
            logicalEdge.offset += call.offset;
            call.sceneObserver(call.sceneObserverContext, logicalEdge, call.streamSample + logicalEdge.offset);
        }
    }, [](void* state, const LaunchSceneTransition& edge) noexcept -> std::optional<LaunchFollowRequest> {
        const auto& call = *static_cast<const Context*>(state);
        if (!call.follow) return std::nullopt;
        const auto next = call.follow->followOnSceneLaunch(edge.scene, edge.tick, edge.ordinal,
            call.incarnation, call.block.followActionsEnabled, call.seed);
        if (next && next->request.scene && call.runtime.sceneClock(*next->request.scene)) return std::nullopt;
        return next;
    }};
    while (context.offset < frames) {
        block.sample = transport.sampleAt(context.offset);
        block.frames = static_cast<std::int32_t>(transport.contiguousFrames(context.offset, frames - context.offset));
        block.arrangementWrap = transport.looping() && block.sample == transport.loopBegin() &&
            (context.offset > 0 || (driver.transportRevision_ == transport.revision() &&
                driver.nextTimelineSample_ == transport.loopEnd() && driver.loopBegin_ == transport.loopBegin() &&
                driver.loopEnd_ == transport.loopEnd()));
        if (block.frames <= 0)
            break;
        driver.gains_.begin(block, view.tracks, project_->pcmRevision());
        if (!driver.launcher_.process(block, view, sink))
            break;
        context.offset += block.frames;
        driver.nextTimelineSample_ = block.sample + block.frames;
        driver.loopBegin_ = transport.loopBegin();
        driver.loopEnd_ = transport.loopEnd();
        driver.transportRevision_ = transport.revision();
    }
}

bool LaunchAudioRuntime::hasScene(std::int64_t scene) const noexcept {
    const auto scenes = project_->view().scenes;
    return std::binary_search(scenes.begin(), scenes.end(), scene);
}

bool LaunchAudioRuntime::addTail(LaunchAudioDriver::Tail& tail, std::int32_t offset,
                                std::int32_t frames) noexcept {
    if (frames <= 0 || tracks_.empty()) return false;
    const auto* pcm = project_->clip(tail.clip);
    if (!pcm) return false;
    const auto duration = launchFadeFrames(pcm->rate());
    if (tail.elapsed >= duration) return false;
    const auto* ids = tracks_.data();
    const auto* end = ids + tracks_.size();
    const auto* track = std::lower_bound(ids, end, tail.track);
    if (track == end || *track != tail.track) return false;
    auto& ranges = ranges_.data()[track - ids];
    if (ranges.tailCount >= ranges.tails.size()) return false;
    const auto source = tail.sourceFrame + std::min(tail.elapsed,
        std::numeric_limits<std::int64_t>::max() - std::max<std::int64_t>(0, tail.sourceFrame));
    const auto length = static_cast<std::int32_t>(std::min<std::int64_t>(frames, duration - tail.elapsed));
    ranges.tails.data()[ranges.tailCount++] = {
        {offset, length, pcm, source, tail.velocity, tail.elapsed}, tail.fadeInAtExit};
    tail.elapsed += length;
    return tail.elapsed < duration;
}

void LaunchAudioRuntime::fadeOut(LaunchAudioDriver& driver, const LaunchTransition& edge,
                                 std::int32_t offset, std::int32_t frames) noexcept {
    if (!edge.oldFade || offset < 0 || offset >= frames) return;
    const auto* pcm = project_->clip(edge.oldClip);
    if (!pcm) return;
    const auto anchor = pcm->frameForTick(edge.oldSourceAnchorTick, project_->bpm());
    const auto source = anchor + std::min(edge.oldFramesSinceSourceAnchor,
        std::numeric_limits<std::int64_t>::max() - std::max<std::int64_t>(0, anchor));
    LaunchAudioDriver::Tail tail{edge.track, edge.oldClip, source, 0,
        edge.oldFramesSinceStart, edge.oldVelocity * driver.gains_.currentMultiplier(edge.track, edge.oldRun)};
    if (addTail(tail, offset, frames - offset) && driver.tailCount_ < driver.tails_.size())
        driver.tails_.data()[driver.tailCount_++] = tail;
}

void LaunchAudioRuntime::add(LaunchClipGainEdits& gains, const LaunchSection& section,
                             const LaunchBlock& block, std::int32_t base) noexcept {
    const auto gain = gains.range(block, section);
    if (section.arrangement || section.frames <= 0 || tracks_.empty())
        return;
    const auto* ids = tracks_.data();
    const auto* end = ids + tracks_.size();
    const auto* track = std::lower_bound(ids, end, section.track);
    if (track == end || *track != section.track)
        return;
    auto& ranges = ranges_.data()[track - ids];
    if (ranges.size >= ranges.items.size()) return;
    if (!ranges.midi.empty()) ranges.midi.data()[ranges.size] = {section, block, base};
    const auto* pcm = project_->clip(section.clip);
    const auto anchor = pcm ? pcm->frameForTick(section.sourceAnchorTick, project_->bpm()) : 0;
    const auto source = anchor <= std::numeric_limits<std::int64_t>::max() - section.framesSinceSourceAnchor
        ? anchor + section.framesSinceSourceAnchor : std::numeric_limits<std::int64_t>::max();
    const auto fade = section.fade ? section.framesSinceStart : -1;
    if (gain.beforeFrames > 0)
        ranges.items.data()[ranges.size++] = {base + section.offset, gain.beforeFrames, pcm, source,
            section.velocity * gain.before, fade};
    if (gain.beforeFrames < section.frames && ranges.size < ranges.items.size()) {
        const auto advanced = source + std::min<std::int64_t>(gain.beforeFrames,
            std::numeric_limits<std::int64_t>::max() - std::max<std::int64_t>(0, source));
        const auto elapsed = fade < 0 ? -1 : fade + std::min<std::int64_t>(gain.beforeFrames,
            std::numeric_limits<std::int64_t>::max() - fade);
        ranges.items.data()[ranges.size++] = {base + section.offset + gain.beforeFrames,
            section.frames - gain.beforeFrames, pcm, advanced, section.velocity * gain.after, elapsed};
    }
}

bool LaunchAudioRuntime::hasTrack(std::int64_t track) const noexcept {
    if (tracks_.empty()) return false;
    const auto* begin = tracks_.data();
    return std::binary_search(begin, begin + tracks_.size(), track);
}

std::span<const LaunchRenderSlice> LaunchAudioRuntime::sectionsFor(std::int64_t track) const noexcept {
    if (!active_ || tracks_.empty()) return {};
    const auto* ids = tracks_.data();
    const auto* end = ids + tracks_.size();
    const auto* found = std::lower_bound(ids, end, track);
    if (found == end || *found != track) return {};
    const auto& ranges = ranges_.data()[found - ids];
    return ranges.midi.empty() ? std::span<const LaunchRenderSlice>{}
                               : std::span<const LaunchRenderSlice>{ranges.midi.data(), ranges.size};
}

void LaunchAudioRuntime::overlay(std::int64_t track, const NodeIo& node) const noexcept {
    if (!active_ || tracks_.empty())
        return;
    const auto* ids = tracks_.data();
    const auto* end = ids + tracks_.size();
    const auto* found = std::lower_bound(ids, end, track);
    if (found == end || *found != track)
        return;
    const auto& ranges = ranges_.data()[found - ids];
    if (ranges.size == 0 && ranges.tailCount == 0)
        return;

    AudioIo io;
    io.frames = node.blockOffset + node.frames;
    io.numOut = node.channels;
    io.out = node.out;
    io.out64 = node.out64;
    const auto* stop = ranges.items.data() + ranges.size;
    const auto* range = std::upper_bound(ranges.items.data(), stop, node.blockOffset,
        [](std::int32_t offset, const Range& item) { return offset < item.offset + item.frames; });
    for (; range != stop && range->offset < io.frames; ++range) {
        const auto start = std::max(range->offset, node.blockOffset);
        const auto finish = std::min(range->offset + range->frames, io.frames);
        const auto advance = start - range->offset;
        const auto available = range->sourceFrame <= std::numeric_limits<std::int64_t>::max() - advance;
        const auto fadeElapsed = range->fadeElapsed < 0 ? -1 : range->fadeElapsed +
            std::min<std::int64_t>(advance, std::numeric_limits<std::int64_t>::max() - range->fadeElapsed);
        renderLaunchAudio(io, start, finish - start, available ? range->pcm : nullptr,
                          available ? range->sourceFrame + advance : 0, range->velocity, fadeElapsed);
    }
    for (std::size_t index = 0; index < ranges.tailCount; ++index) {
        const auto& tail = ranges.tails.data()[index];
        const auto start = std::max(tail.range.offset, node.blockOffset);
        const auto finish = std::min(tail.range.offset + tail.range.frames, io.frames);
        if (start >= finish) continue;
        const auto advance = start - tail.range.offset;
        const auto source = tail.range.sourceFrame + std::min<std::int64_t>(advance,
            std::numeric_limits<std::int64_t>::max() - std::max<std::int64_t>(0, tail.range.sourceFrame));
        mixLaunchAudioTail(io, start, finish - start, *tail.range.pcm, source,
                           tail.range.velocity, tail.range.fadeElapsed + advance, tail.fadeInAtExit);
    }
}

} // namespace adi::engine
