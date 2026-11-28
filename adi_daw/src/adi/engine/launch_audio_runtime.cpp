// SPDX-License-Identifier: AGPL-3.0-or-later
#include "adi/engine/launch_audio_runtime.hpp"

#include "adi/engine/graph.hpp"
#include "adi/engine/launch_capture.hpp"
#include "adi/engine/launch_follow_playback.hpp"
#include "adi/engine/launch_paged_audio.hpp"
#include "adi/engine/launch_quantize.hpp"
#include "adi/engine/launch_warp_source.hpp"
#include "adi/engine/transport.hpp"
#include "adi/textproj.hpp"

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

bool needsCueLease(const LaunchSlot& slot, std::uint32_t rate) noexcept {
    const auto start = slot.physicalLoopEndFrame
        ? std::min(slot.physicalStartFrame,slot.physicalLoopBeginFrame) : slot.physicalStartFrame;
    return slot.physicalStartFrame + slot.physicalFrames - start > 3 * ClipPlayback::readAheadFrames(rate);
}

bool nativeCueClockStable(const LaunchBlock& block, const LaunchFollowPlayer* follow,
                          std::uint64_t performedScene, std::int64_t clip) noexcept {
    return (!block.preparedScene || block.preparedScene->token <= performedScene) &&
        (!block.followActionsEnabled || !follow || !follow->hasClipOrSceneActions(clip));
}

bool nativeLinkedTarget(const LaunchSlot& slot, const LaunchWarpSource& pages,
                        LaunchCueAdmission admission) noexcept {
    if (slot.mode == LaunchMode::Repeat) return false;
    const auto looping = slot.loopEndTick > slot.loopStartTick;
    if (admission == LaunchCueAdmission::Linked) return !looping || !slot.fade;
    return admission == LaunchCueAdmission::MusicalLinked && !looping && pages.player().hasConstantClock();
}

bool needsNativeMusicalCueLease(const LaunchAudioClip& clip, LaunchView view,
                                const LaunchRequest& request) noexcept {
    const auto* pages = clip.warpPageReader();
    if (!pages || pages->wholeSourceFitsPageCache() || !request.track || !request.scene || view.slots.empty())
        return false;
    const auto key = std::pair{*request.track,*request.scene};
    const auto* first = view.slots.data();
    const auto* end = first + view.slots.size();
    const auto* slot = std::lower_bound(first,end,key,[](const LaunchSlot& item,const auto& address) noexcept {
        return std::pair{item.track,item.scene} < address;
    });
    return slot != end && std::pair{slot->track,slot->scene} == key && slot->clip == clip.clip() &&
        nativeLinkedTarget(*slot,*pages,LaunchCueAdmission::MusicalLinked);
}

struct NativeCueWraps {
    std::array<LaunchWarpPosition,kMaximumLaunchWarpCueWraps> positions;
    std::size_t count = 0;
    bool bounded = true;

    [[nodiscard]] std::span<const LaunchWarpPosition> span() const noexcept {
        return {positions.data(),count};
    }
};

NativeCueWraps nativeCueWraps(const LaunchBlock& block, const LaunchSlot& slot,
                             LaunchWarpPosition cue, std::int32_t frames) noexcept {
    NativeCueWraps wraps{};
    if (slot.loopEndTick <= slot.loopStartTick) return wraps;
    constexpr auto limit = std::numeric_limits<std::int64_t>::max();
    if (cue.frame < 0 || frames <= 0 || cue.frame > limit - frames || cue.anchorTick < 0 ||
        cue.anchorSourceTick < 0 || cue.anchorSourceTick >= slot.loopEndTick) {
        wraps.bounded = false;
        return wraps;
    }
    const auto end = cue.frame + frames;
    const auto lapTicks = slot.loopEndTick - slot.loopStartTick;
    auto tick = cue.anchorTick;
    auto previous = cue.frame;
    auto ticksToWrap = slot.loopEndTick - cue.anchorSourceTick;
    if (tick > limit - ticksToWrap) {
        wraps.bounded = false;
        return wraps;
    }
    while (tick <= limit - ticksToWrap) {
        tick += ticksToWrap;
        const auto first = block.sampleAtTick(block.clock,tick);
        if (first >= end) break;
        // The Launcher can stretch a same-sample wrap to one frame. Leave
        // those cases and multi-lap fade tails on the ordinary reader path.
        if (first <= previous || wraps.count == wraps.positions.size() || (wraps.count != 0 && slot.fade)) {
            wraps.bounded = false;
            break;
        }
        wraps.positions[wraps.count++] = {first,tick,slot.loopStartTick};
        previous = first;
        ticksToWrap = lapTicks;
    }
    return wraps;
}

NativeCueWraps nativeCueRepeat(const LaunchBlock& block, const LaunchSlot& slot, LaunchWarpPosition position,
                              std::int32_t frames, const LaunchSourceCue& cue, bool scheduled) noexcept {
    NativeCueWraps wraps{};
    wraps.bounded = false;
    constexpr auto limit = std::numeric_limits<std::int64_t>::max();
    if (!cue.repeat || cue.repeat->tick <= position.anchorTick || frames <= 0 ||
        position.frame < 0 || position.frame > limit - frames) return wraps;
    const auto& restart = *cue.repeat;
    const auto end = position.frame + frames;
    const auto first = block.sampleAtTick(block.clock,restart.tick);
    if (first <= position.frame) return wraps;

    const auto looping = slot.loopEndTick > slot.loopStartTick;
    const auto sourceEnd = looping ? slot.loopEndTick : slot.endTick;
    if (restart.sourceTick < 0 || restart.sourceTick >= sourceEnd) return wraps;
    // A later reset leaves this callback playing the inherited placement,
    // including ordinary wraps. Its saved Start still uses native read-ahead.
    if (first >= end) return nativeCueWraps(block,slot,position,frames);
    const auto lapTicks = sourceEnd - restart.sourceTick;
    if (scheduled) {
        if (position.anchorTick < 0 || position.anchorSourceTick < 0 || position.anchorSourceTick >= sourceEnd)
            return wraps;
        const auto ticksUntilEnd = sourceEnd - position.anchorSourceTick;
        if (position.anchorTick > limit - ticksUntilEnd) return wraps;
        const auto inheritedEnd = block.sampleAtTick(block.clock,position.anchorTick + ticksUntilEnd);
        const auto second = restart.followingTick == limit ? limit : block.sampleAtTick(block.clock,restart.followingTick);
        if (!slot.fade && first <= inheritedEnd) {
            // Use a fresh saved-Start placement at every grid reset. A
            // run spanning an ordinary source wrap needs a different plan.
            wraps.positions[wraps.count++] = {first,restart.tick,restart.sourceTick};
            auto next = LaunchWarpPosition{second,restart.followingTick,restart.sourceTick};
            while (next.frame < end) {
                const auto& previous = wraps.positions[wraps.count-1];
                if (wraps.count == wraps.positions.size() || next.frame <= previous.frame ||
                    previous.anchorTick > limit - lapTicks || next.anchorTick == limit ||
                    next.frame > block.sampleAtTick(block.clock,previous.anchorTick + lapTicks))
                    return wraps;
                wraps.positions[wraps.count++] = next;
                const auto following = launch_quantize::nextGridTick(next.anchorTick+1,restart.grid,block.meters);
                if (!following || *following <= next.anchorTick) return wraps;
                next.anchorTick = *following;
                next.frame = block.sampleAtTick(block.clock,*following);
            }
            const auto lastReset = wraps.positions[wraps.count-1].anchorTick;
            wraps.bounded = lastReset <= limit - lapTicks &&
                block.sampleAtTick(block.clock,lastReset + lapTicks) >= end;
            return wraps;
        }
        if (second < end) return wraps;
        if (first > inheritedEnd) {
            // The frame at the retrigger belongs to saved Start. Project
            // only earlier ordinary wraps, with a single unfaded lap allowed.
            if (!looping || slot.fade) return wraps;
            const auto beforeRestart = static_cast<std::int32_t>(first - position.frame);
            const auto ordinary = nativeCueWraps(block,slot,position,beforeRestart);
            if (!ordinary.bounded || ordinary.count != 1) return wraps;
            wraps.positions[wraps.count++] = ordinary.positions[0];
        }
    }
    if (restart.tick > limit - lapTicks || block.sampleAtTick(block.clock,restart.tick + lapTicks) < end)
        return wraps;

    // The Launcher uses a new clock anchor for saved Start, even on an End tie.
    wraps.positions[wraps.count++] = {first,restart.tick,restart.sourceTick};
    wraps.bounded = true;
    return wraps;
}

bool needsWorkerCueLease(const LaunchAudioClip& clip) noexcept {
    return clip.latencySamples() > 0 && !clip.allWorkerPhasesPrimed();
}

bool needsRetainedPages(const LaunchSlot& slot, std::uint32_t rate) noexcept {
    return slot.physicalLoopEndFrame > 0 || slot.physicalFrames > 3 * ClipPlayback::readAheadFrames(rate);
}

bool samePinnedShape(const LaunchSlot& a, const LaunchSlot& b) noexcept {
    return a.track == b.track && a.scene == b.scene && a.clip == b.clip &&
        a.physicalStartFrame == b.physicalStartFrame && a.physicalFrames == b.physicalFrames &&
        a.physicalLoopBeginFrame == b.physicalLoopBeginFrame && a.physicalLoopEndFrame == b.physicalLoopEndFrame &&
        a.fade == b.fade && a.mode == b.mode &&
        (a.physicalFrames > 0 || (a.startTick == b.startTick && a.endTick == b.endTick));
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
    if (!stored_ && std::ranges::any_of(clips_, [](const LaunchAudioClip& clip) { return clip.usesPages(); }))
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

std::uint64_t LaunchAudioProject::residentBytes() const noexcept {
    std::uint64_t bytes = 0;
    for (const auto& clip : clips_) bytes += clip.bytes();
    return bytes;
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
        driver.gains_.post(edit, gainRevision());
}

bool LaunchAudioRuntime::postRegionEdit(LaunchAudioDriver& driver, const LaunchClipRegionEdit& edit,
                                       std::int64_t sample) const noexcept {
    if (edit.incarnation != project_->incarnation() || !hasTrack(edit.track)) return false;
    const auto& prepared = *mediaAt(sample);
    const auto* pcm = prepared.clip(edit.clip);
    if (pcm) {
        const auto last = std::max(edit.region.end, edit.region.loop ? edit.region.loop->end : std::int64_t{0});
        const auto lastFrame = pcm->frameForTick(last, prepared.bpm());
        const auto aperture = pcm->legacyAperture();
        const auto endFrame = aperture && !aperture->followProjectEnd
            ? static_cast<std::uint64_t>(aperture->preparedEndFrame) : pcm->frames();
        if (pcm->usesPages() || lastFrame < 0 || static_cast<std::uint64_t>(lastFrame) > endFrame) return false;
    } else if (!prepared.isMidiTrack(edit.track)) return false;
    const auto slots = prepared.view().slots;
    const auto slot = std::ranges::find_if(slots,[&](const LaunchSlot& candidate) {
        return candidate.track == edit.track && candidate.clip == edit.clip;
    });
    return driver.launcher_.postRegionEdit(edit,slot != slots.end() ? &*slot : nullptr);
}

LaunchAudioRuntime::LaunchAudioRuntime(std::shared_ptr<const LaunchAudioProject> project,
                                       std::vector<std::int64_t> tracks, std::int32_t maxFrames,
                                       std::shared_ptr<const LaunchAudioProject> beforeClock,
                                       std::int64_t clockSample, std::uint64_t compatibleGainRevision,
                                       LaunchAudioDriver* owner)
    : project_(std::move(project)), beforeClock_(std::move(beforeClock)), clockSample_(clockSample),
      tracks_(std::move(tracks)), ranges_(tracks_.size()), maxFrames_(maxFrames),
      compatibleGainRevision_(compatibleGainRevision) {
    if (!project_ || maxFrames <= 0)
        throw std::invalid_argument("invalid Session launch audio runtime format");
    if (beforeClock_ && (beforeClock_->incarnation() != project_->incarnation() || clockSample_ <= 0))
        throw std::invalid_argument("invalid Session clock media handover");
    const auto known = project_->view().tracks;
    if (!positiveAscending(tracks_) || !std::includes(known.begin(), known.end(), tracks_.begin(), tracks_.end()))
        throw std::invalid_argument("Session audio runtime needs known tracks in ascending order");
    if (owner && project_->stored()) {
        owner->collectPinnedSourceOwners();
        const auto gather = [&](const LaunchAudioProject& bank, std::vector<Candidate>& into) {
            for (const auto& slot : bank.view().slots) {
                if (slot.clip == 0) continue;
                const auto* pcm = bank.clip(slot.clip);
                const bool legacyPaged = pcm && pcm->pageReader() && pcm->legacyRaw();
                if (slot.physicalFrames <= 0 && !legacyPaged) continue;
                into.push_back({slot, pcm && (legacyPaged || needsRetainedPages(slot,pcm->rate()))
                    ? owner->ownerFor(*pcm) : nullptr});
            }
        };
        gather(*project_,pinned_);
        if (beforeClock_) gather(*beforeClock_,beforePinned_);
    }
    // A one-frame loop can produce one range per frame per track. Reserve
    // that bound so a callback never drops or allocates a range.
    for (std::size_t index = 0; index < tracks_.size(); ++index) {
        auto& track = ranges_[index];
        if (project_->isMidiTrack(tracks_[index])) track.midi.resize(static_cast<std::size_t>(maxFrames));
        track.items.resize(static_cast<std::size_t>(maxFrames));
        track.tails.resize(static_cast<std::size_t>(maxFrames) + 2);
    }
}

const std::shared_ptr<const LaunchAudioProject>& LaunchAudioRuntime::mediaAt(std::int64_t sample) const noexcept {
    return beforeClock_ && sample < clockSample_ ? beforeClock_ : project_;
}

const LaunchAudioRuntime::Candidate* LaunchAudioRuntime::pinnedCandidate(std::int64_t sample, std::int64_t track,
                                                                        std::int64_t clip) const noexcept {
    if (!hasTrack(track)) return nullptr;
    const auto& bank = beforeClock_ && sample < clockSample_ ? beforePinned_ : pinned_;
    if (bank.empty()) return nullptr;
    const auto* end = bank.data() + bank.size();
    auto* item = std::lower_bound(bank.data(),end,track,[](const Candidate& candidate,std::int64_t id) {
        return candidate.slot.track < id;
    });
    for (; item != end && item->slot.track == track; ++item)
        if (item->slot.clip == clip) return item;
    return nullptr;
}

void LaunchAudioDriver::collectPinnedSourceOwners() noexcept {
    // Owner is the only thread that acquires/drops a catalog reference. A
    // driver needs a live catalog to acquire its first lease. Existing runs
    // and tails keep their raw leases through graph reclamation and rendering.
    std::erase_if(sourceOwners_, [](const auto& entry) {
        const auto& source = entry.second;
        return source.use_count() == 1 && source->leases.load(std::memory_order_acquire) == 0;
    });
}

std::shared_ptr<LaunchPinnedSource> LaunchAudioDriver::ownerFor(const LaunchAudioClip& clip) {
    const SourceKey key{clip.pageReader(),clip.clip()};
    if (!key.reader) return {};
    const auto position = sourceOwners_.lower_bound(key);
    if (position != sourceOwners_.end() && !(key < position->first)) return position->second;
    auto shared = clip.shareRawPages();
    if (!shared) return {};
    auto owner = std::make_shared<LaunchPinnedSource>(std::move(*shared));
    sourceOwners_.emplace_hint(position,key,owner);
    return owner;
}

LaunchAudioDriver::Retained* LaunchAudioDriver::findRetained(std::int64_t track, std::size_t count) noexcept {
    if (count == 0) return nullptr;
    auto* const end = retained_.data() + count;
    auto* item = std::lower_bound(retained_.data(),end,track,[](const Retained& entry,std::int64_t id) {
        return entry.track < id;
    });
    return item == end || item->track != track ? nullptr : item;
}

const LaunchAudioDriver::Retained* LaunchAudioDriver::retainedFor(std::int64_t track, std::int64_t clip,
                                                                std::uint64_t run) const noexcept {
    if (retainedCount_ == 0 || run == 0) return nullptr;
    const auto* end = retained_.data() + retainedCount_;
    const auto* item = std::lower_bound(retained_.data(),end,track,[](const Retained& entry,std::int64_t id) {
        return entry.track < id;
    });
    return item == end || item->track != track || item->clip != clip || item->run != run ? nullptr : item;
}

void LaunchAudioDriver::retire(LaunchPinnedSource* source) noexcept {
    // At most one retained run per track and two outstanding fades per track.
    // Unfinished ranges still point into these entries until finishRanges().
    if (source) retiring_.data()[retiringCount_++] = source;
}

void LaunchAudioDriver::retargetPinnedRun(const LaunchSlot& previous,const LaunchSlot& next,
                                           std::uint64_t run) noexcept {
    auto* retained = findRetained(previous.track,retainedCount_);
    if (!retained || retained->clip != previous.clip || retained->run != run) return;
    if (!samePinnedShape(retained->slot,previous)) {
        retireRun(previous.track,run);
        return;
    }
    // Launcher verified the copied slot's topology and phase. The old
    // reader keeps its prepared PCM identity until the live run ends.
    retained->clip = next.clip;
    retained->slot = next;
    gains_.retargetRun(next.track,previous.clip,next.clip,run);
}

void LaunchAudioDriver::retireRun(std::int64_t track, std::uint64_t run) noexcept {
    if (run == 0) return;
    auto* entry = findRetained(track,retainedCount_);
    if (!entry || entry->run != run) return;
    retire(entry->source);
    entry->run = 0;
    endedRetainedRun_ = true;
}

void LaunchAudioDriver::finishRanges() noexcept {
    while (retiringCount_ != 0)
        retiring_.data()[--retiringCount_]->leases.fetch_sub(1,std::memory_order_release);
    if (endedRetainedRun_ && retainedCount_ != 0) {
        auto* const start = retained_.data();
        const auto* end = std::remove_if(start,start + retainedCount_,[](const Retained& entry) {
            return entry.run == 0;
        });
        retainedCount_ = static_cast<std::size_t>(end - start);
    }
    endedRetainedRun_ = false;
}

void LaunchAudioDriver::suspend() noexcept {
    if (wasPlaying_) launcher_.suspendStatus();
    wasPlaying_ = false;
    for (std::size_t index = 0; index < tailCount_; ++index)
        retire(tails_.data()[index].pinned);
    tailCount_ = 0;
    finishRanges();
    nextTimelineSample_ = -1;
}

void LaunchAudioDriver::discardIncompatiblePinnedRuns(const LaunchAudioRuntime* after,
                                                       std::int64_t sample) noexcept {
    finishRanges();
    std::size_t retained = 0;
    for (std::size_t index = 0; index < retainedCount_; ++index) {
        const auto& entry = retained_.data()[index];
        const auto* next = after ? after->pinnedCandidate(sample,entry.track,entry.clip) : nullptr;
        if (next && after->incarnation() == entry.incarnation && samePinnedShape(entry.slot,next->slot))
            retained_.data()[retained++] = entry;
        else
            entry.source->leases.fetch_sub(1,std::memory_order_release);
    }
    retainedCount_ = retained;
    if (!after) {
        std::size_t activeTails = 0;
        for (std::size_t index = 0; index < tailCount_; ++index) {
            const auto& tail = tails_.data()[index];
            if (tail.pinned)
                tail.pinned->leases.fetch_sub(1,std::memory_order_release);
            else
                tails_.data()[activeTails++] = tail;
        }
        tailCount_ = activeTails;
    }
}

void LaunchAudioDriver::retainPinnedRuns(const LaunchAudioRuntime& before, const LaunchAudioRuntime& after,
                                         std::int64_t sample, bool preserveExisting) noexcept {
    struct Context {
        LaunchAudioDriver& driver;
        const LaunchAudioRuntime& before;
        const LaunchAudioRuntime& after;
        std::int64_t sample;
        std::size_t settledRuns;
        bool preserveExisting;
    };
    const Context context{*this,before,after,sample,retainedCount_,preserveExisting};
    (void)launcher_.anyRunningClip(&context,[](const void* opaque, std::int64_t track,
                                              std::int64_t clip, std::uint64_t run) noexcept {
        const auto& call = *static_cast<const Context*>(opaque);
        auto& driver = call.driver;
        if (driver.retainedCount_ == driver.retained_.size() || driver.findRetained(track,call.settledRuns) ||
            !call.before.dropsPinnedPages(&call.after,call.sample,track,clip,true) ||
            (!call.preserveExisting && !call.before.canRetainLegacyPages(&call.after,call.sample,track,clip)))
            return false;
        const auto* old = call.before.pinnedCandidate(call.sample,track,clip);
        const auto* next = call.after.pinnedCandidate(call.sample,track,clip);
        if (!old || !old->source || !next || !samePinnedShape(old->slot,next->slot)) return false;
        auto* source = old->source.get();
        source->leases.fetch_add(1,std::memory_order_relaxed);
        driver.retained_.data()[driver.retainedCount_++] = {track,clip,run,call.before.incarnation(),call.before.gainRevision(),source,old->slot};
        return false;
    });
    if (retainedCount_ > context.settledRuns)
        std::sort(retained_.data(),retained_.data() + retainedCount_,[](const Retained& a,const Retained& b) {
            return a.track < b.track;
        });
}

bool LaunchAudioRuntime::dropsWorkerWarp(const LaunchAudioRuntime* after, std::int64_t sample,
                                        std::int64_t track, std::int64_t clip) const noexcept {
    const auto* source = hasTrack(track) ? mediaAt(sample)->clip(clip) : nullptr;
    return source && source->latencySamples() > 0 &&
        (!after || !after->hasTrack(track) || !after->mediaAt(sample)->clip(clip));
}

bool LaunchAudioRuntime::dropsResidentRaw(const LaunchAudioRuntime* after, std::int64_t sample,
                                         std::int64_t track, std::int64_t clip) const noexcept {
    if (!hasTrack(track)) return false;
    const auto* source = mediaAt(sample)->clip(clip);
    if (!source || !source->residentRawPcm()) return false;
    const auto* next = after && after->incarnation() == incarnation() && after->hasTrack(track)
        ? after->mediaAt(sample)->clip(clip) : nullptr;
    return !next || next->residentRawPcm() != source->residentRawPcm();
}

bool LaunchAudioRuntime::canRetainLegacyPages(const LaunchAudioRuntime* after, std::int64_t sample,
                                              std::int64_t track, std::int64_t clip) const noexcept {
    if (!after || after->incarnation() != incarnation() || !hasTrack(track) || !after->hasTrack(track)) return false;
    const auto* source = mediaAt(sample)->clip(clip);
    const auto* next = after->mediaAt(sample)->clip(clip);
    if (!source || !next || !source->legacyPageSource() || source->legacyPageSource() != next->legacyPageSource())
        return false;
    const auto* from = pinnedCandidate(sample,track,clip);
    const auto* to = after->pinnedCandidate(sample,track,clip);
    return from && from->source && to && to->source && samePinnedShape(from->slot,to->slot);
}

bool LaunchAudioRuntime::dropsPinnedPages(const LaunchAudioRuntime* after, std::int64_t sample,
                                         std::int64_t track, std::int64_t clip, bool retainedOnly) const noexcept {
    if (!hasTrack(track)) return false;
    const auto& bank = *mediaAt(sample);
    const auto* source = bank.clip(clip);
    if (!source || !source->pageReader() || source->latencySamples() != 0) return false;
    const auto slots = bank.view().slots;
    if (slots.empty()) return false;
    const auto* end = slots.data() + slots.size();
    auto* slot = std::lower_bound(slots.data(),end,track,
        [](const LaunchSlot& item,std::int64_t id) { return item.track < id; });
    for (; slot != end && slot->track == track; ++slot) {
        if (slot->clip != clip ||
            (!source->legacyRaw() && !needsRetainedPages(*slot,source->rate()))) continue;
        const auto* nextBank = after && after->hasTrack(track) ? after->mediaAt(sample).get() : nullptr;
        if (retainedOnly) {
            // An ordinary refresh may deliberately remove or move the slot.
            // Only wait for a source that is still declared on this track.
            if (!nextBank || nextBank->incarnation() != bank.incarnation()) return false;
            const auto nextSlots = nextBank->view().slots;
            if (nextSlots.empty()) return false;
            const auto* nextEnd = nextSlots.data() + nextSlots.size();
            const auto* retained = std::lower_bound(nextSlots.data(),nextEnd,track,
                [](const LaunchSlot& item,std::int64_t id) { return item.track < id; });
            while (retained != nextEnd && retained->track == track && retained->clip != clip) ++retained;
            if (retained == nextEnd || retained->track != track) return false;
        }
        const auto* next = nextBank ? nextBank->clip(clip) : nullptr;
        return !next || next->pageReader() != source->pageReader();
    }
    return false;
}

bool LaunchAudioDriver::dropsRunningProtectedAudio(const LaunchAudioRuntime& before, const LaunchAudioRuntime* after,
                                               std::int64_t sample, bool externalTempo, bool pinnedPages,
                                               bool residentRaw) const noexcept {
    struct Check {
        const LaunchAudioRuntime& before;
        const LaunchAudioRuntime* after;
        std::int64_t sample;
        bool externalTempo, pinnedPages, residentRaw;
    };
    const Check check{before,after,sample,externalTempo,pinnedPages,residentRaw};
    return launcher_.anyRunningClip(&check,[](const void* opaque, std::int64_t track, std::int64_t clip,
                                              std::uint64_t) noexcept {
        const auto& query = *static_cast<const Check*>(opaque);
        return (query.externalTempo && query.before.dropsWorkerWarp(query.after,query.sample,track,clip)) ||
            (query.residentRaw && query.before.dropsResidentRaw(query.after,query.sample,track,clip)) ||
            (query.pinnedPages && query.before.dropsPinnedPages(query.after,query.sample,track,clip,!query.externalTempo) &&
             !query.before.canRetainLegacyPages(query.after,query.sample,track,clip));
    });
}

std::uint64_t LaunchAudioRuntime::gainRevision() const noexcept {
    if (compatibleGainRevision_ != 0) return compatibleGainRevision_;
    // The before-clock bank still governs the gain of the outgoing run.
    return beforeClock_ ? beforeClock_->pcmRevision() : project_->pcmRevision();
}

void LaunchAudioRuntime::startIfQueued(LaunchAudioDriver& driver, LaunchBlock block, Transport& transport) const noexcept {
    if (!project_->stored() || transport.playing() || transport.countInRemaining() != 0) return;
    block.sample = transport.sampleAt(0);
    block.incarnation = project_->incarnation();
    block.followPass = driver.pass_;
    auto view = mediaAt(block.sample)->view();
    view.tracks = tracks_;
    LaunchSink sink;
    sink.context = &driver;
    sink.transition = [](void* state, const LaunchTransition& edge) noexcept {
        static_cast<LaunchAudioDriver*>(state)->retireRun(edge.track,edge.oldRun);
    };
    sink.retargeted = [](void* state, const LaunchSlot& previous, const LaunchSlot& next, std::uint64_t run) noexcept {
        static_cast<LaunchAudioDriver*>(state)->retargetPinnedRun(previous,next,run);
    };
    if ((block.preparedScene && block.preparedScene->token > driver.performedSceneToken()) ||
        driver.launcher_.admitWhileStopped(block,view,sink)) transport.play();
    driver.finishRanges();
}

LaunchAudioRuntime::TrackRanges* LaunchAudioRuntime::trackRanges(std::int64_t track) noexcept {
    if (tracks_.empty()) return nullptr;
    const auto* first = tracks_.data();
    const auto* end = first + tracks_.size();
    const auto* found = std::lower_bound(first,end,track);
    return found != end && *found == track ? ranges_.data() + (found - first) : nullptr;
}

bool LaunchAudioRuntime::reserveTimedRawScene(LaunchAudioDriver& driver, const LaunchAudioRuntime& outgoing,
        LaunchBlock previous, LaunchBlock candidate, const Transport& transport) noexcept {
    if (active_ || outgoing.active_ || !beforeClock_ || !project_->stored() ||
        !supportsTimedRawSceneTrackCount(tracks_.size()) ||
        candidate.frames <= 0 || candidate.frames > maxFrames_ ||
        clockSample_ > std::numeric_limits<std::int64_t>::max() - candidate.frames ||
        !candidate.preparedScene || !transport.playing() || transport.looping() ||
        transport.position() != clockSample_ || driver.nextTimelineSample_ != clockSample_ ||
        driver.transportRevision_ != transport.revision() || outgoing.mediaAt(clockSample_ - 1) != beforeClock_)
        return false;

    previous.sample = candidate.sample = clockSample_;
    previous.incarnation = beforeClock_->incarnation();
    candidate.incarnation = project_->incarnation();
    previous.followPass = candidate.followPass = driver.pass_;
    auto candidateView = project_->view();
    candidateView.tracks = tracks_;
    if (!driver.launcher_.freezeTimedScenePrefix(previous,candidate.preparedScene->token)) return false;
    std::array<TimedSceneAction,kMaximumTimedRawSceneTracks> plan{};
    const auto count = driver.launcher_.planTimedSceneActions(candidate,candidateView,*candidate.preparedScene,plan);
    if (!count || *count != tracks_.size()) return false;
    const auto actions = std::span(plan).first(*count);
    const auto startCount = std::ranges::count(actions,TimedSceneActionKind::Start,&TimedSceneAction::kind);
    const auto inherited = std::ranges::count_if(actions,[](const auto& action) {
        return action.kind == TimedSceneActionKind::Start && action.outgoingRun != 0;
    });
    if (inherited == 0 || (static_cast<std::size_t>(startCount) != actions.size() &&
                          (actions.size() != 3 || startCount != 2 || inherited != 2)))
        return false;

    for (const auto& action : actions) {
        if (action.kind == TimedSceneActionKind::Stop) continue;
        const auto* pcm = project_->clip(action.clip);
        auto* reader = pcm ? pcm->pageReader() : nullptr;
        auto* ranges = trackRanges(action.track);
        if (action.kind == TimedSceneActionKind::Continue) {
            const auto* before = outgoing.pinnedCandidate(clockSample_-1,action.track,action.clip);
            const auto* after = pinnedCandidate(clockSample_,action.track,action.clip);
            const auto* oldPcm = beforeClock_->clip(action.clip);
            const auto* retained = driver.retainedFor(action.track,action.clip,action.outgoingRun);
            if (!reader || !before || !after || !oldPcm || oldPcm->pageReader() != reader ||
                !samePinnedShape(before->slot,after->slot) || outgoing.gainRevision() != gainRevision() ||
                (retained && (!samePinnedShape(retained->slot,before->slot) ||
                              retained->source->pcm.pageReader() != reader)) ||
                action.cue.physicalFrame < before->slot.physicalStartFrame ||
                action.cue.physicalFrame - before->slot.physicalStartFrame > before->slot.physicalFrames - candidate.frames) {
                finish();
                return false;
            }
        } else if ((action.outgoingRun == 0) != (action.outgoingClip == 0) || action.outgoingClip == action.clip) {
            finish();
            return false;
        }
        if (!reader || !ranges || ranges->cuePages || action.cue.physicalFrame < 0 ||
            !reader->reserveCue(action.clip,action.cue.physicalFrame,candidate.frames,false,false)) {
            finish();
            return false;
        }
        ranges->cuePages = reader;
        ranges->cueClip = action.clip;
        if (action.kind == TimedSceneActionKind::Start) ranges->timedCueFrame = action.cue.physicalFrame;
    }
    return true;
}

void LaunchAudioDriver::abandonSelectedScene(std::uint64_t token) noexcept {
    launcher_.abandonSelectedScene(token);
    if (token > resolvedScene_.load(std::memory_order_relaxed))
        resolvedScene_.store(token,std::memory_order_release);
}

void LaunchAudioRuntime::finish() noexcept {
    active_ = false;
    for (std::size_t index = 0; index < ranges_.size(); ++index) {
        auto& ranges = ranges_.data()[index];
        if (ranges.cuePages) {
            ranges.cuePages->releaseCue(ranges.cueClip);
            ranges.cuePages = nullptr;
            ranges.timedCueFrame = -1;
        }
        if (ranges.warpCuePages) {
            ranges.warpCuePages->releaseCue();
            ranges.warpCuePages = nullptr;
        }
    }
}

void LaunchAudioRuntime::begin(LaunchAudioDriver& driver, LaunchBlock block,
                               const Transport& transport, std::int64_t streamSample,
                               void* observerContext,
                               void (*observer)(void*, const LaunchTransition&, std::int64_t) noexcept,
                               void* sceneObserverContext,
                               void (*sceneObserver)(void*, const LaunchSceneTransition&, std::int64_t) noexcept,
                               LaunchCaptureBuffer* capture) noexcept {
    if (active_) finish();
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
        while (driver.retainedCount_ != 0) {
            const auto& retained = driver.retained_.data()[--driver.retainedCount_];
            if (retained.run != 0) driver.retire(retained.source);
        }
        for (std::size_t index = 0; index < driver.tailCount_; ++index)
            driver.retire(driver.tails_.data()[index].pinned);
        driver.tailCount_ = 0;
        driver.tailIncarnation_ = block.incarnation;
    }
    std::size_t remainingTails = 0;
    for (std::size_t index = 0; index < driver.tailCount_; ++index) {
        auto tail = driver.tails_.data()[index];
        if (addTail(tail, 0, block.frames))
            driver.tails_.data()[remainingTails++] = tail;
        else driver.retire(tail.pinned);
    }
    driver.tailCount_ = remainingTails;
    block.followPass = driver.pass_;
    const auto* follow = project_->followPlayer();
    block.followActionsEnabled &= follow != nullptr;
    struct Context {
        LaunchAudioRuntime& runtime;
        LaunchAudioDriver& driver;
        const LaunchAudioProject* media;
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
        LaunchCaptureBuffer* capture;
        bool arrangementLoop;
        std::int32_t offset = 0;
    };
    if (streamSample < 0 || streamSample > std::numeric_limits<std::int64_t>::max() - frames) {
        observer = nullptr;
        sceneObserver = nullptr;
    }
    Context context{*this, driver, project_.get(), {}, block, follow, block.incarnation,
        followPassSeed(project_->followSeed(), driver.pass_), streamSample, frames, observerContext, observer,
        sceneObserverContext, sceneObserver, capture, transport.looping()};
    const LaunchSink sink{&context, [](void* state, const LaunchSection& section) noexcept {
        auto& call = *static_cast<Context*>(state);
        call.runtime.add(call.driver, *call.media, section, call.block, call.offset, call.capture);
    }, [](void* state, const LaunchTransition& edge) noexcept {
        const auto& call = *static_cast<const Context*>(state);
        const auto& before = call.runtime.beforeClock_;
        const auto* outgoing = before && edge.sample == call.runtime.clockSample_ ? before.get() : call.media;
        call.runtime.fadeOut(call.driver, *outgoing, edge, call.offset + edge.offset, call.frames);
        call.driver.retireRun(edge.track,edge.oldRun);
        if (call.capture || call.observer) {
            auto logicalEdge = edge;
            logicalEdge.offset += call.offset;
            if (call.capture) (void)call.capture->append(logicalEdge);
            if (call.observer) call.observer(call.observerContext, logicalEdge, call.streamSample + logicalEdge.offset);
        }
    }, [](void* state, const LaunchTransition& edge) noexcept -> std::optional<LaunchFollowRequest> {
        const auto& call = *static_cast<const Context*>(state);
        if (!call.follow) return std::nullopt;
        auto next = call.follow->followOnLaunch(edge,call.view,call.incarnation,call.block.followActionsEnabled,call.seed);
        if (next && next->linked && next->physicalLaps == 0 && next->request.clip) {
            const auto* source = call.media->clip(edge.newClip);
            const auto* target = call.media->clip(*next->request.clip);
            // A fixed bank can forecast the wrapped musical phase. Keep the
            // original path when a prepared clock pair or Arrangement wrap
            // can rebase the pending deadline.
            if (!call.runtime.beforeClock_ && !call.arrangementLoop && source &&
                (source->followsTempo() || source->latencySamples() > 0) && target &&
                (needsWorkerCueLease(*target) || (needsNativeMusicalCueLease(*target,call.view,next->request) &&
                    nativeCueClockStable(call.block,call.follow,call.driver.performedSceneToken(),target->clip())))) {
                // Shorter laps can put two adjacent wraps on one sample.
                // The shared forecast and transition then keep the old path.
                const auto ticksPerSample = static_cast<long double>(call.media->bpm()) * textproj::kPPQ /
                    (60.0L * source->rate());
                next->minimumMusicalLapTicks = static_cast<std::int64_t>(std::ceil(ticksPerSample));
            }
        }
        return next;
    }, [](void* state, const LaunchTrackStatus& track) noexcept {
        const auto& call = *static_cast<const Context*>(state);
        if (!track.queued.legatoSourceTick) return;
        const auto due = call.block.sampleAtTick(call.block.clock, track.queued.tick);
        const auto& media = *call.runtime.mediaAt(due);
        if (&media != call.media) return;
        const auto* clip = media.clip(track.queued.clip);
        if (clip && clip->followsTempo()) {
            const LaunchWarpPosition cue{due,track.queued.tick,*track.queued.legatoSourceTick};
            NativeCueWraps wraps{};
            const auto slots = call.view.slots;
            const auto slot = std::ranges::lower_bound(slots,std::pair{track.track,track.queued.scene},{},
                [](const LaunchSlot& item) noexcept { return std::pair{item.track,item.scene}; });
            const auto* nativePages = clip->warpPageReader();
            if (nativePages && !nativePages->wholeSourceFitsPageCache() && slot != slots.end() &&
                slot->clip == track.queued.clip && slot->mode != LaunchMode::Repeat &&
                nativeCueClockStable(call.block,call.follow,call.driver.performedSceneToken(),slot->clip)) {
                wraps = nativeCueWraps(call.block,*slot,cue,call.runtime.maxFrames_);
            }
            clip->prefetchWarp(cue,wraps.bounded ? wraps.span() : std::span<const LaunchWarpPosition>{});
        } else if (clip && clip->pageReader()) {
            const auto* rendered = call.runtime.trackRanges(track.track);
            if (rendered && rendered->cuePages == clip->pageReader() && rendered->cueClip == clip->clip()) return;
            const auto frame = track.queued.legatoSourceFrame ? *track.queued.legatoSourceFrame
                : clip->frameForTick(*track.queued.legatoSourceTick,media.bpm());
            clip->pageReader()->prefetch(clip->clip(),frame);
        }
    }, [](void* state, const LaunchSceneTransition& edge) noexcept {
        const auto& call = *static_cast<const Context*>(state);
        if (edge.preparedToken != 0)
            call.driver.resolvedScene_.store(edge.preparedToken, std::memory_order_release);
        if (call.sceneObserver) {
            auto logicalEdge = edge;
            logicalEdge.offset += call.offset;
            const auto& prepared = call.block.preparedScene;
            if (prepared && edge.preparedToken == prepared->token) {
                logicalEdge.tempoBpm = prepared->tempoBpm;
                logicalEdge.signatureNumerator = prepared->signatureNumerator;
                logicalEdge.signatureDenominator = prepared->signatureDenominator;
            }
            call.sceneObserver(call.sceneObserverContext, logicalEdge, call.streamSample + logicalEdge.offset);
        }
    }, [](void* state, const LaunchSceneTransition& edge) noexcept -> std::optional<LaunchFollowRequest> {
        const auto& call = *static_cast<const Context*>(state);
        if (!call.follow) return std::nullopt;
        const auto next = call.follow->followOnSceneLaunch(edge.scene, edge.tick, edge.ordinal,
            call.incarnation, call.block.followActionsEnabled, call.seed);
        if (next && next->request.scene && call.runtime.sceneClock(*next->request.scene)) return std::nullopt;
        return next;
    }, [](void* state, const LaunchSlot& previous, const LaunchSlot& next, std::uint64_t run) noexcept {
        static_cast<Context*>(state)->driver.retargetPinnedRun(previous,next,run);
    }, [](void* state, const LaunchSlot& target, const LaunchSourceCue& cue, std::int64_t tick,
           std::int32_t offset, LaunchCueAdmission cueAdmission) noexcept {
        auto& call = *static_cast<Context*>(state);
        if (!call.media->stored()) return true;
        const auto physical = target.physicalFrames > 0;
        if (const auto* staged = call.runtime.trackRanges(target.track); staged && staged->timedCueFrame >= 0 &&
            call.block.preparedScene && call.block.sample == call.runtime.clockSample_ && offset == 0 &&
            physical && cueAdmission == LaunchCueAdmission::None)
            return staged->cuePages && staged->cueClip == target.clip && staged->timedCueFrame == cue.physicalFrame;
        const auto scheduled = cueAdmission == LaunchCueAdmission::Scheduled;
        const auto linked = cueAdmission == LaunchCueAdmission::Linked;
        const auto musicalLinked = cueAdmission == LaunchCueAdmission::MusicalLinked;
        const auto readyOnly = cueAdmission == LaunchCueAdmission::ReadyOnly || cueAdmission == LaunchCueAdmission::HeldRepeat;
        if (physical ? !needsCueLease(target,static_cast<std::uint32_t>(call.block.sampleRate))
                     : !scheduled && !linked && !musicalLinked && !readyOnly) return true;
        if (musicalLinked && !physical && (call.runtime.beforeClock_ || call.arrangementLoop)) return true;
        const auto* pcm = call.media->clip(target.clip);
        const auto nativeImmediate = cueAdmission == LaunchCueAdmission::ReadyOnly;
        const auto nativeRepeat = cueAdmission == LaunchCueAdmission::HeldRepeat ||
            (target.mode == LaunchMode::Repeat && scheduled && cue.repeat);
        auto* nativePages = pcm ? pcm->warpPageReader() : nullptr;
        const auto nativeLinked = nativePages && !call.runtime.beforeClock_ && !call.arrangementLoop &&
            nativeLinkedTarget(target,*nativePages,cueAdmission);
        if (nativePages && (scheduled || nativeImmediate || nativeRepeat || nativeLinked) &&
            (nativeRepeat ? target.mode == LaunchMode::Repeat
            : target.mode != LaunchMode::Repeat && (nativeImmediate || !nativePages->wholeSourceFitsPageCache()))) {
            // Arrangement wraps, prepared scenes and Follow Actions can
            // add another placement before this block's overlay. Their
            // launches keep the usual reader path.
            if (call.offset + call.block.frames != call.frames ||
                !nativeCueClockStable(call.block,call.follow,call.driver.performedSceneToken(),target.clip))
                return true;
            auto* rendered = call.runtime.trackRanges(target.track);
            if (!rendered || rendered->cuePages || rendered->warpCuePages) return true;
            const auto sameReader = [nativePages](const Range& range) noexcept {
                return range.pcm && range.pcm->warpPageReader() == nativePages;
            };
            for (std::size_t index = 0; index < rendered->size; ++index)
                if (sameReader(rendered->items.data()[index])) return true;
            for (std::size_t index = 0; index < rendered->tailCount; ++index)
                if (sameReader(rendered->tails.data()[index].range)) return true;
            const auto remaining = call.frames - call.offset - offset;
            const LaunchWarpPosition position{call.block.sample + offset,tick,cue.tick};
            // For Linked loops use the same full window as the forecast.
            // A shorter due slice could cross fewer wraps and calculate
            // a different union of source pages.
            const auto linkedLoop = nativeLinked && target.loopEndTick > target.loopStartTick;
            const auto cueFrames = linkedLoop ? call.runtime.maxFrames_ : remaining;
            const auto wraps = nativeRepeat ? nativeCueRepeat(call.block,target,position,cueFrames,cue,scheduled)
                                            : nativeCueWraps(call.block,target,position,cueFrames);
            if (!wraps.bounded) return true;
            if (!pcm->reserveWarpCue(position,cueFrames,wraps.span())) return false;
            rendered->warpCuePages = nativePages;
            return true;
        }
        if (!pcm) return !physical;
        auto* pages = pcm->pageReader();
        if (!pages || (!physical && !needsWorkerCueLease(*pcm))) return true;
        const auto frame = physical ? cue.physicalFrame : pcm->frameForTick(cue.tick,call.media->bpm());
        if (cueAdmission == LaunchCueAdmission::None || (physical && musicalLinked) || frame < 0) return false;
        auto* rendered = call.runtime.trackRanges(target.track);
        if (!rendered || rendered->cuePages || rendered->warpCuePages) return false;
        // An earlier range on this source could trim the new reservation
        // before the due range reads it. Admit only an idle reader here.
        const auto sameReader = [pages,clip=target.clip](const Range& range) noexcept {
            return range.pcm && range.pcm->pageReader() == pages && range.pcm->clip() == clip;
        };
        for (std::size_t index = 0; index < rendered->size; ++index)
            if (sameReader(rendered->items.data()[index])) return false;
        for (std::size_t index = 0; index < rendered->tailCount; ++index)
            if (sameReader(rendered->tails.data()[index].range)) return false;
        // The exact slice can end earlier than its maximum at a timeline
        // wrap or phase change; reserve the outer callback's remaining PCM.
        const auto remaining = call.frames - call.offset - offset;
        if (physical) {
            const auto requestAhead = scheduled || linked;
            if (!pages->reserveCue(target.clip,frame,remaining,requestAhead,
                                   cueAdmission == LaunchCueAdmission::HeldRepeat)) return false;
        } else {
            const auto sample = call.block.sample + offset;
            const auto sliced = remaining > call.block.frames - offset;
            const auto wrapsInBlock = [&](std::int64_t at, std::int64_t phase) noexcept {
                if (target.loopEndTick <= target.loopStartTick) return false;
                const auto lap = std::min(target.loopEndTick - phase,std::numeric_limits<std::int64_t>::max() - at);
                const auto end = call.block.sampleAtTick(call.block.clock,at + lap);
                // Keep the loop head ready across a driver slice boundary
                // and when fixed-clock rounding lands a frame early at End.
                return sliced || end - sample <= remaining + 1;
            };
            auto wrap = wrapsInBlock(tick,cue.tick);
            std::optional<std::int64_t> restart;
            auto restartInBlock = false;
            if (cue.repeat) {
                restart = pcm->frameForTick(cue.repeat->sourceTick,call.media->bpm());
                const auto due = call.block.sampleAtTick(call.block.clock,cue.repeat->tick);
                // A later slice can rebase the pending deadline at an Arrangement wrap.
                restartInBlock = sliced || std::max(sample + 1,due) - sample < remaining;
                if (restartInBlock) wrap |= wrapsInBlock(cue.repeat->tick,cue.repeat->sourceTick);
            }
            std::array<std::int64_t,kMaximumLaunchWarpCueWraps> wrapOffsets{};
            std::optional<LaunchPagedAudio::WorkerLoopCue> loopCue;
            if (wrap && !sliced && call.offset == 0 && !restart && !target.fade && target.mode != LaunchMode::Repeat &&
                (scheduled || linked || musicalLinked) && !call.runtime.beforeClock_ && !call.arrangementLoop &&
                nativeCueClockStable(call.block,call.follow,call.driver.performedSceneToken(),target.clip)) {
                const auto projected = nativeCueWraps(call.block,target,{sample,tick,cue.tick},remaining);
                if (projected.bounded && projected.count >= 2) {
                    for (std::size_t i = 0; i < projected.count; ++i)
                        wrapOffsets[i] = projected.positions[i].frame - sample;
                    loopCue = LaunchPagedAudio::WorkerLoopCue{pcm->frameForTick(target.loopStartTick,call.media->bpm()),
                        {wrapOffsets.data(),projected.count}};
                }
            }
            if (!pages->reserveWorkerCue(target.clip,frame,remaining,wrap,restart,restartInBlock,loopCue)) return false;
        }
        rendered->cuePages = pages;
        rendered->cueClip = target.clip;
        return true;
    }, [](void* state, const LaunchSlot& target, const LaunchLegatoForecast& forecast) noexcept {
        const auto& call = *static_cast<const Context*>(state);
        if (call.runtime.mediaAt(forecast.sample).get() != call.media) return;
        const auto* clip = call.media->clip(target.clip);
        if (!clip) return;
        if (const auto* native = clip->warpPageReader()) {
            if (!call.runtime.beforeClock_ && !call.arrangementLoop &&
                nativeLinkedTarget(target,*native,forecast.admission) && !native->wholeSourceFitsPageCache() &&
                nativeCueClockStable(call.block,call.follow,call.driver.performedSceneToken(),target.clip)) {
                const LaunchWarpPosition position{forecast.sample,forecast.tick,forecast.cue.tick};
                const auto wraps = nativeCueWraps(call.block,target,position,call.runtime.maxFrames_);
                if (wraps.bounded) clip->prefetchWarp(position,wraps.span());
            }
            return;
        }
        if (!clip->pageReader()) return;
        const auto& cue = forecast.cue;
        const auto physical = target.physicalFrames > 0;
        if (physical ? !needsCueLease(target,clip->rate())
                     : !needsWorkerCueLease(*clip)) return;
        const auto* rendered = call.runtime.trackRanges(target.track);
        if (rendered && rendered->cuePages == clip->pageReader() && rendered->cueClip == target.clip) return;
        const auto frame = physical ? cue.physicalFrame : clip->frameForTick(cue.tick,call.media->bpm());
        clip->pageReader()->prefetch(target.clip,frame);
    }, [](void* state, std::int64_t clip, std::int64_t tick) noexcept {
        const auto& call = *static_cast<const Context*>(state);
        const auto* pcm = call.media->clip(clip);
        return pcm && (pcm->residentRawPcm() || pcm->legacyPageSource()) ? pcm->frameForTick(tick,call.media->bpm()) : -1;
    }};
    while (context.offset < frames) {
        block.sample = transport.sampleAt(context.offset);
        block.frames = static_cast<std::int32_t>(transport.contiguousFrames(context.offset, frames - context.offset));
        if (beforeClock_ && block.sample < clockSample_)
            block.frames = static_cast<std::int32_t>(std::min<std::int64_t>(block.frames, clockSample_ - block.sample));
        block.arrangementWrap = transport.looping() && block.sample == transport.loopBegin() &&
            (context.offset > 0 || (driver.transportRevision_ == transport.revision() &&
                driver.nextTimelineSample_ == transport.loopEnd() && driver.loopBegin_ == transport.loopBegin() &&
                driver.loopEnd_ == transport.loopEnd()));
        if (block.frames <= 0)
            break;
        if (capture && capture->capturesPcm()) {
            if (block.sample <= std::numeric_limits<std::int64_t>::max() - block.frames)
                capture->notePcmTimeline(block.sample, block.sample + block.frames);
            else capture->markPcmDiscontinuity();
        }
        context.media = mediaAt(block.sample).get();
        context.view = context.media->view();
        context.view.tracks = tracks_;
        driver.gains_.begin(block, tracks_, gainRevision(), &driver,
            [](const void* raw, std::int64_t track, std::int64_t clip, std::uint64_t run) noexcept {
                const auto* retained = static_cast<const LaunchAudioDriver*>(raw)->retainedFor(track,clip,run);
                return retained ? retained->gainRevision : std::uint64_t{0};
            });
        if (!driver.launcher_.process(block, context.view, sink))
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
    const auto* media = beforeClock_ && beforeClock_->pcmRevision() == tail.media ? beforeClock_.get() : project_.get();
    if (!tail.pinned && media->pcmRevision() != tail.media) return false;
    const auto* pcm = tail.pinned ? &tail.pinned->pcm : media->clip(tail.clip);
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
    auto warp = tail.warp;
    warp.frame += std::min(tail.elapsed, std::numeric_limits<std::int64_t>::max()-warp.frame);
    ranges.tails.data()[ranges.tailCount++] = {
        {offset, length, pcm, source, tail.velocity, tail.elapsed, warp, tail.sourceEndFrame}, tail.fadeInAtExit};
    tail.elapsed += length;
    return tail.elapsed < duration;
}

void LaunchAudioRuntime::fadeOut(LaunchAudioDriver& driver, const LaunchAudioProject& media, const LaunchTransition& edge,
                                 std::int32_t offset, std::int32_t frames) noexcept {
    if (!edge.oldFade || offset < 0 || offset >= frames) return;
    const auto* retained = driver.retainedFor(edge.track,edge.oldClip,edge.oldRun);
    const bool sceneCut = beforeClock_.get() == &media && edge.sample == clockSample_;
    const auto bankSample = sceneCut ? edge.sample - 1 : edge.sample;
    const auto* declared = retained ? pinnedCandidate(bankSample,edge.track,edge.oldClip) : nullptr;
    auto* pinned = declared && samePinnedShape(retained->slot,declared->slot) ? retained->source : nullptr;
    const auto* bankClip = media.clip(edge.oldClip);
    const auto* pcm = pinned ? &pinned->pcm : bankClip;
    if (!pcm) return;
    const auto anchor = edge.oldPhysicalSourceAnchorFrame >= 0 ? edge.oldPhysicalSourceAnchorFrame
        : pcm->frameForTick(edge.oldSourceAnchorTick, media.bpm());
    const auto source = anchor + std::min(edge.oldFramesSinceSourceAnchor,
        std::numeric_limits<std::int64_t>::max() - std::max<std::int64_t>(0, anchor));
    std::int64_t sourceEnd = -1;
    if (const auto aperture = bankClip ? bankClip->legacyAperture() : std::nullopt) {
        // A paired Scene still fades the outgoing prepared bank. Its next
        // block may already be on the incoming clock.
        sourceEnd = aperture->preparedEndFrame;
        if ((aperture->followProjectEnd || edge.oldRawProjectAperture) && !sceneCut && edge.oldFramesUntilMusicalEnd >= 0)
            sourceEnd = source + std::min(edge.oldFramesUntilMusicalEnd,
                std::numeric_limits<std::int64_t>::max() - std::max<std::int64_t>(0, source));
    }
    LaunchAudioDriver::Tail tail{edge.track, edge.oldClip, source, 0,
        edge.oldFramesSinceStart, edge.oldVelocity * driver.gains_.currentMultiplier(edge.track, edge.oldRun),
        media.pcmRevision(), {edge.sample,edge.oldClockAnchorTick,edge.oldClockAnchorSourceTick}, pinned, sourceEnd};
    if (addTail(tail, offset, frames - offset) && driver.tailCount_ < driver.tails_.size()) {
        if (pinned) pinned->leases.fetch_add(1,std::memory_order_relaxed);
        driver.tails_.data()[driver.tailCount_++] = tail;
    }
}

void LaunchAudioRuntime::add(LaunchAudioDriver& driver, const LaunchAudioProject& media, const LaunchSection& section,
                             const LaunchBlock& block, std::int32_t base, LaunchCaptureBuffer* capture) noexcept {
    const auto gain = driver.gains_.range(block, section);
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
    const auto* retained = driver.retainedFor(section.track,section.clip,section.run);
    const auto* declared = retained ? pinnedCandidate(block.sample,section.track,section.clip) : nullptr;
    const auto* pinned = declared && samePinnedShape(retained->slot,declared->slot) ? retained->source : nullptr;
    const auto* bankClip = media.clip(section.clip);
    const auto* pcm = pinned ? &pinned->pcm : bankClip;
    const auto anchor = section.physicalSourceAnchorFrame >= 0 ? section.physicalSourceAnchorFrame
        : (pcm ? pcm->frameForTick(section.sourceAnchorTick, media.bpm()) : 0);
    const auto source = anchor <= std::numeric_limits<std::int64_t>::max() - section.framesSinceSourceAnchor
        ? anchor + section.framesSinceSourceAnchor : std::numeric_limits<std::int64_t>::max();
    const auto fade = section.fade ? section.framesSinceStart : -1;
    const auto aperture = bankClip ? bankClip->legacyAperture() : std::nullopt;
    const auto sourceEnd = aperture && !aperture->followProjectEnd && !section.rawProjectAperture
        ? aperture->preparedEndFrame : -1;
    const auto available = pcm ? pcm->frames() : 0;
    const auto captureEnd = sourceEnd < 0 ? available : std::min(available,static_cast<std::uint64_t>(sourceEnd));
    const bool tapePcm = capture && capture->capturesPcm() && media.stored() && pcm &&
        !pcm->usesPages() && !pcm->followsTempo() && fade < 0 && section.velocity == 1.0f && source >= 0 &&
        static_cast<std::uint64_t>(source) <= captureEnd &&
        static_cast<std::uint64_t>(section.frames) <= captureEnd - static_cast<std::uint64_t>(source);
    if (capture && !tapePcm && section.clip != 0 && (pinned || section.regionIsLiveEdit ||
        (gain.beforeFrames > 0 && gain.before != 1.0f) ||
        (gain.beforeFrames < section.frames && gain.after != 1.0f))) capture->markSourceEdit();

    const auto addRange = [&](std::int32_t skip, std::int32_t frames, std::int64_t firstSource,
                              float multiplier, std::int64_t fadeElapsed) noexcept {
        const auto first = block.sample + section.offset + skip;
        const LaunchWarpPosition warp{first,section.clockAnchorTick,section.clockAnchorSourceTick};
        ranges.items.data()[ranges.size++] = {base + section.offset + skip, frames, pcm, firstSource,
            section.velocity * multiplier, fadeElapsed, warp, sourceEnd};
        if (tapePcm) {
            (void)capture->appendPcmRange({section.track, section.clip, section.run, first, first + frames, firstSource,
                section.regionIsLiveEdit, multiplier});
        }
    };
    if (gain.beforeFrames > 0) addRange(0, gain.beforeFrames, source, gain.before, fade);
    if (gain.beforeFrames < section.frames && ranges.size < ranges.items.size()) {
        const auto advanced = source + std::min<std::int64_t>(gain.beforeFrames,
            std::numeric_limits<std::int64_t>::max() - std::max<std::int64_t>(0, source));
        const auto elapsed = fade < 0 ? -1 : fade + std::min<std::int64_t>(gain.beforeFrames,
            std::numeric_limits<std::int64_t>::max() - fade);
        addRange(gain.beforeFrames, section.frames - gain.beforeFrames, advanced, gain.after, elapsed);
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

std::int32_t LaunchAudioRuntime::latencyForTrack(std::int64_t track) const noexcept {
    const auto forBank = [track](const LaunchAudioProject& bank) {
        const auto slots = bank.view().slots;
        auto maximum = std::int32_t{0};
        for (auto it = std::ranges::lower_bound(slots, track, {}, &LaunchSlot::track);
             it != slots.end() && it->track == track; ++it) {
            if (const auto* clip = bank.clip(it->clip)) maximum = std::max(maximum, clip->latencySamples());
        }
        return maximum;
    };
    const auto after = forBank(*project_);
    return beforeClock_ ? std::max(after, forBank(*beforeClock_)) : after;
}

void LaunchAudioRuntime::overlay(std::int64_t track, const NodeIo& node, std::span<std::uint8_t> replaces) const noexcept {
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
    auto* replace = replaces.size() >= static_cast<std::size_t>(io.frames) ? replaces.data() : nullptr;
    const auto* stop = ranges.items.data() + ranges.size;
    const auto* range = std::upper_bound(ranges.items.data(), stop, node.blockOffset,
        [](std::int32_t offset, const Range& item) { return offset < item.offset + item.frames; });
    for (; range != stop && range->offset < io.frames; ++range) {
        const auto start = std::max(range->offset, node.blockOffset);
        const auto finish = std::min(range->offset + range->frames, io.frames);
        if (replace) std::fill_n(replace + start, finish - start, std::uint8_t{1});
        const auto advance = start - range->offset;
        const auto available = range->sourceFrame <= std::numeric_limits<std::int64_t>::max() - advance;
        const auto fadeElapsed = range->fadeElapsed < 0 ? -1 : range->fadeElapsed +
            std::min<std::int64_t>(advance, std::numeric_limits<std::int64_t>::max() - range->fadeElapsed);
        auto warp = range->warp;
        warp.frame += std::min<std::int64_t>(advance,std::numeric_limits<std::int64_t>::max()-warp.frame);
        renderLaunchAudio(io, start, finish - start, available ? range->pcm : nullptr,
                          available ? range->sourceFrame + advance : 0, range->velocity, fadeElapsed, warp,
                          range->sourceEndFrame);
    }
    for (std::size_t index = 0; index < ranges.tailCount; ++index) {
        const auto& tail = ranges.tails.data()[index];
        const auto start = std::max(tail.range.offset, node.blockOffset);
        const auto finish = std::min(tail.range.offset + tail.range.frames, io.frames);
        if (start >= finish) continue;
        const auto advance = start - tail.range.offset;
        const auto source = tail.range.sourceFrame + std::min<std::int64_t>(advance,
            std::numeric_limits<std::int64_t>::max() - std::max<std::int64_t>(0, tail.range.sourceFrame));
        auto warp = tail.range.warp;
        warp.frame += std::min<std::int64_t>(advance,std::numeric_limits<std::int64_t>::max()-warp.frame);
        mixLaunchAudioTail(io, start, finish - start, *tail.range.pcm, source,
                           tail.range.velocity, tail.range.fadeElapsed + advance, tail.fadeInAtExit, warp,
                           tail.range.sourceEndFrame);
    }
}

} // namespace adi::engine
