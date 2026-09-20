// SPDX-License-Identifier: GPL-3.0-or-later

#include "adi/engine/graph.hpp"

#include <algorithm>
#include <cstring>

namespace adi::engine {

Graph::~Graph() = default;

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------

NodeId Graph::addNode(Node& node) {
    slots_.push_back(Slot{});
    slots_.back().node = &node;
    prepared_ = false;
    return static_cast<NodeId>(slots_.size() - 1);
}

bool Graph::connect(NodeId from, NodeId to) {
    const auto n = static_cast<NodeId>(slots_.size());
    if (from < 0 || from >= n || to < 0 || to >= n || from == to) return false;
    slots_[static_cast<std::size_t>(to)].inputs.push_back(from);
    prepared_ = false;
    return true;
}

bool Graph::pushInputEvent(NodeId to, const Event& e) {
    if (to < 0 || to >= static_cast<NodeId>(slots_.size())) return false;
    return slots_[static_cast<std::size_t>(to)].events.push(e);
}

std::int32_t Graph::maxFloorFor(double sampleRate) noexcept {
    // ADR-0054. A Continuum emits update frames at 500 Hz; a floor wider than
    // the gap between them puts two frames in one segment and the later one is
    // discarded, which is quantising the stream in TIME rather than in bit
    // depth -- the same mandate broken through a different door.
    if (sampleRate <= 0.0) return 64;
    const auto v = static_cast<std::int32_t>(sampleRate / 500.0);
    return v < 1 ? 1 : v;
}

// ---------------------------------------------------------------------------
// Topological order
// ---------------------------------------------------------------------------

bool Graph::topoSort() {
    const auto n = static_cast<std::int32_t>(slots_.size());
    order_.clear();
    order_.reserve(static_cast<std::size_t>(n));

    std::vector<std::int32_t> indegree(static_cast<std::size_t>(n), 0);
    for (std::int32_t i = 0; i < n; ++i)
        indegree[static_cast<std::size_t>(i)] =
            static_cast<std::int32_t>(slots_[static_cast<std::size_t>(i)].inputs.size());

    // Kahn's algorithm. Ready nodes are taken in id order rather than from a
    // stack, so the order is deterministic for a given graph -- two runs that
    // process the same nodes in a different order would make a floating-point
    // sum differ, and ADR-0021's oracle compares bytes.
    std::vector<NodeId> ready;
    for (std::int32_t i = 0; i < n; ++i)
        if (indegree[static_cast<std::size_t>(i)] == 0) ready.push_back(i);

    while (!ready.empty()) {
        const NodeId id = *std::min_element(ready.begin(), ready.end());
        ready.erase(std::find(ready.begin(), ready.end(), id));
        order_.push_back(id);

        for (std::int32_t j = 0; j < n; ++j) {
            auto& ins = slots_[static_cast<std::size_t>(j)].inputs;
            if (std::find(ins.begin(), ins.end(), id) == ins.end()) continue;
            if (--indegree[static_cast<std::size_t>(j)] == 0) ready.push_back(j);
        }
    }

    if (static_cast<std::int32_t>(order_.size()) != n) {
        // A feedback loop. Refused rather than broken by inserting a delay:
        // a graph that silently became something other than what was asked for
        // is worse than one that says it cannot run.
        error_ = "the graph has a cycle; " + std::to_string(order_.size()) +
                 " of " + std::to_string(n) + " nodes are reachable in order";
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// prepare / release
// ---------------------------------------------------------------------------

void Graph::prepare(double sampleRate, std::int32_t maxFrames) {
    ok_ = false;
    prepared_ = false;
    error_.clear();
    stats_ = GraphStats{};

    if (maxFrames <= 0) { error_ = "maxFrames must be positive"; return; }
    if (slots_.empty()) { error_ = "the graph has no nodes"; return; }
    if (output_ < 0 || output_ >= static_cast<NodeId>(slots_.size())) {
        error_ = "no output node set";
        return;
    }
    if (!topoSort()) return;

    sampleRate_ = sampleRate;
    maxFrames_ = maxFrames;

    // ADR-0042's floor, clamped by ADR-0054's bound. Clamped rather than
    // rejected: a caller asking for 128 at 48 kHz wants fewer segments, and
    // giving them 96 is closer to the request than refusing to run.
    const std::int32_t cap = maxFloorFor(sampleRate);
    if (floor_ > cap) floor_ = cap;
    if (floor_ < 1) floor_ = 1;

    const auto ch = static_cast<std::size_t>(channels_);
    const auto fr = static_cast<std::size_t>(maxFrames);

    for (auto& s : slots_) {
        s.audio.assign(ch * fr, 0.0f);
        s.chanPtrs.resize(ch);
        for (std::size_t c = 0; c < ch; ++c) s.chanPtrs[c] = s.audio.data() + c * fr;
        s.eventStore.assign(static_cast<std::size_t>(eventCapacity_), Event{});
        s.events = EventList(s.eventStore.data(), eventCapacity_);
        // ARMED FROM THE DECLARATION, not zero. Starting at zero suspends a
        // kInfiniteTail node on its very first block -- the one thing ADR-0043
        // says must never happen -- because the counter has expired before
        // anything has had a chance to arm it. It cost a generator that never
        // ran and four tests that looked like fixture errors.
        const std::int64_t t = s.node != nullptr ? s.node->tailSamples() : 0;
        s.tailRemaining = (t == kInfiniteTail) ? 0 : t;
        s.silent = true;
        if (s.node != nullptr) s.node->prepare(sampleRate, maxFrames);
    }

    mixBuf_.assign(ch * fr, 0.0f);
    mixPtrs_.resize(ch);
    for (std::size_t c = 0; c < ch; ++c) mixPtrs_[c] = mixBuf_.data() + c * fr;
    mixEventStore_.assign(static_cast<std::size_t>(eventCapacity_), Event{});
    mixEvents_ = EventList(mixEventStore_.data(), eventCapacity_);

    // One more than the worst case: every distinct frame a split, plus the end.
    splits_.assign(fr + 2, 0);

    ok_ = true;
    prepared_ = true;
}

void Graph::release() {
    for (auto& s : slots_)
        if (s.node != nullptr) s.node->release();
    prepared_ = false;
    ok_ = false;
}

// ---------------------------------------------------------------------------
// process
// ---------------------------------------------------------------------------

void Graph::accumulate(const Slot& src, std::int32_t begin, std::int32_t frames,
                       bool first) noexcept {
    const auto ch = static_cast<std::size_t>(channels_);
    for (std::size_t c = 0; c < ch; ++c) {
        const float* s = src.chanPtrs[c] + begin;
        float* d = mixPtrs_[c] + begin;
        if (first) {
            std::memcpy(d, s, static_cast<std::size_t>(frames) * sizeof(float));
        } else {
            for (std::int32_t i = 0; i < frames; ++i) d[i] += s[i];
        }
    }
}

void Graph::process(const AudioIo& io) noexcept {
    // Silence first and unconditionally, before any early return. A callback
    // that returns without writing hands the driver whatever was in that
    // memory, and on a first callback that is uninitialised.
    for (std::int32_t c = 0; c < io.numOut; ++c) {
        float* dst = io.out != nullptr ? io.out[c] : nullptr;
        if (dst == nullptr) continue;
        for (std::int32_t i = 0; i < io.frames; ++i) dst[i] = 0.0f;
    }

    if (!ok_ || !prepared_) return;
    if (io.frames <= 0 || io.frames > maxFrames_) return;

    ++stats_.blocks;
    const std::int32_t frames = io.frames;
    const auto ch = static_cast<std::size_t>(channels_);

    // --- segment boundaries (ADR-0042) -------------------------------------
    //
    // Split at every DISTINCT event frame, not at every event. A 500 Hz MPE+
    // frame carrying ten notes across three dimensions is one instant, not
    // thirty, so the bound is 500 splits per second rather than 15,000 --
    // which is what makes ADR-0054's requirement affordable at all.
    std::int32_t nsplit = 0;
    splits_[static_cast<std::size_t>(nsplit++)] = 0;
    for (auto& s : slots_) {
        s.events.sortByFrame();
        for (const Event& e : s.events) {
            if (e.frame <= 0 || e.frame >= frames) continue;
            const std::int32_t last = splits_[static_cast<std::size_t>(nsplit - 1)];
            if (e.frame - last < floor_) continue;   // coalesce to the boundary
            splits_[static_cast<std::size_t>(nsplit++)] = e.frame;
        }
    }
    std::sort(splits_.begin(), splits_.begin() + nsplit);
    nsplit = static_cast<std::int32_t>(
        std::unique(splits_.begin(), splits_.begin() + nsplit) - splits_.begin());
    splits_[static_cast<std::size_t>(nsplit)] = frames;

    stats_.segments += nsplit;

    // --- ADR-0043: decide suspension once per block, not per segment -------
    //
    // Per block because the tail counter is in samples and a decision that
    // changed mid-block would let a node run for part of its own tail. The
    // cost is that a node wakes at a block boundary rather than at the exact
    // sample its input returns, which is inaudible and much easier to reason
    // about.
    for (NodeId id : order_) {
        Slot& s = slots_[static_cast<std::size_t>(id)];

        bool inputSilent = true;
        for (NodeId in : s.inputs)
            if (!slots_[static_cast<std::size_t>(in)].silent) inputSilent = false;

        // Events are NOT silence. A node with a pending event, or an
        // instrument with no audio input at all, must run -- this is the bug
        // every implementation of this feature ships once, because naive
        // silence detection suspends every synth in the project.
        const bool hasEvents = !s.events.empty();

        // An infinite tail is a SEPARATE FLAG, not a sentinel in the counter.
        // Storing kInfiniteTail and decrementing it works -- INT64_MAX takes
        // some quadrillions of blocks to reach zero -- but it makes the
        // never-suspend rule impossible to test: planting a defect in the
        // guard changes nothing observable at any realistic duration. A
        // decision that cannot be falsified is one nobody can maintain, so it
        // is a branch instead, and the counter stays a counter.
        const bool infinite = s.node->tailSamples() == kInfiniteTail;

        if (!inputSilent || hasEvents) {
            s.tailRemaining = infinite ? 0 : s.node->tailSamples();
        } else if (!infinite) {
            s.tailRemaining -= frames;
            if (s.tailRemaining < 0) s.tailRemaining = 0;
        }

        const bool suspend = !infinite && !s.node->alwaysProcess() &&
                             inputSilent && !hasEvents && s.tailRemaining == 0;
        if (suspend) {
            ++stats_.nodesSuspended;
            s.silent = true;
            for (std::size_t c = 0; c < ch; ++c)
                std::memset(s.chanPtrs[c], 0,
                            static_cast<std::size_t>(frames) * sizeof(float));
            continue;
        }

        // --- run it, segment by segment ------------------------------------
        for (std::int32_t seg = 0; seg < nsplit; ++seg) {
            const std::int32_t begin = splits_[static_cast<std::size_t>(seg)];
            const std::int32_t end = splits_[static_cast<std::size_t>(seg + 1)];
            const std::int32_t n = end - begin;
            if (n <= 0) continue;

            // Sum this node's inputs into the scratch buffer. Summing here,
            // for every node, is what makes a group (ADR-0044) a node with no
            // device rather than a special case in the scheduler.
            bool any = false;
            for (NodeId in : s.inputs) {
                accumulate(slots_[static_cast<std::size_t>(in)], begin, n, !any);
                any = true;
            }

            NodeIo nio;
            nio.in = any ? mixPtrs_.data() : nullptr;
            nio.out = s.chanPtrs.data();
            nio.channels = channels_;
            nio.frames = n;
            nio.blockOffset = begin;
            nio.inputSilent = inputSilent;
            nio.sampleRate = sampleRate_;

            // Events of this segment only, in block-relative frames.
            const Event* first = nullptr;
            std::int32_t count = 0;
            for (const Event& e : s.events) {
                if (e.frame >= begin && e.frame < end) {
                    if (first == nullptr) first = &e;
                    ++count;
                }
            }
            nio.events = EventSpan{first, count};

            s.node->process(nio);
            ++stats_.nodeCalls;
        }

        // Is this node's output silent? Measured rather than declared: a node
        // that claims silence and writes samples is a bug the next node
        // inherits, and the check is one pass over a buffer we have just
        // written and is therefore in cache.
        bool allZero = true;
        for (std::size_t c = 0; c < ch && allZero; ++c)
            for (std::int32_t i = 0; i < frames; ++i)
                if (s.chanPtrs[c][i] != 0.0f) { allZero = false; break; }
        s.silent = allZero;
    }

    // --- out ---------------------------------------------------------------
    const Slot& outSlot = slots_[static_cast<std::size_t>(output_)];
    const std::int32_t n = (std::min)(io.numOut, channels_);
    for (std::int32_t c = 0; c < n; ++c) {
        float* dst = io.out != nullptr ? io.out[c] : nullptr;
        if (dst == nullptr) continue;
        std::memcpy(dst, outSlot.chanPtrs[static_cast<std::size_t>(c)],
                    static_cast<std::size_t>(frames) * sizeof(float));
    }

    for (auto& s : slots_) {
        stats_.eventsDropped += s.events.dropped();
        s.events.resetDropped();
        s.events.clear();
    }
}

// ---------------------------------------------------------------------------
// Built-in nodes
// ---------------------------------------------------------------------------

void SumNode::process(const NodeIo& io) noexcept {
    for (std::int32_t c = 0; c < io.channels; ++c) {
        float* out = io.out[c];
        if (io.in == nullptr || io.in[c] == nullptr) {
            std::memset(out + io.blockOffset, 0,
                        static_cast<std::size_t>(io.frames) * sizeof(float));
            continue;
        }
        std::memcpy(out + io.blockOffset, io.in[c] + io.blockOffset,
                    static_cast<std::size_t>(io.frames) * sizeof(float));
    }
}

void GainNode::process(const NodeIo& io) noexcept {
    // The value is taken at the START of the segment and held across it. That
    // is exactly what sub-block splitting buys: the segment is short because
    // the scheduler cut it at the event, so "held across the segment" is a
    // ramp at segment resolution rather than a step at block resolution.
    for (const Event& e : io.events)
        if (e.type == EventType::ParamValue && e.paramId == paramId_)
            gain_ = static_cast<float>(e.value);

    for (std::int32_t c = 0; c < io.channels; ++c) {
        float* out = io.out[c] + io.blockOffset;
        const float* in = (io.in != nullptr && io.in[c] != nullptr)
                              ? io.in[c] + io.blockOffset : nullptr;
        if (in == nullptr) {
            std::memset(out, 0, static_cast<std::size_t>(io.frames) * sizeof(float));
            continue;
        }
        for (std::int32_t i = 0; i < io.frames; ++i) out[i] = in[i] * gain_;
    }
}

}  // namespace adi::engine
