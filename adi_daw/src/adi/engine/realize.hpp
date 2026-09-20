// SPDX-License-Identifier: GPL-3.0-or-later
//
// Realisation: turning a `GraphPlan` into a live `Graph` that can be processed.
//
// `plan.hpp` calls this "a later step and a small one: walk the nodes,
// construct, `connect`, `setOutput`." It is the walk, and it turned out to have
// four decisions in it that the plan deliberately does not make:
//
//   1. **A VCA is not an audio node.** The planner emits one, because a VCA is
//      a track and the plan describes tracks. Realising it would add a node
//      that is processed every block, feeds nothing and receives nothing.
//      `outputFor` a VCA is `kInvalidNode`, and that is the answer, not a
//      failure.
//   2. **A track is a CHAIN, not a node.** Its devices run in series between
//      what feeds it and what it feeds, so an edge in the plan becomes an edge
//      between the TAIL of one chain and the HEAD of another. Nothing in the
//      plan knows this, and nothing in the plan should.
//   3. **The devices are injected.** `realize` lives in `adi_core` and compiles
//      on every ABI, so it cannot construct a `Vst3Device` or a `ClapDevice`.
//      It asks for the chain and receives `Node*`s, which is the same seam
//      ADR-0052 decision 4 put in `DeviceNode`: the graph never learns which
//      format it has, and neither does this.
//   4. **A plan with a cycle is refused before anything is constructed.**
//      `Graph::prepare` would also refuse it (ADR-0055), but only after every
//      node exists and every device has been instantiated.
//
// Compensation is NOT a step here. `Graph::prepare` computes it from what the
// nodes declare (ADR-0058 decisions 2-5), so a chain of three plugins reporting
// 64, 0 and 128 is compensated because it was built, not because the realiser
// did arithmetic. That is the property worth having: there is exactly one place
// latency is turned into delay.

#pragma once

#include "adi/engine/graph.hpp"
#include "adi/engine/plan.hpp"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace adi::engine {

/// A summing junction: the node a track IS, before and apart from its devices.
///
/// The graph has already summed this node's inputs into its buffer by the time
/// `process` runs (ADR-0044 -- multiple edges into one node are summed, which
/// is what makes a group a node with no device rather than a special case). So
/// the work here is to pass that sum on.
///
/// IT IS CREATED EVEN FOR A TRACK THAT HAS DEVICES, and the copy that costs is
/// deliberate. The alternative -- making the first device the head of the chain
/// -- means a track's identity in the graph changes the moment someone adds a
/// plugin, so every id held across that edit is stale, including the ones PDC
/// just sized delay lines against. One memcpy per track per block buys an
/// identity that survives editing. At 4096 frames it is also where the strip's
/// gain, pan and phase will go, at which point it stops being a copy at all.
class MixNode final : public Node {
public:
    void process(const NodeIo& io) noexcept override;

    /// Zero, and not the base class's `kInfiniteTail`. A summing junction holds
    /// nothing back: when its inputs go quiet, so does it, immediately. The
    /// conservative default is for nodes that might have state, and this has
    /// none -- inheriting it would keep every track in the project awake for
    /// as long as the project was open, which is ADR-0043 switched off.
    [[nodiscard]] std::int64_t tailSamples() const noexcept override { return 0; }
    [[nodiscard]] const char* name() const noexcept override { return "mix"; }
};

/// How a track's device chain is supplied. Returns the devices in signal order,
/// or empty. The realiser does NOT take ownership: a device outlives a graph
/// rebuild, because ADR-0042 decision 5 says a device change must not disturb
/// the project and a rebuild is not a reason to reload a plugin.
using DeviceChainFn = std::function<std::vector<Node*>(std::int64_t trackId)>;

struct RealizeOptions {
    std::int32_t channels = 2;

    /// Unset means every track is bare, which is the right behaviour for a
    /// project with no plugins and for every test that is about topology.
    DeviceChainFn devicesFor;
};

/// A live graph and everything it owns.
///
/// Not copyable and not movable: `Graph` holds raw `Node&`s into `owned_`, and
/// a type whose validity depends on where it lives should say so rather than
/// rely on a reader noticing. Hence `realize` hands back a `unique_ptr`.
class RealizedGraph final {
public:
    RealizedGraph() = default;
    ~RealizedGraph() = default;
    RealizedGraph(const RealizedGraph&) = delete;
    RealizedGraph& operator=(const RealizedGraph&) = delete;
    RealizedGraph(RealizedGraph&&) = delete;
    RealizedGraph& operator=(RealizedGraph&&) = delete;

    [[nodiscard]] Graph& graph() noexcept { return graph_; }
    [[nodiscard]] const Graph& graph() const noexcept { return graph_; }

    /// Where signal ENTERS this track: the summing junction, ahead of the
    /// devices. A clip reader pushes here, and so does every upstream track.
    [[nodiscard]] NodeId inputFor(std::int64_t trackId) const noexcept;

    /// Where signal LEAVES this track: the last device, or the junction when
    /// there are none. Downstream edges are made FROM this.
    [[nodiscard]] NodeId outputFor(std::int64_t trackId) const noexcept;

    /// `kInvalidNode` for a VCA and for a track that is not in the plan.
    [[nodiscard]] bool has(std::int64_t trackId) const noexcept;

    /// False when the plan could not be realised at all. Distinct from
    /// `graph().ok()`, which is about `prepare`.
    [[nodiscard]] bool ok() const noexcept { return ok_; }
    [[nodiscard]] const std::string& error() const noexcept { return error_; }

    /// Carried forward from the plan, plus anything realisation found. A plan
    /// with problems is still realised where it can be: a routing row naming a
    /// track that is gone should not cost the user the other ninety-nine.
    [[nodiscard]] const std::vector<std::string>& problems() const noexcept {
        return problems_;
    }

    /// How many nodes the graph actually has -- junctions AND devices, so the
    /// VCA decision and the chain arithmetic are assertable rather than
    /// described.
    [[nodiscard]] std::size_t nodeCount() const noexcept { return graph_.nodeCount(); }

private:
    friend std::unique_ptr<RealizedGraph> realize(const GraphPlan&,
                                                  const RealizeOptions&);

    struct Chain {
        NodeId head = kInvalidNode;
        NodeId tail = kInvalidNode;
    };

    Graph graph_;
    /// Only the nodes realisation itself created. Devices are owned elsewhere.
    std::vector<std::unique_ptr<Node>> owned_;
    std::unordered_map<std::int64_t, Chain> chains_;
    std::vector<std::string> problems_;
    std::string error_;
    bool ok_ = false;
};

/// Build it. Never throws on a bad plan; `ok()` says whether it worked and
/// `error()` says why not.
[[nodiscard]] std::unique_ptr<RealizedGraph> realize(const GraphPlan& plan,
                                                     const RealizeOptions& opts = {});

}  // namespace adi::engine
