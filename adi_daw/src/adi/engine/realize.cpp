// SPDX-License-Identifier: GPL-3.0-or-later

#include "adi/engine/realize.hpp"
#include "adi/engine/mixer.hpp"

#include <algorithm>
#include <cstring>
#include <cmath>
#include <limits>
#include <string>
#include <unordered_set>
#include <utility>

namespace adi::engine {

namespace {

std::string trackRef(std::int64_t id) { return "tracks#" + std::to_string(id); }
// Owned by one immutable graph. Gains are computed on its owner; no atomics,
// allocation, worker, source-fader duplication or SDK calls in its callback.
class KeyGain final : public Node {
public:
    KeyGain(double db,double pan) {
        const auto p=panGainsWide(pan,PanLaw::Live);
        gain_=faderGain(db);left_=gain_*p.left;right_=gain_*p.right;
    }
    std::int64_t tailSamples() const noexcept override {return 0;}
    bool processesDouble() const noexcept override {return true;}
    EventFlow eventFlow() const noexcept override {return EventFlow::Consume;}
    const char* name() const noexcept override {return "device key gain";}
    void process(const NodeIo& io) noexcept override {
        if(io.wide())run(io,io.in64,io.out64);
        else run(io,io.in,io.out);
    }
private:
    double gain_=1,left_=1,right_=1;
    template<class T>void run(const NodeIo& io,const T*const* in,T*const* out) noexcept {
        if(!out)return;
        for(std::int32_t c=0;c<io.channels;++c) {
            if(!out[c])continue;
            auto* dst=out[c]+io.blockOffset;
            const auto* src=in&&in[c]?in[c]+io.blockOffset:nullptr;
            const auto gain=static_cast<T>(c==0?left_:c==1?right_:gain_);
            if(!src || gain==T{})std::fill_n(dst,io.frames,T{});
            else if(gain==T{1})std::memcpy(dst,src,static_cast<std::size_t>(io.frames)*sizeof(T));
            else for(std::int32_t i=0;i<io.frames;++i)dst[i]=src[i]*gain;
        }
    }
};

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

std::optional<RackLatency> RealizedGraph::rackLatency(std::int64_t deviceId) const noexcept {
    const auto found = racks_.find(deviceId);
    if (found == racks_.end()) return std::nullopt;
    const auto& rack = found->second;
    auto report = graph_.pathLatencyBetween(rack.router, rack.sum, rack.last);
    if (report) report->known = report->known && rack.known;
    return report;
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
    // Every placement, including feedback and monitor nodes, owns the same
    // lifetime/binding contract. No candidate mutates a shared Node binding.
    const auto add = [&](Node& node) {
        if (auto owner = node.sourceLifetime()) r->sourceOwners_.push_back(std::move(owner));
        const NodeId id = r->graph_.addNode(node);
        if (opts.eventSourceFor) {
            if (auto binding = opts.eventSourceFor(node))
                (void)r->graph_.setEventSource(id, std::move(*binding));
        }
        return id;
    };

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
        const NodeId head = add(*mix);
        r->owned_.push_back(std::move(mix));

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
                const NodeId id = add(*s);
                // A source the session builds afresh at every rebuild names
                // itself; any other is keyed by the object it is.
                const std::uint64_t name = s->historyName();
                if (r->graph_.connect(id, head))
                    r->chainEdges_.push_back({{pn.trackId, RealizedGraph::ChainKey::Leg::Into,
                                               name != 0 ? nullptr : s, name},
                                              id, head, Bus::Main});
            }
        }
        RealizedGraph::Chain built;
        using Leg = RealizedGraph::ChainKey::Leg;
        struct Endpoint {
            NodeId id;
            const Node* node;
        };
        const auto connect = [&](Endpoint from, NodeId to, Leg leg = Leg::Next) {
            if (r->graph_.connect(from.id, to))
                r->chainEdges_.push_back({{pn.trackId, leg, from.node, 0}, from.id, to, Bus::Main});
        };
        const auto place = [&](Node& node) {
            const NodeId id = add(node);
            built.nodes.emplace_back(&node, id);
            return id;
        };
        const auto placeStage = [&](auto&& self, Node* node, Endpoint& before, bool& latencyKnown) -> void {
            if (!node) {
                r->problems_.push_back(trackRef(pn.trackId) + ": a null device in the chain was dropped");
                return;
            }
            if (!placeOnce(node, trackRef(pn.trackId), "a chain node")) return;
            auto rack = opts.rackFor ? opts.rackFor(*node) : std::nullopt;
            if (!rack) {
                const NodeId id = place(*node);
                connect(before, id);
                before = {id, node};
                return;
            }

            const NodeId router = place(*rack->router);
            connect(before, router);
            const NodeId sum = place(*node);
            bool known = rack->latencyKnown;
            const auto startBranch = [&](Node& gate) {
                const auto id = place(gate);
                if (r->graph_.connect(router, id))
                    r->chainEdges_.push_back({{pn.trackId, Leg::Branch, &gate, 0}, router, id, Bus::Main});
                return Endpoint{id, &gate};
            };
            for (auto& branch : rack->branches) {
                auto branchTail = startBranch(*branch.gate);
                for (auto* device : branch.devices) self(self, device, branchTail, known);
                if (branch.strip) {
                    const auto id = place(*branch.strip);
                    connect(branchTail, id);
                    branchTail = {id, branch.strip};
                }
                connect(branchTail, sum, Leg::Join);
            }
            // Expansion places every node from this rack in one contiguous
            // range. Scan that range's applied latency flags for diagnostics.
            const auto last = static_cast<NodeId>(r->graph_.nodeCount() - 1);
            r->racks_.emplace(rack->deviceId, RealizedGraph::RackEndpoints{router, sum, last, known});
            latencyKnown = latencyKnown && known;
            before = {sum, node};
        };
        Endpoint previous{head, nullptr};
        if (opts.devicesFor) {
            bool latencyKnown = true;
            for (auto* stage : opts.devicesFor(pn.trackId)) placeStage(placeStage, stage, previous, latencyKnown);
        }
        built.head = head;
        built.tail = previous.id;
        built.tailNode = previous.node;
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
        if(e.keyDevice!=0) {
            const auto ref="routing#"+std::to_string(e.routingId);
            const auto srcChain=r->chains_.find(srcId),dstChain=r->chains_.find(dstId);
            if(srcChain==r->chains_.end() || dstChain==r->chains_.end()) {
                r->problems_.push_back(ref+": a device key endpoint carries no audio");continue;
            }
            Node* target=opts.keyDeviceFor?opts.keyDeviceFor(e.keyDevice):nullptr;
            const auto targetId=target?r->graph_.find(*target):kInvalidNode;
            const auto& destination=dstChain->second;
            const bool inChain=target && std::any_of(destination.nodes.begin(),destination.nodes.end(),
                [&](const auto& n){return n.first==target;});
            if(targetId==kInvalidNode || !inChain || !opts.keyTapFor) {
                r->problems_.push_back(ref+": key device/input or source tap is unavailable");continue;
            }
            const auto endpoint=opts.keyTapFor(srcId,e.tap);
            if(!endpoint) {r->problems_.push_back(ref+": source key tap is unavailable");continue;}
            Node* tap=*endpoint;
            const auto source=tap?r->graph_.find(*tap):r->inputFor(srcId);
            const auto& chain=srcChain->second;
            const bool sourceHere=!tap || std::any_of(chain.nodes.begin(),chain.nodes.end(),
                [&](const auto& n){return n.first==tap;});
            if(source==kInvalidNode || !sourceHere || !std::isfinite(e.keyGainDb) ||
               !std::isfinite(e.keyPan) || !std::isfinite(faderGain(e.keyGainDb)) ||
               faderGain(e.keyGainDb)>static_cast<double>(std::numeric_limits<float>::max())/2) {
                r->problems_.push_back(ref+": invalid key source/tap/gain");continue;
            }
            auto gain=std::make_unique<KeyGain>(e.keyGainDb,e.keyPan);
            const auto gainId=add(*gain);r->owned_.push_back(std::move(gain));
            if(!r->graph_.connect(source,gainId) || !r->graph_.connect(gainId,targetId,Bus::Sidechain)) {
                r->problems_.push_back(ref+": graph refused device key");continue;
            }
            // Distinct logical identities survive unrelated rebuilds, while a
            // changed source, target or tap cannot inherit another key's rings.
            RealizedGraph::EdgeKey before{srcId,dstId,static_cast<std::uint8_t>(Bus::Main),0,
                e.routingId,e.keyDevice,static_cast<std::uint8_t>(e.tap)};
            auto after=before;after.bus=static_cast<std::uint8_t>(Bus::Sidechain);
            r->planEdges_.push_back({before,source,gainId,Bus::Main});
            r->planEdges_.push_back({after,gainId,targetId,Bus::Sidechain});
            continue;
        }
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
            const Node* tapNode = chain.tailNode;
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
            // A return's send into itself, Live's feedback loop: the send
            // ends in the loop's writer, and the loop's reader feeds the
            // return's junction a block later. Neither edge closes a cycle --
            // the writer is a sink, the reader a source -- and neither delays,
            // so their keys only keep them apart.
            if (e.feedback) {
                const FeedbackNodes loop = opts.feedbackFor ? opts.feedbackFor(e.routingId) : FeedbackNodes{};
                if (loop.writer == nullptr || loop.reader == nullptr) {
                    r->problems_.push_back(ref + ": no loop to close the return's send into itself, so it was not made");
                    continue;
                }
                if (!placeOnce(loop.writer, ref, "a loop's writer") || !placeOnce(loop.reader, ref, "a loop's reader"))
                    continue;
                const NodeId sid = add(*send);
                const NodeId wid = add(*loop.writer);
                const NodeId rid = add(*loop.reader);
                if (!r->graph_.connect(at, sid) || !r->graph_.connect(sid, wid) || !r->graph_.connect(rid, to)) {
                    r->problems_.push_back(ref + ": the graph refused the loop");
                    continue;
                }
                r->chainEdges_.push_back({{srcId, RealizedGraph::ChainKey::Leg::Send, tapNode,
                                           static_cast<std::uint64_t>(e.routingId)},
                                          at, sid, Bus::Main});
                r->chainEdges_.push_back({{srcId, RealizedGraph::ChainKey::Leg::Next, send, 0}, sid, wid, Bus::Main});
                r->chainEdges_.push_back({{dstId, RealizedGraph::ChainKey::Leg::Into, loop.reader, 0}, rid, to, Bus::Main});
                r->loopReaders_.push_back(loop.reader);
                continue;
            }
            // ADR-0174: a send into a summing group enters it through a
            // channel half of its own, as a child does, so the group's buss
            // half decodes what a channel half encoded and the send's level
            // is the same with summing on or off. Null for any other place.
            Node* half = opts.sendTailFor ? opts.sendTailFor(e.routingId) : nullptr;
            if (half != nullptr && !placeOnce(half, ref, "a send's channel half")) continue;
            const NodeId sid = add(*send);
            const NodeId hid = half != nullptr ? add(*half) : kInvalidNode;
            const bool joined = r->graph_.connect(at, sid) &&
                                (half == nullptr ? r->graph_.connect(sid, to)
                                                 : r->graph_.connect(sid, hid) && r->graph_.connect(hid, to));
            if (!joined) {
                r->problems_.push_back(ref + ": the graph refused the send");
                continue;
            }
            r->chainEdges_.push_back({{srcId, RealizedGraph::ChainKey::Leg::Send, tapNode,
                                       static_cast<std::uint64_t>(e.routingId)},
                                      at, sid, Bus::Main});
            if (half == nullptr) {
                r->chainEdges_.push_back({{dstId, RealizedGraph::ChainKey::Leg::Into, send, 0}, sid, to, Bus::Main});
            } else {
                // Into the junction keyed by the send it carries, and named
                // by its wire: a console's, so a rebuild that turns summing
                // off or on never plays one wire's history as the other's,
                // and one that changes the flavour keeps it, as a child's does.
                r->chainEdges_.push_back({{dstId, RealizedGraph::ChainKey::Leg::Next, send, 0}, sid, hid, Bus::Main});
                r->chainEdges_.push_back({{dstId, RealizedGraph::ChainKey::Leg::Into, send, half->wireKind()},
                                          hid, to, Bus::Main});
            }
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
        const Node* tailNode = r->chains_.at(srcId).tailNode;
        rec.key.wire = tailNode != nullptr ? tailNode->wireKind() : 0;
        rec.from = from;
        rec.to = to;
        rec.bus = e.bus;
        r->planEdges_.push_back(rec);
    }
    // Monitoring is a downstream sink: a sidechain from Main aligns its
    // clock without copying Main audio or running any source/device again.
    const auto mainTrack=plan.nodes[*plan.output].trackId;
    const auto mainOutput=r->outputFor(mainTrack);
    for(const auto& monitor:opts.monitors) {
        if(!monitor.gain || !monitor.aligned || !r->has(monitor.trackId))continue;
        const auto source=monitor.source?r->graph_.find(*monitor.source):r->inputFor(monitor.trackId);
        if(source==kInvalidNode || mainOutput==kInvalidNode)continue;
        if(!placeOnce(monitor.gain,"cue", "a monitor gain") || !placeOnce(monitor.aligned,"cue","a monitor sink"))continue;
        const auto gain=add(*monitor.gain),aligned=add(*monitor.aligned);
        r->graph_.connect(source,gain);r->graph_.connect(gain,aligned);r->graph_.connect(mainOutput,aligned,Bus::Sidechain);
        r->chainEdges_.push_back({{monitor.trackId,RealizedGraph::ChainKey::Leg::Monitor,monitor.source,0},source,gain,Bus::Main});
        r->chainEdges_.push_back({{monitor.trackId,RealizedGraph::ChainKey::Leg::Monitor,monitor.gain,1},gain,aligned,Bus::Main});
        r->chainEdges_.push_back({{monitor.trackId,RealizedGraph::ChainKey::Leg::Monitor,monitor.aligned,2},mainOutput,aligned,Bus::Sidechain});
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
    std::sort(r->loopReaders_.begin(), r->loopReaders_.end(), std::less<Node*>());

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
