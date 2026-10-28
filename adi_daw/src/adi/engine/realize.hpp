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
#include <utility>
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
    /// A copy is exact at either width (ADR-0207 d2).
    [[nodiscard]] bool processesDouble() const noexcept override { return true; }

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

/// How a track's SOURCES are supplied: what pushes audio into the junction
/// ahead of the devices -- a clip reader, a live input, a test tone. Same
/// contract as `DeviceChainFn`: caller-owned nodes by default, outliving any
/// graph that holds them (ADR-0122). A streamed source may instead supply a
/// sourceLifetime() token retained by the realised graph (ADR-0151).
using SourceFn = std::function<std::vector<Node*>(std::int64_t trackId)>;

/// Where a send leaves its source (ADR-0203 d3): the node on `trackId`'s
/// chain -- one of the nodes `devicesFor` gave that track -- that a send at
/// `tap` reads from, or null for its junction. That is the node whose output
/// is `tap`, unless the send's own node plays what comes after it: the
/// session's Post send plays its source's strip, so it reads before the strip.
using TapFn = std::function<Node*(std::int64_t trackId, Tap tap)>;

/// The node a send plays through, by its routing row (ADR-0203 d3): its
/// level, pan and switch. Caller-owned and kept across rebuilds, as a strip
/// is; null refuses the send, named.
using SendFn = std::function<Node*(std::int64_t routingId)>;

struct RealizeOptions {
    std::int32_t channels = 2;

    /// Unset means every track is bare, which is the right behaviour for a
    /// project with no plugins and for every test that is about topology.
    DeviceChainFn devicesFor;

    /// Unset means no track has a source, which is every topology test and
    /// a project that is only routing. A source is connected INTO the
    /// junction, so `inputFor` is unchanged by its presence: everything
    /// that feeds a track, from inside or upstream, meets at one node.
    SourceFn sourcesFor;

    /// Unset means every tap is the chain's tail.
    TapFn tapFor;

    /// Unset means no send can be made, and each is named.
    SendFn sendFor;

    /// What a send passes through after its own node, by its routing row:
    /// a channel half of its own when it enters a summing group (ADR-0174),
    /// else null. Unset, nothing.
    SendFn sendTailFor;
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

    /// An edge's identity ACROSS rebuilds (ADR-0092): which track feeds which,
    /// on which bus. Node ids are not it -- a new track shifts every id after
    /// it, and every junction is a fresh object per realisation. What the
    /// history on an edge means is "what track A has been sending to track B",
    /// and that is exactly this key -- with what the wire carries (the tail's
    /// `Node::wireKind`), because a summed child's output into its group is a
    /// console's, not the track's own, and one must not play as the other.
    struct EdgeKey {
        std::int64_t fromTrack = 0;
        std::int64_t toTrack = 0;
        std::uint8_t bus = 0;
        std::uint32_t wire = 0;
        friend bool operator<(const EdgeKey& a, const EdgeKey& b) noexcept {
            if (a.fromTrack != b.fromTrack) return a.fromTrack < b.fromTrack;
            if (a.toTrack != b.toTrack) return a.toTrack < b.toTrack;
            if (a.bus != b.bus) return a.bus < b.bus;
            return a.wire < b.wire;
        }
    };
    struct EdgeRecord {
        EdgeKey key;
        NodeId from = kInvalidNode;
        NodeId to = kInvalidNode;
        Bus bus = Bus::Main;
    };

    /// Every edge realisation made from the plan, SORTED by key, so two
    /// graphs' lists can be matched in one allocation-free walk on the audio
    /// thread. The rings themselves are looked up at the swap, never cached.
    [[nodiscard]] const std::vector<EdgeRecord>& planEdges() const noexcept {
        return planEdges_;
    }

    /// An edge INSIDE a track, across rebuilds (ADR-0092 too): a source into
    /// the junction, the junction into the chain's first node, one chain node
    /// into the next. Such an edge delays when the node before it reports a
    /// negative latency -- a strip playing a track delay (ADR-0172) with a
    /// node after it, as a summed child's channel half is, or a send's node
    /// playing its source's strip -- or, for a source, when something else
    /// into the junction arrives later.
    ///
    /// Keyed by where the edge LEAVES, because a ring holds what that end
    /// sent. A chain node has one next node, so its edge onward is `Next` from
    /// it, whatever takes it now: a summed child's channel half replaced by
    /// another flavour, a device swapped after it. A source is keyed as an
    /// edge INTO its track's junction from it. Its end is the caller's node --
    /// the same object in every graph realised from it, alive while any graph
    /// holding it runs -- or the junction (null), new each time and so named
    /// by its track; or, for a node that is new each time too, the name it
    /// gives itself (`Node::historyName`).
    ///
    /// A send's edges (ADR-0203 d3): its tap into its node is a `Send` from
    /// the tap, named by the routing row, since a tap may feed several sends
    /// as well as its next node; and its node into the destination's junction
    /// is an `Into` from the send's node. Into a summing group the send goes
    /// through a channel half of its own (ADR-0174): `Next` from the send's
    /// node into the half, and the half into the junction is an `Into` from
    /// the send's node named by the half's wire (`Node::wireKind`).
    struct ChainKey {
        enum class Leg : std::uint8_t {
            Into,   ///< from a source into the track's junction
            Next,   ///< from the junction or a chain node to the next node
            Send,   ///< from a tap into a send's node, named by its routing row
        };
        std::int64_t track = 0;
        Leg leg = Leg::Next;
        const Node* from = nullptr;   ///< null: the junction, or a named node
        /// A node's `historyName`, when it has one; for a send's last edge, the
        /// wire it carries.
        std::uint64_t name = 0;
        friend bool operator<(const ChainKey& a, const ChainKey& b) noexcept {
            if (a.track != b.track) return a.track < b.track;
            if (a.leg != b.leg) return a.leg < b.leg;
            if (a.from != b.from) return std::less<const Node*>{}(a.from, b.from);
            return a.name < b.name;
        }
    };
    struct ChainRecord {
        ChainKey key;
        NodeId from = kInvalidNode;
        NodeId to = kInvalidNode;
        Bus bus = Bus::Main;
    };

    /// Every edge realisation made inside a track, SORTED by key, matched
    /// across a swap the same way as `planEdges`.
    [[nodiscard]] const std::vector<ChainRecord>& chainEdges() const noexcept {
        return chainEdges_;
    }

private:
    friend std::unique_ptr<RealizedGraph> realize(const GraphPlan&,
                                                  const RealizeOptions&);

    struct Chain {
        NodeId head = kInvalidNode;
        NodeId tail = kInvalidNode;
        const Node* tailNode = nullptr;   ///< null: the junction is the tail
        /// The caller's nodes after the junction, in order: where a tap is.
        std::vector<std::pair<const Node*, NodeId>> nodes;
    };

    std::vector<std::shared_ptr<void>> sourceOwners_; // destroyed after graph_
    Graph graph_;
    /// Only the nodes realisation itself created. Devices are owned elsewhere.
    std::vector<std::unique_ptr<Node>> owned_;
    std::unordered_map<std::int64_t, Chain> chains_;
    std::vector<EdgeRecord> planEdges_;
    std::vector<ChainRecord> chainEdges_;
    std::vector<std::string> problems_;
    std::string error_;
    bool ok_ = false;
};

/// Build it. Never throws on a bad plan; `ok()` says whether it worked and
/// `error()` says why not.
[[nodiscard]] std::unique_ptr<RealizedGraph> realize(const GraphPlan& plan,
                                                     const RealizeOptions& opts = {});

}  // namespace adi::engine
