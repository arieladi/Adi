// SPDX-License-Identifier: GPL-3.0-or-later

#include "adi/engine/realize.hpp"

#include <algorithm>
#include <cstring>
#include <string>
#include <utility>

namespace adi::engine {

namespace {

std::string trackRef(std::int64_t id) { return "tracks#" + std::to_string(id); }

}  // namespace

// ---------------------------------------------------------------------------
// MixNode
// ---------------------------------------------------------------------------

void MixNode::process(const NodeIo& io) noexcept {
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
                if (auto owner = s->sourceLifetime()) r->sourceOwners_.push_back(std::move(owner));
                if (const auto& owner = s->eventSourceLifetime()) r->sourceOwners_.push_back(owner);
                const NodeId id = r->graph_.addNode(*s);
                r->graph_.connect(id, head);
            }
        }
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
                // ADR-0164: a chain node may hold data the audio thread reads
                // -- a strip's automation lanes -- and this graph keeps it alive.
                if (auto owner = d->sourceLifetime()) r->sourceOwners_.push_back(std::move(owner));
                if (const auto& owner = d->eventSourceLifetime()) r->sourceOwners_.push_back(owner);   // ADR-0165
                const NodeId id = r->graph_.addNode(*d);
                r->graph_.connect(tail, id);
                tail = id;
            }
        }
        r->chains_[pn.trackId] = RealizedGraph::Chain{head, tail};
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
