// SPDX-License-Identifier: AGPL-3.0-or-later
#include "adi/engine/launch_audio_runtime.hpp"

#include "adi/audio/io_audit.hpp"
#include "adi/engine/graph.hpp"
#include "adi/engine/launch_capture.hpp"
#include "adi/engine/launch_follow_playback.hpp"
#include "adi/engine/launch_paged_audio.hpp"
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

const LaunchTempoBank* tempoBankAt(const std::vector<LaunchTempoBank>& banks, std::int64_t sample) noexcept {
    if (banks.empty()) return nullptr;
    // Keep driver searches on raw pointers with MSVC iterator debugging enabled.
    const auto* first = banks.data();
    const auto* next = std::ranges::upper_bound(first,first+banks.size(),sample,{},&LaunchTempoBank::startSample);
    return next == first ? first : next - 1;
}

} // namespace

LaunchAudioClip LaunchAudioClip::fromSamples(std::int64_t clip, std::uint32_t rate, std::uint16_t channels,
                                             std::vector<float> pcm, bool directPcm) {
    if (clip <= 0 || rate == 0 || channels == 0 || pcm.empty() || pcm.size() % channels != 0)
        throw std::invalid_argument("invalid prepared Session clip audio");
    LaunchAudioClip result{clip, 0, rate, channels, std::move(pcm)};
    result.directPcm_ = directPcm;
    return result;
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
    for (auto& slot : slots_) {
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
        if (const auto* pcm = clip(slot.clip); pcm && pcm->pageReader()) {
            slot.liveRegionSupported = pcm->pageReader()->retainsRange(slot.clip,
                pcm->frameForTick(slot.startTick,bpm_),pcm->frameForTick(slot.endTick,bpm_));
        }
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

bool LaunchAudioProject::hasPagedAudio() const noexcept {
    return std::ranges::any_of(clips_, [](const auto& clip) { return clip.pageReader() != nullptr; });
}

const LaunchAudioClip* LaunchAudioProject::clip(std::int64_t id) const noexcept {
    if (id <= 0 || clips_.empty())
        return nullptr;
    const auto* end = clips_.data() + clips_.size();
    const auto* pcm = std::lower_bound(clips_.data(), end, id,
        [](const LaunchAudioClip& candidate, std::int64_t target) { return candidate.clip() < target; });
    return pcm != end && pcm->clip() == id ? pcm : nullptr;
}

bool LaunchAudioDriver::postSource(const LaunchClipSourceEdit& edit, std::uint64_t baseline,
                                   std::unique_ptr<LaunchAudioClip>& replacement) noexcept {
    const auto token = sourceLease_.idleToken();
    if (!token) return false;
    sourceEdit_ = {edit,baseline,std::move(replacement)};
    if (sourceLease_.publish(*token)) return true;
    replacement = std::move(sourceEdit_.clip);
    return false;
}

void LaunchAudioDriver::collectSourceEdit() noexcept {
    if (!audio::onAudioThread && !sourceEditReserved()) sourceEdit_.clip.reset();
}

bool LaunchAudioDriver::hasSourceEdit() const noexcept {
    return sourcePhase_ == SourceEditPhase::Pending || sourcePhase_ == SourceEditPhase::Applied;
}

void LaunchAudioDriver::releaseSourceEdit() noexcept {
    sourcePhase_ = SourceEditPhase::Idle;
    sourceBoundary_.reset();
    // Source ranges from the preceding callback are no longer in use.
    // Only the owner may reclaim the prepared buffer after this release.
    sourceLease_.release();
}

void LaunchAudioDriver::suspendSourceEdits() noexcept {
    sourcePhase_ = SourceEditPhase::Idle;
    sourceBoundary_.reset();
    sourceLease_.suspend();
}

void LaunchAudioDriver::beginSourceCallback() noexcept {
    sourceLease_.resume();
    if (sourcePhase_ == SourceEditPhase::Retiring) releaseSourceEdit();
    if (sourcePhase_ == SourceEditPhase::Idle && sourceEditReserved()) sourcePhase_ = SourceEditPhase::Pending;
}

void LaunchAudioDriver::beginSourceSlice(const LaunchBlock& block, const LaunchAudioProject& media,
                                         std::span<const std::int64_t> tracks) noexcept {
    if (!hasSourceEdit()) return;
    const auto& edit = sourceEdit_.address;
    if (edit.incarnation != block.incarnation || !std::binary_search(tracks.begin(),tracks.end(),edit.track) ||
        !launcher_.matchesRunningClip(edit.track,edit.clip,edit.run) ||
        media.pcmRevision() > sourceEdit_.baseline ||
        (sourcePhase_ == SourceEditPhase::Applied && media.pcmRevision() != sourceEdit_.baseline)) {
        sourcePhase_ = SourceEditPhase::Retiring;
        return;
    }
    // An older graph already pinned by the driver cannot consume a request
    // made against the owner's newer PCM bank. Resolve only when it appears.
    if (sourcePhase_ == SourceEditPhase::Pending && sourceBoundary_ && block.sample != nextTimelineSample_) {
        if (block.arrangementWrap && edit.rate != ClipUpdateRate::None && block.tickAtSample) {
            const auto previous = std::max<std::int64_t>(0,block.tickAtSample(block.clock,nextTimelineSample_));
            const auto now = std::max<std::int64_t>(0,block.tickAtSample(block.clock,block.sample));
            const auto remaining = std::max<std::int64_t>(0,sourceBoundary_->tick - previous);
            sourceBoundary_->tick = now + std::min(remaining,std::numeric_limits<std::int64_t>::max() - now);
        } else sourceBoundary_.reset();
    }
}

void LaunchAudioDriver::trackSourceStatus(const LaunchTrackStatus& status) noexcept {
    if (!hasSourceEdit()) return;
    const auto& edit = sourceEdit_.address;
    if (status.track == edit.track && (status.run != edit.run || status.clip != edit.clip))
        sourcePhase_ = SourceEditPhase::Retiring;
}

LaunchAudioDriver::SourceRange LaunchAudioDriver::sourceRange(const LaunchBlock& block,
    const LaunchSection& section, std::uint64_t baseline) noexcept {
    if (!hasSourceEdit() || sourceEdit_.baseline != baseline)
        return {section.frames};
    const auto& edit = sourceEdit_.address;
    if (section.track != edit.track || section.clip != edit.clip || section.run != edit.run)
        return {section.frames};
    if (sourcePhase_ == SourceEditPhase::Applied) return {0,sourceEdit_.clip.get()};
    if (!sourceBoundary_) sourceBoundary_ = nextClipUpdateBoundary(block,block.sample,edit.rate,true);
    if (!sourceBoundary_) return {section.frames};
    const auto due = edit.rate == ClipUpdateRate::None ? sourceBoundary_->sample
        : block.sampleAtTick(block.clock,sourceBoundary_->tick);
    const auto first = block.sample + section.offset;
    if (due < 0 || due >= first + section.frames) return {section.frames};
    sourcePhase_ = SourceEditPhase::Applied;
    return {static_cast<std::int32_t>(std::max<std::int64_t>(0,due-first)),sourceEdit_.clip.get()};
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
        if (lastFrame < 0 || static_cast<std::uint64_t>(lastFrame) > pcm->frames()) return false;
        if (const auto* pages = pcm->pageReader()) {
            const auto slots = prepared.view().slots;
            const auto slot = std::ranges::find_if(slots,[&](const LaunchSlot& item) {
                return item.track == edit.track && item.clip == edit.clip;
            });
            if (slot == slots.end() || !slot->liveRegionSupported || edit.region.start < slot->startTick ||
                last > slot->endTick || !pages->retainsRange(edit.clip,
                    pcm->frameForTick(edit.region.start,prepared.bpm()),lastFrame)) return false;
        }
    } else if (!prepared.isMidiTrack(edit.track)) return false;
    return driver.launcher_.postRegionEdit(edit);
}

bool LaunchAudioRuntime::postSourceEdit(LaunchAudioDriver& driver, const LaunchClipSourceEdit& edit,
                                        std::unique_ptr<LaunchAudioClip>& replacement) const noexcept {
    if (audio::onAudioThread || !replacement || !tempoBanks_.empty() || edit.incarnation != project_->incarnation() ||
        edit.run == 0 || edit.rate > ClipUpdateRate::Bar || !hasTrack(edit.track) || project_->isMidiTrack(edit.track))
        return false;
    const auto* original = project_->clip(edit.clip);
    if (!original || !original->sameResidentSourceShape(*replacement)) return false;
    const auto slots = project_->view().slots;
    const auto slot = std::ranges::find_if(slots,[&](const LaunchSlot& value) {
        return value.track == edit.track && value.clip == edit.clip;
    });
    if (slot == slots.end() || slot->fade) return false;
    const auto resident = project_->residentBytes();
    if (resident > kSessionResidentPcmBudget || replacement->bytes() > kSessionResidentPcmBudget - resident) return false;
    return driver.postSource(edit,project_->pcmRevision(),replacement);
}

LaunchAudioRuntime::LaunchAudioRuntime(std::shared_ptr<const LaunchAudioProject> project,
                                       std::vector<std::int64_t> tracks, std::int32_t maxFrames,
                                       std::shared_ptr<const LaunchAudioProject> beforeClock,
                                       std::int64_t clockSample)
    : LaunchAudioRuntime(project, std::move(tracks), maxFrames,
        beforeClock ? std::vector<LaunchTempoBank>{{0, std::move(beforeClock)}, {clockSample, project}}
                    : std::vector<LaunchTempoBank>{}) {}

LaunchAudioRuntime::LaunchAudioRuntime(std::shared_ptr<const LaunchAudioProject> project,
                                       std::vector<std::int64_t> tracks, std::int32_t maxFrames,
                                       std::vector<LaunchTempoBank> banks)
    : project_(std::move(project)), tempoBanks_(std::move(banks)),
      tracks_(std::move(tracks)), ranges_(tracks_.size()), maxFrames_(maxFrames) {
    if (!project_ || maxFrames <= 0)
        throw std::invalid_argument("invalid Session launch audio runtime format");
    if (!tempoBanks_.empty() && (tempoBanks_.size() < 2 || tempoBanks_.front().startSample != 0 ||
        tempoBanks_.back().media != project_))
        throw std::invalid_argument("invalid Session clock media handover");
    std::int64_t previous = -1;
    for (const auto& bank : tempoBanks_) {
        if (bank.startSample <= previous || !bank.media || bank.media->incarnation() != project_->incarnation())
            throw std::invalid_argument("invalid Session clock media handover");
        bankRevisions_.push_back(bank.media.get());
        previous = bank.startSample;
    }
    std::ranges::sort(bankRevisions_, {}, &LaunchAudioProject::pcmRevision);
    bankRevisions_.erase(std::unique(bankRevisions_.begin(),bankRevisions_.end()),bankRevisions_.end());
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

const std::shared_ptr<const LaunchAudioProject>& LaunchAudioRuntime::mediaAt(std::int64_t sample) const noexcept {
    const auto* bank = tempoBankAt(tempoBanks_,sample);
    return bank ? bank->media : project_;
}

std::shared_ptr<const LaunchAudioProject> LaunchAudioRuntime::bankAtTempo(double bpm) const noexcept {
    if (project_->bpm() == bpm) return project_;
    const auto bank = std::ranges::find_if(tempoBanks_, [&](const auto& entry) { return entry.media->bpm() == bpm; });
    return bank != tempoBanks_.end() ? bank->media : nullptr;
}

const LaunchAudioProject* LaunchAudioRuntime::mediaByRevision(std::uint64_t revision) const noexcept {
    if (project_->pcmRevision() == revision) return project_.get();
    if (bankRevisions_.empty()) return nullptr;
    const auto* first = bankRevisions_.data();
    const auto* end = first + bankRevisions_.size();
    const auto* bank = std::ranges::lower_bound(first,end,revision,{},&LaunchAudioProject::pcmRevision);
    return bank != end && (*bank)->pcmRevision() == revision ? *bank : nullptr;
}

std::uint64_t LaunchAudioRuntime::gainRevision() const noexcept {
    // All projections render the same owner content. Keep an active clip's
    // gain edit in effect until the scene replaces it, including its tail.
    return tempoBanks_.empty() ? project_->pcmRevision() : tempoBanks_.front().media->pcmRevision();
}

void LaunchAudioRuntime::startIfQueued(LaunchAudioDriver& driver, LaunchBlock block, Transport& transport) const noexcept {
    if (!project_->stored() || transport.playing() || transport.countInRemaining() != 0) return;
    block.sample = transport.sampleAt(0);
    block.incarnation = project_->incarnation();
    block.followPass = driver.pass_;
    auto view = mediaAt(block.sample)->view();
    view.tracks = tracks_;
    if ((block.preparedScene && block.preparedScene->token > driver.performedSceneToken()) ||
        driver.launcher_.admitWhileStopped(block, view)) transport.play();
}

void LaunchAudioRuntime::begin(LaunchAudioDriver& driver, LaunchBlock block,
                               const Transport& transport, std::int64_t streamSample,
                               void* observerContext,
                               void (*observer)(void*, const LaunchTransition&, std::int64_t) noexcept,
                               void* sceneObserverContext,
                               void (*sceneObserver)(void*, const LaunchSceneTransition&, std::int64_t) noexcept,
                               LaunchCaptureBuffer* capture) noexcept {
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
    driver.beginSourceCallback();
    if (!driver.wasPlaying_ || transport.streamedFrames() == 0) ++driver.pass_;
    driver.wasPlaying_ = true;
    block.incarnation = project_->incarnation();
    if (driver.tailIncarnation_ != block.incarnation) {
        driver.tailCount_ = 0;
        driver.tailIncarnation_ = block.incarnation;
        driver.lastMediaBpm_ = 0.0;
        driver.lastMediaRevision_ = 0;
    }
    std::size_t remainingTails = 0;
    for (std::size_t index = 0; index < driver.tailCount_; ++index) {
        auto tail = driver.tails_.data()[index];
        if (addTail(tail, 0, block.frames, capture))
            driver.tails_.data()[remainingTails++] = tail;
    }
    driver.tailCount_ = remainingTails;
    block.followPass = driver.pass_;
    const auto* follow = project_->followPlayer();
    block.followActionsEnabled &= follow != nullptr;
    struct Context {
        LaunchAudioRuntime& runtime;
        LaunchAudioDriver& driver;
        const LaunchAudioProject* media;
        const LaunchAudioProject* previousMedia;
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
        std::int32_t offset = 0;
    };
    if (streamSample < 0 || streamSample > std::numeric_limits<std::int64_t>::max() - frames) {
        observer = nullptr;
        sceneObserver = nullptr;
    }
    Context context{*this, driver, project_.get(), nullptr, {}, block, follow, block.incarnation,
        followPassSeed(project_->followSeed(), driver.pass_), streamSample, frames, observerContext, observer,
        sceneObserverContext, sceneObserver, capture};
    const LaunchSink sink{&context, [](void* state, const LaunchSection& section) noexcept {
        auto& call = *static_cast<Context*>(state);
        call.runtime.add(call.driver, *call.media, section, call.block, call.offset, call.capture);
    }, [](void* state, const LaunchTransition& edge) noexcept {
        const auto& call = *static_cast<const Context*>(state);
        const auto* outgoing = call.previousMedia && edge.offset == 0 && edge.oldFramesSinceStart > 0
            ? call.previousMedia : call.media;
        call.runtime.fadeOut(call.driver, *outgoing, edge, call.offset + edge.offset, call.frames, call.capture);
        if (call.capture || call.observer) {
            auto logicalEdge = edge;
            logicalEdge.offset += call.offset;
            if (call.capture) (void)call.capture->append(logicalEdge);
            if (call.observer) call.observer(call.observerContext, logicalEdge, call.streamSample + logicalEdge.offset);
        }
    }, [](void* state, const LaunchTransition& edge) noexcept -> std::optional<LaunchFollowRequest> {
        const auto& call = *static_cast<const Context*>(state);
        if (!call.follow) return std::nullopt;
        return call.follow->followOnLaunch(edge, call.view, call.incarnation, call.block.followActionsEnabled, call.seed);
    }, [](void* state, const LaunchTrackStatus& track) noexcept {
        const auto& call = *static_cast<const Context*>(state);
        call.driver.trackSourceStatus(track);
        if (!track.queued.legatoSourceTick) return;
        const auto due = call.block.sampleAtTick(call.block.clock, track.queued.tick);
        const auto& media = *call.runtime.mediaAt(due);
        if (&media != call.media) return;
        const auto* clip = media.clip(track.queued.clip);
        if (clip && clip->pageReader())
            clip->pageReader()->prefetch(clip->clip(), clip->frameForTick(*track.queued.legatoSourceTick, media.bpm()));
    }, [](void* state, const LaunchSceneTransition& edge) noexcept {
        const auto& call = *static_cast<const Context*>(state);
        if (edge.preparedToken != 0)
            call.driver.performedScene_.store(edge.preparedToken, std::memory_order_release);
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
    }, [](void* state, std::int64_t clip, std::int64_t oldAnchor, std::int64_t elapsed) noexcept
            -> std::optional<std::int64_t> {
        const auto& call = *static_cast<const Context*>(state);
        const auto* before = call.previousMedia ? call.previousMedia->clip(clip) : nullptr;
        const auto* after = call.media->clip(clip);
        if (!before || !after || elapsed < 0) return std::nullopt;
        const auto oldFrame = before->frameForTick(oldAnchor, call.previousMedia->bpm());
        if (oldFrame < 0 || oldFrame > std::numeric_limits<std::int64_t>::max() - elapsed) return std::nullopt;
        const auto tick = after->tickForFrame(oldFrame + elapsed, call.media->bpm());
        return tick >= 0 ? std::optional{tick} : std::nullopt;
    }};
    while (context.offset < frames) {
        block.sample = transport.sampleAt(context.offset);
        block.frames = static_cast<std::int32_t>(transport.contiguousFrames(context.offset, frames - context.offset));
        const auto* bank = tempoBankAt(tempoBanks_,block.sample);
        if (bank && bank + 1 != tempoBanks_.data() + tempoBanks_.size())
            block.frames = static_cast<std::int32_t>(std::min<std::int64_t>(block.frames, (bank+1)->startSample - block.sample));
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
        context.media = bank ? bank->media.get() : project_.get();
        // Keeping the previous bank's metadata in the pinned graph also lets
        // a stop or replacement fade out the old source after a seek.
        context.previousMedia = context.media->bpm() != driver.lastMediaBpm_
            ? mediaByRevision(driver.lastMediaRevision_) : nullptr;
        block.reanchorAudioSources = context.previousMedia != nullptr;
        context.view = context.media->view();
        context.view.tracks = tracks_;
        driver.beginSourceSlice(block,*context.media,tracks_);
        driver.gains_.begin(block, tracks_, gainRevision());
        if (!driver.launcher_.process(block, context.view, sink))
            break;
        driver.lastMediaBpm_ = context.media->bpm();
        driver.lastMediaRevision_ = context.media->pcmRevision();
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
                                std::int32_t frames, LaunchCaptureBuffer* capture) noexcept {
    if (frames <= 0 || tracks_.empty()) return false;
    const auto* media = mediaByRevision(tail.media);
    if (!media) return false;
    const auto* pcm = media->clip(tail.clip);
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
        {offset, length, pcm, source, tail.velocity, false, tail.elapsed}, tail.fadeInAtExit};
    if (capture && tail.velocity != 0.0f && tail.fadeInAtExit != 0 && pcm->frames() != 0)
        capture->markFadeTail();
    tail.elapsed += length;
    return tail.elapsed < duration;
}

void LaunchAudioRuntime::fadeOut(LaunchAudioDriver& driver, const LaunchAudioProject& media, const LaunchTransition& edge,
                                 std::int32_t offset, std::int32_t frames, LaunchCaptureBuffer* capture) noexcept {
    if (!edge.oldFade || offset < 0 || offset >= frames) return;
    const auto* pcm = media.clip(edge.oldClip);
    if (!pcm) return;
    const auto anchor = pcm->frameForTick(edge.oldSourceAnchorTick, media.bpm());
    const auto source = anchor + std::min(edge.oldFramesSinceSourceAnchor,
        std::numeric_limits<std::int64_t>::max() - std::max<std::int64_t>(0, anchor));
    LaunchAudioDriver::Tail tail{edge.track, edge.oldClip, source, 0,
        edge.oldFramesSinceStart, edge.oldVelocity * driver.gains_.currentMultiplier(edge.track, edge.oldRun),
        media.pcmRevision()};
    if (addTail(tail, offset, frames - offset, capture) && driver.tailCount_ < driver.tails_.size())
        driver.tails_.data()[driver.tailCount_++] = tail;
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
    const auto* pcm = media.clip(section.clip);
    const auto replacement = driver.sourceRange(block,section,media.pcmRevision());
    const auto anchor = pcm ? pcm->frameForTick(section.sourceAnchorTick, media.bpm()) : 0;
    const auto source = anchor <= std::numeric_limits<std::int64_t>::max() - section.framesSinceSourceAnchor
        ? anchor + section.framesSinceSourceAnchor : std::numeric_limits<std::int64_t>::max();
    const auto fade = section.fade ? section.framesSinceStart : -1;
    const bool tapePcm = capture && capture->capturesPcm() && media.stored() && pcm && pcm->hasDirectPcm() && fade < 0 &&
        section.velocity == 1.0f && source >= 0 &&
        static_cast<std::uint64_t>(source) <= pcm->frames() &&
        static_cast<std::uint64_t>(section.frames) <= pcm->frames() - static_cast<std::uint64_t>(source);
    if (capture && replacement.replacement) capture->markSourceEdit();
    if (capture && !tapePcm && section.clip != 0 && (section.regionIsLiveEdit ||
        (gain.beforeFrames > 0 && gain.before != 1.0f) ||
        (gain.beforeFrames < section.frames && gain.after != 1.0f))) capture->markSourceEdit();

    for (auto skip = std::int32_t{0}; skip < section.frames && ranges.size < ranges.items.size();) {
        const bool beforeGain = skip < gain.beforeFrames;
        const bool beforeReplacement = skip < replacement.beforeFrames;
        const auto rangeEnd = std::min(beforeGain ? gain.beforeFrames : section.frames,
            beforeReplacement ? replacement.beforeFrames : section.frames);
        const auto multiplier = beforeGain ? gain.before : gain.after;
        const auto* current = beforeReplacement ? pcm : replacement.replacement;
        const auto advanced = source + std::min<std::int64_t>(skip,
            std::numeric_limits<std::int64_t>::max() - std::max<std::int64_t>(0, source));
        const auto elapsed = fade < 0 ? -1 : fade + std::min<std::int64_t>(skip,
            std::numeric_limits<std::int64_t>::max() - fade);
        ranges.items.data()[ranges.size++] = {base + section.offset + skip, rangeEnd - skip, current, advanced,
            section.velocity * multiplier, !section.regionIsLiveEdit, elapsed};
        if (tapePcm && beforeReplacement) {
            const auto first = block.sample + section.offset;
            (void)capture->appendPcmRange({section.track, section.clip, section.run, first + skip, first + rangeEnd, advanced,
                section.regionIsLiveEdit, multiplier});
        }
        skip = rangeEnd;
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
                          available ? range->sourceFrame + advance : 0, range->velocity, fadeElapsed, range->useAuthoredLoop);
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
