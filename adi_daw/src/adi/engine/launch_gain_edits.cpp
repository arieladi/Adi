// SPDX-License-Identifier: AGPL-3.0-or-later
#include "adi/engine/launch_gain_edits.hpp"

#include "adi/engine/launcher.hpp"

#include <algorithm>
#include <cmath>
#include <functional>
#include <limits>
#include <utility>

namespace adi::engine {
LaunchClipGainEdits::LaunchClipGainEdits(std::size_t maximumTracks)
    : tracks_(maximumTracks), scratch_(maximumTracks), active_(tracks_.data()), spare_(scratch_.data()) {}

bool LaunchClipGainEdits::post(const LaunchClipGainEdit& edit, std::uint64_t pcmRevision) noexcept {
    if (edit.track <= 0 || edit.clip <= 0 || edit.incarnation == 0 || edit.run == 0 ||
        !std::isfinite(edit.multiplier) || edit.multiplier < 0 || edit.rate > ClipUpdateRate::Bar)
        return false;
    const auto written = written_.load(std::memory_order_relaxed);
    if (written - read_.load(std::memory_order_acquire) == capacity)
        return false;
    mailbox_[static_cast<std::size_t>(written % capacity)] = {edit, pcmRevision};
    written_.store(written + 1, std::memory_order_release);
    return true;
}

LaunchClipGainEdits::Track* LaunchClipGainEdits::findTrack(std::int64_t id) noexcept {
    const auto active = std::span<Track>(active_, count_);
    const auto track = std::ranges::lower_bound(active, id, {}, &Track::id);
    return track == active.end() || track->id != id ? nullptr : &*track;
}

void LaunchClipGainEdits::begin(const LaunchBlock& block, std::span<const std::int64_t> ids,
                               std::uint64_t pcmRevision, const void* retainedContext,
                               RetainedRevision retainedRevision) noexcept {
    const auto heldRevision = [&](const Track& track) {
        return retainedRevision ? retainedRevision(retainedContext,track.id,track.clip,track.run) : std::uint64_t{0};
    };
    if (ids.size() > tracks_.size() || block.frames <= 0 || block.sample < 0 ||
        block.sample > std::numeric_limits<std::int64_t>::max() - block.frames) {
        count_ = 0;
        nextSample_ = -1;
        return;
    }
    if (incarnation_ != block.incarnation) {
        count_ = 0;
        nextSample_ = -1;
        incarnation_ = block.incarnation;
    } else if (pcmRevision_ != pcmRevision) {
        // The PCM baseline changed but the launcher may retain the same run.
        // Keep its literal address so commands for the new baseline can queue
        // before its first callback. The old baseline's edits have expired.
        for (auto& track : std::span(active_, count_)) {
            if (heldRevision(track) != 0) continue;
            track.multiplier = 1.0f;
            track.pending.reset();
        }
    }
    pcmRevision_ = pcmRevision;
    if (!std::ranges::equal(ids, std::span(active_, count_), {}, std::identity{}, &Track::id)) {
        std::size_t old = 0, next = 0;
        for (const auto id : ids) {
            while (old < count_ && active_[old].id < id)
                ++old;
            spare_[next++] = old < count_ && active_[old].id == id
                ? active_[old++] : Track{.id = id, .pending = {}};
        }
        std::swap(active_, spare_);
        count_ = next;
    }
    if (nextSample_ != block.sample) {
        for (auto& track : std::span(active_, count_))
            if (track.pending)
                track.pending->boundary.reset();
    }
    nextSample_ = block.sample + block.frames;

    auto read = read_.load(std::memory_order_relaxed);
    const auto written = written_.load(std::memory_order_acquire);
    while (read != written) {
        const auto& queued = mailbox_[static_cast<std::size_t>(read++ % capacity)];
        const auto& edit = queued.edit;
        auto* track = findTrack(edit.track);
        const auto oldRevision = track ? heldRevision(*track) : std::uint64_t{0};
        if (track && (queued.pcmRevision == pcmRevision_ || (oldRevision != 0 && queued.pcmRevision == oldRevision)) &&
            edit.incarnation == incarnation_ &&
            edit.run == track->run && edit.clip == track->clip)
            track->pending = Pending{edit, {}};
    }
    read_.store(read, std::memory_order_release);
}

LaunchClipGainRange LaunchClipGainEdits::range(const LaunchBlock& block,
                                              const LaunchSection& section) noexcept {
    LaunchClipGainRange plan{section.frames, 1.0f, 1.0f};
    auto* track = findTrack(section.track);
    if (!track)
        return plan;
    if (section.run != track->run || section.clip != track->clip) {
        track->run = section.run;
        track->clip = section.clip;
        track->multiplier = 1.0f;
        track->pending.reset();
    }
    if (section.arrangement || section.clip <= 0 || section.frames <= 0)
        return plan;
    plan.before = plan.after = track->multiplier;
    if (!track->pending)
        return plan;

    auto& pending = *track->pending;
    const auto start = block.sample + section.offset;
    if (!pending.boundary)
        pending.boundary = nextClipUpdateBoundary(block, start, pending.edit.rate, true);
    if (!pending.boundary)
        return plan;
    const auto due = pending.edit.rate == ClipUpdateRate::None || !block.sampleAtTick
        ? pending.boundary->sample : block.sampleAtTick(block.clock, pending.boundary->tick);
    if (due >= 0 && due < start + section.frames) {
        plan.beforeFrames = static_cast<std::int32_t>(std::max<std::int64_t>(0, due - start));
        plan.after = pending.edit.multiplier;
        track->multiplier = plan.after;
        track->pending.reset();
    }
    return plan;
}

void LaunchClipGainEdits::retargetRun(std::int64_t id, std::int64_t previousClip, std::int64_t clip,
                                     std::uint64_t run) noexcept {
    auto* track = findTrack(id);
    if (!track || track->clip != previousClip || track->run != run) return;
    track->clip = clip;
    if (track->pending) track->pending->edit.clip = clip;
}

float LaunchClipGainEdits::currentMultiplier(std::int64_t trackId, std::uint64_t run) noexcept {
    const auto* track = findTrack(trackId);
    return track && track->run == run ? track->multiplier : 1.0f;
}
} // namespace adi::engine
