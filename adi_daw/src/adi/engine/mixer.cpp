// SPDX-License-Identifier: GPL-3.0-or-later
#include "adi/engine/mixer.hpp"

#include <algorithm>
#include <cmath>
#include <numbers>
#include <queue>

namespace adi::engine {

StereoGain panGains(double pan, PanLaw law) noexcept {
    const double p = std::isfinite(pan) ? std::clamp(pan, -1.0, 1.0) : 0.0;
    // THE CENTRE AND THE SIDES ARE EXACT, not computed. A centred track at 0 dB
    // must pass its samples through bit for bit -- ADR-0021's oracle compares
    // bytes, and every project rendered before the strip existed is centred --
    // and a hard side must be silence, not cos(pi/2) = 6e-17.
    const double theta = (p + 1.0) * std::numbers::pi / 4.0;
    switch (law) {
        case PanLaw::EqualPower:
            if (p == -1.0) return {1.0f, 0.0f};
            if (p == 1.0) return {0.0f, 1.0f};
            return {static_cast<float>(std::cos(theta)), static_cast<float>(std::sin(theta))};
        case PanLaw::Balance:
            return p <= 0.0 ? StereoGain{1.0f, static_cast<float>(1.0 + p)}
                            : StereoGain{static_cast<float>(1.0 - p), 1.0f};
        case PanLaw::Linear:
            return {static_cast<float>((1.0 - p) / 2.0), static_cast<float>((1.0 + p) / 2.0)};
        case PanLaw::Live:
            break;
    }
    if (p == 0.0) return {1.0f, 1.0f};
    if (p == -1.0) return {static_cast<float>(std::numbers::sqrt2), 0.0f};
    if (p == 1.0) return {0.0f, static_cast<float>(std::numbers::sqrt2)};
    return {static_cast<float>(std::numbers::sqrt2 * std::cos(theta)),
            static_cast<float>(std::numbers::sqrt2 * std::sin(theta))};
}

double faderGain(double volumeDb) noexcept {
    if (std::isnan(volumeDb)) return 1.0;           // named by MixerStrips::sync
    if (volumeDb <= kSilenceDb) return 0.0;          // -inf included
    if (volumeDb == 0.0) return 1.0;                 // exact, for the same reason as the centre
    return std::pow(10.0, volumeDb / 20.0);
}

// ---------------------------------------------------------------------------

void StripNode::setTarget(float left, float right, float other) noexcept {
    targetL_.store(left, std::memory_order_relaxed);
    targetR_.store(right, std::memory_order_relaxed);
    targetOther_.store(other, std::memory_order_relaxed);
}

void StripNode::prepare(double sampleRate, std::int32_t) {
    // A time, not a count: five milliseconds at every rate ADR-0157 allows.
    const auto frames = std::lround(kRampSeconds * sampleRate);
    rampFrames_ = static_cast<std::int32_t>(std::max<long>(1, frames));
}

void StripNode::process(const NodeIo& io) noexcept {
    const float target[3] = {targetL_.load(std::memory_order_relaxed),
                             targetR_.load(std::memory_order_relaxed),
                             targetOther_.load(std::memory_order_relaxed)};
    if (!started_) {
        for (int k = 0; k < 3; ++k) cur_[k] = rampTo_[k] = target[k];
        remaining_ = 0;
        started_ = true;
    } else if (target[0] != rampTo_[0] || target[1] != rampTo_[1] || target[2] != rampTo_[2]) {
        // A new target, from wherever the gain is now -- mid-ramp included.
        for (int k = 0; k < 3; ++k) {
            rampTo_[k] = target[k];
            step_[k] = (target[k] - cur_[k]) / static_cast<float>(rampFrames_);
        }
        remaining_ = rampFrames_;
    }

    const std::int32_t off = io.blockOffset;
    auto gainIndex = [](std::int32_t c) { return c < 2 ? c : 2; };

    if (remaining_ == 0) {
        // Steady: one gain per channel for the whole segment.
        for (std::int32_t c = 0; c < io.channels; ++c) {
            float* out = io.out[c] + off;
            const float* in = (io.in != nullptr && io.in[c] != nullptr) ? io.in[c] + off : nullptr;
            const float g = cur_[gainIndex(c)];
            if (in == nullptr || g == 0.0f) std::fill_n(out, io.frames, 0.0f);
            else if (g == 1.0f) std::copy_n(in, io.frames, out);
            else for (std::int32_t i = 0; i < io.frames; ++i) out[i] = in[i] * g;
        }
        return;
    }

    for (std::int32_t i = 0; i < io.frames; ++i) {
        if (remaining_ > 0) {
            for (int k = 0; k < 3; ++k) cur_[k] += step_[k];
            if (--remaining_ == 0)
                for (int k = 0; k < 3; ++k) cur_[k] = rampTo_[k];   // land exactly
        }
        for (std::int32_t c = 0; c < io.channels; ++c) {
            const float x = (io.in != nullptr && io.in[c] != nullptr) ? io.in[c][off + i] : 0.0f;
            io.out[c][off + i] = x * cur_[gainIndex(c)];
        }
    }
}

// ---------------------------------------------------------------------------

std::map<std::int64_t, bool> audibleTracks(const rows::Model& model, const GraphPlan& plan) {
    std::map<std::int64_t, const rows::Track*> track;
    for (const rows::Track& t : model.tracks) track[t.id] = &t;
    const std::size_t n = plan.nodes.size();

    std::vector<bool> muted(n, false), soloed(n, false), defeat(n, false);
    bool anySolo = false;
    for (std::size_t i = 0; i < n; ++i) {
        const auto it = track.find(plan.nodes[i].trackId);
        if (it == track.end()) continue;
        muted[i] = it->second->muted;
        defeat[i] = it->second->soloDefeat;
        // Soloing the master would solo everything it hears, which is nothing
        // a user means; it is ignored rather than refused.
        soloed[i] = it->second->soloed && plan.nodes[i].kind != PlannedKind::Master;
        anySolo = anySolo || soloed[i];
    }

    std::map<std::int64_t, bool> audible;
    if (!anySolo) {
        for (std::size_t i = 0; i < n; ++i) audible[plan.nodes[i].trackId] = !muted[i];
        return audible;
    }

    // Main routes only. A sidechain key is not listening, so a soloed track's
    // key source is not kept audible by it (ADR-0163 d4).
    std::vector<std::vector<std::size_t>> down(n), up(n);
    for (const PlannedEdge& e : plan.edges) {
        if (e.bus != Bus::Main || e.from >= n || e.to >= n) continue;
        down[e.from].push_back(e.to);
        up[e.to].push_back(e.from);
    }
    auto reach = [&](const std::vector<std::vector<std::size_t>>& adj) {
        std::vector<bool> seen(n, false);
        std::queue<std::size_t> q;
        for (std::size_t i = 0; i < n; ++i)
            if (soloed[i]) { seen[i] = true; q.push(i); }
        while (!q.empty()) {
            const std::size_t i = q.front();
            q.pop();
            for (const std::size_t j : adj[i])
                if (!seen[j]) { seen[j] = true; q.push(j); }
        }
        return seen;
    };
    const std::vector<bool> fed = reach(down);      // what a soloed track feeds: its group, the master
    const std::vector<bool> feeding = reach(up);    // what feeds it: a soloed group's children
    for (std::size_t i = 0; i < n; ++i) {
        const bool kept = fed[i] || feeding[i] || defeat[i] || plan.nodes[i].kind == PlannedKind::Master;
        audible[plan.nodes[i].trackId] = !muted[i] && kept;
    }
    return audible;
}

// ---------------------------------------------------------------------------

void MixerStrips::sync(const rows::Model& model) {
    problems_.clear();
    const GraphPlan plan = planGraph(model);
    const std::map<std::int64_t, bool> audible = audibleTracks(model, plan);
    std::map<std::int64_t, const rows::MixerStrip*> row;
    for (const rows::MixerStrip& s : model.strips) row[s.trackId] = &s;

    std::map<std::int64_t, bool> present;
    for (const PlannedNode& node : plan.nodes) {
        if (node.kind == PlannedKind::Vca) continue;   // a VCA carries no audio (ADR-0072)
        present[node.trackId] = true;
        auto& strip = strips_[node.trackId];
        if (!strip) strip = std::make_unique<StripNode>();

        double volume = 0.0, pan = 0.0;
        PanLaw law = PanLaw::Live;
        const std::string who = "tracks#" + std::to_string(node.trackId);
        if (const auto it = row.find(node.trackId); it != row.end()) {
            const rows::MixerStrip& r = *it->second;
            volume = r.volumeDb;
            pan = r.pan;
            if (r.panLaw >= 0 && r.panLaw <= 3) law = static_cast<PanLaw>(r.panLaw);
            else problems_.push_back(who + ": pan law " + std::to_string(r.panLaw) +
                                     " is not one of SPEC 6.9's; Live's is used");
            if (std::isnan(volume)) {
                problems_.push_back(who + ": volume is not a number; 0 dB is used");
                volume = 0.0;
            }
            if (!std::isfinite(pan)) {
                problems_.push_back(who + ": pan is not finite; centre is used");
                pan = 0.0;
            }
        }
        const auto heard = audible.find(node.trackId);
        const double fader = (heard != audible.end() && heard->second) ? faderGain(volume) : 0.0;
        const StereoGain g = panGains(pan, law);
        strip->setTarget(static_cast<float>(fader * g.left), static_cast<float>(fader * g.right),
                         static_cast<float>(fader));
    }
    // A departed track's strip stays, silent, until the session ends.
    for (auto& [id, strip] : strips_)
        if (!present.count(id)) strip->setTarget(0.0f, 0.0f, 0.0f);
}

StripNode* MixerStrips::stripFor(std::int64_t trackId) noexcept {
    const auto it = strips_.find(trackId);
    return it == strips_.end() ? nullptr : it->second.get();
}

}  // namespace adi::engine
