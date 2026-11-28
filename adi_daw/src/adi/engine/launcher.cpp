// SPDX-License-Identifier: AGPL-3.0-or-later
#include "adi/engine/launcher.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

namespace adi::engine {
namespace {
constexpr auto lastTick = std::numeric_limits<std::int64_t>::max();
// Live 12 readings N2, N3, N5 and N11 are still owed for these edge cases.
constexpr bool gateReleaseCancelsUnheardLaunch = true;
constexpr bool togglePressCancelsUnheardLaunch = true;
constexpr bool repeatWithoutQuantizationRestartsAtClipEnd = true;
constexpr bool legatoUsesPreviousClipRelativePosition = true;
constexpr bool backToArrangementIsImmediate = true;
constexpr bool stopAllUsesGlobalLaunchQuantization = true;
constexpr bool scenesUseGlobalLaunchQuantization = true; // L3: scene vs explicit clip setting
constexpr bool followBypassesGlobalLaunchQuantization = true; // Live 12 manual 16.7
// While the clock is stopped, a scene with nothing to play settles its Stop
// buttons without starting the clock. The Live 12.4 empty-scene case is owed.
constexpr bool emptySceneStartsStoppedTransport = false;
constexpr bool preserveQuantizedDeadlinesAcrossArrangementWrap = true;
constexpr bool stopAllCancelsSceneFollow = true;
constexpr auto bufferWriting = std::numeric_limits<std::uint32_t>::max();
static_assert(std::atomic<std::uint32_t>::is_always_lock_free);

std::int64_t add(std::int64_t lhs, std::int64_t rhs) noexcept {
    return rhs > lastTick - lhs ? lastTick : lhs + rhs;
}

std::int64_t atSample(const LaunchBlock& block, std::int64_t sample) noexcept {
    return std::max<std::int64_t>(0, block.tickAtSample(block.clock, sample));
}

std::int32_t offsetAt(const LaunchBlock& block, std::int64_t tick) noexcept {
    if (tick == lastTick) return block.frames;
    const auto sample = block.sampleAtTick(block.clock, tick);
    if (sample <= block.sample) return 0;
    return static_cast<std::int32_t>(std::min<std::int64_t>(block.frames, sample - block.sample));
}

std::int64_t gridAtOrAfterSample(const LaunchBlock& block, LaunchQuantization grid,
                                 std::int64_t sample) noexcept {
    if (sample == lastTick) return lastTick;
    // A grid tick can round onto this sample even when sample->tick is
    // already past the grid. Start the search just after the previous sample.
    const auto lowerTick = sample == 0 ? 0 : add(atSample(block,sample - 1),1);
    auto target = launch_quantize::nextGridTick(lowerTick,grid,block.meters).value_or(lastTick);
    if (target != lastTick && block.sampleAtTick(block.clock,target) < sample)
        target = launch_quantize::nextGridTick(add(target,1),grid,block.meters).value_or(lastTick);
    return target;
}

std::int64_t gridForPress(const LaunchBlock& block, LaunchQuantization grid,
                         std::int64_t now) noexcept {
    return grid == LaunchQuantization::None ? now : gridAtOrAfterSample(block,grid,block.sample);
}

std::span<const LaunchSlot> trackSlots(const LaunchView& view, std::int64_t track) noexcept {
    const auto first = std::lower_bound(view.slots.begin(), view.slots.end(), track,
        [](const LaunchSlot& slot, auto key) { return slot.track < key; });
    const auto last = std::upper_bound(first, view.slots.end(), track,
        [](auto key, const LaunchSlot& slot) { return key < slot.track; });
    return {first, last};
}

const LaunchSlot* findSlot(const LaunchView& view, std::int64_t track,
                           std::int64_t scene) noexcept {
    const auto key = std::pair(track,scene);
    const auto it = std::lower_bound(view.slots.begin(), view.slots.end(), key,
        [](const LaunchSlot& slot, const auto& address) { return std::pair(slot.track,slot.scene) < address; });
    return it != view.slots.end() && it->track == track && it->scene == scene ? &*it : nullptr;
}

const LaunchSlot* findClip(const LaunchView& view, std::int64_t track,
                           std::int64_t clip) noexcept {
    if (clip == 0) return nullptr;
    const auto slots = trackSlots(view,track);
    const auto it = std::find_if(slots.begin(), slots.end(),
        [clip](const LaunchSlot& slot) { return slot.clip == clip; });
    return it == slots.end() ? nullptr : &*it;
}

const LaunchSlot* addressedSlot(const LaunchView& view, const LaunchRequest& request) noexcept {
    if (!request.track) return nullptr;
    if (request.scene) {
        const auto* slot = findSlot(view,*request.track,*request.scene);
        return slot && (!request.clip || *request.clip == slot->clip) ? slot : nullptr;
    }
    return request.clip ? findClip(view,*request.track,*request.clip) : nullptr;
}

bool occupied(const LaunchSlot* slot) noexcept {
    return slot && slot->clip > 0 && slot->startTick >= 0 && slot->endTick > slot->startTick;
}

bool loops(const LaunchSlot& slot) noexcept {
    return slot.loopStartTick >= 0 && slot.loopEndTick > slot.loopStartTick;
}

std::int64_t sourceEnd(const LaunchSlot& slot) noexcept {
    return loops(slot) ? slot.loopEndTick : slot.endTick;
}

struct PhysicalLap {
    std::int64_t startFrame = -1, endFrame = -1, startTick = 0, endTick = 0, cueFrame = -1;
    std::int64_t originFrame() const noexcept { return cueFrame < 0 ? startFrame : cueFrame; }
    std::int64_t length() const noexcept { return endFrame - originFrame(); }
    PhysicalLap withCue(std::int64_t frame) const noexcept {
        if (startFrame < 0 || frame < startFrame || frame >= endFrame) return *this;
        return {startFrame,endFrame,startTick,endTick,frame};
    }
    std::int64_t tickAt(std::int64_t elapsed) const noexcept {
        if (elapsed >= length()) return endTick;
        const auto offset = originFrame() - startFrame + std::max<std::int64_t>(0,elapsed);
        const auto fraction = static_cast<long double>(offset) / static_cast<long double>(endFrame - startFrame);
        return startTick + static_cast<std::int64_t>(fraction * static_cast<long double>(endTick - startTick));
    }
    std::int64_t frameAt(std::int64_t tick) const noexcept {
        if (tick <= startTick) return startFrame;
        if (tick >= endTick) return endFrame;
        const auto fraction = static_cast<long double>(tick - startTick) / static_cast<long double>(endTick - startTick);
        return startFrame + std::llround(fraction * static_cast<long double>(endFrame - startFrame));
    }
};

PhysicalLap physicalExtent(const LaunchSlot& slot) noexcept {
    return {slot.physicalStartFrame,slot.physicalStartFrame + slot.physicalFrames,slot.startTick,slot.endTick};
}

PhysicalLap physicalLap(const LaunchSlot* slot, bool wrapped) noexcept {
    if (!slot || slot->physicalFrames <= 0) return {};
    if (!slot->physicalLoopEndFrame)
        return physicalExtent(*slot);
    return {wrapped ? slot->physicalLoopBeginFrame : slot->physicalStartFrame,slot->physicalLoopEndFrame,
            wrapped ? slot->loopStartTick : slot->startTick,slot->loopEndTick};
}

PhysicalLap cuedPhysicalLap(const LaunchSlot* slot, std::uint64_t completedLoops, std::int64_t cue) noexcept {
    // A Legato cue in the repeated region can precede the authored Start.
    const bool wrapped = cue < 0 ? completedLoops != 0
        : slot && slot->physicalLoopEndFrame && cue >= slot->physicalLoopBeginFrame;
    return physicalLap(slot,wrapped).withCue(cue);
}

std::int64_t wrapSource(const LaunchSlot& slot, std::int64_t position) noexcept {
    if (loops(slot) && position >= slot.loopEndTick)
        return slot.loopStartTick + (position - slot.loopEndTick) % (slot.loopEndTick - slot.loopStartTick);
    return position;
}

std::int64_t launchSourceTick(const LaunchSlot& target, const LaunchSlot* previous,
                             std::int64_t previousSourceTick) noexcept {
    auto position = target.startTick;
    if (previous && target.legato && previous->clip != target.clip) {
        const auto previousPosition = wrapSource(*previous,previousSourceTick);
        position = legatoUsesPreviousClipRelativePosition
            ? add(target.startTick,std::max<std::int64_t>(0,previousPosition - previous->startTick)) : previousPosition;
    }
    position = wrapSource(target,position);
    return position < sourceEnd(target) ? position : target.startTick;
}

LaunchSourceCue launchSourceCue(const LaunchSlot& target, const LaunchSlot* previous,
                                std::int64_t previousTick, std::int64_t previousFrame = -1) noexcept {
    const auto tick = launchSourceTick(target,previous,previousTick);
    // A same-slot retrigger uses the saved Start. Projecting it back
    // through rounded loop endpoints can move a physical Start.
    if (!target.legato || !previous || previous->clip == target.clip) return {tick,-1};
    const auto first = physicalLap(&target,false);
    if (first.startFrame < 0) return {tick,-1};
    const auto loop = physicalLap(&target,true);

    auto frame = first.startFrame;
    if (previousFrame >= 0 && previous->physicalStartFrame >= 0) {
        frame = legatoUsesPreviousClipRelativePosition
            ? add(first.startFrame,std::max<std::int64_t>(0,previousFrame - previous->physicalStartFrame))
            : std::max(first.startFrame,previousFrame);
        if (frame >= first.endFrame)
            frame = target.physicalLoopEndFrame ? loop.startFrame + (frame - loop.endFrame) % loop.length() : first.startFrame;
    } else {
        const auto& mapping = target.physicalLoopEndFrame && tick >= loop.startTick ? loop : first;
        frame = std::min(mapping.endFrame - 1,mapping.frameAt(tick));
    }
    return {cuedPhysicalLap(&target,0,frame).tickAt(0),frame};
}

bool validRegion(const LaunchClipRegion& region) noexcept {
    return region.start >= 0 && region.end > region.start &&
        (!region.loop || (region.loop->begin >= region.start && region.loop->end > region.loop->begin &&
                         region.loop->end <= region.end));
}

bool regionFits(const LaunchSlot* prepared, const LaunchClipRegion& region) noexcept {
    return occupied(prepared) && region.start >= prepared->startTick && region.end <= prepared->endTick;
}

bool physicalRegionFits(const LaunchSlot* slot, const LaunchPhysicalRegion& region) noexcept {
    return occupied(slot) && slot->physicalFrames > 0 && region.startFrame >= slot->physicalStartFrame &&
        region.endFrame > region.startFrame && region.endFrame <= slot->physicalStartFrame + slot->physicalFrames &&
        (!region.loopEndFrame || (region.loopBeginFrame >= region.startFrame &&
            region.loopEndFrame > region.loopBeginFrame && region.loopEndFrame <= region.endFrame));
}

std::optional<LaunchPhysicalRegion> physicalRegionFor(const LaunchSlot* slot, const LaunchClipRegion& region) noexcept {
    if (!regionFits(slot,region) || slot->physicalFrames <= 0) return {};
    const auto extent = physicalExtent(*slot);
    const LaunchPhysicalRegion frames{extent.frameAt(region.start),extent.frameAt(region.end),
        region.loop ? extent.frameAt(region.loop->begin) : 0,
        region.loop ? extent.frameAt(region.loop->end) : 0};
    if (!physicalRegionFits(slot,frames)) return {};
    return frames;
}

const LaunchSlot* effectiveSlot(const LaunchSlot* prepared, const std::optional<LaunchClipRegion>& region,
                                const std::optional<LaunchPhysicalRegion>& physical, LaunchSlot& scratch) noexcept {
    if (physical) {
        if (!physicalRegionFits(prepared,*physical)) return prepared;
        const auto extent = physicalExtent(*prepared);
        const auto tick = [&](std::int64_t frame) { return extent.tickAt(frame - extent.startFrame); };
        scratch = *prepared;
        scratch.startTick = tick(physical->startFrame);
        scratch.endTick = tick(physical->endFrame);
        scratch.loopStartTick = physical->loopEndFrame ? tick(physical->loopBeginFrame) : 0;
        scratch.loopEndTick = physical->loopEndFrame ? tick(physical->loopEndFrame) : 0;
        scratch.physicalStartFrame = physical->startFrame;
        scratch.physicalFrames = physical->endFrame - physical->startFrame;
        scratch.physicalLoopBeginFrame = physical->loopBeginFrame;
        scratch.physicalLoopEndFrame = physical->loopEndFrame;
        return &scratch;
    }
    if (!region || !regionFits(prepared,*region)) return prepared;
    scratch = *prepared;
    scratch.startTick = region->start;
    scratch.endTick = region->end;
    scratch.loopStartTick = region->loop ? region->loop->begin : 0;
    scratch.loopEndTick = region->loop ? region->loop->end : 0;
    scratch.physicalStartFrame = -1;
    scratch.physicalFrames = 0;
    scratch.physicalLoopBeginFrame = scratch.physicalLoopEndFrame = 0;
    return &scratch;
}

float launchVelocity(const LaunchSlot& slot, float velocity) noexcept {
    const auto amount = std::isfinite(slot.velocityAmount)
        ? std::clamp(slot.velocityAmount, 0.0f, 1.0f) : 0.0f;
    return 1.0f - amount * (1.0f - std::clamp(velocity,0.0f,1.0f));
}
} // namespace

Launcher::Launcher(std::size_t maximumTracks)
    : tracks_(maximumTracks), scratch_(maximumTracks),
      activeTracks_(tracks_.data()), spareTracks_(scratch_.data()) {
    for (auto& frame : statusBuffers_) frame.rows = std::make_unique<LaunchTrackStatus[]>(maximumTracks);
}

bool Launcher::postRegionEdit(const LaunchClipRegionEdit& edit, const LaunchSlot* prepared) noexcept {
    if (edit.track <= 0 || edit.clip <= 0 || edit.incarnation == 0 || edit.run == 0 ||
        !validRegion(edit.region) || edit.rate > ClipUpdateRate::Bar)
        return false;
    const auto physical = physicalRegionFor(prepared,edit.region);
    if (prepared && prepared->physicalFrames > 0 && !physical) return false;
    const auto write = regionWritten_.load(std::memory_order_relaxed);
    if (write - regionRead_.load(std::memory_order_acquire) == regionMailbox_.size()) return false;
    regionMailbox_[static_cast<std::size_t>(write % regionMailbox_.size())] = {edit,physical};
    regionWritten_.store(write + 1,std::memory_order_release);
    return true;
}

void Launcher::drainRegionEdits(const LaunchBlock& block, const LaunchView& view) noexcept {
    auto read = regionRead_.load(std::memory_order_relaxed);
    const auto write = regionWritten_.load(std::memory_order_acquire);
    while (read != write) {
        const auto& request = regionMailbox_[static_cast<std::size_t>(read++ % regionMailbox_.size())];
        const auto& edit = request.edit;
        auto* track = findTrack(edit.track);
        if (!track || track->run.clip != edit.clip || track->run.serial != edit.run || edit.incarnation != block.incarnation)
            continue;
        const auto* prepared = findClip(view,edit.track,edit.clip);
        const auto physical = request.physical ? request.physical : physicalRegionFor(prepared,edit.region);
        if (physical ? !physicalRegionFits(prepared,*physical) :
            !regionFits(prepared,edit.region) || prepared->physicalFrames > 0) continue;
        if (const auto boundary = nextClipUpdateBoundary(block,block.sample,edit.rate,true))
            track->regionEdit = RegionPending{edit,*boundary,physical};
    }
    regionRead_.store(read,std::memory_order_release);
}

Launcher::Track* Launcher::findTrack(std::int64_t id) noexcept {
    const std::span active(activeTracks_,trackCount_);
    const auto it = std::lower_bound(active.begin(), active.end(), id,
        [](const Track& track, auto target) { return track.id < target; });
    return it != active.end() && it->id == id ? &*it : nullptr;
}

void Launcher::adopt(const LaunchBlock& block, const LaunchView& view, LaunchSink sink) noexcept {
    if (incarnation_ != block.incarnation) {
        trackCount_ = 0;
        nextSample_ = -1;
        incarnation_ = block.incarnation;
        userScene_.reset();
        followScene_.reset();
        sceneOrdinal_ = 0;
        lastPreparedScene_ = 0;
        stoppedPreparedAllThrough_ = 0;
        stopAllSuppressesSceneFollow_ = false;
    }
    if (view.tracks.size() == trackCount_ &&
        std::equal(view.tracks.begin(), view.tracks.end(), activeTracks_,
                   [](auto id, const Track& track) { return id == track.id; })) return;

    std::size_t old = 0, next = 0;
    const auto stopRemovedTrack = [&](const Track& track) {
        if (track.run.clip != 0 && sink.transition) {
            sink.transition(sink.context, {track.id, track.run.clip, 0, 0, block.sample,
                atSample(block,block.sample), 0, false, track.run.serial, 0, 0, 1.0f, false,
                track.run.pcmAnchorTick,track.run.frames-track.run.pcmAnchorFrame,
                track.run.frames,track.run.velocity,track.run.fade,track.run.anchorTick,track.run.anchorSourceTick});
        }
    };
    for (const auto id : view.tracks) {
        while (old < trackCount_ && activeTracks_[old].id < id) stopRemovedTrack(activeTracks_[old++]);
        if (old < trackCount_ && activeTracks_[old].id == id) spareTracks_[next++] = activeTracks_[old++];
        else {
            spareTracks_[next] = {};
            spareTracks_[next++].id = id;
        }
    }
    while (old < trackCount_) stopRemovedTrack(activeTracks_[old++]);
    std::swap(activeTracks_,spareTracks_);
    trackCount_ = next;
}

bool Launcher::valid(const LaunchRequest& request, const LaunchBlock& block,
                     const LaunchView& view) noexcept {
    if (request.incarnation != block.incarnation || !std::isfinite(request.velocity)) return false;
    switch (request.kind) {
    case LaunchRequestKind::SlotPress:
    case LaunchRequestKind::SlotRelease:
        return request.track && findTrack(*request.track) && occupied(addressedSlot(view,request));
    case LaunchRequestKind::ScenePress:
        return request.scene && std::binary_search(view.scenes.begin(),view.scenes.end(),*request.scene);
    case LaunchRequestKind::StopTrack:
        return request.track && findTrack(*request.track);
    case LaunchRequestKind::BackToArrangement:
        return !request.track || findTrack(*request.track);
    case LaunchRequestKind::StopAll:
        return !request.track && !request.scene && !request.clip;
    case LaunchRequestKind::CancelScene:
        return request.scene && *request.scene > 0 && !request.track && !request.clip;
    case LaunchRequestKind::RetargetRunning:
        return false; // this gesture is admitted as a separate prepared batch
    }
    return false;
}

void Launcher::queueStop(Track& track, const Pending& stop, std::uint64_t preparedThrough) noexcept {
    track.user = stop;
    track.back.reset();
    track.gateStop.reset();
    track.follow.reset();
    track.repeat.reset();
    track.sceneFollow.reset();
    track.dueScene.reset();
    track.stoppedPreparedThrough = std::max(track.stoppedPreparedThrough,preparedThrough);
    track.stopSuppressesFollow = true;
}

bool Launcher::preparedFollowersCanceled(const Track& track, std::uint64_t token) const noexcept {
    return token && token <= std::max(stoppedPreparedAllThrough_,track.stoppedPreparedThrough);
}

void Launcher::applyUser(const LaunchRequest& request, const LaunchBlock& block,
                         const LaunchView& view, std::int64_t now) noexcept {
    // The owner has already published this scene's clock. Its performed
    // boundary must remain paired with that published timing generation.
    if (userScene_ && userScene_->preparedToken &&
        (request.kind == LaunchRequestKind::ScenePress || request.kind == LaunchRequestKind::CancelScene ||
         (request.kind == LaunchRequestKind::BackToArrangement && !request.track)))
        return;
    switch (request.kind) {
    case LaunchRequestKind::CancelScene:
        if (userScene_ && userScene_->scene == *request.scene) userScene_.reset();
        if (followScene_ && followScene_->scene == *request.scene) cancelSceneFollow();
        for (auto& track : std::span(activeTracks_,trackCount_)) {
            if (track.user && track.user->sceneGesture && track.user->scene == *request.scene)
                track.user.reset();
        }
        return;
    case LaunchRequestKind::StopTrack:
    case LaunchRequestKind::StopAll: {
        const bool all = request.kind == LaunchRequestKind::StopAll;
        const auto grid = !all || stopAllUsesGlobalLaunchQuantization ? block.global : LaunchQuantization::None;
        const Pending stop{Action::Stop,gridForPress(block,grid,now),0,0,1.0f,grid,false,0,false,admission_};
        const auto preparedThrough = std::max({request.preparedSceneToken,
            block.preparedScene ? block.preparedScene->token : 0, userScene_ ? userScene_->preparedToken : 0});
        if (all) {
            // A published clock still settles when the owner cannot withdraw it.
            // Its followers and scene Follow Action are canceled independently.
            if (userScene_ && !userScene_->preparedToken) userScene_.reset();
            stoppedPreparedAllThrough_ = std::max(stoppedPreparedAllThrough_,preparedThrough);
            if (stopAllCancelsSceneFollow) {
                cancelSceneFollow();
                stopAllSuppressesSceneFollow_ = true;
            }
            for (auto& track : std::span(activeTracks_,trackCount_)) queueStop(track,stop,preparedThrough);
        } else {
            queueStop(*findTrack(*request.track),stop,preparedThrough);
        }
        return;
    }
    case LaunchRequestKind::BackToArrangement: {
        const auto grid = backToArrangementIsImmediate ? LaunchQuantization::None : block.global;
        const Pending back{Action::Arrangement,gridForPress(block,grid,now),0,0,1.0f,grid,false,0,false,admission_};
        const auto handBack = [&](Track& track) {
            track.user.reset();
            track.gateStop.reset();
            // A later, quantized slot press must leave Arrangement audible
            // from this immediate hand-back until its own due sample.
            track.back = back;
        };
        if (request.track) handBack(*findTrack(*request.track));
        else {
            userScene_.reset();
            cancelSceneFollow();
            for (auto& track : std::span(activeTracks_,trackCount_)) handBack(track);
        }
        return;
    }
    case LaunchRequestKind::ScenePress:
        armUserScene({*request.scene,gridForPress(block,block.global,now),0,block.global,admission_},
            request.velocity,block,view,now);
        return;
    case LaunchRequestKind::SlotPress:
    case LaunchRequestKind::SlotRelease:
        break;
    case LaunchRequestKind::RetargetRunning:
        return;
    }

    const auto& slot = *addressedSlot(view,request);
    auto& track = *findTrack(slot.track);
    const auto edge = request.kind == LaunchRequestKind::SlotPress ? LaunchEdge::Press : LaunchEdge::Release;
    const auto running = track.run.clip == slot.clip;
    const auto queued = track.user && track.user->action == Action::Start && track.user->clip == slot.clip;
    if (edge == LaunchEdge::Press && track.gateStop && track.gateStop->clip == slot.clip)
        track.gateStop.reset();
    if (edge == LaunchEdge::Release && slot.mode == LaunchMode::Gate && queued &&
        gateReleaseCancelsUnheardLaunch) {
        track.user.reset();
        if (!running) return;
    }
    if (edge == LaunchEdge::Press && slot.mode == LaunchMode::Toggle && queued && !running &&
        togglePressCancelsUnheardLaunch) {
        track.user.reset();
        return;
    }
    const auto decision = launchEdge(running ? LaunchPhase::Running : LaunchPhase::Idle,slot.mode,edge);
    if (decision.repeat == LaunchRepeat::Release) {
        if (track.repeat && track.repeat->clip == slot.clip) track.repeat.reset();
        if (queued && track.user) track.user->repeat = false;
    }
    if (decision.playback == LaunchPlayback::Unchanged) return;
    const auto grid = slot.quantization.value_or(block.global);
    const auto tick = gridForPress(block,grid,now);
    const auto action = decision.playback == LaunchPlayback::Stop ? Action::Stop : Action::Start;
    const Pending pending{action,tick,slot.scene,slot.clip,request.velocity,grid,
                          decision.repeat == LaunchRepeat::Hold,0,false,admission_};
    if (edge == LaunchEdge::Release && slot.mode == LaunchMode::Gate) track.gateStop = pending;
    else track.user = pending;
}

void Launcher::armUserScene(ScenePending scene, float velocity, const LaunchBlock& block,
                            const LaunchView& view, std::int64_t now) noexcept {
    const auto displaced = userScene_;
    scene.velocity = velocity;
    userScene_ = scene;
    for (auto& track : std::span(activeTracks_,trackCount_)) {
        if (displaced && track.user && track.user->sceneGesture && track.user->scene == displaced->scene)
            track.user.reset();
        if (preparedFollowersCanceled(track,scene.preparedToken)) continue;
        const auto* slot = findSlot(view,track.id,scene.scene);
        if (occupied(slot)) {
            const auto grid = scene.preparedToken || scenesUseGlobalLaunchQuantization
                ? scene.grid : slot->quantization.value_or(scene.grid);
            const auto tick = grid == scene.grid ? scene.tick : gridForPress(block,grid,now);
            track.user = Pending{Action::Start,tick,slot->scene,slot->clip,velocity,grid,
                false,scene.earliestSample,true,scene.admission};
        } else if (!slot || slot->hasStop) {
            track.user = Pending{Action::Stop,scene.tick,scene.scene,0,1.0f,scene.grid,
                false,scene.earliestSample,true,scene.admission};
        }
    }
}

void Launcher::applyFollow(const LaunchFollowRequest& follow, const LaunchBlock& block,
                           const LaunchView& view, std::int64_t now, std::int64_t earliestSample) noexcept {
    const auto& request = follow.request;
    if (!block.followActionsEnabled || follow.tick < 0 || !valid(request,block,view)) return;
    const auto due = std::max(follow.tick,now);
    const auto followGrid = followBypassesGlobalLaunchQuantization ? LaunchQuantization::None : block.global;
    if (request.kind == LaunchRequestKind::SlotPress) {
        const auto& slot = *addressedSlot(view,request);
        auto& track = *findTrack(slot.track);
        if (track.stopSuppressesFollow) return;
        const auto grid = slot.quantization.value_or(followGrid);
        const auto tick = launch_quantize::nextGridTick(due,grid,block.meters).value_or(lastTick);
        track.follow = Pending{Action::Start,tick,slot.scene,slot.clip,request.velocity,
            grid,false,earliestSample};
        track.follow->physicalLaps = follow.physicalLaps;
        track.follow->linkedPhysical = follow.physicalLaps > 0;
    }
    if (request.kind == LaunchRequestKind::StopTrack) {
        auto& track = *findTrack(*request.track);
        if (track.stopSuppressesFollow) return;
        track.follow = Pending{Action::Stop,due,0,0,1.0f,LaunchQuantization::None,false,earliestSample};
        track.follow->physicalLaps = follow.physicalLaps;
    }
    if (request.kind == LaunchRequestKind::ScenePress) {
        if (stopAllSuppressesSceneFollow_) return;
        followScene_ = ScenePending{*request.scene,due,earliestSample};
        for (auto& track : std::span(activeTracks_,trackCount_)) {
            track.sceneFollow.reset();
            if (track.stopSuppressesFollow) continue;
            const auto* slot = findSlot(view,track.id,*request.scene);
            if (occupied(slot)) {
                track.sceneFollow = Pending{Action::Start,due,slot->scene,slot->clip,request.velocity,
                    LaunchQuantization::None,false,earliestSample,true};
            } else if (!slot || slot->hasStop) {
                track.sceneFollow = Pending{Action::Stop,due,*request.scene,0,1.0f,
                    LaunchQuantization::None,false,earliestSample,true};
            }
        }
    }
}

void Launcher::cancelSceneFollow() noexcept {
    if (!followScene_) return;
    followScene_.reset();
    for (auto& track : std::span(activeTracks_,trackCount_)) track.sceneFollow.reset();
}

void Launcher::discardUnpublishedScene(const LaunchBlock& block) noexcept {
    if (!block.preparedScenePinned || !userScene_ || !userScene_->preparedToken ||
        (block.preparedScene && block.preparedScene->token == userScene_->preparedToken)) return;
    for (auto& track : std::span(activeTracks_,trackCount_)) {
        if (track.user && track.user->sceneGesture && track.user->scene == userScene_->scene) track.user.reset();
    }
    userScene_.reset();
}

void Launcher::settleScene(const ScenePending& pending, bool fromFollow, bool rolling,
                           const LaunchBlock& block, const LaunchView& view, LaunchSink sink,
                           std::int32_t offset) noexcept {
    const bool validScene = std::binary_search(view.scenes.begin(),view.scenes.end(),pending.scene);
    const auto sample = block.sample + offset;
    const auto tick = rolling && block.sampleAtTick(block.clock,pending.tick) == sample
        ? pending.tick : atSample(block,sample);
    if (fromFollow) {
        for (auto& track : std::span(activeTracks_,trackCount_)) {
            track.dueScene = validScene ? track.sceneFollow : std::nullopt;
            track.sceneFollow.reset();
            if (track.dueScene) {
                track.dueScene->tick = tick;
                track.dueScene->earliestSample = sample;
            }
        }
    }
    if (!validScene) return;
    if (pending.preparedToken) {
        for (auto& track : std::span(activeTracks_,trackCount_)) {
            if (preparedFollowersCanceled(track,pending.preparedToken)) continue;
            const auto* slot = findSlot(view,track.id,pending.scene);
            if (occupied(slot) || !slot || slot->hasStop) {
                const auto action = occupied(slot) ? Action::Start : Action::Stop;
                track.dueScene = Pending{action,tick,pending.scene,slot ? slot->clip : 0,pending.velocity,
                    LaunchQuantization::None,false,sample,true,pending.admission,true};
                if (track.user && track.user->sceneGesture && track.user->scene == pending.scene)
                    track.user.reset();
            }
        }
    }
    const bool clockOnly = pending.preparedToken && pending.preparedToken <= stoppedPreparedAllThrough_;
    if (!fromFollow && !clockOnly) {
        cancelSceneFollow();
        stopAllSuppressesSceneFollow_ = false;
    }
    const LaunchSceneTransition edge{pending.scene,sample,tick,offset,++sceneOrdinal_,fromFollow,pending.preparedToken};
    if (sink.scenePerformed) sink.scenePerformed(sink.context,edge);
    if (!clockOnly && rolling && block.followActionsEnabled && sink.followOnSceneLaunch) {
        const auto follow = sink.followOnSceneLaunch(sink.context,edge);
        if (follow && follow->tick > tick && follow->request.kind == LaunchRequestKind::ScenePress)
            applyFollow(*follow,block,view,tick,sample + 1);
    }
}

bool Launcher::transition(Track& track, const LaunchBlock& block, const LaunchView& view,
                          LaunchSink sink, const Pending& pending, std::int32_t offset,
                          std::int64_t tick, LaunchCueAdmission cueAdmission) noexcept {
    const auto previous = track.run;
    const auto* target = pending.clip ? findSlot(view,track.id,pending.scene) : nullptr;
    if (pending.sceneGesture && !std::binary_search(view.scenes.begin(),view.scenes.end(),pending.scene)) return false;
    if (pending.action == Action::Start) {
        if (!occupied(target) || target->clip != pending.clip) return false;
    } else if (pending.clip != 0 && previous.clip != pending.clip) {
        return false;
    } else if (pending.sceneGesture) {
        const auto* slot = findSlot(view,track.id,pending.scene);
        if (slot && (occupied(slot) || !slot->hasStop)) return false;
    }

    LaunchSlot oldGeometry;
    const auto* oldSlot = previous.clip
        ? effectiveSlot(findClip(view,track.id,previous.clip),previous.region,previous.physicalRegion,oldGeometry) : nullptr;
    const auto oldLap = cuedPhysicalLap(oldSlot,previous.completedLoops,previous.physicalCueFrame);
    const auto elapsed = previous.frames - previous.pcmAnchorFrame;
    LaunchSourceCue cue;
    if (pending.action == Action::Start) {
        const auto oldFrame = oldLap.startFrame < 0 ? -1 :
            oldSlot->physicalLoopEndFrame && elapsed == oldLap.length() ? oldSlot->physicalLoopBeginFrame :
            oldLap.originFrame() + std::min(elapsed,oldLap.length());
        const auto sourceTick = oldLap.startFrame >= 0 ? oldLap.tickAt(elapsed) : previous.sourceTick;
        cue = launchSourceCue(*target,oldSlot,sourceTick,oldFrame);
        if (occupied(oldSlot) && target->clip != previous.clip && target->legato && sink.admitLegatoCue &&
            !sink.admitLegatoCue(sink.context,*target,cue,tick,offset,cueAdmission)) return false;
    }
    track.follow.reset();
    track.repeat.reset();
    track.regionEdit.reset();
    track.run = {};
    track.run.arrangement = pending.action == Action::Arrangement;
    if (pending.action == Action::Start) {
        track.stopSuppressesFollow = false;
        const auto position = cue.tick;
        track.run = {target->scene,target->clip,position,0,launchVelocity(*target,pending.velocity),
                     ++runSerial_,false,tick,position,position,0,0,target->fade};
        track.run.physicalCueFrame = cue.physicalFrame;
        if (pending.repeat) {
            const bool repeatAtSourceEnd = pending.grid == LaunchQuantization::None && repeatWithoutQuantizationRestartsAtClipEnd;
            const auto next = repeatAtSourceEnd
                ? add(tick,sourceEnd(*target) - position)
                : launch_quantize::nextGridTick(add(tick,1),pending.grid,block.meters).value_or(lastTick);
            if (next > tick) {
                track.repeat = pending;
                track.repeat->tick = next;
                track.repeat->earliestSample = block.sample + offset + 1;
                track.repeat->physicalLaps = repeatAtSourceEnd && target->physicalFrames > 0 ? 1 : 0;
            }
        }
    }
    if (previous.arrangement == track.run.arrangement && previous.serial == track.run.serial) return true;
    const auto followOrdinal = track.run.clip != 0 ? ++track.followOrdinal : 0;
    const LaunchTransition edge{track.id,previous.clip,track.run.clip,track.run.sourceTick,
        block.sample + offset,tick,offset,track.run.arrangement,previous.serial,track.run.serial,followOrdinal,
        track.run.velocity,track.run.fade,oldLap.startFrame >= 0 ? oldLap.tickAt(0) : previous.pcmAnchorTick,elapsed,
        previous.frames,previous.velocity,previous.fade,previous.anchorTick,previous.anchorSourceTick,
        oldLap.originFrame()};
    if (sink.transition) sink.transition(sink.context,edge);
    if (block.followActionsEnabled && sink.followOnLaunch && edge.newClip != 0) {
        const auto follow = sink.followOnLaunch(sink.context,edge);
        if (follow && follow->tick > tick && follow->request.track == track.id &&
            (follow->request.kind == LaunchRequestKind::StopTrack ||
             follow->request.kind == LaunchRequestKind::SlotPress)) {
            applyFollow(*follow,block,view,tick,edge.sample + 1);
        }
    }
    return true;
}

void Launcher::render(Track& track, const LaunchBlock& block, const LaunchView& view,
                      LaunchSink sink, std::int64_t tick, std::int32_t cursor,
                      std::int32_t end) noexcept {
    auto phaseChangedAtCursor = false;
    auto refusedUserStartAtCursor = false;
    while (cursor < end) {
        const LaunchSlot* preparedSlot = nullptr;
        if (track.run.clip != 0) {
            preparedSlot = findSlot(view,track.id,track.run.scene);
            if (!preparedSlot || preparedSlot->clip != track.run.clip)
                preparedSlot = findClip(view,track.id,track.run.clip);
            if (preparedSlot) {
                track.run.scene = preparedSlot->scene;
                track.run.fade = preparedSlot->fade;
            }
        }
        LaunchSlot regionGeometry;
        const auto* slot = effectiveSlot(preparedSlot,track.run.region,track.run.physicalRegion,regionGeometry);
        const auto elapsed = track.run.frames - track.run.pcmAnchorFrame;
        const auto lap = cuedPhysicalLap(slot,track.run.completedLoops,track.run.physicalCueFrame);
        if (lap.startFrame >= 0) {
            track.run.pcmAnchorTick = lap.tickAt(0);
            track.run.sourceTick = lap.tickAt(elapsed);
        }
        auto sourceOffset = block.frames;
        auto sourceTick = lastTick;
        if (track.run.clip != 0 && !occupied(slot)) {
            sourceOffset = cursor;
            sourceTick = tick;
        } else if (lap.startFrame >= 0) {
            const auto remaining = std::max<std::int64_t>(0,lap.length() - elapsed);
            if (remaining <= block.frames - cursor) {
                sourceOffset = cursor + static_cast<std::int32_t>(remaining);
                sourceTick = atSample(block,block.sample + sourceOffset);
            }
        } else if (slot) {
            const auto untilEnd = std::max<std::int64_t>(0,sourceEnd(*slot) - track.run.anchorSourceTick);
            sourceTick = add(track.run.anchorTick,untilEnd);
            sourceOffset = std::max(cursor,offsetAt(block,sourceTick));
            if (sourceOffset == cursor && untilEnd > 0 && phaseChangedAtCursor) ++sourceOffset;
        }
        std::optional<Pending>* next = nullptr;
        auto boundary = end;
        bool editRegion = false;
        const bool prepared = track.dueScene && track.dueScene->prepared;
        const auto usesPhysicalLaps = [lap](const Pending& pending) noexcept {
            return lap.startFrame >= 0 && pending.physicalLaps > 0;
        };
        for (auto* candidate : {prepared ? &track.dueScene : &track.back, &track.user, &track.gateStop,
                                prepared ? &track.back : &track.dueScene, &track.follow, &track.repeat}) {
            if (!*candidate) continue;
            const auto earliest = static_cast<std::int32_t>(std::clamp<std::int64_t>(
                (*candidate)->earliestSample - block.sample, 0, block.frames));
            auto due = offsetAt(block,(*candidate)->tick);
            if (usesPhysicalLaps(**candidate)) {
                if (track.run.completedLoops + 1 < static_cast<std::uint64_t>((*candidate)->physicalLaps))
                    continue;
                due = sourceOffset;
            }
            const auto offset = std::max({cursor,earliest,due});
            if (offset < boundary) {
                boundary = offset;
                next = candidate;
            }
        }
        if (track.regionEdit) {
            const auto& pending = *track.regionEdit;
            const auto fits = pending.physical ? physicalRegionFits(preparedSlot,*pending.physical)
                                               : regionFits(preparedSlot,pending.edit.region);
            if (pending.edit.clip != track.run.clip || pending.edit.run != track.run.serial || !fits) {
                track.regionEdit.reset();
            } else {
                const auto editSample = pending.edit.rate == ClipUpdateRate::None
                    ? pending.boundary.sample : block.sampleAtTick(block.clock,pending.boundary.tick);
                const auto relative = std::max(block.sample,editSample) - block.sample;
                const auto offset = static_cast<std::int32_t>(std::min<std::int64_t>(
                    block.frames,std::max<std::int64_t>(cursor,relative)));
                if (offset < boundary) {
                    boundary = offset;
                    next = nullptr;
                    editRegion = true;
                }
            }
        }
        if (sourceOffset < boundary) {
            boundary = sourceOffset;
            next = nullptr;
            editRegion = false;
        }
        auto edgeTick = next && !usesPhysicalLaps(**next) ? (*next)->tick : sourceTick;
        if (editRegion) {
            const auto target = track.regionEdit->boundary.tick;
            edgeTick = block.sampleAtTick(block.clock,target) == block.sample + boundary
                ? target : atSample(block,block.sample + boundary);
        }
        if (offsetAt(block,edgeTick) < boundary)
            edgeTick = atSample(block,block.sample + boundary);
        const auto nextTick = boundary < end ? edgeTick : atSample(block,block.sample + boundary);
        const auto atSourceEdge = slot && sourceOffset == boundary &&
            (boundary < block.frames || (sourceTick != lastTick &&
             block.sampleAtTick(block.clock,sourceTick) == block.sample + boundary));
        if (boundary > cursor) {
            auto endSource = track.run.sourceTick;
            if (slot) {
                if (lap.startFrame >= 0)
                    endSource = lap.tickAt(add(elapsed,boundary-cursor));
                else
                    endSource = atSourceEdge ? sourceEnd(*slot) : std::min(sourceEnd(*slot),
                        add(track.run.anchorSourceTick,std::max<std::int64_t>(0,nextTick - track.run.anchorTick)));
            }
            if (sink.section) sink.section(sink.context, {track.id,track.run.clip,track.run.sourceTick,
                endSource,cursor,boundary-cursor,track.run.velocity,track.run.arrangement,
                slot && slot->fade,track.run.serial,track.run.frames,tick,
                track.run.pcmAnchorTick,elapsed,track.run.region.has_value(),
                track.run.anchorTick,track.run.anchorSourceTick,lap.originFrame()});
            track.run.sourceTick = endSource;
            track.run.frames = add(track.run.frames,boundary-cursor);
            cursor = boundary;
            phaseChangedAtCursor = false;
            refusedUserStartAtCursor = false;
        }
        if (cursor == end) break;
        if (next) {
            auto pending = **next;
            next->reset();
            if (usesPhysicalLaps(pending) && pending.grid != LaunchQuantization::None) {
                const auto sample = block.sample + cursor;
                const auto target = gridAtOrAfterSample(block,pending.grid,sample);
                const auto targetSample = target == lastTick ? lastTick : block.sampleAtTick(block.clock,target);
                if (targetSample != sample) {
                    pending.tick = target;
                    pending.physicalLaps = 0;
                    *next = pending;
                    continue;
                }
                edgeTick = target;
            }
            if (next == &track.follow && pending.linkedPhysical && refusedUserStartAtCursor) {
                // A due manual Start owns its sample even when its cue is cold.
                // A Follow deferred to a later grid already continued above.
                tick = nextTick;
                continue;
            }
            if (atSourceEdge) track.run.sourceTick = sourceEnd(*slot);
            const auto run = track.run.serial;
            auto cueAdmission = LaunchCueAdmission::None;
            if (next == &track.user && !pending.sceneGesture) {
                if (pending.grid != LaunchQuantization::None) cueAdmission = LaunchCueAdmission::Scheduled;
                else cueAdmission = pending.repeat ? LaunchCueAdmission::HeldRepeat : LaunchCueAdmission::ReadyOnly;
            } else if (next == &track.follow && pending.linkedPhysical) {
                cueAdmission = LaunchCueAdmission::Linked;
            }
            if (transition(track,block,view,sink,pending,cursor,edgeTick,cueAdmission)) {
                if (track.dueScene && track.dueScene->earliestSample <= block.sample + cursor)
                    track.dueScene.reset();
                if (pending.prepared) {
                    for (auto* other : {&track.back,&track.user,&track.gateStop}) {
                        if (*other && (*other)->earliestSample <= block.sample + cursor &&
                            offsetAt(block,(*other)->tick) <= cursor) other->reset();
                    }
                }
            } else if (next == &track.user && pending.action == Action::Start && !pending.sceneGesture) {
                const auto* requested = findSlot(view,track.id,pending.scene);
                refusedUserStartAtCursor = occupied(requested) && requested->clip == pending.clip;
            }
            phaseChangedAtCursor = phaseChangedAtCursor || run != track.run.serial;
        } else if (editRegion) {
            const auto region = track.regionEdit->edit.region;
            track.run.physicalRegion = track.regionEdit->physical;
            track.regionEdit.reset();
            track.run.region = region;
            if (const auto& frames = track.run.physicalRegion; frames && lap.startFrame >= 0) {
                const auto played = std::min(lap.length(),track.run.frames - track.run.pcmAnchorFrame);
                auto frame = std::max(frames->startFrame,lap.originFrame() + played);
                if (frames->loopEndFrame && frame >= frames->loopEndFrame)
                    frame = frames->loopBeginFrame + (frame - frames->loopEndFrame) %
                        (frames->loopEndFrame - frames->loopBeginFrame);
                else if (frame >= frames->endFrame)
                    frame = frames->startFrame;
                const auto extent = physicalExtent(*preparedSlot);
                const auto editedSourceTick = extent.tickAt(frame - extent.startFrame);
                track.run.physicalCueFrame = frame;
                track.run.sourceTick = track.run.anchorSourceTick = track.run.pcmAnchorTick = editedSourceTick;
                track.run.anchorTick = edgeTick;
                track.run.pcmAnchorFrame = track.run.frames;
                phaseChangedAtCursor = true;
                tick = nextTick;
                continue;
            }
            auto revisedPosition = std::max(track.run.sourceTick,region.start);
            if (region.loop && revisedPosition >= region.loop->end) {
                revisedPosition = region.loop->begin + (revisedPosition - region.loop->end) %
                    (region.loop->end - region.loop->begin);
            } else if (revisedPosition >= region.end) {
                revisedPosition = region.start;
            }
            if (revisedPosition != track.run.sourceTick) {
                track.run.sourceTick = track.run.anchorSourceTick = revisedPosition;
                track.run.anchorTick = edgeTick;
                track.run.pcmAnchorTick = revisedPosition;
                track.run.pcmAnchorFrame = track.run.frames;
                phaseChangedAtCursor = true;
            }
        } else if (occupied(slot) && loops(*slot)) {
            track.run.sourceTick = track.run.anchorSourceTick = slot->loopStartTick;
            track.run.anchorTick = nextTick;
            track.run.pcmAnchorTick = slot->loopStartTick;
            track.run.pcmAnchorFrame = track.run.frames;
            track.run.physicalCueFrame = -1;
            ++track.run.completedLoops;
            phaseChangedAtCursor = true;
        } else {
            // A follow action can be due after a one-shot has stopped sounding.
            const auto follow = track.follow;
            const auto repeat = track.repeat;
            transition(track,block,view,sink,Pending{Action::Stop,nextTick},cursor,nextTick);
            track.follow = follow;
            track.repeat = repeat;
        }
        tick = nextTick;
    }
}

Launcher::RetargetResult Launcher::retarget(std::span<const LaunchRequest> requests,
                                            const LaunchBlock& block, const LaunchView& view, LaunchSink sink) noexcept {
    if (requests.empty() || requests.size() > LaunchGestureQueue::capacity || !requests.front().scene)
        return RetargetResult::Rejected;
    const auto scene = *requests.front().scene;
    std::array<std::int64_t, LaunchGestureQueue::capacity> tracks{};
    std::size_t count = 0;
    for (const auto& request : requests) {
        if (request.kind != LaunchRequestKind::RetargetRunning || request.incarnation != block.incarnation ||
            request.scene != scene || scene <= 0 || !request.track || !request.clip || !request.expectedClip ||
            *request.clip <= 0 || request.clip == request.expectedClip || request.expectedRun == 0)
            return RetargetResult::Rejected;
        const auto* track = findTrack(*request.track);
        if (!track || track->run.clip != *request.expectedClip || track->run.serial != request.expectedRun)
            return RetargetResult::Rejected;
        tracks[count++] = *request.track;
    }
    auto* end = tracks.data() + count;
    std::sort(tracks.data(),end);
    if (std::adjacent_find(tracks.data(),end) != end)
        return RetargetResult::Rejected;
    if (!std::binary_search(view.scenes.begin(),view.scenes.end(),scene))
        return RetargetResult::PendingView;

    for (const auto& request : requests) {
        const auto* before = findClip(view,*request.track,*request.expectedClip);
        const auto* after = findSlot(view,*request.track,scene);
        if (!occupied(before) || !occupied(after) || after->clip != *request.clip ||
            before->startTick != after->startTick || before->endTick != after->endTick ||
            before->loopStartTick != after->loopStartTick || before->loopEndTick != after->loopEndTick ||
            before->physicalStartFrame != after->physicalStartFrame || before->physicalFrames != after->physicalFrames ||
            before->physicalLoopBeginFrame != after->physicalLoopBeginFrame ||
            before->physicalLoopEndFrame != after->physicalLoopEndFrame ||
            before->mode != after->mode || before->quantization != after->quantization ||
            before->legato != after->legato || before->fade != after->fade || before->velocityAmount != after->velocityAmount)
            return RetargetResult::Rejected;
    }
    for (const auto& request : requests) {
        auto& track = *findTrack(*request.track);
        const auto rebindRunRequest = [&](std::optional<Pending>& pending) {
            if (pending && pending->clip == *request.expectedClip) {
                pending->clip = *request.clip;
                pending->scene = scene;
            }
        };
        rebindRunRequest(track.repeat);
        rebindRunRequest(track.gateStop);
        if (track.user && track.user->action == Action::Stop && !track.user->sceneGesture)
            rebindRunRequest(track.user);
        // No new run edge, chase, gain ramp or Follow Action draw. The new
        // clip section names the copy and the old run's exact clock and age.
        if (sink.retargeted) {
            const auto& before = *findClip(view,track.id,*request.expectedClip);
            const auto& after = *findSlot(view,track.id,scene);
            sink.retargeted(sink.context,before,after,track.run.serial);
        }
        track.run.clip = *request.clip;
        track.run.scene = scene;
    }
    return RetargetResult::Applied;
}

void Launcher::drain(const LaunchBlock& block, const LaunchView& view, std::int64_t now, LaunchSink sink) noexcept {
    ++admission_;
    if (deferredRetargetSize_ &&
        retarget(std::span(deferredRetarget_).first(deferredRetargetSize_),block,view,sink) != RetargetResult::PendingView)
        deferredRetargetSize_ = 0;
    for (std::size_t remaining = LaunchGestureQueue::capacity; remaining > 0;) {
        const auto result = gestures_.pop(batch_);
        if (result.status != LaunchGestureQueue::PopStatus::Ready) break;
        const auto gesture = std::span(batch_).first(result.size);
        if (std::any_of(gesture.begin(),gesture.end(),[](const LaunchRequest& request) {
                return request.kind == LaunchRequestKind::RetargetRunning;
            })) {
            switch (retarget(gesture,block,view,sink)) {
            case RetargetResult::PendingView:
                std::copy_n(gesture.data(),gesture.size(),deferredRetarget_.data());
                deferredRetargetSize_ = gesture.size();
                break;
            case RetargetResult::Applied:
                deferredRetargetSize_ = 0;
                break;
            case RetargetResult::Rejected:
                break;
            }
        } else if (std::all_of(gesture.begin(),gesture.end(),
                        [&](const auto& request) { return valid(request,block,view); })) {
            for (const auto& request : gesture) applyUser(request,block,view,now);
        }
        // A producer may refill the ring while it is drained. Bound the
        // callback's admission work even under continuous owner activity.
        if (result.size >= remaining) break;
        remaining -= result.size;
    }
}

bool Launcher::admitWhileStopped(const LaunchBlock& block, const LaunchView& view, LaunchSink sink) noexcept {
    if (!block.tickAtSample || !block.sampleAtTick || block.sample < 0 || view.tracks.size() > tracks_.size())
        return false;
    adopt(block,view,sink);
    discardUnpublishedScene(block);
    const auto now = atSample(block,block.sample);
    drain(block,view,now,sink);
    const auto active = std::span(activeTracks_,trackCount_);
    const auto fresh = [this](const std::optional<Pending>& pending) {
        return pending && pending->admission == admission_;
    };
    if (std::any_of(active.begin(),active.end(),[&](const Track& track) {
            return fresh(track.user) && (track.user->action == Action::Start ||
                (emptySceneStartsStoppedTransport && track.user->sceneGesture));
        })) return true;
    if (userScene_ && userScene_->admission == admission_) {
        const auto scene = *userScene_;
        userScene_.reset();
        settleScene(scene,false,false,block,view,sink,0);
    }
    for (auto& track : active) {
        for (auto* request : {&track.back,&track.user,&track.gateStop}) {
            if (!fresh(*request)) continue;
            const auto pending = **request;
            request->reset();
            transition(track,block,view,sink,pending,0,now);
        }
        if (track.run.clip && !occupied(findClip(view,track.id,track.run.clip)))
            transition(track,block,view,sink,Pending{Action::Stop,now},0,now);
    }
    publishStatus(block,view,sink,false,now);
    return false;
}

bool Launcher::process(const LaunchBlock& block, const LaunchView& view, LaunchSink sink,
                       std::span<const LaunchFollowRequest> follow) noexcept {
    if (!block.tickAtSample || !block.sampleAtTick || block.frames <= 0 || block.sample < 0 ||
        block.sample > lastTick - block.frames || view.tracks.size() > tracks_.size()) return false;
    adopt(block,view,sink);
    discardUnpublishedScene(block);
    const auto now = atSample(block,block.sample);
    const auto active = std::span(activeTracks_,trackCount_);
    if (followPass_ != block.followPass) {
        for (auto& track : active) {
            track.followOrdinal = 0;
            track.follow.reset();
        }
        sceneOrdinal_ = 0;
        cancelSceneFollow();
        followPass_ = block.followPass;
    }
    if (nextSample_ >= 0 && (block.sample != nextSample_ || now != nextTick_)) {
        for (auto& track : active) {
            track.run.anchorTick = now;
            track.run.anchorSourceTick = track.run.sourceTick;
        }
    }
    if (!block.followActionsEnabled) {
        for (auto& track : active) track.follow.reset();
        cancelSceneFollow();
    }
    if (nextSample_ >= 0 && block.sample != nextSample_) {
        // A natural loop must still reach a grid deadline even if the loop is
        // shorter than its grid. An explicit locate resolves its new grid.
        for (auto& track : active) {
            for (auto* pending : {&track.back,&track.user,&track.gateStop}) {
                if (*pending) {
                    if (userScene_ && userScene_->preparedToken && (*pending)->sceneGesture &&
                        (*pending)->scene == userScene_->scene)
                        continue;
                    (*pending)->tick = block.arrangementWrap && preserveQuantizedDeadlinesAcrossArrangementWrap
                        ? add(now,std::max<std::int64_t>(0,(*pending)->tick-nextTick_))
                        : gridForPress(block,(*pending)->grid,now);
                }
            }
            for (auto* pending : {&track.follow,&track.repeat}) {
                if (*pending) {
                    (*pending)->tick = add(now,std::max<std::int64_t>(0,(*pending)->tick-nextTick_));
                    (*pending)->earliestSample = block.sample;
                }
            }
            if (track.regionEdit) {
                auto& pending = *track.regionEdit;
                if (block.arrangementWrap) {
                    pending.boundary.tick = add(now,std::max<std::int64_t>(0,pending.boundary.tick-nextTick_));
                    pending.boundary.sample = block.sampleAtTick(block.clock,pending.boundary.tick);
                } else if (const auto boundary = nextClipUpdateBoundary(block,block.sample,pending.edit.rate,true)) {
                    pending.boundary = *boundary;
                } else {
                    track.regionEdit.reset();
                }
            }
        }
        if (userScene_ && !userScene_->preparedToken) {
            userScene_->tick = block.arrangementWrap && preserveQuantizedDeadlinesAcrossArrangementWrap
                ? add(now,std::max<std::int64_t>(0,userScene_->tick-nextTick_))
                : gridForPress(block,userScene_->grid,now);
        }
        if (followScene_) {
            followScene_->tick = add(now,std::max<std::int64_t>(0,followScene_->tick-nextTick_));
            followScene_->earliestSample = block.sample;
        }
    }
    drain(block,view,now,sink);
    drainRegionEdits(block,view);
    if (block.preparedScene && block.preparedScene->token > lastPreparedScene_) {
        const auto& scene = *block.preparedScene;
        lastPreparedScene_ = scene.token;
        if (scene.tick >= 0 && std::isfinite(scene.velocity) &&
            std::binary_search(view.scenes.begin(),view.scenes.end(),scene.scene))
            armUserScene({scene.scene,scene.tick,0,LaunchQuantization::None,admission_,scene.token},
                scene.velocity,block,view,now);
    }
    for (const auto& due : follow) applyFollow(due,block,view,now);
    auto cursor = std::int32_t{0};
    while (cursor < block.frames) {
        const auto sceneOffset = [&](const std::optional<ScenePending>& pending) {
            if (!pending) return block.frames;
            const auto earliest = static_cast<std::int32_t>(std::clamp<std::int64_t>(
                pending->earliestSample - block.sample,0,block.frames));
            return std::max({cursor,earliest,offsetAt(block,pending->tick)});
        };
        const auto userOffset = sceneOffset(userScene_);
        const auto followOffset = sceneOffset(followScene_);
        const auto end = std::min(userOffset,followOffset);
        if (end > cursor) {
            const auto tick = atSample(block,block.sample + cursor);
            for (auto& track : active) render(track,block,view,sink,tick,cursor,end);
            cursor = end;
        }
        if (cursor == block.frames) break;
        const bool fromFollow = followOffset < userOffset;
        auto& next = fromFollow ? followScene_ : userScene_;
        const auto pending = *next;
        next.reset();
        settleScene(pending,fromFollow,true,block,view,sink,cursor);
    }
    nextSample_ = block.sample + block.frames;
    nextTick_ = atSample(block,nextSample_);
    publishStatus(block,view,sink,true,nextTick_);
    return true;
}

std::optional<LaunchSourceCue> Launcher::queuedLegatoSourceCue(const Track& track, const Pending& pending,
    const LaunchSlot& target, const LaunchSlot* current, const LaunchBlock& block, std::int64_t endSample,
    std::int64_t* due) const noexcept {
    const auto& run = track.run;
    if (!target.legato || target.clip == run.clip || !occupied(current) || pending.tick == lastTick) return {};
    const auto lap = cuedPhysicalLap(current,run.completedLoops,run.physicalCueFrame);
    const auto physicalSample = [&](const Pending& event) {
        if (event.physicalLaps > 0 && lap.startFrame >= 0) {
            const auto first = std::max<std::int64_t>(0,lap.length() - (run.frames - run.pcmAnchorFrame));
            const auto period = physicalLap(current,true).length();
            const auto completed = std::min(run.completedLoops,static_cast<std::uint64_t>(event.physicalLaps));
            const auto laps = std::max<std::int64_t>(0,event.physicalLaps - static_cast<std::int64_t>(completed) - 1);
            if (laps > (lastTick - first) / period) return lastTick;
            const auto lapEnd = std::max(event.earliestSample,add(endSample,first + laps * period));
            if (event.grid == LaunchQuantization::None) return lapEnd;
            const auto tick = gridAtOrAfterSample(block,event.grid,lapEnd);
            return tick == lastTick ? lastTick : block.sampleAtTick(block.clock,tick);
        }
        return event.tick == lastTick ? lastTick
            : std::max(event.earliestSample,block.sampleAtTick(block.clock,event.tick));
    };
    const auto dueSample = physicalSample(pending);
    if (dueSample == lastTick) return {};
    // Match render's tie order so a linked launch never hides an earlier or
    // coincident user Start/Stop. Leave scene settlement conservative.
    const bool prepared = track.dueScene && track.dueScene->prepared;
    auto winsTie = true;
    for (auto* event : {prepared ? &track.dueScene : &track.back, &track.user, &track.gateStop,
                        prepared ? &track.back : &track.dueScene, &track.follow, &track.repeat}) {
        if (!*event) continue;
        if (&**event == &pending) { winsTie = false; continue; }
        const auto sample = physicalSample(**event);
        if (sample < dueSample || (sample == dueSample && winsTie)) return {};
    }
    if (track.sceneFollow && physicalSample(*track.sceneFollow) <= dueSample) return {};
    if (track.regionEdit && (track.regionEdit->edit.rate == ClipUpdateRate::None
        ? track.regionEdit->boundary.sample : block.sampleAtTick(block.clock,track.regionEdit->boundary.tick)) <= dueSample)
        return {};

    if (due) *due = dueSample;
    if (current->physicalFrames > 0) {
        const auto elapsed = add(run.frames - run.pcmAnchorFrame,std::max<std::int64_t>(0,dueSample - endSample));
        if (elapsed < lap.length() || (!current->physicalLoopEndFrame && elapsed == lap.length()))
            return launchSourceCue(target,current,lap.tickAt(elapsed),lap.originFrame()+elapsed);
        if (!current->physicalLoopEndFrame) return {};
        const auto wrapped = physicalLap(current,true);
        const auto phase = (elapsed - lap.length()) % wrapped.length();
        return launchSourceCue(target,current,wrapped.tickAt(phase),wrapped.startFrame+phase);
    }
    const auto dueTick = block.sampleAtTick(block.clock,pending.tick) == dueSample
        ? pending.tick : atSample(block,dueSample);
    const auto end = sourceEnd(*current);
    const auto firstEndTick = add(run.anchorTick,std::max<std::int64_t>(0,end - run.anchorSourceTick));
    const auto firstEndSample = block.sampleAtTick(block.clock,firstEndTick);
    const auto sourceAtDue = add(run.anchorSourceTick,std::max<std::int64_t>(0,dueTick - run.anchorTick));
    if (!loops(*current)) {
        if (firstEndSample < dueSample) return {};
        return launchSourceCue(target,current,firstEndSample == dueSample ? end : std::min(end,sourceAtDue));
    }

    // A rounded source wrap and a later grid tick can occupy the same physical
    // sample. Render takes the wrapped phase on that sample.
    const auto length = current->loopEndTick - current->loopStartTick;
    const auto wrap = dueTick <= firstEndTick ? firstEndTick : dueTick - (dueTick - firstEndTick) % length;
    const auto wrapSample = block.sampleAtTick(block.clock,wrap);
    const auto nextWrapSample = block.sampleAtTick(block.clock,add(wrap,length));
    if (wrapSample == nextWrapSample) return {};
    return launchSourceCue(target,current,wrapSample == dueSample || nextWrapSample == dueSample ? end : sourceAtDue);
}

LaunchTrackStatus Launcher::trackStatus(const Track& track, const LaunchBlock& block,
                                        const LaunchView& view, std::int64_t endSample) const noexcept {
    const auto& run = track.run;
    LaunchTrackStatus status;
    status.track = track.id;
    status.clip = run.clip;
    status.scene = run.scene;
    status.sourceTick = run.sourceTick;
    status.run = run.serial;
    status.completedLoops = run.completedLoops;
    const LaunchSlot* slot = nullptr;
    LaunchSlot regionGeometry;
    if (run.clip) {
        status.source = LaunchTrackSource::Session;
        slot = findSlot(view,track.id,run.scene);
        if (!slot || slot->clip != run.clip) slot = findClip(view,track.id,run.clip);
        slot = effectiveSlot(slot,run.region,run.physicalRegion,regionGeometry);
        const auto lap = cuedPhysicalLap(slot,run.completedLoops,run.physicalCueFrame);
        const auto elapsed = run.frames - run.pcmAnchorFrame;
        if (lap.startFrame >= 0) status.sourceTick = lap.tickAt(elapsed);
        if (slot && loops(*slot)) {
            status.loopStartTick = slot->loopStartTick;
            status.loopLengthTicks = slot->loopEndTick - slot->loopStartTick;
        } else if (lap.startFrame >= 0) {
            status.remainingSamples = std::max<std::int64_t>(0,lap.length() - elapsed);
        } else if (occupied(slot)) {
            const auto end = add(run.anchorTick,std::max<std::int64_t>(0,slot->endTick - run.anchorSourceTick));
            status.remainingSamples = std::max<std::int64_t>(0,block.sampleAtTick(block.clock,end) - endSample);
        }
    } else if (!run.arrangement) {
        status.source = LaunchTrackSource::Silence;
    }
    const Pending* pending = track.user ? &*track.user : nullptr;
    if (track.gateStop && (!pending || track.gateStop->tick < pending->tick)) pending = &*track.gateStop;
    if (track.back && (!pending || track.back->tick <= pending->tick)) pending = &*track.back;
    if (pending) {
        if (pending->sceneGesture && !std::binary_search(view.scenes.begin(),view.scenes.end(),pending->scene))
            return status;
        const auto* target = pending->scene ? findSlot(view,track.id,pending->scene) : nullptr;
        LaunchQueuedAction action = LaunchQueuedAction::None;
        if (occupied(target) && target->clip == pending->clip) action = LaunchQueuedAction::Start;
        if (pending->action == Action::Stop) {
            const auto mayStop = pending->clip ? pending->clip == run.clip
                : !pending->sceneGesture || !target || (!occupied(target) && target->hasStop);
            action = mayStop ? LaunchQueuedAction::Stop : LaunchQueuedAction::None;
        }
        if (pending->action == Action::Arrangement) action = LaunchQueuedAction::BackToArrangement;
        if (action != LaunchQueuedAction::None) status.queued = {action,pending->clip,pending->scene,pending->tick};
        if (action == LaunchQueuedAction::Start) {
            if (const auto cue = queuedLegatoSourceCue(track,*pending,*target,slot,block,endSample)) {
                status.queued.legatoSourceTick = cue->tick;
                if (cue->physicalFrame >= 0) status.queued.legatoSourceFrame = cue->physicalFrame;
            }
        }
    }
    return status;
}

void Launcher::prefetchLinkedLegato(const Track& track, const LaunchView& view, const LaunchBlock& block,
                                    LaunchSink sink, std::int64_t endSample) const noexcept {
    if (!track.follow || !track.follow->linkedPhysical || !track.run.clip) return;
    const auto& pending = *track.follow;
    const auto* target = findSlot(view,track.id,pending.scene);
    if (!occupied(target) || target->clip != pending.clip || target->physicalFrames <= 0) return;
    const auto* current = findSlot(view,track.id,track.run.scene);
    if (!current || current->clip != track.run.clip) current = findClip(view,track.id,track.run.clip);
    LaunchSlot region;
    current = effectiveSlot(current,track.run.region,track.run.physicalRegion,region);
    std::int64_t due = 0;
    const auto cue = queuedLegatoSourceCue(track,pending,*target,current,block,endSample,&due);
    if (cue && cue->physicalFrame >= 0) sink.prefetchLinkedLegato(sink.context,*target,due,cue->physicalFrame);
}

std::uint32_t Launcher::claimStatusBuffer() noexcept {
    const auto current = publishedStatus_.load(std::memory_order_relaxed);
    for (std::uint32_t index = 0; index < statusBufferCount; ++index) {
        if (index == current) continue;
        auto idle = std::uint32_t{0};
        if (statusBuffers_[index].readers.compare_exchange_strong(idle,bufferWriting,
                std::memory_order_acq_rel,std::memory_order_relaxed)) return index;
    }
    return statusBufferCount;
}

void Launcher::finishStatusBuffer(std::uint32_t index) noexcept {
    auto& frame = statusBuffers_[index];
    frame.header.revision = ++statusSerial_;
    publishedStatus_.store(index,std::memory_order_release);
    // A reader acquiring the unlocked frame also observes its publication.
    frame.readers.store(0,std::memory_order_release);
}

void Launcher::publishStatus(const LaunchBlock& block, const LaunchView& view, LaunchSink sink,
                             bool playing, std::int64_t endTick) noexcept {
    const auto index = claimStatusBuffer();
    const auto endSample = playing ? block.sample + block.frames : block.sample;
    auto* frame = index < statusBufferCount ? &statusBuffers_[index] : nullptr;
    const bool prefetch = playing && sink.prefetchLinkedLegato;
    if (!frame && !sink.trackStatus && !prefetch) return;
    auto back = false;
    for (std::size_t row = 0; row < trackCount_; ++row) {
        const auto status = trackStatus(activeTracks_[row],block,view,endSample);
        back = back || status.source != LaunchTrackSource::Arrangement;
        if (frame) frame->rows[row] = status;
        if (sink.trackStatus) sink.trackStatus(sink.context,status);
        if (prefetch) prefetchLinkedLegato(activeTracks_[row],view,block,sink,endSample);
    }
    if (frame) {
        frame->header = {block.incarnation,0,endSample,endTick,trackCount_,playing,back};
        finishStatusBuffer(index);
    }
}

void Launcher::suspendStatus() noexcept {
    const auto current = publishedStatus_.load(std::memory_order_relaxed);
    if (current >= statusBufferCount || !statusBuffers_[current].header.transportPlaying) return;
    const auto index = claimStatusBuffer();
    if (index >= statusBufferCount) return;
    const auto& source = statusBuffers_[current];
    auto& target = statusBuffers_[index];
    std::copy_n(source.rows.get(),source.header.trackCount,target.rows.get());
    target.header = source.header;
    target.header.transportPlaying = false;
    finishStatusBuffer(index);
}

bool Launcher::anyRunningClip(const void* context,
    bool (*matches)(const void*, std::int64_t track, std::int64_t clip, std::uint64_t run) noexcept) const noexcept {
    for (std::size_t index = 0; index < trackCount_; ++index) {
        const auto& track = activeTracks_[index];
        if (track.run.clip && matches(context,track.id,track.run.clip,track.run.serial)) return true;
    }
    return false;
}

bool Launcher::readStatus(std::span<LaunchTrackStatus> out, LaunchStatusHeader& header) const noexcept {
    constexpr auto attempts = 4;
    for (int attempt = 0; attempt < attempts; ++attempt) {
        const auto index = publishedStatus_.load(std::memory_order_acquire);
        if (index >= statusBufferCount) return false;
        const auto& frame = statusBuffers_[index];
        auto readers = frame.readers.load(std::memory_order_relaxed);
        if (readers >= bufferWriting - 1 || !frame.readers.compare_exchange_strong(readers,readers + 1,
                std::memory_order_acq_rel,std::memory_order_relaxed)) continue;
        if (publishedStatus_.load(std::memory_order_acquire) != index) {
            frame.readers.fetch_sub(1,std::memory_order_release);
            continue;
        }
        header = frame.header;
        const auto fits = out.size() >= header.trackCount;
        if (fits) std::copy_n(frame.rows.get(),header.trackCount,out.data());
        frame.readers.fetch_sub(1,std::memory_order_release);
        return fits;
    }
    return false;
}

} // namespace adi::engine
