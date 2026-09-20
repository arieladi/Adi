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

#include <cstdint>
#include <memory>
#include <string>
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
        std::int32_t level = 0;
        std::vector<float> audio;       ///< channels * maxFrames, interleaved by channel
        std::vector<float*> chanPtrs;
        std::vector<Event> eventStore;
        EventList events;
        std::int64_t tailRemaining = 0;
        bool silent = true;
    };

    bool topoSort();

    /// Accumulate one input's segment into the mix scratch. `first` copies,
    /// the rest add -- which is the whole of ADR-0044's summing, and why a
    /// group is a node with no device rather than a case in the scheduler.
    void accumulate(std::vector<float*>& dst, const Slot& src, std::int32_t begin,
                    std::int32_t frames, bool first) noexcept;
    void runNode(Slot& s, std::int32_t frames, std::int32_t nsplit) noexcept;

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

    double sampleRate_ = 0.0;
    std::int32_t maxFrames_ = 0;
    std::int32_t channels_ = 2;
    std::int32_t eventCapacity_ = 0;      ///< 0 = derive at prepare
    std::int32_t maxPolyphony_ = 16;
    std::int32_t floor_ = 64;
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
