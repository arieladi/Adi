// SPDX-License-Identifier: AGPL-3.0-or-later
#include "adi/engine/launch_audio_runtime.hpp"

#include "adi/engine/graph.hpp"
#include "adi/engine/launch_follow_playback.hpp"
#include "adi/engine/transport.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <unordered_set>
#include <utility>

namespace adi::engine {
namespace {

bool positiveAscending(const std::vector<std::int64_t>& ids) {
    return (ids.empty() || ids.front() > 0) &&
        std::is_sorted(ids.begin(), ids.end()) &&
        std::adjacent_find(ids.begin(), ids.end()) == ids.end();
}

} // namespace

LaunchAudioProject::LaunchAudioProject(std::uint64_t incarnation, double bpm, std::vector<std::int64_t> tracks,
                                       std::vector<LaunchSlot> slots, std::vector<std::int64_t> scenes,
                                       std::vector<LaunchAudioClip> clips, LaunchQuantization global,
                                       std::shared_ptr<const LaunchFollowPlayer> follow, std::uint64_t followSeed)
    : incarnation_(incarnation), bpm_(bpm), tracks_(std::move(tracks)), scenes_(std::move(scenes)),
      slots_(std::move(slots)), clips_(std::move(clips)), global_(global), follow_(std::move(follow)),
      followSeed_(followSeed) {
    if (incarnation_ == 0 || !std::isfinite(bpm_) || bpm_ < 20.0 || bpm_ > 999.0 ||
        !isLaunchQuantizationCode(static_cast<std::int64_t>(global_)) ||
        !positiveAscending(tracks_) || !positiveAscending(scenes_))
        throw std::invalid_argument("invalid Session audio project clock, tracks or scenes");

    const auto lessClip = [](const LaunchAudioClip& a, const LaunchAudioClip& b) {
        return a.clip() < b.clip();
    };
    std::sort(clips_.begin(), clips_.end(), lessClip);
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
            (slot.clip > 0 && (invalidPlayback || !clip(slot.clip) || !occupied.insert(slot.clip).second)))
            throw std::invalid_argument("invalid Session audio slot or missing prepared PCM");
        previous = key;
    }
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
    for (auto& track : ranges_)
        track.items.resize(static_cast<std::size_t>(maxFrames));
}

void LaunchAudioRuntime::begin(LaunchAudioDriver& driver, LaunchBlock block,
                               const Transport& transport, std::int64_t streamSample,
                               void* observerContext,
                               void (*observer)(void*, const LaunchTransition&, std::int64_t) noexcept) noexcept {
    for (std::size_t index = 0; index < ranges_.size(); ++index)
        ranges_.data()[index].size = 0;
    const auto frames = block.frames;
    active_ = frames > 0 && frames <= maxFrames_ && transport.playing();
    if (active_ && (!driver.wasPlaying_ || transport.streamedFrames() == 0))
        ++driver.pass_;
    driver.wasPlaying_ = active_;
    if (!active_)
        return;
    block.incarnation = project_->incarnation();
    block.global = project_->globalQuantization();
    block.followPass = driver.pass_;
    const auto* follow = project_->followPlayer();
    block.followActionsEnabled = follow != nullptr;
    auto view = project_->view();
    view.tracks = tracks_;
    struct Context {
        LaunchAudioRuntime& runtime;
        LaunchView view;
        const LaunchFollowPlayer* follow;
        std::uint64_t incarnation, seed;
        std::int64_t streamSample;
        void* observerContext;
        void (*observer)(void*, const LaunchTransition&, std::int64_t) noexcept;
        std::int32_t offset = 0;
    };
    if (streamSample < 0 || streamSample > std::numeric_limits<std::int64_t>::max() - frames)
        observer = nullptr;
    Context context{*this, view, follow, block.incarnation, followPassSeed(project_->followSeed(), driver.pass_),
        streamSample, observerContext, observer};
    const LaunchSink sink{&context, [](void* state, const LaunchSection& section) noexcept {
        auto& call = *static_cast<Context*>(state);
        call.runtime.add(section, call.offset);
    }, [](void* state, const LaunchTransition& edge) noexcept {
        const auto& call = *static_cast<const Context*>(state);
        if (call.observer) {
            auto logicalEdge = edge;
            logicalEdge.offset += call.offset;
            call.observer(call.observerContext, logicalEdge, call.streamSample + logicalEdge.offset);
        }
    }, [](void* state, const LaunchTransition& edge) noexcept -> std::optional<LaunchFollowRequest> {
        const auto& call = *static_cast<const Context*>(state);
        if (!call.follow) return std::nullopt;
        return call.follow->followOnLaunch(edge, call.view, call.incarnation, true, call.seed);
    }};
    while (context.offset < frames) {
        block.sample = transport.sampleAt(context.offset);
        block.frames = static_cast<std::int32_t>(transport.contiguousFrames(context.offset, frames - context.offset));
        if (block.frames <= 0 || !driver.launcher_.process(block, view, sink))
            break;
        context.offset += block.frames;
    }
}

void LaunchAudioRuntime::add(const LaunchSection& section, std::int32_t base) noexcept {
    if (section.arrangement || section.frames <= 0 || tracks_.empty())
        return;
    const auto* ids = tracks_.data();
    const auto* end = ids + tracks_.size();
    const auto* track = std::lower_bound(ids, end, section.track);
    if (track == end || *track != section.track)
        return;
    auto& ranges = ranges_.data()[track - ids];
    const auto* pcm = project_->clip(section.clip);
    const auto anchor = pcm ? pcm->frameForTick(section.sourceAnchorTick, project_->bpm()) : 0;
    const auto source = anchor <= std::numeric_limits<std::int64_t>::max() - section.framesSinceSourceAnchor
        ? anchor + section.framesSinceSourceAnchor : std::numeric_limits<std::int64_t>::max();
    if (ranges.size < ranges.items.size())
        ranges.items.data()[ranges.size++] = {base + section.offset, section.frames, pcm, source, section.velocity,
            section.fade ? section.framesSinceStart : -1};
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
    if (ranges.size == 0)
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
}

} // namespace adi::engine
