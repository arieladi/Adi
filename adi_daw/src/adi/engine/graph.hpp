// SPDX-License-Identifier: GPL-3.0-or-later
//
// The audio graph: nodes, the port pair, and the scheduler that decides what
// runs and at what resolution.
//
// Four ADRs meet here and they were taken together, which is why it fits in
// one loop rather than four layers:
//
//   ADR-0042  a block is split at every event boundary, with a floor, so
//             automation does not step at 85 ms
//   ADR-0043  a node with silent inputs, no events and an expired tail is
//             skipped, and its outputs are then silent too
//   ADR-0044  a group is a summing node; suspension propagates up a group
//             tree with no special case for groups
//   ADR-0045  every port is audio AND events, so a hybrid track needs no
//             special case anywhere
//   ADR-0054  the split floor MUST NOT exceed sample_rate/500, or MPE+ is
//             quantised in time
//
// No JUCE, no SQLite, no allocation after `prepare` (ADR-0010, ADR-0036). A
// `Graph` is a `BlockProcessor`, so `DeviceCore` drives it with no adapter.
//
// WHAT THIS IS NOT, yet: one channel count for the whole graph, one input bus
// and one output per node, and no parallelism across nodes. Each of those is a
// real DAW requirement and none of them changes the decisions above; they are
// left out because an untested generalisation is worse than a named limit.

#pragma once

#include "adi/engine/events.hpp"
#include "adi/engine/process.hpp"

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace adi::engine {

/// Never skip this node, however quiet it is. `IAudioProcessor::getTailSamples`
/// uses the same convention and this is deliberately the same value.
inline constexpr std::int64_t kInfiniteTail = INT64_MAX;

/// Which input bus an edge feeds.
///
/// Two, not N, and deliberately: a sidechain is the one auxiliary input the
/// rest of the system already names -- ADR-0043 requires that a live sidechain
/// prevent suspension, and it could not say that without the graph being able
/// to tell one apart. An arbitrary bus count is a real requirement and is not
/// this one; it arrives when something needs it.
enum class Bus : std::uint8_t { Main = 0, Sidechain = 1 };

/// What a node is handed for one SEGMENT of one block.
struct NodeIo {
    /// Summed inputs. Null when the node has none — an instrument or a clip
    /// reader — and a node must check rather than assume.
    const float* const* in = nullptr;
    float* const* out = nullptr;

    /// Summed sidechain inputs, or null when nothing feeds that bus. A
    /// compressor reads this; everything else ignores it.
    const float* const* sidechain = nullptr;

    std::int32_t channels = 0;
    std::int32_t frames = 0;        ///< frames in THIS segment

    /// Where this segment starts within the block. Events carry block-relative
    /// frames (events.hpp), so a node that wants an offset inside its own
    /// segment computes `e.frame - blockOffset`.
    std::int32_t blockOffset = 0;

    /// Only the events landing in this segment, already sorted by frame.
    EventSpan events;

    /// True when every input channel is silent for this block. A node may use
    /// it to skip work; the scheduler has already used it to decide whether to
    /// call at all.
    bool inputSilent = false;

    /// Separately, because a node may want to know its key input is live while
    /// its main input is not -- which is the point of a sidechain.
    bool sidechainSilent = true;

    double sampleRate = 0.0;
};

/// What a node does with the note stream passing through it (ADR-0091).
enum class EventFlow : std::uint8_t {
    /// Note events pass on to whatever this node feeds. The default, and the
    /// right answer for a junction, an audio effect, and a note effect alike.
    Through,
    /// They stop here: an instrument turns notes into audio, and the effects
    /// after it have no use for them.
    Consume,
};

/// One processing node.
///
/// The three ADR-0043 declarations default to the CONSERVATIVE answer, matching
/// ADR-0040's rule that a device declaring nothing is never suspended. A node
/// that says nothing keeps running, which is the behaviour that is merely slow
/// rather than the one that is wrong.
class Node {
public:
    virtual ~Node() = default;
    Node(const Node&) = delete;
    Node& operator=(const Node&) = delete;

    virtual void prepare(double /*sampleRate*/, std::int32_t /*maxFrames*/) {}
    virtual void release() {}

    /// Audio thread. No allocation, no locks, no throwing.
    virtual void process(const NodeIo& io) noexcept = 0;

    /// How long this node keeps producing after its input goes quiet.
    /// `kInfiniteTail` means never suspend — the right answer for a feedback
    /// delay and for anything that does not know.
    [[nodiscard]] virtual std::int64_t tailSamples() const noexcept {
        return kInfiniteTail;
    }

    /// How many samples this node DELAYS its output by (ADR-0058 decision 1).
    ///
    /// The default is **0, which is the opposite of the tail's default, and
    /// deliberately so.** A node that fails to declare a tail is merely
    /// processed more often than it needed to be. A node that fails to declare
    /// a LATENCY is compensated wrongly -- and a wrong compensation is worse
    /// than none, because it moves audio that was already aligned. Kick
    /// against bass is the case that exposes it, and it would be our own code
    /// producing the complaint.
    ///
    /// So the conservative answer differs per question: never suspend, and
    /// never shift. Both defaults are the one that cannot corrupt.
    ///
    /// NOT included: the device buffer (ADR-0042 decision 7, ADR-0058
    /// decision 4). This is the node's own latency and nothing else; folding
    /// the block size in here makes every compensated track wrong by up to
    /// 4096 samples, which is 85 ms at 48 kHz.
    ///
    /// Reported in samples at the rate passed to `prepare`. A node whose
    /// latency changes at runtime -- a VST3 switching to linear phase --
    /// reports it and does NOT recompute anything itself; ADR-0066 owns what
    /// happens next, off this thread.
    [[nodiscard]] virtual std::int32_t latencySamples() const noexcept { return 0; }

    /// The `devices.always_process` escape hatch (ADR-0043), for a plugin that
    /// reports no tail and then produces one.
    [[nodiscard]] virtual bool alwaysProcess() const noexcept { return false; }

    /// Whether note events stop at this node or pass on (ADR-0091).
    ///
    /// **Through by default, and that is the conservative answer here.** The
    /// two failures are not symmetric. A node that forgets to say `Consume`
    /// passes notes to the effects after it, which ignore them. A node that
    /// said `Consume` by default and forgot `Through` would swallow every note
    /// before it reached the instrument -- silence, which is precisely the
    /// defect this ADR exists to remove. Same principle as the tail and
    /// latency defaults: choose the default whose failure is the harmless one.
    ///
    /// CALLED ON THE AUDIO THREAD, per edge, every block (`forwardEvents`). It
    /// must not allocate, lock or call into a plugin. A device that learns
    /// whether it is an instrument from something expensive -- JUCE's
    /// `getPluginDescription()` builds a description full of `String`s --
    /// answers once at construction and returns the cached value here.
    [[nodiscard]] virtual EventFlow eventFlow() const noexcept { return EventFlow::Through; }

    [[nodiscard]] virtual const char* name() const noexcept { return "node"; }

protected:
    Node() = default;
};

using NodeId = std::int32_t;
inline constexpr NodeId kInvalidNode = -1;

struct GraphStats {
    std::int64_t blocks = 0;
    std::int64_t segments = 0;       ///< sum over blocks; 1 per block when no events
    std::int64_t nodeCalls = 0;
    std::int64_t nodesSuspended = 0; ///< node-blocks skipped by ADR-0043
    std::int64_t eventsDropped = 0;
    std::int64_t eventsForwarded = 0; ///< deliveries made along an edge (ADR-0091)
    std::int64_t eventsDeferred = 0;  ///< held for a later block by a delay
};

/// A fixed delay, applied to one input edge so that two paths of unequal
/// latency reach their consumer aligned (ADR-0058).
///
/// A ring rather than a memmove: at 4096 frames and a few thousand samples of
/// compensation, shuffling the history every block is real work for no reason.
/// Prepared once, never allocates afterwards.
class DelayLine {
public:
    DelayLine() = default;

    // MOVE, WRITTEN BY HAND, because the atomics make the implicit one
    // disappear and `Slot::inDelays` is a vector that resizes. Only the
    // message thread moves a delay line -- during construction, before the
    // graph runs -- so loading the atomics relaxed here is not a shortcut.
    DelayLine(DelayLine&& o) noexcept { *this = std::move(o); }
    DelayLine& operator=(DelayLine&& o) noexcept {
        if (this == &o) return *this;
        buf_ = std::move(o.buf_);
        incoming_ = std::move(o.incoming_);
        capacity_ = o.capacity_;
        ring_ = o.ring_;
        delay_ = o.delay_;
        channels_ = o.channels_;
        write_ = o.write_;
        inCapacity_ = o.inCapacity_;
        inRing_ = o.inRing_;
        inWrite_ = o.inWrite_;
        targetDelay_ = o.targetDelay_;
        primeRemaining_ = o.primeRemaining_;
        target_.store(o.target_.load(std::memory_order_relaxed),
                      std::memory_order_relaxed);
        gliding_.store(o.gliding_.load(std::memory_order_relaxed),
                       std::memory_order_relaxed);
        grow_.store(o.grow_.load(std::memory_order_relaxed),
                    std::memory_order_relaxed);
        return *this;
    }
    DelayLine(const DelayLine&) = delete;
    DelayLine& operator=(const DelayLine&) = delete;

    /// `capacity` is the LARGEST delay this line will ever be asked for, not
    /// the delay itself. Sizing the ring bigger than the tap is what makes a
    /// runtime latency change possible without allocating: the tap moves
    /// inside a ring that already holds the history (ADR-0079).
    void prepare(std::int32_t channels, std::int32_t capacity);

    /// The delay in effect, taking hold at once. Message thread, before the
    /// graph runs.
    void setDelay(std::int32_t d) noexcept;

    void reset() noexcept;

    /// `frames` samples of `src` delayed into `dst`, for one channel. Safe to
    /// call with src == dst.
    void process(std::int32_t channel, const float* src, float* dst,
                 std::int32_t frames) noexcept;

    [[nodiscard]] std::int32_t delay() const noexcept { return delay_; }
    [[nodiscard]] std::int32_t capacity() const noexcept { return capacity_; }

    // --- moving the tap within the ring (ADR-0079) -------------------------

    /// Ask for a new delay, reached by a crossfade across the next block.
    /// Refused -- and returns false -- when it does not fit the ring, because
    /// growing the ring means allocating and that is the audio thread's one
    /// prohibition (ADR-0010). A false here is the signal to escalate.
    ///
    /// Safe to call from the message thread while the audio thread processes:
    /// it writes one atomic and nothing else.
    bool beginGlide(std::int32_t d) noexcept;

    [[nodiscard]] bool gliding() const noexcept {
        return gliding_.load(std::memory_order_relaxed);
    }
    [[nodiscard]] std::int32_t target() const noexcept {
        return target_.load(std::memory_order_relaxed);
    }

    // --- growing the ring itself (ADR-0085) --------------------------------

    /// Hand this line a BIGGER ring for a delay that does not fit the current
    /// one. MESSAGE THREAD: the allocation happens in the caller, which is the
    /// entire point.
    ///
    /// The handover is not instant and cannot be. A new ring holds no history,
    /// and the history for a delay longer than the old capacity **was never
    /// stored anywhere** -- so there is nothing to copy and no cleverness that
    /// avoids waiting. The line therefore writes into BOTH rings while
    /// continuing to read the old one at the old delay, and switches once the
    /// new ring genuinely holds `targetDelay` samples. The compensation is
    /// stale for that window, and stale is the right failure: the alternative
    /// is silence, which is a dropout.
    ///
    /// Returns false when a previous offer is still in flight; the caller
    /// retries after `collectRing`.
    bool offerRing(std::vector<float> ring, std::int32_t capacity,
                   std::int32_t targetDelay);

    /// MESSAGE THREAD. Takes back the buffer the audio thread finished with,
    /// and frees the line for another offer. Empty when there is nothing to
    /// collect. The audio thread never deallocates; it swaps and parks.
    std::vector<float> collectRing();

    /// An offer is in flight: priming, fading, or waiting to be collected.
    [[nodiscard]] bool growing() const noexcept {
        return grow_.load(std::memory_order_acquire) != Grow::Idle;
    }
    /// Samples of history the new ring still needs. Zero once it is ready.
    [[nodiscard]] std::int32_t primeRemaining() const noexcept {
        return primeRemaining_;
    }

    /// The furthest back any tap in flight reads: the current delay, a glide's
    /// target, or a growing ring's target, whichever is longest. AUDIO THREAD.
    ///
    /// This is how many samples of this edge's past are still due to come out.
    /// A node whose inputs have gone silent must keep running at least this
    /// long, or those samples are never heard (see `Graph::runNode`).
    [[nodiscard]] std::int32_t reach() const noexcept {
        std::int32_t r = delay_;
        if (gliding_.load(std::memory_order_acquire)) {
            const std::int32_t t = target_.load(std::memory_order_relaxed);
            if (t > r) r = t;
        }
        const Grow g = grow_.load(std::memory_order_acquire);
        if ((g == Grow::Priming || g == Grow::Fading) && targetDelay_ > r) r = targetDelay_;
        return r;
    }

    /// Bytes this line holds, both rings included while one is in flight.
    [[nodiscard]] std::size_t bytes() const noexcept {
        return (buf_.size() + incoming_.size()) * sizeof(float);
    }

    /// Anything that spans a block is in progress, so the caller must not take
    /// a shortcut. One predicate rather than two, because the fast path in
    /// `Graph::accumulate` forgot `gliding()` once already.
    [[nodiscard]] bool busy() const noexcept { return gliding() || growing(); }

    // --- per-edge state, shared across channels ----------------------------

    /// The cursors and counters an edge shares across its channels. A caller
    /// processing several channels must restore these between them and call
    /// `endEdge` once after the last: getting it wrong does not pull stereo
    /// apart -- every channel drifts identically -- it makes the compensation
    /// the wrong LENGTH from the next block on. See `Graph::accumulate`.
    struct Cursors {
        std::int32_t write = 0;
        std::int32_t inWrite = 0;
        std::int32_t primeRemaining = 0;
    };
    [[nodiscard]] Cursors cursors() const noexcept {
        return Cursors{write_, inWrite_, primeRemaining_};
    }
    void setCursors(const Cursors& c) noexcept {
        write_ = c.write;
        inWrite_ = c.inWrite;
        primeRemaining_ = c.primeRemaining;
    }

    /// Called ONCE per edge, after every channel has been processed. Ends a
    /// glide and advances the grow state machine.
    void endEdge() noexcept;

    /// ADR-0092: take the history `old` has been accumulating, so this line's
    /// tap reads what the old one would have read. AUDIO THREAD, at a graph
    /// swap: it copies and allocates nothing.
    ///
    /// Only what THIS line's tap reaches is copied -- `min(delay, capacity,
    /// old capacity)` samples per channel. A ring carries thousands of samples
    /// of headroom (ADR-0088), and copying all of it would make a rebuild cost
    /// in proportion to headroom rather than to compensation. What lies beyond
    /// the old ring's capacity was never stored, and is not invented.
    void adoptHistory(const DelayLine& old) noexcept;

private:
    enum class Grow : std::uint8_t {
        Idle,      ///< one ring
        Priming,   ///< two rings; writing both, reading the old
        Fading,    ///< two rings; crossfading old tap to new
        Spent,     ///< swapped; the old buffer is parked for collection
    };

    [[nodiscard]] static float tapAt(const float* hist, std::int32_t w,
                                     std::int32_t ring, std::int32_t d) noexcept {
        std::int32_t r = w - d;
        if (r < 0) r += ring;
        return hist[r];
    }
    void processGrowing(std::int32_t channel, const float* src, float* dst,
                        std::int32_t frames, Grow state) noexcept;

    std::vector<float> buf_;        ///< channels * ring_
    std::int32_t capacity_ = 0;     ///< the largest delay that fits
    std::int32_t ring_ = 0;         ///< capacity_ + 1; see graph.cpp
    std::int32_t delay_ = 0;
    std::int32_t channels_ = 0;
    std::int32_t write_ = 0;

    // The offered ring, and after the swap the retired one. Only ever touched
    // by one thread at a time: the message thread fills it before publishing
    // `Priming`, the audio thread owns it until it publishes `Spent`, and the
    // message thread takes it back after observing that.
    std::vector<float> incoming_;
    std::int32_t inCapacity_ = 0;
    std::int32_t inRing_ = 0;
    std::int32_t inWrite_ = 0;
    std::int32_t targetDelay_ = 0;
    std::int32_t primeRemaining_ = 0;

    // Written by the message thread, read by the audio thread. Two plain
    // atomics rather than a published schedule: while the new delay FITS, a
    // latency change needs no new buffers, so there is nothing to publish and
    // nothing to reclaim (ADR-0079).
    std::atomic<std::int32_t> target_{0};
    std::atomic<bool> gliding_{false};
    std::atomic<Grow> grow_{Grow::Idle};
};

/// A DAG of nodes, scheduled per block.
class Graph final : public BlockProcessor {
public:
    Graph() = default;
    ~Graph() override;

    // --- construction, message thread only ---------------------------------

    /// Adds a node and returns its id. The graph does not own it; nodes outlive
    /// the graph, because ADR-0042's decision 5 says a device change must not
    /// disturb the project and the project is where nodes come from.
    NodeId addNode(Node& node);

    /// `from` feeds `to`. Multiple edges into one node are SUMMED, which is
    /// what makes a group a node with no device rather than a special case
    /// (ADR-0044).
    bool connect(NodeId from, NodeId to, Bus bus = Bus::Main);

    /// How many nodes have been added. Lets a caller assert what it built
    /// rather than describe it -- realisation's decision to emit no node for a
    /// VCA is only a claim without this.
    [[nodiscard]] std::size_t nodeCount() const noexcept { return slots_.size(); }

    /// The node whose output is the graph's output. Exactly one.
    void setOutput(NodeId id) { output_ = id; }

    /// Channels for every buffer in the graph. One count for now; see the
    /// header's list of what this is not.
    void setChannels(std::int32_t n) { channels_ = n > 0 ? n : 2; }

    /// Per-node event capacity. At 500 Hz with heavy polyphony this is a real
    /// design number rather than a formality (events.hpp).
    /// Per-node event capacity. Zero -- the default -- DERIVES it at `prepare`
    /// from the block size, because one fixed number is wrong at one end or
    /// the other of a 64-to-4096 range.
    ///
    /// The arithmetic that forced this: a Continuum at 500 Hz across a
    /// 4096-frame block is 42.7 update frames, and at three dimensions that is
    /// 128 events per note. A fixed 1024 therefore drops packets from TEN
    /// NOTES onwards -- on exactly the instrument ADR-0054 was written for,
    /// and silently apart from a counter nobody was reading.
    void setEventCapacity(std::int32_t n) { eventCapacity_ = n > 0 ? n : 0; }

    /// Simultaneous notes the derived capacity is sized for.
    void setMaxPolyphony(std::int32_t n) { maxPolyphony_ = n > 0 ? n : 16; }

    /// What `prepare` would allocate per node, so the arithmetic is testable
    /// rather than a comment (ADR-0056).
    [[nodiscard]] static std::int32_t deriveEventCapacity(
        double sampleRate, std::int32_t maxFrames, std::int32_t polyphony) noexcept;

    /// The capacity actually in use after `prepare`.
    [[nodiscard]] std::int32_t eventCapacity() const noexcept { return eventCapacity_; }

    /// Events for the next block, in block-relative frames. Called from the
    /// message thread before the block, or by a clip reader node.
    bool pushInputEvent(NodeId to, const Event& e);

    // --- BlockProcessor ----------------------------------------------------

    void prepare(double sampleRate, std::int32_t maxFrames) override;
    void process(const AudioIo& io) noexcept override;
    void release() override;

    // --- what happened -----------------------------------------------------

    [[nodiscard]] bool ok() const noexcept { return ok_; }

    /// Why `prepare` failed. A cycle, no output, a dangling edge.
    [[nodiscard]] const std::string& error() const { return error_; }

    [[nodiscard]] const GraphStats& stats() const noexcept { return stats_; }

    /// The split floor actually in use, after ADR-0054's clamp.
    [[nodiscard]] std::int32_t floorFrames() const noexcept { return floor_; }

    /// ADR-0054: a floor wider than the gap between 500 Hz MPE+ frames puts two
    /// of them in one segment and discards the later one, which is quantising
    /// the stream in time. Exposed so a test can assert the bound rather than
    /// trust the comment.
    [[nodiscard]] static std::int32_t maxFloorFor(double sampleRate) noexcept;

    /// Spare ring capacity on every compensated edge, in samples, so a
    /// plugin's latency can change while the graph runs without anyone
    /// allocating (ADR-0079).
    ///
    /// THE DEFAULT IS 8192, AND IT IS MEASURED (ADR-0088). It was 0, and 0
    /// switches the feature off: with no headroom every change misses its
    /// ring, escalates and primes, so the cheap path never once runs. A
    /// default that disables the thing it configures is not a default.
    ///
    /// 8192 is the next power of two above the largest swing measured on a
    /// real plugin -- Pro-Q 3 3.24 moves 0 -> 320 -> 5120 samples across its
    /// phase modes. The sentence this replaces guessed "low thousands", which
    /// would have made 2048 and 4096 both look sufficient and both miss.
    ///
    /// Cost is `channels * (delay + headroom) * 4` bytes per edge; see
    /// `compensationBytes()`, which measures it rather than estimating.
    void setLatencyHeadroom(std::int32_t n) noexcept {
        latencyHeadroom_ = n > 0 ? n : 0;
    }
    [[nodiscard]] std::int32_t latencyHeadroom() const noexcept {
        return latencyHeadroom_;
    }

    /// Re-read every node's `latencySamples()` and move the taps to match,
    /// crossfaded across the next block. MESSAGE THREAD, while the audio
    /// thread runs.
    ///
    /// Returns false when some edge needs more delay than its ring holds. That
    /// is not a failure to handle here: it is the signal that this change
    /// needs new buffers, which means building a schedule off-thread and
    /// publishing it (ADR-0019). Everything that DID fit has already been
    /// moved, because a partial correction is closer than none.
    bool retapLatency() noexcept;

    /// ADR-0085. For every edge whose required delay does not fit its ring,
    /// ALLOCATE a bigger one here -- on the message thread -- and hand it over.
    /// The audio thread primes it against the old one and switches when it
    /// genuinely holds the history. Returns how many edges were grown.
    ///
    /// This is the escalation ADR-0079 decision 4 named. It is per EDGE and
    /// not per graph: growing one ring leaves every other edge's history
    /// untouched, where swapping a whole schedule would reset all of them and
    /// glitch the entire project to fix one plugin.
    std::size_t escalateLatency();

    /// Message thread, on a timer. Frees the rings the audio thread has
    /// finished with. Returns how many were reclaimed.
    std::size_t collectRings();

    /// Edges with an offer still in flight, so a test can assert the handover
    /// rather than describe it.
    [[nodiscard]] std::size_t growingEdges() const noexcept;

    /// Bytes currently held by every compensation ring in the graph, including
    /// one that has been offered but not yet swapped. Exposed so the memory
    /// cost of a headroom setting is MEASURED in a test rather than asserted
    /// in a comment -- which is how "low thousands" survived long enough to
    /// make the default wrong.
    [[nodiscard]] std::size_t compensationBytes() const noexcept;

    /// The compensation ring on the edge `from -> to`, or null. Resolved each
    /// time rather than cached by a caller, because `prepare` rebuilds the
    /// per-slot ring vectors and a pointer held across it can dangle -- which
    /// the probe's pattern of feeding a graph and re-preparing it after a
    /// rebuild would hit on the first swap.
    [[nodiscard]] DelayLine* edgeLine(NodeId from, NodeId to, Bus bus) noexcept;

    /// The graph's own latency: how far behind the output is (ADR-0058).
    /// Excludes the device buffer -- that is ADR-0042 decision 7, and folding
    /// it in here makes every compensated track wrong by up to a block.
    [[nodiscard]] std::int32_t latencySamples() const noexcept {
        return graphLatency_.load(std::memory_order_acquire);
    }

    /// Samples of compensation inserted on the edge `from -> to`, or -1 when
    /// there is no such edge. Exposed so the arithmetic is testable directly
    /// rather than only through its audible effect.
    [[nodiscard]] std::int32_t compensationFor(NodeId from, NodeId to,
                                               Bus bus = Bus::Main) const noexcept;

    /// When the whole signal a node sees is expected to arrive, in samples
    /// from the block start. For tests and for a mixer that wants to show it.
    [[nodiscard]] std::int32_t arrivalOf(NodeId) const noexcept;

    /// Topological order, valid after a successful `prepare`. For tests.
    [[nodiscard]] const std::vector<NodeId>& order() const noexcept { return order_; }

    /// Nodes grouped by dependency depth: everything in `levels()[k]` depends
    /// only on levels below it, so a level may be run in ANY order -- including
    /// concurrently -- without changing a single output byte.
    ///
    /// That determinism is the reason this is safe, and it is not incidental.
    /// Every node writes its own buffer, and summation into a consumer happens
    /// in that consumer's fixed input order, so no float is ever added in a
    /// different sequence. ADR-0021's oracle compares bytes, and a scheduler
    /// that reordered a sum would break it in a way that surfaces as a digest
    /// mismatch weeks later.
    ///
    /// The thread pool that would exploit this is NOT built. The levels and
    /// the property are, and the property is tested by running each level in
    /// the opposite order and comparing output byte for byte (ADR-0056).
    [[nodiscard]] const std::vector<std::vector<NodeId>>& levels() const noexcept {
        return levels_;
    }

    /// Run each level backwards. Exists so a test can demonstrate
    /// order-independence without a thread pool; a real pool would produce
    /// some other order and must produce the same bytes.
    void setReverseWithinLevel(bool v) noexcept { reverseWithinLevel_ = v; }

private:
    struct Slot {
        Node* node = nullptr;
        std::vector<NodeId> inputs;
        std::vector<NodeId> sidechains;
        std::vector<DelayLine> inDelays;     ///< parallel to `inputs`
        std::vector<DelayLine> sideDelays;   ///< parallel to `sidechains`
        std::int32_t arrival = 0;            ///< ADR-0058: when this node's input is whole
        std::int32_t level = 0;
        std::vector<float> audio;       ///< channels * maxFrames, interleaved by channel
        std::vector<float*> chanPtrs;
        std::vector<Event> eventStore;
        EventList events;

        /// ADR-0091: note events delayed past the end of this block, by a
        /// node's latency or an edge's compensation, keyed by the absolute
        /// sample they are due. Fixed capacity, sized at `prepare`; an event
        /// that does not fit is counted as dropped rather than allocated for.
        struct Pending {
            std::int64_t due = 0;
            Event e;
        };
        std::vector<Pending> pending;
        std::int32_t pendingCount = 0;

        std::int64_t tailRemaining = 0;
        bool silent = true;
    };

    bool topoSort();

    /// Accumulate one input's segment into the mix scratch. `first` copies,
    /// the rest add -- which is the whole of ADR-0044's summing, and why a
    /// group is a node with no device rather than a case in the scheduler.
    void accumulate(std::vector<float*>& dst, const Slot& src, DelayLine& delay,
                    std::int32_t begin, std::int32_t frames, bool first) noexcept;
    void computeCompensation();
    void prepareLine(DelayLine& line, std::int32_t delaySamples);
    void runNode(Slot& s, std::int32_t frames, std::int32_t nsplit) noexcept;

    /// ADR-0091, before the splits are computed: carry each slot's note events
    /// to the slots it feeds, delayed by what the audio beside them is delayed
    /// by. Doing it up front is what keeps a forwarded event on a segment
    /// boundary -- forwarding as nodes run would put it in a segment chosen
    /// before it existed.
    void forwardEvents(std::int32_t frames) noexcept;

    /// ADR-0042's split points, computed from a per-frame mark rather than
    /// while walking slots. See the comment at the call.
    std::int32_t computeSplits(std::int32_t frames) noexcept;

    std::vector<Slot> slots_;
    std::vector<NodeId> order_;
    NodeId output_ = kInvalidNode;

    std::vector<float> mixBuf_;         ///< summed main inputs for the node in hand
    std::vector<float*> mixPtrs_;
    std::vector<float> sideBuf_;        ///< summed sidechain inputs
    std::vector<float*> sidePtrs_;
    std::vector<std::vector<NodeId>> levels_;
    std::vector<Event> mixEventStore_;
    EventList mixEvents_;
    std::vector<std::int32_t> splits_;  ///< segment boundaries, sized at prepare
    std::vector<std::uint8_t> frameMark_; ///< one byte per frame: an event lands here

    /// Samples rendered since `prepare`. Deferred events are keyed to it, so a
    /// delay that crosses any number of block boundaries needs no bookkeeping
    /// beyond one subtraction.
    std::int64_t now_ = 0;
    std::vector<float> delayScratch_;   ///< one channel of one segment

    double sampleRate_ = 0.0;
    std::int32_t maxFrames_ = 0;
    std::int32_t channels_ = 2;
    std::int32_t eventCapacity_ = 0;      ///< 0 = derive at prepare
    std::int32_t maxPolyphony_ = 16;
    std::int32_t floor_ = 64;
    // ATOMIC, because `retapLatency` writes it from the message thread while
    // the transport may be reading it to compensate the playhead. A torn read
    // is not the real risk on the platforms we target; the data race is, and
    // an int32 costs nothing to do properly.
    std::atomic<std::int32_t> graphLatency_{0};
    std::int32_t latencyHeadroom_ = 8192;
    bool ok_ = false;
    bool prepared_ = false;
    bool reverseWithinLevel_ = false;
    std::string error_;
    GraphStats stats_;
};

// ---------------------------------------------------------------------------
// Nodes that the graph itself provides
// ---------------------------------------------------------------------------

/// Sums its inputs and passes events through. A group track (ADR-0044) is one
/// of these with a device chain after it, and because it is an ordinary node
/// ADR-0043's suspension propagates up a group tree with nothing written for
/// groups specifically.
class SumNode final : public Node {
public:
    void process(const NodeIo& io) noexcept override;
    [[nodiscard]] std::int64_t tailSamples() const noexcept override { return 0; }
    [[nodiscard]] const char* name() const noexcept override { return "sum"; }
};

/// Applies a gain that events can change, sample-accurately.
///
/// Exists to make ADR-0042's own test possible: run a ramp across one 4096
/// block and assert the output is a ramp rather than a staircase. Without
/// sub-block splitting that test fails, which is the property worth having --
/// it is not a test that splitting exists, it is a test that its absence is
/// detected.
class GainNode final : public Node {
public:
    explicit GainNode(std::uint32_t paramId = 0) : paramId_(paramId) {}

    void prepare(double, std::int32_t) override { gain_ = 1.0f; }
    void process(const NodeIo& io) noexcept override;

    [[nodiscard]] std::int64_t tailSamples() const noexcept override { return 0; }
    [[nodiscard]] const char* name() const noexcept override { return "gain"; }
    [[nodiscard]] float gain() const noexcept { return gain_; }

private:
    std::uint32_t paramId_ = 0;
    float gain_ = 1.0f;
};

}  // namespace adi::engine
