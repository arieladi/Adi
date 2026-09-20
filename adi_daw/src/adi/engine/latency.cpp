// SPDX-License-Identifier: GPL-3.0-or-later

#include "adi/engine/latency.hpp"

#include <utility>

namespace adi::engine {

void LatencyCoalescer::addSource(std::string name,
                                 std::function<std::uint64_t()> epoch) {
    Source s;
    s.name = std::move(name);
    s.epoch = std::move(epoch);
    // SEEDED, not zeroed. A device that has already reported once before it was
    // registered would otherwise look like a fresh report the moment it joined,
    // so adding a plugin to a project would retap the graph for no reason.
    s.seen = s.epoch ? s.epoch() : 0;
    sources_.push_back(std::move(s));
}

void LatencyCoalescer::clearSources() {
    sources_.clear();
    pending_ = false;
    lastReporter_.clear();
}

bool LatencyCoalescer::poll(std::int64_t nowMs) {
    if (graph_ == nullptr) return false;

    // --- 1. sample everything, once ----------------------------------------
    //
    // Every source is read on every poll even after one has changed. Stopping
    // early would leave the others' `seen` stale, so their reports would be
    // attributed to the NEXT burst and counted twice.
    bool changed = false;
    for (Source& s : sources_) {
        const std::uint64_t e = s.epoch ? s.epoch() : 0;
        if (e != s.seen) {
            s.seen = e;
            changed = true;
            ++stats_.reports;
            lastReporter_ = s.name;
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

    // ADR-0079: this moves the taps that fit and reports when one does not.
    // A false is not an error to swallow -- it is the signal that this change
    // needs new buffers, and only a rebuild off-thread can supply them.
    if (!graph_->retapLatency()) {
        rebuildNeeded_ = true;
        ++stats_.rebuildsNeeded;
    }
    return true;
}

}  // namespace adi::engine
