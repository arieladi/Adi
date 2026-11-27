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

std::int64_t gridForPress(const LaunchBlock& block, LaunchQuantization grid,
                         std::int64_t now) noexcept {
    if (grid == LaunchQuantization::None) return now;
    // A tick may project to the current sample even when sample->tick is
    // already past the grid. Start the search just after the previous sample.
    const auto lowerTick = block.sample == 0 ? 0 : add(atSample(block, block.sample - 1), 1);
    auto target = launch_quantize::nextGridTick(lowerTick, grid, block.meters).value_or(lastTick);
    if (target != lastTick && block.sampleAtTick(block.clock, target) < block.sample)
        target = launch_quantize::nextGridTick(add(target,1), grid, block.meters).value_or(lastTick);
    return target;
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

std::int64_t wrapSource(const LaunchSlot& slot, std::int64_t position) noexcept {
    if (loops(slot) && position >= slot.loopEndTick)
        return slot.loopStartTick + (position - slot.loopEndTick) % (slot.loopEndTick - slot.loopStartTick);
    return position;
}

float launchVelocity(const LaunchSlot& slot, float velocity) noexcept {
    const auto amount = std::isfinite(slot.velocityAmount)
        ? std::clamp(slot.velocityAmount, 0.0f, 1.0f) : 0.0f;
    return 1.0f - amount * (1.0f - std::clamp(velocity,0.0f,1.0f));
}
} // namespace

Launcher::Launcher(std::size_t maximumTracks)
    : tracks_(maximumTracks), scratch_(maximumTracks),
      activeTracks_(tracks_.data()), spareTracks_(scratch_.data()) {}

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
    }
    if (view.tracks.size() == trackCount_ &&
        std::equal(view.tracks.begin(), view.tracks.end(), activeTracks_,
                   [](auto id, const Track& track) { return id == track.id; })) return;

    std::size_t old = 0, next = 0;
    const auto stopRemovedTrack = [&](const Track& track) {
        if (track.run.clip != 0 && sink.transition) {
            sink.transition(sink.context, {track.id, track.run.clip, 0, 0, block.sample,
                atSample(block,block.sample), 0, false, track.run.serial, 0});
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
    }
    return false;
}

void Launcher::applyUser(const LaunchRequest& request, const LaunchBlock& block,
                         const LaunchView& view, std::int64_t now) noexcept {
    switch (request.kind) {
    case LaunchRequestKind::CancelScene:
        for (auto& track : std::span(activeTracks_,trackCount_)) {
            for (auto* pending : {&track.user,&track.follow}) {
                if (*pending && (*pending)->sceneGesture && (*pending)->scene == *request.scene)
                    pending->reset();
            }
        }
        return;
    case LaunchRequestKind::StopAll: {
        const auto grid = stopAllUsesGlobalLaunchQuantization ? block.global : LaunchQuantization::None;
        const Pending stop{Action::Stop,gridForPress(block,grid,now),0,0,1.0f,grid};
        for (auto& track : std::span(activeTracks_,trackCount_)) track.user = stop;
        return;
    }
    case LaunchRequestKind::BackToArrangement: {
        const auto grid = backToArrangementIsImmediate ? LaunchQuantization::None : block.global;
        const Pending back{Action::Arrangement,gridForPress(block,grid,now),0,0,1.0f,grid};
        if (request.track) findTrack(*request.track)->user = back;
        else for (auto& track : std::span(activeTracks_,trackCount_)) track.user = back;
        return;
    }
    case LaunchRequestKind::StopTrack:
        findTrack(*request.track)->user = Pending{Action::Stop,gridForPress(block,block.global,now),
                                                0,0,1.0f,block.global};
        return;
    case LaunchRequestKind::ScenePress: {
        const auto sceneTick = gridForPress(block,block.global,now);
        for (auto& track : std::span(activeTracks_,trackCount_)) {
            const auto* slot = findSlot(view,track.id,*request.scene);
            if (occupied(slot)) {
                const auto grid = scenesUseGlobalLaunchQuantization
                    ? block.global : slot->quantization.value_or(block.global);
                const auto tick = grid == block.global ? sceneTick : gridForPress(block,grid,now);
                track.user = Pending{Action::Start,tick,slot->scene,slot->clip,request.velocity,grid,false,0,true};
            } else if (!slot || slot->hasStop) {
                track.user = Pending{Action::Stop,sceneTick,*request.scene,0,1.0f,block.global,false,0,true};
            }
        }
        return;
    }
    case LaunchRequestKind::SlotPress:
    case LaunchRequestKind::SlotRelease:
        break;
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
                          decision.repeat == LaunchRepeat::Hold};
    if (edge == LaunchEdge::Release && slot.mode == LaunchMode::Gate) track.gateStop = pending;
    else track.user = pending;
}

void Launcher::applyFollow(const LaunchFollowRequest& follow, const LaunchBlock& block,
                           const LaunchView& view, std::int64_t now) noexcept {
    const auto& request = follow.request;
    if (!block.followActionsEnabled || follow.tick < 0 || !valid(request,block,view)) return;
    const auto due = std::max(follow.tick,now);
    const auto followGrid = followBypassesGlobalLaunchQuantization ? LaunchQuantization::None : block.global;
    if (request.kind == LaunchRequestKind::SlotPress) {
        const auto& slot = *addressedSlot(view,request);
        const auto grid = slot.quantization.value_or(followGrid);
        const auto tick = launch_quantize::nextGridTick(due,grid,block.meters).value_or(lastTick);
        findTrack(slot.track)->follow = Pending{Action::Start,tick,slot.scene,slot.clip,request.velocity,grid};
    }
    if (request.kind == LaunchRequestKind::StopTrack)
        findTrack(*request.track)->follow = Pending{Action::Stop,due};
    if (request.kind == LaunchRequestKind::ScenePress) {
        for (auto& track : std::span(activeTracks_,trackCount_)) {
            const auto* slot = findSlot(view,track.id,*request.scene);
            if (occupied(slot)) {
                track.follow = Pending{Action::Start,due,slot->scene,slot->clip,request.velocity,
                                      LaunchQuantization::None,false,0,true};
            } else if (!slot || slot->hasStop) {
                track.follow = Pending{Action::Stop,due,*request.scene,0,1.0f,
                                      LaunchQuantization::None,false,0,true};
            }
        }
    }
}

void Launcher::transition(Track& track, const LaunchBlock& block, const LaunchView& view,
                          LaunchSink sink, const Pending& pending, std::int32_t offset,
                          std::int64_t tick) noexcept {
    const auto previous = track.run;
    const auto* target = pending.clip ? findSlot(view,track.id,pending.scene) : nullptr;
    if (pending.sceneGesture && !std::binary_search(view.scenes.begin(),view.scenes.end(),pending.scene)) return;
    if (pending.action == Action::Start) {
        if (!occupied(target) || target->clip != pending.clip) return;
    } else if (pending.clip != 0 && previous.clip != pending.clip) {
        return;
    } else if (pending.sceneGesture) {
        const auto* slot = findSlot(view,track.id,pending.scene);
        if (slot && (occupied(slot) || !slot->hasStop)) return;
    }

    track.follow.reset();
    track.repeat.reset();
    track.run = {};
    track.run.arrangement = pending.action == Action::Arrangement;
    if (pending.action == Action::Start) {
        auto position = target->startTick;
        const auto* oldSlot = findClip(view,track.id,previous.clip);
        if (oldSlot && target->legato && previous.clip != target->clip) {
            const auto oldPosition = wrapSource(*oldSlot,previous.sourceTick);
            position = legatoUsesPreviousClipRelativePosition
                ? add(target->startTick,std::max<std::int64_t>(0,oldPosition - oldSlot->startTick)) : oldPosition;
        }
        position = wrapSource(*target,position);
        if (position >= sourceEnd(*target)) position = target->startTick;
        track.run = {target->scene,target->clip,position,0,launchVelocity(*target,pending.velocity),
                     ++runSerial_,false,tick,position,position,0};
        if (pending.repeat) {
            const auto next = pending.grid == LaunchQuantization::None && repeatWithoutQuantizationRestartsAtClipEnd
                ? add(tick,sourceEnd(*target) - position)
                : launch_quantize::nextGridTick(add(tick,1),pending.grid,block.meters).value_or(lastTick);
            if (next > tick) {
                track.repeat = pending;
                track.repeat->tick = next;
                track.repeat->earliestSample = block.sample + offset + 1;
            }
        }
    }
    if (previous.arrangement == track.run.arrangement && previous.serial == track.run.serial) return;
    const auto followOrdinal = track.run.clip != 0 ? ++track.followOrdinal : 0;
    const LaunchTransition edge{track.id,previous.clip,track.run.clip,track.run.sourceTick,
        block.sample + offset,tick,offset,track.run.arrangement,previous.serial,track.run.serial,followOrdinal,
        track.run.velocity,track.run.clip != 0 && target && target->fade};
    if (sink.transition) sink.transition(sink.context,edge);
    if (block.followActionsEnabled && sink.followOnLaunch && edge.newClip != 0) {
        const auto follow = sink.followOnLaunch(sink.context,edge);
        if (follow && follow->tick > tick && follow->request.track == track.id &&
            (follow->request.kind == LaunchRequestKind::StopTrack ||
             follow->request.kind == LaunchRequestKind::SlotPress)) {
            applyFollow(*follow,block,view,tick);
            if (track.follow) track.follow->earliestSample = edge.sample + 1;
        }
    }
}

void Launcher::render(Track& track, const LaunchBlock& block, const LaunchView& view,
                      LaunchSink sink, std::int64_t tick) noexcept {
    std::int32_t cursor = 0;
    auto phaseChangedAtCursor = false;
    while (cursor < block.frames) {
        const LaunchSlot* slot = nullptr;
        if (track.run.clip != 0) {
            slot = findSlot(view,track.id,track.run.scene);
            if (!slot || slot->clip != track.run.clip) slot = findClip(view,track.id,track.run.clip);
            if (slot) track.run.scene = slot->scene;
        }
        auto sourceOffset = block.frames;
        auto sourceTick = lastTick;
        if (track.run.clip != 0 && !occupied(slot)) {
            sourceOffset = cursor;
            sourceTick = tick;
        } else if (slot) {
            const auto untilEnd = std::max<std::int64_t>(0,sourceEnd(*slot) - track.run.anchorSourceTick);
            sourceTick = add(track.run.anchorTick,untilEnd);
            sourceOffset = std::max(cursor,offsetAt(block,sourceTick));
            if (sourceOffset == cursor && untilEnd > 0 && phaseChangedAtCursor) ++sourceOffset;
        }
        std::optional<Pending>* next = nullptr;
        auto boundary = block.frames;
        for (auto* candidate : {&track.user,&track.gateStop,&track.follow,&track.repeat}) {
            if (!*candidate) continue;
            const auto earliest = static_cast<std::int32_t>(std::clamp<std::int64_t>(
                (*candidate)->earliestSample - block.sample, 0, block.frames));
            const auto offset = std::max({cursor,earliest,offsetAt(block,(*candidate)->tick)});
            if (offset < boundary) {
                boundary = offset;
                next = candidate;
            }
        }
        if (sourceOffset < boundary) {
            boundary = sourceOffset;
            next = nullptr;
        }
        auto edgeTick = next ? (*next)->tick : sourceTick;
        if (offsetAt(block,edgeTick) < boundary)
            edgeTick = atSample(block,block.sample + boundary);
        const auto nextTick = boundary < block.frames ? edgeTick : atSample(block,block.sample + boundary);
        const auto atSourceEdge = slot && sourceOffset == boundary &&
            (boundary < block.frames || (sourceTick != lastTick &&
             block.sampleAtTick(block.clock,sourceTick) == block.sample + boundary));
        if (boundary > cursor) {
            auto endSource = track.run.sourceTick;
            if (slot) {
                endSource = atSourceEdge ? sourceEnd(*slot) : std::min(sourceEnd(*slot),
                    add(track.run.anchorSourceTick,std::max<std::int64_t>(0,nextTick - track.run.anchorTick)));
            }
            if (sink.section) sink.section(sink.context, {track.id,track.run.clip,track.run.sourceTick,
                endSource,cursor,boundary-cursor,track.run.velocity,track.run.arrangement,
                slot && slot->fade,track.run.serial,track.run.frames,tick,
                track.run.pcmAnchorTick,track.run.frames-track.run.pcmAnchorFrame});
            track.run.sourceTick = endSource;
            track.run.frames = add(track.run.frames,boundary-cursor);
            cursor = boundary;
            phaseChangedAtCursor = false;
        }
        if (cursor == block.frames) break;
        if (next) {
            const auto pending = **next;
            next->reset();
            if (atSourceEdge) track.run.sourceTick = sourceEnd(*slot);
            const auto run = track.run.serial;
            transition(track,block,view,sink,pending,cursor,edgeTick);
            phaseChangedAtCursor = phaseChangedAtCursor || run != track.run.serial;
        } else if (occupied(slot) && loops(*slot)) {
            track.run.sourceTick = track.run.anchorSourceTick = slot->loopStartTick;
            track.run.anchorTick = nextTick;
            track.run.pcmAnchorTick = slot->loopStartTick;
            track.run.pcmAnchorFrame = track.run.frames;
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

bool Launcher::process(const LaunchBlock& block, const LaunchView& view, LaunchSink sink,
                       std::span<const LaunchFollowRequest> follow) noexcept {
    if (!block.tickAtSample || !block.sampleAtTick || block.frames <= 0 || block.sample < 0 ||
        block.sample > lastTick - block.frames || view.tracks.size() > tracks_.size()) return false;
    adopt(block,view,sink);
    const auto now = atSample(block,block.sample);
    const auto active = std::span(activeTracks_,trackCount_);
    if (followPass_ != block.followPass) {
        for (auto& track : active) {
            track.followOrdinal = 0;
            track.follow.reset();
        }
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
    }
    if (nextSample_ >= 0 && block.sample != nextSample_) {
        // The caller split a transport discontinuity. Re-grid user launches
        // on the new timeline and preserve the elapsed-time countdowns.
        for (auto& track : active) {
            for (auto* pending : {&track.user,&track.gateStop}) {
                if (*pending) (*pending)->tick = gridForPress(block,(*pending)->grid,now);
            }
            for (auto* pending : {&track.follow,&track.repeat}) {
                if (*pending) {
                    (*pending)->tick = add(now,std::max<std::int64_t>(0,(*pending)->tick-nextTick_));
                    (*pending)->earliestSample = block.sample;
                }
            }
        }
    }
    for (std::size_t remaining = LaunchGestureQueue::capacity; remaining > 0;) {
        const auto result = gestures_.pop(batch_);
        if (result.status != LaunchGestureQueue::PopStatus::Ready) break;
        const auto gesture = std::span(batch_).first(result.size);
        if (std::all_of(gesture.begin(),gesture.end(),
                        [&](const auto& request) { return valid(request,block,view); })) {
            for (const auto& request : gesture) applyUser(request,block,view,now);
        }
        // A producer may refill the ring while it is drained. Bound the
        // callback's admission work even under continuous owner activity.
        if (result.size >= remaining) break;
        remaining -= result.size;
    }
    for (const auto& due : follow) applyFollow(due,block,view,now);
    for (auto& track : active) render(track,block,view,sink,now);
    nextSample_ = block.sample + block.frames;
    nextTick_ = atSample(block,nextSample_);
    return true;
}

} // namespace adi::engine
