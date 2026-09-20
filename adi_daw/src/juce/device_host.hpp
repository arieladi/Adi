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

#include "adi/engine/latency.hpp"
#include "juce/clap_host.hpp"
#include "juce/device_model.hpp"

#include <cstdint>
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

    /// Point the coalescer at the live graph. Called again after every swap,
    /// because the old one is gone and a coalescer holding it would retap a
    /// graph nobody is rendering.
    void attachGraph(engine::Graph& g) noexcept;

    /// Take ownership of a device and wire its reports.
    ///
    /// Returns the node, which the caller connects into the graph. The node
    /// is owned here too: it must outlive the graph that references it
    /// (ADR-0042 decision 5 — a device change must not disturb the project).
    DeviceNode& add(std::unique_ptr<DeviceInstance> dev, std::string name);

    /// A CLAP device reports through its host glue rather than through
    /// itself, so the glue is registered once and its three counters become
    /// three sources with the right Kinds (ADR-0084).
    void watchClapGlue(ClapHostGlue& glue, std::string name);

    /// MESSAGE THREAD, called from a timer. `nowMs` is any monotonic
    /// millisecond clock; the coalescer only ever subtracts two of them.
    ///
    /// Returns true when the graph was retapped this tick.
    bool tick(std::int64_t nowMs);

    [[nodiscard]] engine::LatencyCoalescer& coalescer() noexcept { return coalescer_; }
    [[nodiscard]] const engine::LatencyCoalescer& coalescer() const noexcept {
        return coalescer_;
    }

    [[nodiscard]] std::size_t deviceCount() const noexcept { return devices_.size(); }
    [[nodiscard]] DeviceInstance& deviceAt(std::size_t i) { return *devices_[i]; }
    [[nodiscard]] DeviceNode& nodeAt(std::size_t i) { return *nodes_[i]; }

    /// Drain every CLAP plugin's deferred main-thread work. Called from the
    /// same tick, because it is the same thread and the same cadence, and a
    /// second timer for it would be a second thing to forget to start.
    void dispatchPluginCallbacks();

private:
    std::vector<std::unique_ptr<DeviceInstance>> devices_;
    std::vector<std::unique_ptr<DeviceNode>> nodes_;
    std::vector<ClapHostGlue*> glues_;
    engine::LatencyCoalescer coalescer_;
};

}  // namespace adi::device
