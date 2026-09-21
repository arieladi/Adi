// SPDX-License-Identifier: GPL-3.0-or-later
//
// See device_host.hpp for why the coalescer lives here and not in the graph.

#include "juce/device_host.hpp"

#include <utility>

namespace adi::device {

DeviceHost::DeviceHost() = default;

void DeviceHost::attachGraph(engine::Graph& g) noexcept { coalescer_.attach(g); }

DeviceNode& DeviceHost::add(std::unique_ptr<DeviceInstance> dev, std::string name,
                            std::int64_t trackId) {
    Placed p;
    p.device = std::move(dev);
    DeviceInstance* inst = p.device.get();
    p.node = std::make_unique<DeviceNode>(*inst);
    p.trackId = trackId;
    devices_.push_back(std::move(p));

    // Registered through the CONTRACT, not by asking what format this is.
    // The first version of this dynamic_cast'd to Vst3Device, which is a
    // format-agnostic host sniffing for a format -- ADR-0052 decision 4's
    // exact failure. The compiler caught it: the cast needs a JUCE header in
    // a file that has none.
    //
    // A VST3 returns a real epoch here because JUCE's audioProcessorChanged
    // carries a flag word and the device itself sees it. A CLAP device
    // returns 0 and reports through its host glue instead, because
    // clap_host_latency.changed is a HOST callback (ADR-0084) -- which is
    // why watchClapGlue exists and why this registers nothing for one.
    // EVERY device, unconditionally. One that never reports keeps returning
    // 0, the coalescer sees no change and does nothing -- which costs one
    // lambda call per poll and removes the last reason to ask what format
    // this is.
    coalescer_.addSource(std::move(name), [inst] { return inst->latencyEpoch(); },
                         engine::LatencyCoalescer::Kind::Latency);

    return *devices_.back().node;
}

std::vector<engine::Node*> DeviceHost::chainFor(std::int64_t trackId) const {
    std::vector<engine::Node*> chain;
    if (trackId == 0) return chain;  // unplaced never reaches a graph
    for (const auto& p : devices_)
        if (p.trackId == trackId) chain.push_back(p.node.get());
    return chain;
}

engine::DeviceChainFn DeviceHost::chainSupplier() noexcept {
    return [this](std::int64_t trackId) { return chainFor(trackId); };
}

void DeviceHost::attachHost(engine::GraphHost& host, RebuildSpec spec) {
    host_ = &host;
    spec_ = std::move(spec);
    coalescer_.attach(host);
}

bool DeviceHost::rebuildNow() {
    lastError_.clear();
    if (host_ == nullptr) { lastError_ = "no host attached"; return false; }
    if (!spec_.model)     { lastError_ = "no model supplier"; return false; }

    const rows::Model* model = spec_.model();
    if (model == nullptr) { lastError_ = "no model"; return false; }

    engine::RealizeOptions opts;
    opts.channels = spec_.channels;
    opts.devicesFor = chainSupplier();

    if (!host_->rebuild(*model, opts, spec_.sampleRate, spec_.maxFrames)) {
        lastError_ = host_->error();
        return false;
    }
    return true;
}

void DeviceHost::watchClapGlue(ClapHostGlue& glue, std::string name) {
    glues_.push_back(&glue);

    // ADR-0084's three cases, with ADR-0085's two Kinds. The split is the
    // whole point: a latency change is a tap move, a shape change is a
    // different graph, and a restart nobody explained is treated as the
    // second because the conservative answer is the one that cannot corrupt.
    coalescer_.addSource(name + " latency", [&glue] { return glue.latencyChanges(); },
                         engine::LatencyCoalescer::Kind::Latency);
    coalescer_.addSource(name + " ports", [&glue] { return glue.portChanges(); },
                         engine::LatencyCoalescer::Kind::Shape);
    coalescer_.addSource(name + " bare", [&glue] { return glue.unexplainedRestarts(); },
                         engine::LatencyCoalescer::Kind::Shape);
}

void DeviceHost::dispatchPluginCallbacks() {
    for (auto* g : glues_)
        if (g != nullptr) g->dispatchMainThread();
}

bool DeviceHost::tick(std::int64_t nowMs) {
    // Callbacks first. A plugin that deferred work may well be deferring the
    // thing that then reports a latency change, and running the coalescer
    // first would poll before that work had happened -- costing a whole
    // quiet period for no reason.
    dispatchPluginCallbacks();
    const bool retapped = coalescer_.poll(nowMs);

    if (host_ != nullptr) {
        if (coalescer_.rebuildNeeded()) {
            // CLEARED BEFORE THE ATTEMPT, AND NOT RE-RAISED ON FAILURE.
            //
            // The flag means "a report asked for a rebuild", not "the graph
            // is wrong". Leaving it raised when the rebuild fails turns one
            // unrealisable model into a rebuild attempt every tick -- fifty
            // full plan-realise-prepare cycles a second on the message
            // thread, for a model that will refuse identically every time.
            //
            // The cost of clearing is real and is stated rather than hidden:
            // a failed rebuild leaves the graph stale against a plugin whose
            // ports moved, and nothing retries until the NEXT report. What
            // makes that survivable is that the failure is not silent --
            // `stats().rebuildsFailed` counts it and `lastRebuildError()`
            // names it, which is what a UI surfaces. ADR-0090.
            coalescer_.clearRebuildNeeded();
            if (rebuildNow()) ++stats_.rebuilds;
            else              ++stats_.rebuildsFailed;
        }

        // AFTER the rebuild, not before. A graph retired on this tick cannot
        // be freed on this tick -- the audio thread has not moved past it
        // yet -- so this collects the one retired on an EARLIER tick. Doing
        // it before the rebuild would simply delay every reclamation by one
        // tick for no gain.
        host_->collect();
    }
    return retapped;
}

}  // namespace adi::device
