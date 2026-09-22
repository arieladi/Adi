// SPDX-License-Identifier: GPL-3.0-or-later

#include "adi/engine/latency.hpp"

#include "adi/engine/host.hpp"

#include <utility>

namespace adi::engine {

void LatencyCoalescer::addSource(std::string name,
                                 std::function<std::uint64_t()> epoch, Kind kind) {
    Source s;
    s.name = std::move(name);
    s.epoch = std::move(epoch);
    s.kind = kind;
    // SEEDED, not zeroed. A device that has already reported once before it was
    // registered would otherwise look like a fresh report the moment it joined,
    // so adding a plugin to a project would retap the graph for no reason.
    s.seen = s.epoch ? s.epoch() : 0;
    sources_.push_back(std::move(s));
}

void LatencyCoalescer::clearSources() {
    sources_.clear();
    pending_ = false;
    burstNeedsRebuild_ = false;
    lastReporter_.clear();
}

Graph* LatencyCoalescer::target() const noexcept {
    if (graph_ != nullptr) return graph_;
    return host_ != nullptr ? host_->currentGraph() : nullptr;
}

bool LatencyCoalescer::poll(std::int64_t nowMs) {
    // RESOLVED ONCE PER POLL, never stored. A rebuild between two polls
    // retires the graph this would otherwise be holding.
    Graph* const graph = target();
    if (graph == nullptr) return false;

    // --- 1. sample everything, once ----------------------------------------
    //
    // Every source is read on every poll even after one has changed. Stopping
    // early would leave the others' `seen` stale, so their reports would be
    // attributed to the NEXT burst and counted twice.
    // RECLAIM FIRST, AND UNCONDITIONALLY. This sat at the bottom of the
    // function to begin with, after every early return -- so an old ring was
    // only ever freed on a poll that also retapped, and a graph that settled
    // and went quiet held its retired buffers until the next plugin happened
    // to report. Collection has nothing to do with whether anything changed.
    stats_.ringsReclaimed += static_cast<std::int64_t>(graph->collectRings());

    bool changed = false;
    for (Source& s : sources_) {
        const std::uint64_t e = s.epoch ? s.epoch() : 0;
        if (e != s.seen) {
            s.seen = e;
            changed = true;
            ++stats_.reports;
            lastReporter_ = s.name;
            if (s.kind == Kind::Shape) {
                burstNeedsRebuild_ = true;
                ++stats_.shapeReports;
            }
        }
    }

    // --- 2. a report opens or extends a burst -------------------------------
    if (changed) {
        if (!pending_) {
            pending_ = true;
            firstAt_ = nowMs;
            ++stats_.bursts;
        }
        lastAt_ = nowMs;
    }
    if (!pending_) return false;

    // --- 3. act when it has gone quiet, or when it has gone on too long -----
    //
    // No special case for "a report arrived on this very poll": with the
    // default quiet period it cannot be quiet yet, and with a quiet period of
    // zero acting immediately is exactly what zero should mean.
    const bool quiet = (nowMs - lastAt_) >= quietMs_;
    const bool tooLong = maxWaitMs_ > 0 && (nowMs - firstAt_) >= maxWaitMs_;
    if (!quiet && !tooLong) return false;
    if (!quiet) ++stats_.maxWaitTrips;

    pending_ = false;
    ++stats_.retaps;

    // ADR-0084: a shape change is not a number that moved, it is a different
    // graph. Retapping still runs -- a partial correction is closer to right
    // than none, and the rebuild may be a frame away -- but the flag is raised
    // regardless of whether the retap happens to succeed.
    const bool shapeMoved = burstNeedsRebuild_;
    burstNeedsRebuild_ = false;
    if (shapeMoved) {
        rebuildNeeded_ = true;
        ++stats_.rebuildsNeeded;
    }

    // ADR-0079: this moves the taps that fit and reports when one does not.
    // A false is not an error to swallow -- it is the signal that this change
    // needs new buffers, and only a rebuild off-thread can supply them.
    if (!graph->retapLatency()) {
        // ADR-0085. An edge wants more delay than its ring holds, so grow that
        // ring -- here, on the message thread, where allocating is allowed.
        // Per EDGE and not per graph: the others keep their history.
        const std::size_t grown = autoEscalate_ ? graph->escalateLatency() : 0;
        if (grown > 0) {
            stats_.escalations += static_cast<std::int64_t>(grown);
        } else if (!shapeMoved) {
            // Nothing could be grown, so this is not a size problem. The
            // topology changed under us and only a rebuild will do. Guarded on
            // `shapeMoved` so a burst that already raised the flag is not
            // counted twice.
            rebuildNeeded_ = true;
            ++stats_.rebuildsNeeded;
        }
    }

    return true;
}

}  // namespace adi::engine
