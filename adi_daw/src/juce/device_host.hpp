// SPDX-License-Identifier: GPL-3.0-or-later
//
// Who owns the devices, and who ticks the latency coalescer.
//
// ADR-0082 built the coalescer and left two questions open: what calls
// `poll()`, and where the thing lives relative to the graph and the device
// list. This answers both, and the answer follows from lifetimes rather than
// from taste.
//
// A `Graph` is REPLACED — ADR-0019 publishes a new one and swaps by atomic
// pointer, and ADR-0085 rebuilds one when a shape report arrives. A device
// OUTLIVES any particular graph, because the project owns the device and the
// graph is a rendering of the project. And the coalescer watches devices and
// acts on whichever graph is current.
//
// So the coalescer cannot live in the `Graph` (wrong lifetime — it would be
// destroyed by the swap it exists to cause) and it cannot live in a device
// (it is one-per-engine, not one-per-plugin). It lives HERE, beside the
// device list, and `attachGraph` points it at whichever graph is live.
//
// NO JUCE. The tick is a plain call taking a millisecond clock, so the logic
// runs on all seven ABIs and the only JUCE-shaped part — a timer that calls
// it — is three lines in the bridge. Same split as `device_core.hpp`, for the
// same reason: a test that needs a message loop is a test that runs in one
// CI job.

#pragma once

#include "adi/engine/host.hpp"
#include "adi/engine/latency.hpp"
#include "adi/engine/realize.hpp"
#include "juce/clap_host.hpp"
#include "juce/device_model.hpp"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace adi::device {

/// Owns the loaded devices and the one coalescer that watches them.
///
/// Message thread only. Nothing here is called from the audio thread — the
/// audio thread sees `DeviceNode`s through the graph and never this.
class DeviceHost {
public:
    DeviceHost();

    /// Point the coalescer at ONE graph. Fine for a test that never
    /// rebuilds; wrong for a session that can, because `GraphHost::collect`
    /// frees the graph this names. Use `attachHost` for anything live.
    void attachGraph(engine::Graph& g) noexcept;

    /// What a rebuild needs, re-read every time rather than captured once.
    ///
    /// `model` is a CALLBACK and not a `const rows::Model*`, and that is the
    /// same lesson twice. A coalescer storing a `Graph*` dangled the first
    /// time the graph was replaced; a device host storing a `Model*` would
    /// dangle the first time the projection was re-read after an edit, which
    /// is every edit. Whatever outlives the thing it points at has to ask
    /// again.
    ///
    /// Returning nullptr means "no model to build from", which is refused
    /// rather than crashed: a session that has not opened a project yet is a
    /// real state, not a bug.
    struct RebuildSpec {
        std::function<const rows::Model*()> model;
        std::int32_t channels = 2;
        double sampleRate = 48000.0;
        std::int32_t maxFrames = 512;
        /// ADR-0122: a session places devices from the ROWS, so it supplies
        /// the chain itself. Unset, `chainSupplier()` places by the track id
        /// given to `add` (ADR-0090's behaviour, and every test of it).
        engine::DeviceChainFn devicesFor;
        /// What pushes into a track's junction (ADR-0122). Unset, nothing.
        engine::SourceFn sourcesFor;
    };

    /// Attach to the HOST that owns the live graph, and take responsibility
    /// for answering `rebuildNeeded()`.
    ///
    /// After this, `tick` closes the loop ADR-0085 opened: poll, rebuild if a
    /// shape report demanded one, collect. The coalescer re-reads the host's
    /// current graph on every poll, so nothing has to re-attach after a swap.
    void attachHost(engine::GraphHost& host, RebuildSpec spec);

    /// Realise and publish a new graph from the current model, now. Returns
    /// false with `lastRebuildError()` set.
    ///
    /// The caller uses this for the FIRST graph — there is nothing to detect
    /// before a session has a graph at all — and after a model edit. `tick`
    /// uses it for a shape report. Message thread.
    bool rebuildNow();

    /// The chain supplier to hand `realize()`. Bound to this host, so a
    /// rebuild re-injects the DEVICES THAT ARE ALREADY LOADED rather than
    /// reloading the plugins (ADR-0042 decision 5).
    ///
    /// The returned callable holds `this`. A `DeviceHost` outliving every
    /// graph it supplies is the whole premise of the file, so that is a
    /// statement of the invariant rather than a capture to worry about.
    [[nodiscard]] engine::DeviceChainFn chainSupplier() noexcept;

    /// The devices placed on one track, in signal order. Empty for a track
    /// with none, which is most of them.
    [[nodiscard]] std::vector<engine::Node*> chainFor(std::int64_t trackId) const;

    /// Take ownership of a device and wire its reports.
    ///
    /// Returns the node, which the caller connects into the graph. The node
    /// is owned here too: it must outlive the graph that references it
    /// (ADR-0042 decision 5 — a device change must not disturb the project).
    /// `trackId` places the device at the end of that track's chain, so a
    /// rebuild puts it back where it was. **0 means unplaced** — the host
    /// owns it and watches it, and no graph contains it. Track ids come from
    /// a SQLite `INTEGER PRIMARY KEY` and start at 1, so 0 cannot collide
    /// with a real track.
    DeviceNode& add(std::unique_ptr<DeviceInstance> dev, std::string name,
                    std::int64_t trackId = 0);

    /// A CLAP device reports through its host glue rather than through
    /// itself, so the glue is registered once and its three counters become
    /// three sources with the right Kinds (ADR-0084).
    void watchClapGlue(ClapHostGlue& glue, std::string name);

    /// MESSAGE THREAD, called from a timer. `nowMs` is any monotonic
    /// millisecond clock; the coalescer only ever subtracts two of them.
    ///
    /// Returns true when the graph was retapped this tick. A REBUILD is not
    /// a retap and does not set it; `stats().rebuilds` counts those.
    bool tick(std::int64_t nowMs);

    struct Stats {
        std::int64_t rebuilds = 0;        ///< shape reports answered
        std::int64_t rebuildsFailed = 0;  ///< ...and refused by realise or prepare
    };
    [[nodiscard]] const Stats& stats() const noexcept { return stats_; }
    [[nodiscard]] const std::string& lastRebuildError() const noexcept {
        return lastError_;
    }

    [[nodiscard]] engine::LatencyCoalescer& coalescer() noexcept { return coalescer_; }
    [[nodiscard]] const engine::LatencyCoalescer& coalescer() const noexcept {
        return coalescer_;
    }

    [[nodiscard]] std::size_t deviceCount() const noexcept { return devices_.size(); }
    [[nodiscard]] DeviceInstance& deviceAt(std::size_t i) { return *devices_[i].device; }
    [[nodiscard]] DeviceNode& nodeAt(std::size_t i) { return *devices_[i].node; }
    [[nodiscard]] std::int64_t trackAt(std::size_t i) const { return devices_[i].trackId; }

    /// Drain every CLAP plugin's deferred main-thread work. Called from the
    /// same tick, because it is the same thread and the same cadence, and a
    /// second timer for it would be a second thing to forget to start.
    void dispatchPluginCallbacks();

private:
    struct Placed {
        std::unique_ptr<DeviceInstance> device;
        std::unique_ptr<DeviceNode> node;
        std::int64_t trackId = 0;
    };

    std::vector<Placed> devices_;
    std::vector<ClapHostGlue*> glues_;
    engine::LatencyCoalescer coalescer_;

    engine::GraphHost* host_ = nullptr;
    RebuildSpec spec_;
    Stats stats_;
    std::string lastError_;
};

}  // namespace adi::device
