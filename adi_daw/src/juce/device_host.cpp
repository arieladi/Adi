// SPDX-License-Identifier: GPL-3.0-or-later
//
// See device_host.hpp for why the coalescer lives here and not in the graph.

#include "juce/device_host.hpp"

namespace adi::device {

DeviceHost::DeviceHost() = default;

void DeviceHost::attachGraph(engine::Graph& g) noexcept { coalescer_.attach(g); }

DeviceNode& DeviceHost::add(std::unique_ptr<DeviceInstance> dev, std::string name) {
    devices_.push_back(std::move(dev));
    DeviceInstance* inst = devices_.back().get();
    nodes_.push_back(std::make_unique<DeviceNode>(*inst));

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

    return *nodes_.back();
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
    return coalescer_.poll(nowMs);
}

}  // namespace adi::device
