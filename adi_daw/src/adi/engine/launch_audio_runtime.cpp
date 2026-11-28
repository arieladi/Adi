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

bool needsRetainedPages(const LaunchSlot& slot, std::uint32_t rate) noexcept {
    return slot.physicalLoopEndFrame > 0 || slot.physicalFrames > 3 * ClipPlayback::readAheadFrames(rate);
}

bool samePinnedShape(const LaunchSlot& a, const LaunchSlot& b) noexcept {
    return a.track == b.track && a.scene == b.scene && a.clip == b.clip &&
        a.physicalStartFrame == b.physicalStartFrame && a.physicalFrames == b.physicalFrames &&
        a.physicalLoopBeginFrame == b.physicalLoopBeginFrame && a.physicalLoopEndFrame == b.physicalLoopEndFrame &&
        a.fade == b.fade && a.mode == b.mode;
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
    if (!stored_ && std::ranges::any_of(clips_, [](const LaunchAudioClip& clip) { return clip.usesPages(); }))
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
            const auto physical = slot.physicalStartFrame >= 0;
            const auto first = physical ?
                (slot.physicalLoopEndFrame > 0 ? std::min(slot.physicalStartFrame,slot.physicalLoopBeginFrame)
                                               : slot.physicalStartFrame) : pcm->frameForTick(slot.startTick,bpm_);
            const auto last = physical ? slot.physicalStartFrame + slot.physicalFrames : pcm->frameForTick(slot.endTick,bpm_);
            slot.liveRegionSupported = pcm->pageReader()->retainsRange(slot.clip,first,last);
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
    return std::ranges::any_of(clips_, &LaunchAudioClip::usesPages);
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
    const auto slots = prepared.view().slots;
    const auto slot = std::ranges::find_if(slots,[&](const LaunchSlot& candidate) {
        return candidate.track == edit.track && candidate.clip == edit.clip;
    });
    if (pcm) {
        const auto last = std::max(edit.region.end, edit.region.loop ? edit.region.loop->end : std::int64_t{0});
        const auto lastFrame = pcm->frameForTick(last, prepared.bpm());
        if (pcm->usesPages() && !pcm->pageReader()) return false;
        const bool physical = slot != slots.end() && slot->physicalStartFrame >= 0;
        if (!physical && (lastFrame < 0 || static_cast<std::uint64_t>(lastFrame) > pcm->frames())) return false;
        if (const auto* pages = pcm->pageReader()) {
            if (slot == slots.end() || !slot->liveRegionSupported || edit.region.start < slot->startTick ||
                last > slot->endTick || (!physical && !pages->retainsRange(edit.clip,
                    pcm->frameForTick(edit.region.start,prepared.bpm()),lastFrame))) return false;
        }
    } else if (!prepared.isMidiTrack(edit.track)) return false;
    return driver.launcher_.postRegionEdit(edit,slot != slots.end() ? &*slot : nullptr);
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
                                       std::int64_t clockSample, std::uint64_t compatibleGainRevision,
                                       LaunchAudioDriver* owner)
    : LaunchAudioRuntime(project, std::move(tracks), maxFrames,
        beforeClock ? std::vector<LaunchTempoBank>{{0, std::move(beforeClock)}, {clockSample, project}}
                    : std::vector<LaunchTempoBank>{}, compatibleGainRevision, owner) {}

LaunchAudioRuntime::LaunchAudioRuntime(std::shared_ptr<const LaunchAudioProject> project,
                                       std::vector<std::int64_t> tracks, std::int32_t maxFrames,
                                       std::vector<LaunchTempoBank> banks, std::uint64_t compatibleGainRevision,
                                       LaunchAudioDriver* owner)
    : project_(std::move(project)), tempoBanks_(std::move(banks)),
      tracks_(std::move(tracks)), ranges_(tracks_.size()), maxFrames_(maxFrames),
      compatibleGainRevision_(compatibleGainRevision) {
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
    if (owner && project_->stored()) {
        owner->collectPinnedSourceOwners();
        const auto gather = [&](const LaunchAudioProject& bank) {
            auto& into = pinnedBanks_.emplace_back(PinnedBank{&bank, {}}).candidates;
            for (const auto& slot : bank.view().slots) {
                if (slot.clip == 0 || slot.physicalFrames <= 0) continue;
                const auto* pcm = bank.clip(slot.clip);
                into.push_back({slot, pcm && needsRetainedPages(slot,pcm->rate()) ? owner->ownerFor(*pcm) : nullptr});
            }
        };
        gather(*project_);
        for (const auto* bank : bankRevisions_)
            if (bank != project_.get()) gather(*bank);
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

const LaunchAudioRuntime::Candidate* LaunchAudioRuntime::pinnedCandidate(std::int64_t sample, std::int64_t track,
                                                                        std::int64_t clip) const noexcept {
    return pinnedCandidate(*mediaAt(sample),track,clip);
}

const LaunchAudioRuntime::Candidate* LaunchAudioRuntime::pinnedCandidate(const LaunchAudioProject& media,
                                                                        std::int64_t track, std::int64_t clip) const noexcept {
    if (pinnedBanks_.empty() || !hasTrack(track)) return nullptr;
    const auto* start = pinnedBanks_.data();
    const auto* stop = start + pinnedBanks_.size();
    const auto* bank = std::find_if(start,stop,[&](const PinnedBank& entry) { return entry.media == &media; });
    if (bank == stop || bank->candidates.empty()) return nullptr;
    const auto* first = bank->candidates.data();
    const auto* end = first + bank->candidates.size();
    auto* item = std::lower_bound(first,end,track,[](const Candidate& candidate,std::int64_t id) {
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
    suspendSourceEdits();
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
                                         std::int64_t sample) noexcept {
    struct Context {
        LaunchAudioDriver& driver;
        const LaunchAudioRuntime& before;
        const LaunchAudioRuntime& after;
        std::int64_t sample;
        std::size_t settledRuns;
    };
    const Context context{*this,before,after,sample,retainedCount_};
    (void)launcher_.anyRunningClip(&context,[](const void* opaque, std::int64_t track,
                                              std::int64_t clip, std::uint64_t run) noexcept {
        const auto& call = *static_cast<const Context*>(opaque);
        auto& driver = call.driver;
        if (driver.retainedCount_ == driver.retained_.size() || driver.findRetained(track,call.settledRuns) ||
            !call.before.dropsPinnedPages(&call.after,call.sample,track,clip,true))
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
        if (slot->clip != clip || !needsRetainedPages(*slot,source->rate())) continue;
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
                                               std::int64_t sample, bool externalTempo, bool pinnedPages) const noexcept {
    struct Check {
        const LaunchAudioRuntime& before;
        const LaunchAudioRuntime* after;
        std::int64_t sample;
        bool externalTempo, pinnedPages;
    };
    const Check check{before,after,sample,externalTempo,pinnedPages};
    return launcher_.anyRunningClip(&check,[](const void* opaque, std::int64_t track, std::int64_t clip,
                                              std::uint64_t) noexcept {
        const auto& query = *static_cast<const Check*>(opaque);
        return (query.externalTempo && query.before.dropsWorkerWarp(query.after,query.sample,track,clip)) ||
            (query.pinnedPages && query.before.dropsPinnedPages(query.after,query.sample,track,clip,!query.externalTempo));
    });
}

std::uint64_t LaunchAudioRuntime::gainRevision() const noexcept {
    if (compatibleGainRevision_ != 0) return compatibleGainRevision_;
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

void LaunchAudioRuntime::finish() noexcept {
    active_ = false;
    for (std::size_t index = 0; index < ranges_.size(); ++index) {
        auto& ranges = ranges_.data()[index];
        if (ranges.cuePages) {
            ranges.cuePages->releaseCue(ranges.cueClip);
            ranges.cuePages = nullptr;
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
    driver.beginSourceCallback();
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
        driver.lastMediaBpm_ = 0.0;
        driver.lastMediaRevision_ = 0;
    }
    std::size_t remainingTails = 0;
    for (std::size_t index = 0; index < driver.tailCount_; ++index) {
        auto tail = driver.tails_.data()[index];
        if (addTail(tail, 0, block.frames, capture))
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
    LaunchSink sink{&context, [](void* state, const LaunchSection& section) noexcept {
        auto& call = *static_cast<Context*>(state);
        call.runtime.add(call.driver, *call.media, section, call.block, call.offset, call.capture);
    }, [](void* state, const LaunchTransition& edge) noexcept {
        const auto& call = *static_cast<const Context*>(state);
        const auto* outgoing = call.media;
        if (call.previousMedia && edge.offset == 0 && edge.oldFramesSinceStart > 0)
            outgoing = call.previousMedia;
        else if (edge.sample > 0)
            outgoing = call.runtime.mediaAt(edge.sample - 1).get();
        call.runtime.fadeOut(call.driver, *outgoing, edge, call.offset + edge.offset, call.frames, call.capture);
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
        return call.follow->followOnLaunch(edge, call.view, call.incarnation, call.block.followActionsEnabled, call.seed);
    }, [](void* state, const LaunchTrackStatus& track) noexcept {
        const auto& call = *static_cast<const Context*>(state);
        call.driver.trackSourceStatus(track);
        if (!track.queued.legatoSourceTick) return;
        const auto due = call.block.sampleAtTick(call.block.clock, track.queued.tick);
        const auto& media = *call.runtime.mediaAt(due);
        if (&media != call.media) return;
        const auto* clip = media.clip(track.queued.clip);
        if (clip && clip->followsTempo())
            clip->prefetchWarp(track.queued.tick,*track.queued.legatoSourceTick);
        else if (clip && clip->pageReader()) {
            const auto* rendered = call.runtime.trackRanges(track.track);
            if (rendered && rendered->cuePages == clip->pageReader() && rendered->cueClip == clip->clip()) return;
            const auto frame = track.queued.legatoSourceFrame ? *track.queued.legatoSourceFrame
                : clip->frameForTick(*track.queued.legatoSourceTick,media.bpm());
            clip->pageReader()->prefetch(clip->clip(),frame);
        }
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
    }};
    sink.retargeted = [](void* state, const LaunchSlot& previous, const LaunchSlot& next, std::uint64_t run) noexcept {
        static_cast<Context*>(state)->driver.retargetPinnedRun(previous,next,run);
    };
    sink.admitLegatoCue = [](void* state, const LaunchSlot& target, const LaunchSourceCue& cue,
           std::int32_t offset, bool quantizedHandoff) noexcept {
        auto& call = *static_cast<Context*>(state);
        if (!call.media->stored() || target.physicalLoopEndFrame || target.physicalFrames <=
            3 * ClipPlayback::readAheadFrames(static_cast<std::uint32_t>(call.block.sampleRate))) return true;
        const auto* pcm = call.media->clip(target.clip);
        if (!pcm) return false;
        auto* pages = pcm->pageReader();
        if (!pages) return true;
        if (!quantizedHandoff || cue.physicalFrame < 0) return false;
        auto* rendered = call.runtime.trackRanges(target.track);
        if (!rendered || rendered->cuePages) return false;
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
        if (!pages->reserveCue(target.clip,cue.physicalFrame,remaining)) return false;
        rendered->cuePages = pages;
        rendered->cueClip = target.clip;
        return true;
    };
    sink.prefetchLinkedLegato = [](void* state, const LaunchSlot& target, std::int64_t due, std::int64_t frame) noexcept {
        const auto& call = *static_cast<const Context*>(state);
        if (call.runtime.mediaAt(due).get() != call.media) return;
        const auto* clip = call.media->clip(target.clip);
        if (!clip || !clip->pageReader() || target.physicalFrames <= 3 * ClipPlayback::readAheadFrames(clip->rate())) return;
        const auto* rendered = call.runtime.trackRanges(target.track);
        if (rendered && rendered->cuePages == clip->pageReader() && rendered->cueClip == target.clip) return;
        clip->pageReader()->prefetch(target.clip,frame);
    };
    sink.rawSourceTickAfterBankChange =
        [](void* state, std::int64_t clip, std::int64_t oldAnchor, std::int64_t elapsed) noexcept
            -> std::optional<std::int64_t> {
        const auto& call = *static_cast<const Context*>(state);
        const auto* before = call.previousMedia ? call.previousMedia->clip(clip) : nullptr;
        const auto* after = call.media->clip(clip);
        if (!before || !after || elapsed < 0) return std::nullopt;
        const auto oldFrame = before->frameForTick(oldAnchor, call.previousMedia->bpm());
        if (oldFrame < 0 || oldFrame > std::numeric_limits<std::int64_t>::max() - elapsed) return std::nullopt;
        const auto tick = after->tickForFrame(oldFrame + elapsed, call.media->bpm());
        return tick >= 0 ? std::optional{tick} : std::nullopt;
    };
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
        driver.gains_.begin(block, tracks_, gainRevision(), &driver,
            [](const void* raw, std::int64_t track, std::int64_t clip, std::uint64_t run) noexcept {
                const auto* retained = static_cast<const LaunchAudioDriver*>(raw)->retainedFor(track,clip,run);
                return retained ? retained->gainRevision : std::uint64_t{0};
            });
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
    if (!tail.pinned && !media) return false;
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
        {offset, length, pcm, source, tail.velocity, false, tail.elapsed, warp}, tail.fadeInAtExit};
    if (capture && tail.velocity != 0.0f && tail.fadeInAtExit != 0 && pcm->frames() != 0)
        capture->markFadeTail();
    tail.elapsed += length;
    return tail.elapsed < duration;
}

void LaunchAudioRuntime::fadeOut(LaunchAudioDriver& driver, const LaunchAudioProject& media, const LaunchTransition& edge,
                                 std::int32_t offset, std::int32_t frames, LaunchCaptureBuffer* capture) noexcept {
    if (!edge.oldFade || offset < 0 || offset >= frames) return;
    const auto* retained = driver.retainedFor(edge.track,edge.oldClip,edge.oldRun);
    const auto* declared = retained ? pinnedCandidate(media,edge.track,edge.oldClip) : nullptr;
    auto* pinned = declared && samePinnedShape(retained->slot,declared->slot) ? retained->source : nullptr;
    const auto* pcm = pinned ? &pinned->pcm : media.clip(edge.oldClip);
    if (!pcm) return;
    const auto anchor = edge.oldPhysicalSourceAnchorFrame >= 0 ? edge.oldPhysicalSourceAnchorFrame
        : pcm->frameForTick(edge.oldSourceAnchorTick, media.bpm());
    const auto source = anchor + std::min(edge.oldFramesSinceSourceAnchor,
        std::numeric_limits<std::int64_t>::max() - std::max<std::int64_t>(0, anchor));
    LaunchAudioDriver::Tail tail{edge.track, edge.oldClip, source, 0,
        edge.oldFramesSinceStart, edge.oldVelocity * driver.gains_.currentMultiplier(edge.track, edge.oldRun),
        media.pcmRevision(), {edge.sample,edge.oldClockAnchorTick,edge.oldClockAnchorSourceTick}, pinned};
    if (addTail(tail, offset, frames - offset, capture) && driver.tailCount_ < driver.tails_.size()) {
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
    const auto* pcm = pinned ? &pinned->pcm : media.clip(section.clip);
    const auto replacement = driver.sourceRange(block,section,media.pcmRevision());
    const auto anchor = section.physicalSourceAnchorFrame >= 0 ? section.physicalSourceAnchorFrame
        : (pcm ? pcm->frameForTick(section.sourceAnchorTick, media.bpm()) : 0);
    const auto source = anchor <= std::numeric_limits<std::int64_t>::max() - section.framesSinceSourceAnchor
        ? anchor + section.framesSinceSourceAnchor : std::numeric_limits<std::int64_t>::max();
    const auto fade = section.fade ? section.framesSinceStart : -1;
    const bool tapePcm = capture && capture->capturesPcm() && media.stored() && pcm && pcm->hasDirectPcm() && fade < 0 &&
        section.velocity == 1.0f && source >= 0 &&
        static_cast<std::uint64_t>(source) <= pcm->frames() &&
        static_cast<std::uint64_t>(section.frames) <= pcm->frames() - static_cast<std::uint64_t>(source);
    if (capture && replacement.replacement) capture->markSourceEdit();
    if (capture && !tapePcm && section.clip != 0 && (pinned || section.regionIsLiveEdit ||
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
        const auto first = block.sample + section.offset + skip;
        const LaunchWarpPosition warp{first,section.clockAnchorTick,section.clockAnchorSourceTick};
        ranges.items.data()[ranges.size++] = {base + section.offset + skip, rangeEnd - skip, current, advanced,
            section.velocity * multiplier, !section.regionIsLiveEdit, elapsed, warp};
        if (tapePcm && beforeReplacement) {
            (void)capture->appendPcmRange({section.track, section.clip, section.run, first, first + rangeEnd - skip, advanced,
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
    auto maximum = forBank(*project_);
    for (const auto* bank : bankRevisions_) maximum = std::max(maximum,forBank(*bank));
    return maximum;
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
                          available ? range->sourceFrame + advance : 0, range->velocity, fadeElapsed, warp, range->useAuthoredLoop);
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
                           tail.range.velocity, tail.range.fadeElapsed + advance, tail.fadeInAtExit, warp);
    }
}

} // namespace adi::engine
