// SPDX-License-Identifier: GPL-3.0-or-later

#include "adi/engine/host.hpp"

#include <cstring>
#include <utility>

namespace adi::engine {

bool GraphHost::rebuild(const rows::Model& model, const RealizeOptions& opts,
                        double sampleRate, std::int32_t maxFrames) {
    error_.clear();

    const GraphPlan plan = planGraph(model);
    std::unique_ptr<RealizedGraph> rg = realize(plan, opts);
    problems_ = rg->problems();

    if (!rg->ok()) {
        error_ = rg->error();
        ++stats_.refused;
        return false;
    }

    rg->graph().setLatencyHeadroom(headroom_);
    rg->graph().prepare(sampleRate, maxFrames);
    if (!rg->graph().ok()) {
        error_ = rg->graph().error();
        ++stats_.refused;
        return false;
    }

    // PUBLISHED LAST, and only once everything above has succeeded. Up to this
    // line the running graph has not been touched, so a model with a cycle or
    // no master costs a wasted build and nothing else.
    live_ = rg.get();
    pub_.publish(std::make_unique<PublishedGraph>(std::move(rg)));
    ++stats_.published;
    return true;
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
    if (fadeRemaining_ <= 0 || fadeLength_ <= 0 || io.out == nullptr) return;

    const std::int32_t n = io.frames < fadeRemaining_ ? io.frames : fadeRemaining_;
    // Where in the ramp this block starts. The fade can span several blocks
    // when the block is shorter than the fade, so the position is carried
    // rather than restarted -- restarting it would retrigger the ramp on every
    // block and never finish.
    const std::int32_t done = fadeLength_ - fadeRemaining_;

    for (std::int32_t c = 0; c < io.numOut; ++c) {
        float* dst = io.out[c];
        if (dst == nullptr) continue;
        for (std::int32_t i = 0; i < n; ++i) {
            const float g = static_cast<float>(done + i + 1) /
                            static_cast<float>(fadeLength_);
            dst[i] *= g;
        }
    }
    fadeRemaining_ -= n;
}

std::int32_t GraphHost::handover(const PublishedGraph& from,
                                const PublishedGraph& to) noexcept {
    const auto& a = from.graph().planEdges();
    const auto& b = to.graph().planEdges();
    Graph& ga = from.graph().graph();
    Graph& gb = to.graph().graph();

    // One walk over two sorted lists: no allocation, no map, and linear in the
    // number of edges. Keyed by (track, track, bus), because node ids shift
    // when a track is added and every junction is a new object per rebuild.
    std::int32_t carried = 0;
    std::size_t i = 0, j = 0;
    while (i < a.size() && j < b.size()) {
        if (a[i].key < b[j].key) { ++i; continue; }
        if (b[j].key < a[i].key) { ++j; continue; }
        const DelayLine* oldLine = ga.edgeLine(a[i].from, a[i].to, a[i].bus);
        DelayLine* newLine = gb.edgeLine(b[j].from, b[j].to, b[j].bus);
        // Only a line that DELAYS has history anyone will hear. A zero tap
        // reads the sample just written, so there is nothing to carry.
        if (oldLine != nullptr && newLine != nullptr && newLine->delay() > 0) {
            newLine->adoptHistory(*oldLine);
            ++carried;
        }
        ++i;
        ++j;
    }
    return carried;
}

void GraphHost::process(const AudioIo& io) noexcept {
    ++stats_.blocks;

    // PEEK, DO NOT ANNOUNCE YET (ADR-0092). The graph rendered last block was
    // announced last block, so it is protected by the publisher's strictly-
    // greater rule until we announce something newer -- which is the only
    // window in which it is safe to read its rings.
    const PublishedGraph* snap = pub_.peek();
    if (snap == nullptr) {
        // Silence, WRITTEN. Returning without writing hands the driver
        // uninitialised memory on the first call and the previous block on
        // every one after, which is the artefact people describe as a stutter.
        if (io.out != nullptr)
            for (std::int32_t c = 0; c < io.numOut; ++c)
                if (io.out[c] != nullptr)
                    std::memset(io.out[c], 0,
                                static_cast<std::size_t>(io.frames) * sizeof(float));
        return;
    }

    if (snap->seq != lastSeq_) {
        if (lastSnap_ != nullptr && keepHistory_)
            stats_.historyCarried += handover(*lastSnap_, *snap);

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

    snap->graph().graph().process(io);
    fadeIn(io);
}

}  // namespace adi::engine
