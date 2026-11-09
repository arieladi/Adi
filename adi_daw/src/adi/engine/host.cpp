// SPDX-License-Identifier: GPL-3.0-or-later

#include "adi/engine/host.hpp"
#include "adi/engine/render_workers.hpp"

#include <cmath>
#include <cstring>
#include <exception>
#include <utility>

namespace adi::engine {

bool GraphHost::setParallelRendering(std::uint32_t workers,std::int32_t minimumFrames,std::uint32_t minimumNodes,RenderWorkers::Initializer initialize,void* context) noexcept {
    if(workers>RenderWorkers::maximum || minimumFrames<1 || minimumNodes<2)return false;
    if (renderWorkers_ != workers || parallelMinimumFrames_ != minimumFrames ||
        parallelMinimumNodes_ != minimumNodes || workerInitializer_ != initialize ||
        workerInitializationContext_ != context) ++configuration_;
    renderWorkers_=workers;parallelMinimumFrames_=minimumFrames;parallelMinimumNodes_=minimumNodes;
    workerInitializer_=initialize;workerInitializationContext_=context;return true;
}

std::int32_t GraphHost::latencyHeadroomAt(double sampleRate) const noexcept {
    // ADR-0157 d3: the configured headroom is samples at 48 kHz; a higher
    // rate gets the same time, a lower one keeps the measured floor.
    if (headroom_ <= 0 || !(sampleRate > 48000.0)) return headroom_;
    const double scaled = std::ceil(static_cast<double>(headroom_) * sampleRate / 48000.0);
    constexpr double kMax = 1 << 24;   // 16.7 M samples: far above any real rate's need
    return static_cast<std::int32_t>(scaled < kMax ? scaled : kMax);
}

GraphHost::PrepareResult GraphHost::prepare(const rows::Model& model, const RealizeOptions& opts,
        double sampleRate, std::int32_t maxFrames,
        const std::function<void(GraphPlan&)>& transformPlan,
        std::shared_ptr<const GraphTransportState> transportState, NodePreparePolicy policy) {
    PrepareResult result;
    error_.clear();
    try {
        if (!std::isfinite(sampleRate) || sampleRate <= 0.0 || maxFrames <= 0 || opts.channels <= 0) {
            result.error = "invalid graph preparation format";
        } else {
            GraphPlan plan = planGraph(model);
            if (transformPlan) transformPlan(plan);
            auto realized = realize(plan, opts);
            result.problems = realized->problems();
            if (!realized->ok()) {
                result.error = realized->error();
            } else {
                auto& graph = realized->graph();
                graph.setLatencyHeadroom(latencyHeadroomAt(sampleRate));
                graph.setPrecision(precision_);
                graph.setDelayCompensation(delayCompensation_);
                graph.setNodePreparePolicy(policy);
                (void)graph.setParallelRendering(renderWorkers_, parallelMinimumFrames_, parallelMinimumNodes_,
                                                workerInitializer_, workerInitializationContext_);
                graph.prepare(sampleRate, maxFrames);
                if (!graph.ok()) {
                    result.error = graph.error();
                } else {
                    auto ready = std::unique_ptr<PreparedGraph>(new PreparedGraph);
                    ready->host_ = identity_;
                    ready->configuration_ = configuration_;
                    ready->formatEpoch_ = formatEpoch_;
                    ready->sampleRate_ = sampleRate;
                    ready->maxFrames_ = maxFrames;
                    ready->channels_ = opts.channels;
                    ready->value_ = std::make_unique<PublishedGraph>(std::move(realized), std::move(transportState), maxFrames);
                    result.graph = std::move(ready);
                }
            }
        }
    } catch (const std::exception& exception) {
        result.error = exception.what();
        result.graph.reset();
    }
    problems_ = result.problems;
    error_ = result.error;
    if (!result.graph) ++stats_.refused;
    return result;
}

bool GraphHost::publish(PreparedGraph& ready) {
    error_.clear();
    if (!ready.value_ || ready.host_.lock() != identity_) {
        error_ = "the ready graph is empty, expired or belongs to another host";
    } else if (ready.configuration_ != configuration_ || ready.formatEpoch_ != formatEpoch_) {
        error_ = "the ready graph belongs to an older host configuration";
    } else if (!ready.value_->graph().graph().preparationCurrent()) {
        error_ = "a ready graph node changed preparation or shape";
    } else {
        try {
            // Every throwing allocation precedes exchange. The preserving
            // publisher leaves ready.value_ owned if retirement preflight fails.
            problems_ = ready.value_->graph().problems();
            auto* realized = &ready.value_->graph();
            pub_.publishPrepared(ready.value_);
            live_ = realized;
            if (publishedSampleRate_ != ready.sampleRate_ || publishedMaxFrames_ != ready.maxFrames_ ||
                publishedChannels_ != ready.channels_) ++formatEpoch_;
            publishedSampleRate_ = ready.sampleRate_;
            publishedMaxFrames_ = ready.maxFrames_;
            publishedChannels_ = ready.channels_;
            ++stats_.published;
            return true;
        } catch (const std::exception& exception) {
            error_ = exception.what();
        }
    }
    ++stats_.refused;
    return false;
}

bool GraphHost::rebuild(const rows::Model& model, const RealizeOptions& opts, double sampleRate,
        std::int32_t maxFrames, const std::function<void(GraphPlan&)>& transformPlan,
        std::shared_ptr<const GraphTransportState> transportState, NodePreparePolicy policy) {
    auto result = prepare(model, opts, sampleRate, maxFrames, transformPlan, std::move(transportState), policy);
    return result.graph != nullptr && publish(*result.graph);
}

std::size_t GraphHost::collect() {
    const std::size_t n = pub_.collect();
    stats_.reclaimed += static_cast<std::int64_t>(n);
    return n;
}

Graph* GraphHost::currentGraph() const noexcept {
    return live_ != nullptr ? &live_->graph() : nullptr;
}

void GraphHost::fadeIn(const AudioIo& io) noexcept {
    if (fadeRemaining_ <= 0 || fadeLength_ <= 0 || (io.out == nullptr && io.out64 == nullptr)) return;

    const std::int32_t n = io.frames < fadeRemaining_ ? io.frames : fadeRemaining_;
    // Where in the ramp this block starts. The fade can span several blocks
    // when the block is shorter than the fade, so the position is carried
    // rather than restarted -- restarting it would retrigger the ramp on every
    // block and never finish.
    const std::int32_t done = fadeLength_ - fadeRemaining_;

    for (std::int32_t c = 0; c < io.numOut; ++c) {
        float* dst = io.out != nullptr ? io.out[c] : nullptr;
        if (dst != nullptr)
            for (std::int32_t i = 0; i < n; ++i) {
                const float g = static_cast<float>(done + i + 1) /
                                static_cast<float>(fadeLength_);
                dst[i] *= g;
            }
        // The same ramp on the full-width output (ADR-0207 d2), so the two
        // outputs of one block never disagree.
        double* wdst = io.out64 != nullptr ? io.out64[c] : nullptr;
        if (wdst != nullptr)
            for (std::int32_t i = 0; i < n; ++i)
                wdst[i] *= static_cast<double>(done + i + 1) / static_cast<double>(fadeLength_);
    }
    fadeRemaining_ -= n;
}

namespace {

// One walk over two sorted lists: no allocation, no map, and linear in the
// number of edges. Keyed by what an edge joins, never by node id, because ids
// shift when a track is added and every junction is a new object per rebuild.
template <typename Record>
std::int32_t carryHistory(const std::vector<Record>& a, const std::vector<Record>& b,
                          Graph& ga, Graph& gb) noexcept {
    std::int32_t carried = 0;
    std::size_t i = 0, j = 0;
    while (i < a.size() && j < b.size()) {
        if (a[i].key < b[j].key) { ++i; continue; }
        if (b[j].key < a[i].key) { ++j; continue; }
        const DelayLine* oldLine = ga.edgeLine(a[i].from, a[i].to, a[i].bus);
        DelayLine* newLine = gb.edgeLine(b[j].from, b[j].to, b[j].bus);
        // Only a line that DELAYS has history anyone will hear. A zero tap
        // reads the sample just written, so there is nothing to carry. And
        // only a line that took some is counted: one whose old ring held
        // nothing, or had none, carried nothing.
        if (oldLine != nullptr && newLine != nullptr && newLine->delay() > 0 &&
            newLine->adoptHistory(*oldLine))
            ++carried;
        ++i;
        ++j;
    }
    return carried;
}

}  // namespace

std::int32_t GraphHost::handover(const PublishedGraph& from,
                                const PublishedGraph& to) noexcept {
    Graph& ga = from.graph().graph();
    Graph& gb = to.graph().graph();
    // The edges between tracks, (track, track, bus); then the edges inside
    // them, which delay too: a track delay on a strip that is not the last
    // node of its chain, a source compensated against a later input.
    return carryHistory(from.graph().planEdges(), to.graph().planEdges(), ga, gb) +
           carryHistory(from.graph().chainEdges(), to.graph().chainEdges(), ga, gb) +
           gb.adoptBypassHistory(ga);
}

namespace {
void clearOutput(const AudioIo& io) noexcept {
    if (io.frames <= 0) return;
    for (std::int32_t c = 0; c < io.numOut; ++c) {
        if (io.out && io.out[c])
            std::memset(io.out[c], 0, static_cast<std::size_t>(io.frames) * sizeof(float));
        if (io.out64 && io.out64[c])
            std::memset(io.out64[c], 0, static_cast<std::size_t>(io.frames) * sizeof(double));
    }
}
}

const PublishedGraph* GraphHost::acquireRender() noexcept {
    ++stats_.blocks;
    ++renderSerial_;
    renderActive_ = true;
    // Retain the previous publication until its history has been carried. The
    // publisher's strictly-greater reclamation rule protects both pointers.
    const PublishedGraph* snap = pub_.peek();
    if (!snap) return nullptr;

    const bool adopting = snap->seq != lastSeq_;
    if (adopting) {
        // Accepted MIDI ownership is independent of the optional audio ring
        // history policy. Transfer pending deliveries before the old graph can
        // retire even when ring-history carry has deliberately been disabled.
        if (lastSnap_ != nullptr)
            (void)snap->graph().graph().adoptEventHistory(lastSnap_->graph().graph());
        if (lastSnap_ != nullptr && keepHistory_)
            stats_.historyCarried += handover(*lastSnap_, *snap);

        // ADR-0203 d3: a feedback loop in play again after time out of it
        // starts empty. Decided here, against the graph that really played
        // last block, and not by whoever last published: a loop the audio
        // thread never stopped playing keeps its echoes, and one it did stop
        // never plays what it held when it left.
        const std::vector<Node*>& now = snap->graph().loopReaders();
        const std::vector<Node*>* before = lastSnap_ != nullptr ? &lastSnap_->graph().loopReaders() : nullptr;
        std::size_t j = 0;
        for (Node* reader : now) {
            while (before != nullptr && j < before->size() && std::less<Node*>()((*before)[j], reader)) ++j;
            if (before == nullptr || j == before->size() || (*before)[j] != reader) reader->enterPlay();
        }

        // NOT on the first graph. `lastSeq_` is 0 until something has played,
        // and ramping the opening milliseconds of every session is an artefact
        // rather than the absence of one -- there is nothing to fade FROM.
        if (lastSeq_ != 0) {
            fadeLength_ = fadeFrames_;
            fadeRemaining_ = fadeFrames_;
        }
        lastSeq_ = snap->seq;
        ++stats_.swaps;
    }

    // NOW, and only now. From this store on the previous graph may be freed,
    // and nothing below touches it.
    pub_.announce(snap);
    lastSnap_ = snap;

    if (adopting) {
        if (const auto* state = snap->transportState()) state->adopted();
    }
    return snap;
}

GraphHost::RenderBlock::RenderBlock(GraphHost& host) noexcept
    : host_(host), snapshot_(host.acquireRender()), serial_(host.renderSerial_) {}
GraphHost::RenderBlock::~RenderBlock() {
    if (serial_ == host_.renderSerial_) host_.renderActive_ = false;
}
bool GraphHost::RenderBlock::valid() const noexcept {
    return host_.renderActive_ && serial_ == host_.renderSerial_ && snapshot_ != nullptr;
}
std::uint64_t GraphHost::RenderBlock::generation() const noexcept {
    return valid() ? snapshot_->seq : 0;
}
std::int32_t GraphHost::RenderBlock::latencySamples() const noexcept {
    return valid() ? snapshot_->graph().graph().latencySamples() : 0;
}
const GraphTransportState* GraphHost::RenderBlock::transportState() const noexcept {
    return valid() ? snapshot_->transportState() : nullptr;
}
bool GraphHost::RenderBlock::process(const AudioIo& io) noexcept {
    if (!valid() || io.frames <= 0 || io.numOut < 0 || io.numIn < 0 ||
        io.frames > snapshot_->maxFrames()) {
        clearOutput(io);
        return false;
    }
    AudioIo block = io;
    if (const auto* state = snapshot_->transportState()) block.transport = state->beginBlock();
    snapshot_->graph().graph().process(block);
    host_.fadeIn(io);
    return true;
}
void GraphHost::process(const AudioIo& io) noexcept {
    auto block = beginRender();
    (void)block.process(io);
}

const GraphTransportState* GraphHost::renderedTransportState()const noexcept {
    return lastSnap_?lastSnap_->transportState():nullptr;
}

bool GraphHost::copyRenderedOutput(const Node& node,const AudioIo& io) const noexcept {
    if(!lastSnap_)return false;
    const auto& graph=lastSnap_->graph().graph();
    return graph.copyNodeOutput(graph.find(node),io);
}

std::int32_t GraphHost::renderedLatency() const noexcept {
    return lastSnap_ ? lastSnap_->graph().graph().latencySamples() : 0;
}

}  // namespace adi::engine
