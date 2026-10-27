// SPDX-License-Identifier: GPL-3.0-or-later

#include "adi/engine/realize.hpp"

#include <algorithm>
#include <cstring>
#include <string>
#include <unordered_set>
#include <utility>

namespace adi::engine {

namespace {

std::string trackRef(std::int64_t id) { return "tracks#" + std::to_string(id); }

}  // namespace

// ---------------------------------------------------------------------------
// MixNode
// ---------------------------------------------------------------------------

void MixNode::process(const NodeIo& io) noexcept {
    if (io.wide()) {
        // ADR-0207 d2: the same copy at full width.
        const auto n = static_cast<std::size_t>(io.frames);
        for (std::int32_t c = 0; c < io.channels; ++c) {
            double* dst = io.out64[c];
            if (dst == nullptr) continue;
            dst += io.blockOffset;
            const double* src = (io.in64 != nullptr) ? io.in64[c] : nullptr;
            if (src != nullptr) std::memcpy(dst, src + io.blockOffset, n * sizeof(double));
            else std::memset(dst, 0, n * sizeof(double));
        }
        return;
    }
    if (io.out == nullptr) return;
    for (std::int32_t c = 0; c < io.channels; ++c) {
        float* dst = io.out[c];
        if (dst == nullptr) continue;
        // BLOCK pointers, SEGMENT length. The device paths all got this wrong
        // at once and wrote every segment at index 0, so it is spelled out
        // rather than assumed (ADR-0042).
        dst += io.blockOffset;
        const float* src = (io.in != nullptr) ? io.in[c] : nullptr;
        if (src != nullptr) {
            src += io.blockOffset;
            std::memcpy(dst, src, static_cast<std::size_t>(io.frames) * sizeof(float));
        } else {
            // A track with nothing feeding it is silent, and silence has to be
            // WRITTEN. Leaving the buffer alone hands the next node last
            // block's audio, which is the one failure mode that reaches the
            // monitors.
            std::memset(dst, 0, static_cast<std::size_t>(io.frames) * sizeof(float));
        }
    }
}

// ---------------------------------------------------------------------------
// RealizedGraph
// ---------------------------------------------------------------------------

NodeId RealizedGraph::inputFor(std::int64_t trackId) const noexcept {
    const auto it = chains_.find(trackId);
    return (it == chains_.end()) ? kInvalidNode : it->second.head;
}

NodeId RealizedGraph::outputFor(std::int64_t trackId) const noexcept {
    const auto it = chains_.find(trackId);
    return (it == chains_.end()) ? kInvalidNode : it->second.tail;
}

bool RealizedGraph::has(std::int64_t trackId) const noexcept {
    return chains_.find(trackId) != chains_.end();
}

// ---------------------------------------------------------------------------
// realize
// ---------------------------------------------------------------------------

std::unique_ptr<RealizedGraph> realize(const GraphPlan& plan,
                                       const RealizeOptions& opts) {
    auto r = std::make_unique<RealizedGraph>();
    r->problems_ = plan.problems;

    // Refused before anything is constructed. `Graph::prepare` would refuse a
    // cycle too (ADR-0055), but only after every node exists and every plugin
    // in the project has been instantiated -- which for a cyclic routing row is
    // a few seconds of loading to reach an error the plan already knew about.
    if (plan.cycle) {
        r->error_ = "the plan has a cycle; nothing was constructed";
        return r;
    }
    if (!plan.output) {
        r->error_ = "the plan has no master, so there is nothing to output";
        return r;
    }
    if (*plan.output >= plan.nodes.size()) {
        r->error_ = "the plan's output index is out of range";
        return r;
    }

    r->graph_.setChannels(opts.channels);

    // A caller's node is placed ONCE. Twice -- in two chains, or twice in one
    // -- the graph would run it twice a block, and an edge keyed by its
    // address (ADR-0092) would name two edges. The second is dropped, named.
    std::unordered_set<const Node*> placed;
    auto placeOnce = [&](const Node* n, const std::string& where, const char* what) {
        if (placed.insert(n).second) return true;
        r->problems_.push_back(where + ": " + what + " given twice; the second was dropped");
        return false;
    };

    // --- one chain per planned node ----------------------------------------
    for (const PlannedNode& pn : plan.nodes) {
        // A VCA controls; it does not carry audio. The planner emits one
        // because a VCA is a track and the plan describes tracks, but a node
        // here would be processed every block to move nothing.
        if (pn.kind == PlannedKind::Vca) continue;

        auto mix = std::make_unique<MixNode>();
        const NodeId head = r->graph_.addNode(*mix);
        r->owned_.push_back(std::move(mix));

        NodeId tail = head;
        // Sources first, so a track's own audio is an edge into the junction
        // like every upstream track's (ADR-0044 sums them all there). Not
        // owned: the same rule as devices, for the same reason.
        if (opts.sourcesFor) {
            const std::vector<Node*> sources = opts.sourcesFor(pn.trackId);
            for (Node* s : sources) {
                if (s == nullptr) {
                    r->problems_.push_back(trackRef(pn.trackId) +
                                           ": a null source was dropped");
                    continue;
                }
                if (!placeOnce(s, trackRef(pn.trackId), "a source")) continue;
                if (auto owner = s->sourceLifetime()) r->sourceOwners_.push_back(std::move(owner));
                if (const auto& owner = s->eventSourceLifetime()) r->sourceOwners_.push_back(owner);
                const NodeId id = r->graph_.addNode(*s);
                if (r->graph_.connect(id, head))
                    r->chainEdges_.push_back({{pn.trackId, s, nullptr}, id, head, Bus::Main});
            }
        }
        const Node* prev = nullptr;   // the junction
        RealizedGraph::Chain built;
        if (opts.devicesFor) {
            const std::vector<Node*> chain = opts.devicesFor(pn.trackId);
            for (Node* d : chain) {
                if (d == nullptr) {
                    // Named, not skipped silently. A null in a chain means the
                    // caller lost a device between planning and realising, and
                    // the signal path it produces is shorter than the project
                    // says -- exactly what ADR-0011 has `MissingDevice` for.
                    r->problems_.push_back(trackRef(pn.trackId) +
                                           ": a null device in the chain was dropped");
                    continue;
                }
                if (!placeOnce(d, trackRef(pn.trackId), "a chain node")) continue;
                // ADR-0164: a chain node may hold data the audio thread reads
                // -- a strip's automation lanes -- and this graph keeps it alive.
                if (auto owner = d->sourceLifetime()) r->sourceOwners_.push_back(std::move(owner));
                if (const auto& owner = d->eventSourceLifetime()) r->sourceOwners_.push_back(owner);   // ADR-0165
                const NodeId id = r->graph_.addNode(*d);
                if (r->graph_.connect(tail, id))
                    r->chainEdges_.push_back({{pn.trackId, prev, d}, tail, id, Bus::Main});
                tail = id;
                prev = d;
                built.nodes.emplace_back(d, id);
            }
        }
        built.head = head;
        built.tail = tail;
        r->chains_[pn.trackId] = std::move(built);
    }

    // --- the plan's edges, between chains ----------------------------------
    //
    // An edge in the plan joins two TRACKS. Here it joins the TAIL of one chain
    // to the HEAD of another, so a plugin on the source is upstream of the
    // destination and a plugin on the destination is downstream of the sum.
    // That is the only place the chain shape is visible, and it is why the plan
    // does not need to know about it.
    for (const PlannedEdge& e : plan.edges) {
        if (e.from >= plan.nodes.size() || e.to >= plan.nodes.size()) {
            r->problems_.push_back("an edge names a node that is not in the plan");
            continue;
        }
        const std::int64_t srcId = plan.nodes[e.from].trackId;
        const std::int64_t dstId = plan.nodes[e.to].trackId;
        const NodeId from = r->outputFor(srcId);
        const NodeId to = r->inputFor(dstId);

        if (from == kInvalidNode || to == kInvalidNode) {
            // In practice: a routing row naming a VCA. The planner will not
            // produce one by default, but a user row can, and a VCA has no
            // audio port to attach to.
            r->problems_.push_back(trackRef(srcId) + " -> " + trackRef(dstId) +
                                   ": one end carries no audio (a VCA?), so the "
                                   "connection was not made");
            continue;
        }
        // ADR-0203 d3: a SEND leaves its source at its tap, plays through a
        // node of its own -- its level, pan and switch -- and enters the
        // destination's junction beside everything else that feeds it. Both
        // of its edges have one of the caller's nodes at an end, so they are
        // keyed as a chain's edges are and keep their history across a
        // rebuild (ADR-0092).
        if (e.routingId != 0) {
            const std::string ref = "routing#" + std::to_string(e.routingId);
            Node* send = opts.sendFor ? opts.sendFor(e.routingId) : nullptr;
            if (send == nullptr) {
                r->problems_.push_back(ref + ": no node to play the send through, so it was not made");
                continue;
            }
            const RealizedGraph::Chain& chain = r->chains_.at(srcId);
            const Node* tapNode = chain.nodes.empty() ? nullptr : chain.nodes.back().first;
            NodeId at = chain.tail;
            if (e.tap != Tap::Tail && opts.tapFor) {
                tapNode = opts.tapFor(srcId, e.tap);
                at = tapNode == nullptr ? chain.head : kInvalidNode;
                for (const auto& [n, id] : chain.nodes)
                    if (n == tapNode) at = id;
                if (at == kInvalidNode) {
                    r->problems_.push_back(ref + ": its tap is not a node of " + trackRef(srcId) +
                                           "'s chain, so it was not made");
                    continue;
                }
            }
            if (!placeOnce(send, ref, "a send's node")) continue;
            if (auto owner = send->sourceLifetime()) r->sourceOwners_.push_back(std::move(owner));
            if (const auto& owner = send->eventSourceLifetime()) r->sourceOwners_.push_back(owner);
            const NodeId sid = r->graph_.addNode(*send);
            if (!r->graph_.connect(at, sid) || !r->graph_.connect(sid, to)) {
                r->problems_.push_back(ref + ": the graph refused the send");
                continue;
            }
            r->chainEdges_.push_back({{srcId, tapNode, send}, at, sid, Bus::Main});
            r->chainEdges_.push_back({{dstId, send, nullptr}, sid, to, Bus::Main});
            continue;
        }
        if (!r->graph_.connect(from, to, e.bus)) {
            r->problems_.push_back(trackRef(srcId) + " -> " + trackRef(dstId) +
                                   ": the graph refused the connection");
            continue;
        }
        RealizedGraph::EdgeRecord rec;
        rec.key.fromTrack = srcId;
        rec.key.toTrack = dstId;
        rec.key.bus = static_cast<std::uint8_t>(e.bus);
        rec.from = from;
        rec.to = to;
        rec.bus = e.bus;
        r->planEdges_.push_back(rec);
    }
    // Sorted here, on the message thread, so the audio thread can match two
    // graphs' edges by walking both lists once (ADR-0092).
    std::sort(r->planEdges_.begin(), r->planEdges_.end(),
              [](const RealizedGraph::EdgeRecord& a, const RealizedGraph::EdgeRecord& b) {
                  return a.key < b.key;
              });
    std::sort(r->chainEdges_.begin(), r->chainEdges_.end(),
              [](const RealizedGraph::ChainRecord& a, const RealizedGraph::ChainRecord& b) {
                  return a.key < b.key;
              });

    // --- the output ---------------------------------------------------------
    const std::int64_t masterId = plan.nodes[*plan.output].trackId;
    const NodeId out = r->outputFor(masterId);
    if (out == kInvalidNode) {
        r->error_ = trackRef(masterId) + " is the plan's master but carries no audio";
        return r;
    }
    r->graph_.setOutput(out);

    r->ok_ = true;
    return r;
}

}  // namespace adi::engine
