// SPDX-License-Identifier: GPL-3.0-or-later
//
// The rebuild path — ADR-0089, answering the flag ADR-0085 raises.
//
// `LatencyCoalescer::rebuildNeeded()` has been raised and counted since
// ADR-0084 taught it to tell a shape change from a latency change, and nothing
// has ever answered it. This is the answer: plan, realise, prepare and publish
// a NEW graph, on the message thread, and let the audio thread pick it up at a
// block boundary.
//
// WHY THIS IS NOT THE GRAPH'S OWN JOB. A `Graph` is the thing being replaced.
// Anything that outlives the replacement — the publisher, the reclamation, the
// fade across the seam, the coalescer — cannot live inside it, or it is
// destroyed by the swap it exists to cause. mac made that point about the
// coalescer and it generalises: the host is whatever has to survive.
//
// THE SWAP IS FADED IN, NOT CROSSFADED, and the reason is the same one that
// killed ADR-0066 decision 4. A crossfade needs both graphs to render the same
// block, and both graphs hold the SAME `Node*`s, because devices are injected
// and outlive a rebuild (ADR-0042 d5). Rendering both would call `process()`
// twice on every plugin in the project.
//
// It is not faded OUT either, and that is a decision rather than an omission.
// Fading out means deferring the swap by a block — deliberately running a graph
// we have already decided is wrong. A rebuild is triggered by a TOPOLOGY change,
// so the stale graph may be routing audio through a node whose port layout just
// moved underneath it. One more block of that is worse than a clean cut, and
// the incoming graph's rings are empty anyway, so what it renders first is
// near-silence that the fade simply bounds.

#pragma once

#include "adi/engine/publisher.hpp"
#include "adi/engine/realize.hpp"
#include "adi/store_rows.hpp"

#include <cstdint>
#include <memory>
#include <string>

namespace adi::engine {

/// One realised graph, published. The `Sequenced` base is what the publisher
/// numbers; everything else here is the payload.
class PublishedGraph final : public Sequenced {
public:
    explicit PublishedGraph(std::unique_ptr<RealizedGraph> g) : graph_(std::move(g)) {}

    /// NON-CONST through a const `PublishedGraph`, on purpose. `AudioRead`
    /// hands out `const PublishedGraph*` because the snapshot's IDENTITY is
    /// immutable — which graph this is, and its sequence number. The graph's
    /// buffers are not: they are the single audio reader's scratch, written
    /// every block. `const` on a `unique_ptr` does not propagate to the
    /// pointee, so this needs no `mutable` and no cast.
    [[nodiscard]] RealizedGraph& graph() const noexcept { return *graph_; }

private:
    std::unique_ptr<RealizedGraph> graph_;
};

/// Holds the live graph, replaces it, and renders through whichever one is
/// current.
class GraphHost final {
public:
    struct Stats {
        std::int64_t published = 0;   ///< rebuilds that took effect
        std::int64_t refused = 0;     ///< rebuilds that never reached the audio thread
        std::int64_t swaps = 0;       ///< times the audio thread picked up a new graph
        std::int64_t reclaimed = 0;   ///< retired graphs freed
        std::int64_t blocks = 0;
    };

    GraphHost() = default;
    GraphHost(const GraphHost&) = delete;
    GraphHost& operator=(const GraphHost&) = delete;

    // --- message thread -----------------------------------------------------

    /// Plan, realise, prepare and publish. Returns false with `error()` set
    /// when the model cannot be realised.
    ///
    /// **A FAILED REBUILD DOES NOT DISTURB THE RUNNING GRAPH.** Nothing is
    /// published until the new graph has been prepared successfully, so a
    /// model with a cycle or no master leaves the session playing exactly what
    /// it was playing. The alternative — swap first, discover second — turns a
    /// bad edit into silence.
    bool rebuild(const rows::Model& model, const RealizeOptions& opts,
                 double sampleRate, std::int32_t maxFrames);

    /// Free retired graphs the audio thread has demonstrably moved past. Call
    /// it on the same timer that polls the coalescer; it never blocks audio.
    std::size_t collect();

    /// The graph a message-thread caller should act on — retap it, register
    /// its devices, read its latency. Null before the first rebuild.
    ///
    /// NOT held across a rebuild. This pointer is retired by the next
    /// successful `rebuild` and freed by a later `collect`, which is exactly
    /// why `LatencyCoalescer` attaches to the HOST and re-reads this every
    /// poll rather than storing it once.
    [[nodiscard]] Graph* currentGraph() const noexcept;

    /// The realised graph behind it, for callers that need a track's chain
    /// head or tail -- a clip reader pushing into `inputFor`, or a device
    /// chain being registered. Same lifetime warning as `currentGraph`.
    [[nodiscard]] RealizedGraph* current() const noexcept { return live_; }

    /// Applied to each graph as it is built. Kept on the host because it
    /// outlives any one graph (ADR-0088).
    void setLatencyHeadroom(std::int32_t n) noexcept { headroom_ = n > 0 ? n : 0; }
    [[nodiscard]] std::int32_t latencyHeadroom() const noexcept { return headroom_; }

    /// Samples the incoming graph is ramped up over. 0 disables the fade,
    /// which is what an offline render wants: a bounce has no seam to hide,
    /// because it re-renders rather than swapping mid-stream (ADR-0066 d5).
    void setFadeFrames(std::int32_t n) noexcept { fadeFrames_ = n > 0 ? n : 0; }
    [[nodiscard]] std::int32_t fadeFrames() const noexcept { return fadeFrames_; }

    [[nodiscard]] const std::string& error() const noexcept { return error_; }
    [[nodiscard]] const Stats& stats() const noexcept { return stats_; }

    /// Problems carried out of the last realisation, so a caller can surface
    /// them without holding the graph.
    [[nodiscard]] const std::vector<std::string>& problems() const noexcept {
        return problems_;
    }

    // --- audio thread -------------------------------------------------------

    /// Render one block through whatever is current. Allocates nothing, locks
    /// nothing, waits for nothing.
    ///
    /// With no graph published it writes SILENCE rather than returning. A
    /// callback that returns without writing hands the driver the previous
    /// block, repeated — which is audible and reaches the monitors.
    void process(const AudioIo& io) noexcept;

private:
    using Pub = SnapshotPublisher<PublishedGraph>;

    void fadeIn(const AudioIo& io) noexcept;

    Pub pub_;
    Stats stats_;
    std::string error_;
    std::vector<std::string> problems_;
    std::int32_t headroom_ = 8192;
    std::int32_t fadeFrames_ = 256;

    // Audio thread only.
    std::uint64_t lastSeq_ = 0;
    std::int32_t fadeRemaining_ = 0;
    std::int32_t fadeLength_ = 0;

    // Message thread only: the raw pointer the publisher currently holds, so
    // `currentGraph` does not have to construct an `AudioRead` and pretend to
    // be the reader.
    RealizedGraph* live_ = nullptr;
};

}  // namespace adi::engine
