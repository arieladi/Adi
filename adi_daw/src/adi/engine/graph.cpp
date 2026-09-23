// SPDX-License-Identifier: GPL-3.0-or-later

#include "adi/engine/graph.hpp"

#include <algorithm>
#include <cstring>
#include <random>

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

bool Graph::connect(NodeId from, NodeId to, Bus bus) {
    const auto n = static_cast<NodeId>(slots_.size());
    if (from < 0 || from >= n || to < 0 || to >= n || from == to) return false;
    Slot& dst = slots_[static_cast<std::size_t>(to)];
    if (bus == Bus::Sidechain) dst.sidechains.push_back(from);
    else                       dst.inputs.push_back(from);
    prepared_ = false;
    return true;
}

bool Graph::pushInputEvent(NodeId to, const Event& e) {
    if (to < 0 || to >= static_cast<NodeId>(slots_.size())) return false;
    return slots_[static_cast<std::size_t>(to)].events.push(e);
}

std::int32_t Graph::deriveEventCapacity(double sampleRate, std::int32_t maxFrames,
                                        std::int32_t polyphony) noexcept {
    // ADR-0054's instrument at ADR-0049's largest block: 500 Hz update frames
    // x three dimensions x polyphony, plus a note-on and note-off each, plus a
    // floor so a 64-frame block still holds a chord arriving at once.
    if (sampleRate <= 0.0 || maxFrames <= 0) return 1024;
    const double frames = 500.0 * (static_cast<double>(maxFrames) / sampleRate);
    const double total = frames * 3.0 * static_cast<double>(polyphony)
                       + 2.0 * static_cast<double>(polyphony);
    const auto v = static_cast<std::int32_t>(total) + 64;
    return v < 256 ? 256 : v;
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

void Graph::permuteLevelsForTest(std::uint32_t seed) {
    std::mt19937 random(seed);
    for (auto& level : levels_) {
        std::sort(level.begin(), level.end());
        // Explicit Fisher-Yates: unlike std::shuffle, this mapping from a
        // seed to a permutation is the same across standard libraries.
        for (std::size_t count = level.size(); count > 1; --count) {
            const auto choice = static_cast<std::size_t>(random()) % count;
            std::swap(level[count - 1], level[choice]);
        }
    }
}

bool Graph::topoSort() {
    const auto n = static_cast<std::int32_t>(slots_.size());
    order_.clear();
    order_.reserve(static_cast<std::size_t>(n));

    std::vector<std::int32_t> indegree(static_cast<std::size_t>(n), 0);
    for (std::int32_t i = 0; i < n; ++i) {
        const Slot& sl = slots_[static_cast<std::size_t>(i)];
        indegree[static_cast<std::size_t>(i)] =
            static_cast<std::int32_t>(sl.inputs.size() + sl.sidechains.size());
    }

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
            Slot& sj = slots_[static_cast<std::size_t>(j)];
            std::int32_t edges = 0;
            for (NodeId in : sj.inputs)     if (in == id) ++edges;
            for (NodeId in : sj.sidechains) if (in == id) ++edges;
            if (edges == 0) continue;
            indegree[static_cast<std::size_t>(j)] -= edges;
            if (indegree[static_cast<std::size_t>(j)] == 0) ready.push_back(j);
        }
    }

    // Dependency depth. Every node in one level depends only on levels below
    // it, so a level may run in any order -- including concurrently -- and the
    // output is unchanged, because each node writes its own buffer and every
    // sum happens in its consumer's fixed input order (ADR-0056).
    levels_.clear();
    if (static_cast<std::int32_t>(order_.size()) == n) {
        for (NodeId id : order_) {
            Slot& sl = slots_[static_cast<std::size_t>(id)];
            std::int32_t depth = 0;
            for (NodeId in : sl.inputs)
                depth = (std::max)(depth, slots_[static_cast<std::size_t>(in)].level + 1);
            for (NodeId in : sl.sidechains)
                depth = (std::max)(depth, slots_[static_cast<std::size_t>(in)].level + 1);
            sl.level = depth;
            if (static_cast<std::int32_t>(levels_.size()) <= depth)
                levels_.resize(static_cast<std::size_t>(depth) + 1);
            levels_[static_cast<std::size_t>(depth)].push_back(id);
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
// Delay, and the compensation that uses it
// ---------------------------------------------------------------------------

void DelayLine::prepare(std::int32_t channels, std::int32_t capacity) {
    channels_ = channels > 0 ? channels : 0;
    capacity_ = capacity > 0 ? capacity : 0;
    // ONE MORE THAN THE CAPACITY. The write happens before the read, so a tap
    // at distance `capacity_` must land on a slot the write has not just
    // overwritten. With a ring of exactly `capacity_` it lands on the write
    // index itself and the longest delay silently becomes no delay at all.
    ring_ = capacity_ > 0 ? capacity_ + 1 : 0;
    buf_.assign(static_cast<std::size_t>(channels_) * static_cast<std::size_t>(ring_),
                0.0f);
    delay_ = 0;
    write_ = 0;
    incoming_.clear();
    inCapacity_ = inRing_ = inWrite_ = targetDelay_ = primeRemaining_ = 0;
    target_.store(0, std::memory_order_relaxed);
    gliding_.store(false, std::memory_order_relaxed);
    grow_.store(Grow::Idle, std::memory_order_relaxed);
}

void DelayLine::setDelay(std::int32_t d) noexcept {
    if (d < 0) d = 0;
    if (d > capacity_) d = capacity_;
    delay_ = d;
    target_.store(d, std::memory_order_relaxed);
    gliding_.store(false, std::memory_order_relaxed);
}

void DelayLine::reset() noexcept {
    for (auto& v : buf_) v = 0.0f;
    write_ = 0;
}

bool DelayLine::beginGlide(std::int32_t d) noexcept {
    if (d < 0 || d > capacity_) return false;
    if (d == delay_) return true;               // nothing to do, and not a failure
    target_.store(d, std::memory_order_relaxed);
    gliding_.store(true, std::memory_order_release);
    return true;
}

bool DelayLine::offerRing(std::vector<float> ring, std::int32_t capacity,
                          std::int32_t targetDelay) {
    if (grow_.load(std::memory_order_acquire) != Grow::Idle) return false;
    if (capacity <= 0 || targetDelay < 0 || targetDelay > capacity) return false;
    const std::int32_t r = capacity + 1;
    if (ring.size() < static_cast<std::size_t>(channels_) * static_cast<std::size_t>(r))
        return false;

    incoming_ = std::move(ring);
    for (auto& v : incoming_) v = 0.0f;   // message thread: the audio thread must not
    inCapacity_ = capacity;
    inRing_ = r;
    inWrite_ = 0;
    targetDelay_ = targetDelay;
    // The new ring is valid for a tap at `targetDelay` only once it holds that
    // many real samples. Until then it holds zeros, and reading it would be a
    // fade to silence rather than a change of alignment.
    primeRemaining_ = targetDelay;

    // RELEASE, and last: everything above must be visible to the audio thread
    // before it can observe the state that tells it to look.
    grow_.store(Grow::Priming, std::memory_order_release);
    return true;
}

std::vector<float> DelayLine::collectRing() {
    if (grow_.load(std::memory_order_acquire) != Grow::Spent) return {};
    std::vector<float> old = std::move(incoming_);
    incoming_.clear();
    inCapacity_ = inRing_ = inWrite_ = 0;
    grow_.store(Grow::Idle, std::memory_order_release);
    return old;
}

void DelayLine::adoptHistory(const DelayLine& old) noexcept {
    if (ring_ <= 0 || old.ring_ <= 0 || channels_ != old.channels_) return;

    std::int32_t k = delay_;
    if (k > capacity_) k = capacity_;
    if (k > old.capacity_) k = old.capacity_;
    if (k <= 0) return;

    // `buf_` and `write_` are the live history in every state: while an old
    // ring is growing both rings are written and `buf_` is the one read, and
    // after its swap `buf_` IS the grown ring. So nothing here has to know
    // what the old line was in the middle of.
    for (std::int32_t c = 0; c < channels_; ++c) {
        const float* src = old.buf_.data() + static_cast<std::size_t>(c) *
                                             static_cast<std::size_t>(old.ring_);
        float* dst = buf_.data() + static_cast<std::size_t>(c) *
                                   static_cast<std::size_t>(ring_);
        for (std::int32_t i = 0; i < k; ++i) {
            // dst[0] is the OLDEST sample carried and dst[k-1] the newest.
            // `old.write_` is the NEXT write, so the newest written sample is
            // one behind it.
            std::int32_t r = (old.write_ - k + i) % old.ring_;
            if (r < 0) r += old.ring_;
            dst[i] = src[r];
        }
    }
    // The next write lands right after the newest carried sample, so a tap at
    // `delay_` reads exactly what the old line's tap would have read next.
    write_ = k % ring_;
}

void DelayLine::endEdge() noexcept {
    if (gliding_.load(std::memory_order_acquire)) {
        delay_ = target_.load(std::memory_order_relaxed);
        gliding_.store(false, std::memory_order_relaxed);
    }

    const Grow g = grow_.load(std::memory_order_acquire);
    if (g == Grow::Priming) {
        // PRIMING ENDS ON A BLOCK BOUNDARY, not mid-call. Letting it end
        // mid-call means one call that is part prime and part crossfade, and
        // the bookkeeping for that is worth more than the one extra block it
        // saves.
        if (primeRemaining_ <= 0) {
            primeRemaining_ = 0;
            grow_.store(Grow::Fading, std::memory_order_release);
        }
        return;
    }
    if (g == Grow::Fading) {
        // The swap. A vector swap moves pointers -- no allocation, which is
        // what lets this happen on the audio thread at all. The old buffer
        // lands in `incoming_` and waits for `collectRing`; the audio thread
        // never deallocates.
        buf_.swap(incoming_);
        std::swap(capacity_, inCapacity_);
        std::swap(ring_, inRing_);
        write_ = inWrite_;
        delay_ = targetDelay_;
        target_.store(delay_, std::memory_order_relaxed);
        grow_.store(Grow::Spent, std::memory_order_release);
    }
}

void DelayLine::processGrowing(std::int32_t channel, const float* src, float* dst,
                               std::int32_t frames, Grow state) noexcept {
    float* oldHist = buf_.data() + static_cast<std::size_t>(channel) *
                                   static_cast<std::size_t>(ring_);
    float* newHist = incoming_.data() + static_cast<std::size_t>(channel) *
                                        static_cast<std::size_t>(inRing_);
    std::int32_t w = write_, nw = inWrite_;

    // BOTH RINGS ARE WRITTEN THROUGHOUT. The new one is accumulating the
    // history it will need; the old one is still the one being read, and will
    // be until the very last sample of the fade.
    if (state == Grow::Priming) {
        const std::int32_t d = delay_;
        for (std::int32_t i = 0; i < frames; ++i) {
            const float in = src[i];   // src == dst is legal: read before writing
            oldHist[w] = in;
            newHist[nw] = in;
            dst[i] = tapAt(oldHist, w, ring_, d);
            if (++w == ring_) w = 0;
            if (++nw == inRing_) nw = 0;
        }
        write_ = w;
        inWrite_ = nw;
        primeRemaining_ -= frames;
        if (primeRemaining_ < 0) primeRemaining_ = 0;
        return;
    }

    // Fading: the old tap and the new one are genuinely different samples --
    // that is what a latency change IS -- so this is a crossfade and not a
    // reconciliation. One block, same as ADR-0079's tap move, except the two
    // taps live in different rings.
    const std::int32_t dOld = delay_;
    const std::int32_t dNew = targetDelay_;
    const float step = frames > 0 ? 1.0f / static_cast<float>(frames) : 0.0f;
    float t = 0.0f;
    for (std::int32_t i = 0; i < frames; ++i) {
        const float in = src[i];
        oldHist[w] = in;
        newHist[nw] = in;
        t += step;
        const float a = tapAt(oldHist, w, ring_, dOld);
        const float b = tapAt(newHist, nw, inRing_, dNew);
        dst[i] = a + (b - a) * t;
        if (++w == ring_) w = 0;
        if (++nw == inRing_) nw = 0;
    }
    write_ = w;
    inWrite_ = nw;
}

void DelayLine::process(std::int32_t channel, const float* src, float* dst,
                        std::int32_t frames) noexcept {
    if (ring_ <= 0 || channel < 0 || channel >= channels_) {
        if (src != dst)
            std::memcpy(dst, src, static_cast<std::size_t>(frames) * sizeof(float));
        return;
    }

    const Grow g = grow_.load(std::memory_order_acquire);
    if (g == Grow::Priming || g == Grow::Fading) {
        processGrowing(channel, src, dst, frames, g);
        return;
    }

    float* hist = buf_.data() + static_cast<std::size_t>(channel) *
                                static_cast<std::size_t>(ring_);
    std::int32_t w = write_;

    const bool glide = gliding_.load(std::memory_order_acquire);
    const std::int32_t to = glide ? target_.load(std::memory_order_relaxed) : delay_;

    if (!glide || to == delay_ || frames <= 0) {
        const std::int32_t d = delay_;
        for (std::int32_t i = 0; i < frames; ++i) {
            const float in = src[i];   // src == dst is legal, so read before writing
            hist[w] = in;
            dst[i] = tapAt(hist, w, ring_, d);
            if (++w == ring_) w = 0;
        }
        write_ = w;
        return;
    }

    // THE TAP MOVES, THE AUDIO IS RENDERED ONCE (ADR-0079).
    //
    // Both taps read the SAME history, which is the whole reason this works
    // and the reason a freshly published buffer cannot: a new ring holds
    // nothing, so fading into it is a fade to silence for `delay` samples,
    // not a transition between two alignments.
    //
    // A latency change IS a time shift, so the two taps genuinely differ and
    // no crossfade makes that inaudible. What it does buy is that the
    // difference arrives as a brief flange rather than as a click.
    const std::int32_t from = delay_;
    const float step = 1.0f / static_cast<float>(frames);
    float t = 0.0f;
    for (std::int32_t i = 0; i < frames; ++i) {
        const float in = src[i];
        hist[w] = in;
        t += step;
        const float a = tapAt(hist, w, ring_, from);
        const float b = tapAt(hist, w, ring_, to);
        dst[i] = a + (b - a) * t;
        if (++w == ring_) w = 0;
    }
    write_ = w;
}

void Graph::prepareLine(DelayLine& line, std::int32_t delaySamples) {
    const std::int32_t d = delaySamples > 0 ? delaySamples : 0;
    // The ring is the delay PLUS the headroom, so a plugin that switches to
    // linear phase can move its tap without anyone allocating (ADR-0079).
    // Headroom 0 -- the default -- gives exactly today's behaviour and costs
    // exactly today's memory, so a project that never changes latency at
    // runtime pays nothing for the ability.
    //
    // An edge with no delay AND no headroom gets no ring at all. With headroom
    // it gets one, because an edge at zero today is the one most likely to
    // need a delay tomorrow: it is the direct path everything else is
    // compensated against.
    const std::int32_t cap = d + latencyHeadroom_;
    line.prepare(channels_, cap);
    line.setDelay(d);
}

void Graph::computeCompensation() {
    // ADR-0058 decision 2. A node's input is whole only once the LATEST of its
    // feeds has arrived, so its arrival is the max over inputs of (that input's
    // arrival + that input's own latency). Every earlier feed is then delayed
    // to match, and that is the whole of phase alignment.
    //
    // Walking `order_` means every input's arrival is already final when it is
    // read. That is what the topological order is for, beyond scheduling.
    for (NodeId id : order_) {
        Slot& s = slots_[static_cast<std::size_t>(id)];
        std::int64_t latest = 0;
        for (NodeId in : s.inputs) {
            const Slot& u = slots_[static_cast<std::size_t>(in)];
            const std::int64_t ready = static_cast<std::int64_t>(u.arrival) +
                (u.node != nullptr ? u.node->latencySamples() : 0);
            if (ready > latest) latest = ready;
        }
        // ADR-0058 decision 5: a sidechain is compensated WITH the rest, not
        // apart from it. A compressor whose key arrives early ducks early,
        // which is the same defect as a misaligned kick and harder to hear.
        for (NodeId in : s.sidechains) {
            const Slot& u = slots_[static_cast<std::size_t>(in)];
            const std::int64_t ready = static_cast<std::int64_t>(u.arrival) +
                (u.node != nullptr ? u.node->latencySamples() : 0);
            if (ready > latest) latest = ready;
        }
        s.arrival = static_cast<std::int32_t>(latest);
    }

    for (NodeId id : order_) {
        Slot& s = slots_[static_cast<std::size_t>(id)];
        s.inDelays.resize(s.inputs.size());
        for (std::size_t k = 0; k < s.inputs.size(); ++k) {
            const Slot& u = slots_[static_cast<std::size_t>(s.inputs[k])];
            const std::int32_t ready =
                u.arrival + (u.node != nullptr ? u.node->latencySamples() : 0);
            const std::int32_t d = s.arrival - ready;
            prepareLine(s.inDelays[k], d);
        }
        s.sideDelays.resize(s.sidechains.size());
        for (std::size_t k = 0; k < s.sidechains.size(); ++k) {
            const Slot& u = slots_[static_cast<std::size_t>(s.sidechains[k])];
            const std::int32_t ready =
                u.arrival + (u.node != nullptr ? u.node->latencySamples() : 0);
            const std::int32_t d = s.arrival - ready;
            prepareLine(s.sideDelays[k], d);
        }
    }

    // ADR-0123: the answers, kept for the audio thread. Asked here, on the
    // message thread, because a node may be a CLAP plugin whose latency
    // getter is main-thread only.
    if (slotLatency_.size() != slots_.size())
        slotLatency_ = std::vector<std::atomic<std::int32_t>>(slots_.size());
    for (std::size_t i = 0; i < slots_.size(); ++i)
        slotLatency_[i].store(slots_[i].node != nullptr ? slots_[i].node->latencySamples() : 0,
                              std::memory_order_relaxed);
    const Slot& out = slots_[static_cast<std::size_t>(output_)];
    graphLatency_.store(out.arrival + (out.node != nullptr ? out.node->latencySamples() : 0),
                        std::memory_order_release);
}

bool Graph::retapLatency() noexcept {
    if (!prepared_) return false;

    // The same arithmetic as `computeCompensation`, against the latencies the
    // nodes report NOW. Deliberately not factored into one function with it:
    // that one ALLOCATES and this one must not, and a shared helper is one
    // edit away from allocating on the audio thread's behalf.
    bool allFit = true;

    for (NodeId id : order_) {
        Slot& s = slots_[static_cast<std::size_t>(id)];
        std::int64_t latest = 0;
        for (NodeId in : s.inputs) {
            const Slot& u = slots_[static_cast<std::size_t>(in)];
            const std::int64_t ready = static_cast<std::int64_t>(u.arrival) +
                (u.node != nullptr ? u.node->latencySamples() : 0);
            if (ready > latest) latest = ready;
        }
        for (NodeId in : s.sidechains) {
            const Slot& u = slots_[static_cast<std::size_t>(in)];
            const std::int64_t ready = static_cast<std::int64_t>(u.arrival) +
                (u.node != nullptr ? u.node->latencySamples() : 0);
            if (ready > latest) latest = ready;
        }
        s.arrival = static_cast<std::int32_t>(latest);
    }

    for (NodeId id : order_) {
        Slot& s = slots_[static_cast<std::size_t>(id)];
        for (std::size_t k = 0; k < s.inputs.size() && k < s.inDelays.size(); ++k) {
            const Slot& u = slots_[static_cast<std::size_t>(s.inputs[k])];
            const std::int32_t ready =
                u.arrival + (u.node != nullptr ? u.node->latencySamples() : 0);
            const std::int32_t d = s.arrival - ready;
            // EVERYTHING THAT FITS IS MOVED, even when something else does not.
            // A partial correction is closer to right than none, and the edges
            // that failed are about to be rebuilt anyway.
            if (!s.inDelays[k].beginGlide(d > 0 ? d : 0)) allFit = false;
        }
        for (std::size_t k = 0; k < s.sidechains.size() && k < s.sideDelays.size(); ++k) {
            const Slot& u = slots_[static_cast<std::size_t>(s.sidechains[k])];
            const std::int32_t ready =
                u.arrival + (u.node != nullptr ? u.node->latencySamples() : 0);
            const std::int32_t d = s.arrival - ready;
            if (!s.sideDelays[k].beginGlide(d > 0 ? d : 0)) allFit = false;
        }
    }

    // ADR-0123: refresh the audio thread's copies. The event delay jumps to the
    // new value while the audio tap glides; that is what it did before, when
    // the audio thread asked the node itself.
    for (std::size_t i = 0; i < slots_.size() && i < slotLatency_.size(); ++i)
        slotLatency_[i].store(slots_[i].node != nullptr ? slots_[i].node->latencySamples() : 0,
                              std::memory_order_relaxed);
    const Slot& out = slots_[static_cast<std::size_t>(output_)];
    graphLatency_.store(out.arrival + (out.node != nullptr ? out.node->latencySamples() : 0),
                        std::memory_order_release);
    return allFit;
}

namespace {

/// The required delay on one edge, from the arrivals already computed.
std::int32_t requiredDelay(std::int32_t toArrival, std::int32_t fromArrival,
                           const Node* fromNode) noexcept {
    const std::int32_t ready =
        fromArrival + (fromNode != nullptr ? fromNode->latencySamples() : 0);
    const std::int32_t d = toArrival - ready;
    return d > 0 ? d : 0;
}

}  // namespace

std::size_t Graph::escalateLatency() {
    if (!prepared_) return 0;

    std::size_t grown = 0;
    // THE ALLOCATION IS HERE, on the message thread, and that is the whole
    // point of the split. The audio thread receives a ready-made buffer and
    // never does anything but write into it and swap a pointer.
    auto offer = [&](DelayLine& line, std::int32_t want) {
        if (want <= line.capacity()) return;   // the tap move already handles it
        if (line.growing()) return;            // an offer is already in flight

        // Headroom on top of the requirement, so a plugin that steps its
        // latency up repeatedly does not force a grow per step. Doubling is
        // the usual answer and is wrong here: a 2-sample edge growing to 4 is
        // a grow every time. The requirement plus the graph's headroom gives
        // the same slack the edge would have had if it had been built at this
        // size in the first place.
        const std::int32_t cap = want + latencyHeadroom_;
        std::vector<float> ring(
            static_cast<std::size_t>(channels_) * static_cast<std::size_t>(cap + 1),
            0.0f);
        if (line.offerRing(std::move(ring), cap, want)) ++grown;
    };

    for (NodeId id : order_) {
        Slot& s = slots_[static_cast<std::size_t>(id)];
        for (std::size_t k = 0; k < s.inputs.size() && k < s.inDelays.size(); ++k) {
            const Slot& u = slots_[static_cast<std::size_t>(s.inputs[k])];
            offer(s.inDelays[k], requiredDelay(s.arrival, u.arrival, u.node));
        }
        for (std::size_t k = 0; k < s.sidechains.size() && k < s.sideDelays.size(); ++k) {
            const Slot& u = slots_[static_cast<std::size_t>(s.sidechains[k])];
            offer(s.sideDelays[k], requiredDelay(s.arrival, u.arrival, u.node));
        }
    }
    return grown;
}

std::size_t Graph::collectRings() {
    std::size_t freed = 0;
    // The returned vector is destroyed here, on the message thread. That is
    // the deallocation the audio thread deliberately did not do.
    auto take = [&](DelayLine& line) {
        std::vector<float> old = line.collectRing();
        if (!old.empty()) ++freed;
    };
    for (Slot& s : slots_) {
        for (DelayLine& d : s.inDelays) take(d);
        for (DelayLine& d : s.sideDelays) take(d);
    }
    return freed;
}

DelayLine* Graph::edgeLine(NodeId from, NodeId to, Bus bus) noexcept {
    if (to < 0 || to >= static_cast<NodeId>(slots_.size())) return nullptr;
    Slot& s = slots_[static_cast<std::size_t>(to)];
    auto& edges = (bus == Bus::Sidechain) ? s.sidechains : s.inputs;
    auto& lines = (bus == Bus::Sidechain) ? s.sideDelays : s.inDelays;
    for (std::size_t k = 0; k < edges.size() && k < lines.size(); ++k)
        if (edges[k] == from) return &lines[k];
    return nullptr;
}

std::size_t Graph::compensationBytes() const noexcept {
    std::size_t n = 0;
    for (const Slot& s : slots_) {
        for (const DelayLine& d : s.inDelays) n += d.bytes();
        for (const DelayLine& d : s.sideDelays) n += d.bytes();
    }
    return n;
}

std::size_t Graph::growingEdges() const noexcept {
    std::size_t n = 0;
    for (const Slot& s : slots_) {
        for (const DelayLine& d : s.inDelays) if (d.growing()) ++n;
        for (const DelayLine& d : s.sideDelays) if (d.growing()) ++n;
    }
    return n;
}

std::int32_t Graph::arrivalOf(NodeId id) const noexcept {
    if (id < 0 || id >= static_cast<NodeId>(slots_.size())) return -1;
    return slots_[static_cast<std::size_t>(id)].arrival;
}

std::int32_t Graph::compensationFor(NodeId from, NodeId to, Bus bus) const noexcept {
    if (to < 0 || to >= static_cast<NodeId>(slots_.size())) return -1;
    const Slot& s = slots_[static_cast<std::size_t>(to)];
    const auto& edges = (bus == Bus::Sidechain) ? s.sidechains : s.inputs;
    const auto& lines = (bus == Bus::Sidechain) ? s.sideDelays : s.inDelays;
    for (std::size_t k = 0; k < edges.size() && k < lines.size(); ++k)
        if (edges[k] == from) return lines[k].delay();
    return -1;
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

    if (eventCapacity_ <= 0)
        eventCapacity_ = deriveEventCapacity(sampleRate, maxFrames, maxPolyphony_);

    const auto ch = static_cast<std::size_t>(channels_);
    const auto fr = static_cast<std::size_t>(maxFrames);

    for (auto& s : slots_) {
        s.audio.assign(ch * fr, 0.0f);
        s.chanPtrs.resize(ch);
        for (std::size_t c = 0; c < ch; ++c) s.chanPtrs[c] = s.audio.data() + c * fr;
        s.eventStore.assign(static_cast<std::size_t>(eventCapacity_), Event{});
        s.events = EventList(s.eventStore.data(), eventCapacity_);
        // Same capacity as the live list. A deferred event is one that WOULD
        // have been in the list if its delay had been shorter, so the two
        // share one budget rather than one being an afterthought.
        s.pending.assign(static_cast<std::size_t>(eventCapacity_), Slot::Pending{});
        s.pendingCount = 0;
        // ARMED FROM THE DECLARATION, not zero. Starting at zero suspends a
        // kInfiniteTail node on its very first block -- the one thing ADR-0043
        // says must never happen -- because the counter has expired before
        // anything has had a chance to arm it. It cost a generator that never
        // ran and four tests that looked like fixture errors.
        const std::int64_t t = s.node != nullptr ? s.node->tailSamples() : 0;
        s.tailRemaining = (t == kInfiniteTail) ? 0 : t;
        s.silent = true;
        s.zeroed = true;                 // `assign` above filled it with zeros
        if (s.node != nullptr) s.node->prepare(sampleRate, maxFrames);
    }

    mixBuf_.assign(ch * fr, 0.0f);
    mixPtrs_.resize(ch);
    for (std::size_t c = 0; c < ch; ++c) mixPtrs_[c] = mixBuf_.data() + c * fr;
    sideBuf_.assign(ch * fr, 0.0f);
    sidePtrs_.resize(ch);
    for (std::size_t c = 0; c < ch; ++c) sidePtrs_[c] = sideBuf_.data() + c * fr;
    mixEventStore_.assign(static_cast<std::size_t>(eventCapacity_), Event{});
    mixEvents_ = EventList(mixEventStore_.data(), eventCapacity_);

    // One more than the worst case: every distinct frame a split, plus the end.
    splits_.assign(fr + 2, 0);
    frameMark_.assign(fr, 0);
    now_ = 0;
    delayScratch_.assign(fr, 0.0f);

    // ADR-0058 decisions 2-5, after the buffers exist: a delay line is a
    // buffer too, and `process` may not allocate.
    computeCompensation();

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

void Graph::accumulate(std::vector<float*>& dst, const Slot& src, DelayLine& delay,
                       std::int32_t begin, std::int32_t frames, bool first) noexcept {
    const auto ch = static_cast<std::size_t>(channels_);

    // Zero compensation is the common case and must cost nothing: most edges in
    // most projects join paths of equal latency, and this is the path they take.
    //
    // A PENDING GLIDE DISQUALIFIES IT. An edge at zero that has been asked to
    // move to 128 is exactly the case this fast path would swallow: it would
    // memcpy, return, and leave the glide pending forever (ADR-0079).
    if (!delay.busy() && delay.delay() == 0) {
        // A SLEEPING SOURCE IS NOT READ. `zeroed` says its whole buffer holds
        // +0.0f (set only by the suspension clear, dropped the moment it
        // processes), and this edge has nothing in flight, so reading it is
        // 2 MB of zeros per block for 63 sleeping tracks -- the residual left
        // after #60. This is the "active inputs" list, derived rather than
        // kept: the source ran before this consumer in topological order on
        // this same thread, so the flag is current, and no signalling, no
        // second list and no lock can be more right than reading it.
        //
        // Bytes: `first` writes the zeros it would have copied, identical. A
        // later input's add of +0.0f is skipped, which differs from the sum
        // in exactly one case -- a -0.0f already in the mix stays -0.0f
        // instead of becoming +0.0f -- and testMixSkipsSleepingSources pins
        // that so it is a fact rather than a surprise. A source that is
        // silent but NOT zeroed (a generator emitting -0.0f, a node whose
        // block was all zero but never slept) is read as before.
        if (src.zeroed) {
            if (first)
                for (std::size_t c = 0; c < ch; ++c)
                    std::memset(dst[c] + begin, 0, static_cast<std::size_t>(frames) * sizeof(float));
            ++stats_.inputsSkipped;
            return;
        }
        for (std::size_t c = 0; c < ch; ++c) {
            const float* s = src.chanPtrs[c] + begin;
            float* d = dst[c] + begin;
            if (first) {
                std::memcpy(d, s, static_cast<std::size_t>(frames) * sizeof(float));
            } else {
                for (std::int32_t i = 0; i < frames; ++i) d[i] += s[i];
            }
        }
        return;
    }

    // One write cursor is shared across the channels of an edge, so every
    // channel must start from the same position and only the last may leave it
    // advanced. Each channel owns its own history region, and the cursor is the
    // only thing that says where in that region it stopped.
    //
    // Getting this wrong does NOT pull the channels apart -- every channel
    // drifts by the same (channels - 1) * frames, so left still matches right
    // exactly, which is why a stereo comparison cannot see it. What it does is
    // make the compensation the wrong LENGTH on every block after the first:
    // the ring is self-consistent within a call, so a single-block test is also
    // blind to it. It takes two blocks and a delay that does not divide the
    // block size. See testPhaseAlignment.
    const DelayLine::Cursors before = delay.cursors();
    for (std::size_t c = 0; c < ch; ++c) {
        delay.setCursors(before);
        const float* s = src.chanPtrs[c] + begin;
        float* d = dst[c] + begin;
        if (first) {
            delay.process(static_cast<std::int32_t>(c), s, d, frames);
        } else {
            // Through scratch, then summed. `process` WRITES its output; used
            // directly on a second input it would clobber the first rather than
            // add to it, and the first input would vanish silently.
            float* tmp = delayScratch_.data();
            std::memcpy(tmp, s, static_cast<std::size_t>(frames) * sizeof(float));
            delay.process(static_cast<std::int32_t>(c), tmp, tmp, frames);
            for (std::int32_t i = 0; i < frames; ++i) d[i] += tmp[i];
        }
    }
    // ONCE PER EDGE, after every channel -- the same rule as the cursor, and
    // for the same reason: the glide state is shared across the channels of
    // one edge. Ending it inside the loop would crossfade the left channel and
    // hard-switch the right.
    delay.endEdge();
}

void Graph::forwardEvents(std::int32_t frames) noexcept {
    const std::int64_t end = now_ + frames;

    // TOPOLOGICAL ORDER, which is the whole of the correctness argument: when a
    // slot is reached, every slot feeding it has already received everything it
    // will receive this block, so what it forwards is complete.
    for (NodeId id : order_) {
        Slot& s = slots_[static_cast<std::size_t>(id)];

        // 1. Anything deferred INTO this slot on an earlier block that is due
        //    now. Compacted in place: no allocation, and order does not matter
        //    because the list is sorted by frame before anyone reads it.
        if (s.pendingCount > 0) {
            std::int32_t w = 0;
            for (std::int32_t i = 0; i < s.pendingCount; ++i) {
                const Slot::Pending& p = s.pending[static_cast<std::size_t>(i)];
                if (p.due < end) {
                    Event e = p.e;
                    const std::int64_t f = p.due - now_;
                    e.frame = f > 0 ? static_cast<std::int32_t>(f) : 0;
                    s.events.push(e);            // overflow is counted by the list
                } else {
                    s.pending[static_cast<std::size_t>(w++)] = p;
                }
            }
            s.pendingCount = w;
        }

        // 2. What this slot's MAIN inputs forward to it.
        //
        // Main edges only. A sidechain is an audio KEY: a compressor keyed from
        // a kick track has no use for that track's notes, and handing them to
        // it would make every keyed plugin a second instrument. Notes from
        // another track are a note-input bus, which is a different feature and
        // is not designed yet.
        for (std::size_t k = 0; k < s.inputs.size(); ++k) {
            const Slot& u = slots_[static_cast<std::size_t>(s.inputs[k])];
            if (u.node == nullptr || u.events.empty()) continue;
            if (u.node->eventFlow() == EventFlow::Consume) continue;

            // THE SAME DELAY AS THE AUDIO BESIDE IT. The node's own latency,
            // because its output audio emerges that much later than its input;
            // plus this edge's compensation, because that is how far this input
            // is held back to meet the others. An event that skipped either
            // would reach an instrument ahead of the graph's own arithmetic --
            // ADR-0058 right for audio and wrong for everything a note drives.
            // The node's latency as last asked on the message thread, never
            // the node itself: this is the audio thread, and a CLAP plugin's
            // getter is main-thread only (linux's audit, C6; ADR-0123).
            const std::size_t ui = static_cast<std::size_t>(s.inputs[k]);
            const std::int64_t delay =
                static_cast<std::int64_t>(ui < slotLatency_.size()
                                              ? slotLatency_[ui].load(std::memory_order_relaxed)
                                              : 0) +
                (k < s.inDelays.size() ? s.inDelays[k].delay() : 0);

            for (const Event& e : u.events) {
                if (!isNoteStream(e.type)) continue;   // addressed: stays put
                const std::int64_t due = now_ + e.frame + delay;
                if (due < end) {
                    Event f = e;
                    f.frame = static_cast<std::int32_t>(due - now_);
                    if (s.events.push(f)) ++stats_.eventsForwarded;
                } else if (s.pendingCount < static_cast<std::int32_t>(s.pending.size())) {
                    s.pending[static_cast<std::size_t>(s.pendingCount++)] =
                        Slot::Pending{due, e};
                    ++stats_.eventsForwarded;
                    ++stats_.eventsDeferred;
                } else {
                    // Counted, never allocated for. A delay queue that grew
                    // would be the audio thread allocating on the one path a
                    // plugin latency change makes busiest.
                    ++stats_.eventsDropped;
                }
            }
        }
    }
}

std::int32_t Graph::computeSplits(std::int32_t frames) noexcept {
    // MARK, THEN WALK.
    //
    // The loop this replaces coalesced WHILE it collected, comparing each event
    // with the last split pushed. That is only right if events arrive in time
    // order, and they arrive in SLOT order: a later slot's event at frame 100,
    // met after an earlier slot's event at frame 200, came out as -100 against
    // the floor and was dropped. A real, distinct frame with no boundary at it.
    //
    // A byte per frame makes the collection order irrelevant. It costs one
    // memset and one pass over the block -- 8192 byte tests at the largest
    // block ADR-0049 allows -- and it does not care how many slots hold the
    // same frame, which after forwarding is most of them.
    std::memset(frameMark_.data(), 0, static_cast<std::size_t>(frames));
    for (auto& s : slots_) {
        // Sorted here because `runNode` slices each segment's events as one
        // contiguous run, which is only true of a list in frame order.
        s.events.sortByFrame();
        for (const Event& e : s.events)
            if (e.frame > 0 && e.frame < frames)
                frameMark_[static_cast<std::size_t>(e.frame)] = 1;
    }

    std::int32_t nsplit = 0;
    splits_[static_cast<std::size_t>(nsplit++)] = 0;
    for (std::int32_t f = 1; f < frames; ++f) {
        if (frameMark_[static_cast<std::size_t>(f)] == 0) continue;
        const std::int32_t last = splits_[static_cast<std::size_t>(nsplit - 1)];
        if (f - last < floor_) continue;          // coalesce to the boundary
        splits_[static_cast<std::size_t>(nsplit++)] = f;
    }
    splits_[static_cast<std::size_t>(nsplit)] = frames;
    return nsplit;
}

void Graph::runNode(Slot& s, std::int32_t frames, std::int32_t nsplit) noexcept {
    const auto ch = static_cast<std::size_t>(channels_);

    bool mainSilent = true;
    for (NodeId in : s.inputs)
        if (!slots_[static_cast<std::size_t>(in)].silent) mainSilent = false;

    // A LIVE SIDECHAIN PREVENTS SUSPENSION, which ADR-0043 requires by name: a
    // compressor whose key input is playing is working, however quiet its main
    // input is, and suspending it would release the gain reduction.
    bool sideSilent = true;
    for (NodeId in : s.sidechains)
        if (!slots_[static_cast<std::size_t>(in)].silent) sideSilent = false;

    const bool inputSilent = mainSilent && sideSilent;

    // Events are NOT silence. A node with a pending event, or an instrument
    // with no audio input at all, must run -- this is the bug every
    // implementation of this feature ships once, because naive silence
    // detection suspends every synth in the project.
    const bool hasEvents = !s.events.empty();

    // An infinite tail is a SEPARATE FLAG, not a sentinel in the counter.
    // Storing kInfiniteTail and decrementing it works -- INT64_MAX takes some
    // quadrillions of blocks to reach zero -- but it makes the never-suspend
    // rule impossible to test: planting a defect in the guard changes nothing
    // observable at any realistic duration. A decision that cannot be
    // falsified is one nobody can maintain, so it is a branch instead.
    const bool infinite = s.node->tailSamples() == kInfiniteTail;

    if (!inputSilent || hasEvents) {
        // RE-ARMED WITH WHAT IS STILL IN FLIGHT, not only the node's own tail.
        //
        // A compensated input delays its audio on the way in (ADR-0058). When
        // every input goes silent, the rings still hold up to `reach` samples
        // that are due out over the next `reach` samples -- and a junction's
        // own tail is zero, so it used to sleep on the spot and those samples
        // were never heard. At 5120 samples of linear-phase compensation that
        // was the last 107 ms of a dry track, whenever it was the last thing
        // playing.
        std::int64_t reach = 0;
        for (const DelayLine& d : s.inDelays)
            if (d.reach() > reach) reach = d.reach();
        for (const DelayLine& d : s.sideDelays)
            if (d.reach() > reach) reach = d.reach();
        s.tailRemaining = infinite ? 0 : s.node->tailSamples() + reach;
    }

    // JUDGED AT THE START OF THE BLOCK, before any tail is spent.
    //
    // This used to decrement first and decide after, so the block in which a
    // tail's last samples belonged was itself skipped: a tail of one block or
    // less was never heard at all, and every longer one lost its final partial
    // block -- up to 85 ms at the 4096-frame blocks this engine is built for.
    // The old test accepted "3 to 6 blocks" for a tail that is exactly 4.
    const bool suspend = !infinite && !s.node->alwaysProcess() &&
                         inputSilent && !hasEvents && s.tailRemaining == 0;
    if (suspend) {
        ++stats_.nodesSuspended;
        s.silent = true;
        // ONCE, TO CAPACITY, not every block to `frames`. Consumers read this
        // buffer as zeros for as long as the node sleeps, so it has to BE zeros
        // -- all of it, because a later block may be longer than this one and
        // would read what an earlier loud block left beyond `frames`. Clearing
        // it every block was the whole cost of a sleeping node, and it scaled
        // with frames: 315 sleeping nodes cost 1.8 ms at 4096 (ADR-0102 d5).
        // Cleared once per silence and the flag does the rest; `process`
        // below clears the flag because the node has written.
        if (!s.zeroed) {
            for (std::size_t c = 0; c < ch; ++c)
                std::memset(s.chanPtrs[c], 0,
                            static_cast<std::size_t>(maxFrames_) * sizeof(float));
            s.zeroed = true;
            ++stats_.suspendClears;
        }
        return;
    }
    s.zeroed = false;   // the node is about to write; what it leaves is its own

    // Only a block that actually runs on silence spends tail.
    if (inputSilent && !hasEvents && !infinite) {
        s.tailRemaining -= frames;
        if (s.tailRemaining < 0) s.tailRemaining = 0;
    }

    for (std::int32_t seg = 0; seg < nsplit; ++seg) {
        const std::int32_t begin = splits_[static_cast<std::size_t>(seg)];
        const std::int32_t end = splits_[static_cast<std::size_t>(seg + 1)];
        const std::int32_t n = end - begin;
        if (n <= 0) continue;

        // Summing here, for every node, is what makes a group (ADR-0044) a
        // node with no device rather than a special case in the scheduler.
        bool anyMain = false;
        for (std::size_t k = 0; k < s.inputs.size(); ++k) {
            accumulate(mixPtrs_, slots_[static_cast<std::size_t>(s.inputs[k])],
                       s.inDelays[k], begin, n, !anyMain);
            anyMain = true;
        }
        bool anySide = false;
        for (std::size_t k = 0; k < s.sidechains.size(); ++k) {
            accumulate(sidePtrs_, slots_[static_cast<std::size_t>(s.sidechains[k])],
                       s.sideDelays[k], begin, n, !anySide);
            anySide = true;
        }

        NodeIo nio;
        nio.in = anyMain ? mixPtrs_.data() : nullptr;
        nio.sidechain = anySide ? sidePtrs_.data() : nullptr;
        nio.out = s.chanPtrs.data();
        nio.channels = channels_;
        nio.frames = n;
        nio.blockOffset = begin;
        nio.inputSilent = mainSilent;
        nio.sidechainSilent = sideSilent;
        nio.sampleRate = sampleRate_;

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

    // Is this node's output silent? Measured rather than declared: a node that
    // claims silence and writes samples is a bug the next node inherits, and
    // the check is one pass over a buffer just written and therefore in cache.
    bool allZero = true;
    for (std::size_t c = 0; c < ch && allZero; ++c)
        for (std::int32_t i = 0; i < frames; ++i)
            if (s.chanPtrs[c][i] != 0.0f) { allZero = false; break; }
    s.silent = allZero;
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

    // --- ADR-0091: events travel along edges, BEFORE the splits ------------
    //
    // A note pushed at the head of a chain has to reach the instrument at the
    // tail. It is forwarded now, up front, rather than as each node runs:
    // forwarding during the run would hand a node an event at a frame the
    // splits were chosen without, and ADR-0042's promise that a value lands on
    // its own segment boundary would hold for pushed events and quietly fail
    // for forwarded ones.
    forwardEvents(frames);

    // --- segment boundaries (ADR-0042) -------------------------------------
    //
    // Split at every DISTINCT event frame, not at every event. A 500 Hz MPE+
    // frame carrying ten notes across three dimensions is one instant, not
    // thirty, so the bound is 500 splits per second rather than 15,000 --
    // which is what makes ADR-0054's requirement affordable at all.
    const std::int32_t nsplit = computeSplits(frames);

    stats_.segments += nsplit;

    // --- ADR-0043: decide suspension once per block, not per segment -------
    //
    // Per block because the tail counter is in samples and a decision that
    // changed mid-block would let a node run for part of its own tail. The
    // cost is that a node wakes at a block boundary rather than at the exact
    // sample its input returns, which is inaudible and much easier to reason
    // about.
    for (const auto& level : levels_) {
        // Any order within a level gives the same bytes -- that is the property
        // a thread pool would rely on, and it is exercised here rather than
        // asserted. See ADR-0056 and setReverseWithinLevel.
        if (reverseWithinLevel_) {
            for (auto it = level.rbegin(); it != level.rend(); ++it)
                runNode(slots_[static_cast<std::size_t>(*it)], frames, nsplit);
        } else {
            for (NodeId id : level)
                runNode(slots_[static_cast<std::size_t>(id)], frames, nsplit);
        }
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

    // Last, after every use of this block's times. Deferred events are keyed
    // to it, so advancing it early would make everything due one block late.
    now_ += frames;
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
