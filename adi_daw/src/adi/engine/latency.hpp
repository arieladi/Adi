// SPDX-License-Identifier: GPL-3.0-or-later
//
// The latency coalescer — ADR-0066 decision 2, on top of ADR-0079's tap move.
//
// A plugin that changes its reported latency tells us so by bumping a counter.
// This turns a stream of those bumps into at most one `Graph::retapLatency()`,
// on a thread we chose, at a time we chose.
//
// THREE CONSTRAINTS SHAPE THE WHOLE DESIGN, and each of them rules out the
// obvious alternative:
//
//   1. **The producer has no thread affinity.** CLAP's `request_restart` and
//      VST3's `audioProcessorChanged` are called from whatever thread the
//      plugin picked — a worker, its own GUI thread, or the audio thread. So
//      the reporters do one thing: bump an atomic and return. Anything
//      callback-driven would run our recompute on the plugin's thread while it
//      waits, which is the bug ADR-0066 exists to prevent.
//
//      That is why this POLLS. Polling is usually the lazy answer; here it is
//      the only one that keeps the work on a thread we control, and the cost
//      is one relaxed atomic load per device per tick.
//
//   2. **A burst is not a decision.** A plugin switching to linear phase can
//      report several times in a few milliseconds. Acting per report builds
//      schedules nobody uses. So a report starts a quiet period and the action
//      happens when it ends.
//
//   3. **A quiet period that never ends must not starve.** Some plugins report
//      on every block. A pure debounce would then never fire, and the graph
//      would stay wrong forever while looking busy. `maxWaitMs` bounds it: a
//      burst is acted on after that long whether or not it has gone quiet.
//
// THE CLOCK IS AN ARGUMENT, not a call to `std::chrono` inside. A coalescer
// that reads the clock itself can only be tested by sleeping, and a test that
// sleeps is a test that is slow, flaky on a loaded CI box, and unable to
// exercise the boundary it cares about. `poll(nowMs)` lets a test step time by
// exactly one millisecond either side of the threshold.

#pragma once

#include "adi/engine/graph.hpp"

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace adi::engine {

class GraphHost;

/// Turns bumps from any number of reporters into at most one retap.
///
/// `poll`, `addSource`, `attach` and the setters are MESSAGE THREAD. The
/// callables handed to `addSource` are invoked on that thread too, so a
/// reporter's getter must be safe to call there while the plugin writes from
/// somewhere else — which for an atomic counter it is, and for a plain one it
/// is not.
class LatencyCoalescer {
public:
    struct Stats {
        std::int64_t reports = 0;         ///< epoch changes observed
        std::int64_t bursts = 0;          ///< distinct quiet periods started
        std::int64_t retaps = 0;          ///< times the graph was retapped
        std::int64_t rebuildsNeeded = 0;  ///< misfits nothing could fix
        std::int64_t maxWaitTrips = 0;    ///< bursts cut short by the ceiling
        std::int64_t escalations = 0;     ///< edges handed a bigger ring (ADR-0085)
        std::int64_t ringsReclaimed = 0;  ///< old rings freed on the message thread
        std::int64_t shapeReports = 0;    ///< reports that were a topology change
    };

    /// What a source's counter MEANS when it moves. ADR-0084: CLAP says why it
    /// wants a restart, through `clap_host_latency.changed` and
    /// `clap_host_audio_ports.rescan`, and treating those two the same throws
    /// the distinction away.
    enum class Kind {
        /// The node's latency moved and nothing else. This is the cheap path:
        /// re-read, move the taps, grow a ring if one is too small.
        Latency,
        /// The node's SHAPE moved -- ports, channel counts -- or it asked for a
        /// restart without saying why. No amount of retapping fixes a topology
        /// change, so this escalates straight to a rebuild.
        Shape,
    };

    /// Attach to one graph directly. Fine for a test or an offline render,
    /// and WRONG for a session that can rebuild: the pointer is dangling the
    /// moment `GraphHost::collect()` frees the graph it names.
    void attach(Graph& g) noexcept { graph_ = &g; host_ = nullptr; }

    /// Attach to the HOST, which is what a live session does. The graph is
    /// re-read on every poll, so a rebuild between two polls is invisible here
    /// -- where storing the graph once would be a use-after-free the first
    /// time a plugin rescanned its ports.
    void attach(GraphHost& h) noexcept { host_ = &h; graph_ = nullptr; }

    /// `epoch` returns a counter that only ever increases. The name is for
    /// diagnostics: "which plugin keeps doing this" is the first question
    /// anyone asks when a session retaps every second.
    void addSource(std::string name, std::function<std::uint64_t()> epoch,
                   Kind kind = Kind::Latency);

    /// Changing the SET of sources is a rebuild, not a retap: the graph's
    /// topology changed, so its rings have to be sized again anyway.
    void clearSources();
    [[nodiscard]] std::size_t sourceCount() const noexcept { return sources_.size(); }

    /// How long a burst must be quiet before it is acted on. Zero means act on
    /// the first poll that sees a change, which is the right answer for an
    /// offline render and the wrong one for a live session.
    void setQuietPeriodMs(std::int64_t ms) noexcept { quietMs_ = ms > 0 ? ms : 0; }

    /// The ceiling on a single burst. A plugin reporting continuously is
    /// otherwise never quiet, and the compensation stays stale indefinitely.
    void setMaxWaitMs(std::int64_t ms) noexcept { maxWaitMs_ = ms > 0 ? ms : 0; }

    [[nodiscard]] std::int64_t quietPeriodMs() const noexcept { return quietMs_; }
    [[nodiscard]] std::int64_t maxWaitMs() const noexcept { return maxWaitMs_; }

    /// Drive it, from a timer on the message thread. `nowMs` is a monotonic
    /// millisecond clock the CALLER owns.
    ///
    /// Returns true when a retap ran on this call. Never throws, never blocks,
    /// and never touches a reporter's object beyond calling its getter.
    bool poll(std::int64_t nowMs);

    /// A burst is open and has not been acted on yet.
    [[nodiscard]] bool pending() const noexcept { return pending_; }

    /// ADR-0085: when a retap reports a misfit, grow that edge's ring rather
    /// than giving up. On by default, because it is the only sensible response
    /// to "this delay does not fit" and there is no second option to weigh.
    /// The switch exists so an offline render -- which has no real-time
    /// constraint and can simply rebuild from the top -- can decline the
    /// machinery entirely.
    void setAutoEscalate(bool v) noexcept { autoEscalate_ = v; }
    [[nodiscard]] bool autoEscalate() const noexcept { return autoEscalate_; }

    /// A misfit that growing could NOT fix, which means it was never a size
    /// problem: the topology changed under us and only a rebuild will do.
    /// Sticky until cleared, because whoever rebuilds is not necessarily
    /// whoever polls.
    [[nodiscard]] bool rebuildNeeded() const noexcept { return rebuildNeeded_; }
    void clearRebuildNeeded() noexcept { rebuildNeeded_ = false; }

    [[nodiscard]] const Stats& stats() const noexcept { return stats_; }

    /// The last source whose epoch moved.
    [[nodiscard]] const std::string& lastReporter() const noexcept { return lastReporter_; }

private:
    struct Source {
        std::string name;
        std::function<std::uint64_t()> epoch;
        std::uint64_t seen = 0;
        Kind kind = Kind::Latency;
    };

    /// Whichever graph this poll should act on: the one attached directly, or
    /// the host's current one. Never stored.
    [[nodiscard]] Graph* target() const noexcept;

    Graph* graph_ = nullptr;
    GraphHost* host_ = nullptr;
    std::vector<Source> sources_;
    Stats stats_;
    std::string lastReporter_;

    std::int64_t quietMs_ = 50;      ///< long enough to swallow a mode switch
    std::int64_t maxWaitMs_ = 500;   ///< short enough that stale is brief
    std::int64_t firstAt_ = 0;       ///< when the open burst started
    std::int64_t lastAt_ = 0;        ///< when it last saw a report
    bool pending_ = false;
    bool burstNeedsRebuild_ = false;
    bool autoEscalate_ = true;
    bool rebuildNeeded_ = false;
};

}  // namespace adi::engine
