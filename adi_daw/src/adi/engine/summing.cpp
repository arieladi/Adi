// SPDX-License-Identifier: GPL-3.0-or-later

#include "adi/engine/summing.hpp"

#include "adi/engine/plan.hpp"
#include "adi/summing_flavors.hpp"

#include <algorithm>
#include <cmath>
#include <functional>
#include <map>
#include <mutex>
#include <set>
#include <string_view>
#include <vector>

// Airwindows' console algorithms (airwin2rack, MIT): their code, whose
// warnings are not ours.
#if defined(_MSC_VER)
#pragma warning(push, 0)
#elif defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wall"
#pragma GCC diagnostic ignored "-Wextra"
#pragma GCC diagnostic ignored "-Wconversion"
#pragma GCC diagnostic ignored "-Wsign-conversion"
#pragma GCC diagnostic ignored "-Wpedantic"
#endif
#include "autogen_airwin/AtmosphereBuss.h"
#include "autogen_airwin/AtmosphereChannel.h"
#include "autogen_airwin/C5RawBuss.h"
#include "autogen_airwin/C5RawChannel.h"
#include "autogen_airwin/Console9Buss.h"
#include "autogen_airwin/Console9Channel.h"
#include "autogen_airwin/ConsoleLABuss.h"
#include "autogen_airwin/ConsoleLAChannel.h"
#include "autogen_airwin/ConsoleMCBuss.h"
#include "autogen_airwin/ConsoleMCChannel.h"
#include "autogen_airwin/ConsoleMDBuss.h"
#include "autogen_airwin/ConsoleMDChannel.h"
#include "autogen_airwin/PDBuss.h"
#include "autogen_airwin/PDChannel.h"
#include "autogen_airwin/PurestConsole3Buss.h"
#include "autogen_airwin/PurestConsole3Channel.h"
#if defined(_MSC_VER)
#pragma warning(pop)
#elif defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

namespace adi::engine {

namespace {

using Base = AirwinConsolidatedBase;

struct Maker {
    std::string_view algorithm;
    Base* (*make)();
};

#define ADI_CONSOLE(sym) {#sym, +[]() -> Base* { return new airwinconsolidated::sym::sym(0); }}
constexpr Maker kMakers[] = {
    ADI_CONSOLE(AtmosphereBuss),   ADI_CONSOLE(AtmosphereChannel),  ADI_CONSOLE(C5RawBuss),
    ADI_CONSOLE(C5RawChannel),     ADI_CONSOLE(Console9Buss),       ADI_CONSOLE(Console9Channel),
    ADI_CONSOLE(ConsoleLABuss),    ADI_CONSOLE(ConsoleLAChannel),   ADI_CONSOLE(ConsoleMCBuss),
    ADI_CONSOLE(ConsoleMCChannel), ADI_CONSOLE(ConsoleMDBuss),      ADI_CONSOLE(ConsoleMDChannel),
    ADI_CONSOLE(PDBuss),           ADI_CONSOLE(PDChannel),          ADI_CONSOLE(PurestConsole3Buss),
    ADI_CONSOLE(PurestConsole3Channel),
};
#undef ADI_CONSOLE

float dbToGain(double db) { return static_cast<float>(std::pow(10.0, db / 20.0)); }

std::unique_ptr<Base> makeAlgorithm(std::string_view algorithm) {
    // The base class asserts on a rate of 0 until prepare sets the real one.
    if (Base::defaultSampleRate <= 0.0f) Base::defaultSampleRate = 48000.0f;
    for (const Maker& m : kMakers)
        if (m.algorithm == algorithm) return std::unique_ptr<Base>(m.make());
    return nullptr;
}

/// The gain that puts a flavour's channel-then-buss at unity, MEASURED.
///
/// Each console carries its designer's gain staging inside it -- Console9's
/// channel pans with a sine law and trims by 0.764 before its curve, LA and MC
/// attenuate by bit shifts in several places -- and summing is a colour, not a
/// level change. Reverse-engineering every fader law would be wrong the day one
/// changes, so the level is measured instead: a fresh pair, a -40 dBFS sine at
/// 1 kHz where every curve is linear, 0.2 s at 48 kHz, the second half compared
/// with its input. The buss half's output is scaled by the result. The console
/// still sees exactly the level its designer set; only its output is matched.
/// Once per flavour, on the message thread, cached.
double unityGain(const SummingFlavor& f) {
    static std::mutex lock;
    static std::map<std::string, double, std::less<>> cache;
    const std::lock_guard<std::mutex> hold(lock);
    if (const auto it = cache.find(f.key); it != cache.end()) return it->second;
    auto channel = makeAlgorithm(f.channel);
    auto buss = makeAlgorithm(f.buss);
    double ratio = 1.0;
    if (channel && buss) {
        channel->setSampleRate(48000.0f);
        buss->setSampleRate(48000.0f);
        constexpr int kFrames = 9600;
        constexpr int kBlock = 480;
        std::vector<float> l(kBlock), r(kBlock);
        double in = 0.0, out = 0.0;
        for (int start = 0; start < kFrames; start += kBlock) {
            for (std::size_t i = 0; i < l.size(); ++i) {
                const double t = static_cast<double>(start + static_cast<int>(i)) / 48000.0;
                l[i] = r[i] = static_cast<float>(0.01 * std::sin(2.0 * 3.14159265358979323846 * 1000.0 * t));
            }
            const bool measured = start >= kFrames / 2;
            if (measured)
                for (std::size_t i = 0; i < l.size(); ++i)
                    in += static_cast<double>(l[i]) * l[i] + static_cast<double>(r[i]) * r[i];
            float* ch[2] = {l.data(), r.data()};
            channel->processReplacing(ch, ch, kBlock);
            buss->processReplacing(ch, ch, kBlock);
            if (measured)
                for (std::size_t i = 0; i < l.size(); ++i)
                    out += static_cast<double>(l[i]) * l[i] + static_cast<double>(r[i]) * r[i];
        }
        if (out > 0.0 && in > 0.0) ratio = std::sqrt(in / out);
    }
    ratio = std::clamp(ratio, 0.25, 4.0);    // +-12 dB: past that, something is broken
    cache.emplace(std::string(f.key), ratio);
    return ratio;
}

}  // namespace

// ---------------------------------------------------------------------------

ConsoleNode::ConsoleNode(std::unique_ptr<Base> fx, std::string what)
    : fx_(std::move(fx)), what_(std::move(what)) {}

ConsoleNode::~ConsoleNode() = default;

void ConsoleNode::setGains(float pre, float post) noexcept {
    pre_.store(pre, std::memory_order_relaxed);
    post_.store(post, std::memory_order_relaxed);
}

void ConsoleNode::prepare(double sampleRate, std::int32_t) {
    // A persistent half is prepared again by every graph that places it, while
    // the graph before may still be playing it: write only on a real change.
    if (sampleRate > 0.0 && sampleRate != sampleRate_) {
        fx_->setSampleRate(static_cast<float>(sampleRate));
        sampleRate_ = sampleRate;
    }
}

std::shared_ptr<void> ConsoleNode::sourceLifetime() const {
    return std::const_pointer_cast<ConsoleNode>(shared_from_this());
}

void ConsoleNode::process(const NodeIo& io) noexcept {
    const std::int32_t off = io.blockOffset;
    const std::int32_t n = io.frames;
    const float pre = pre_.load(std::memory_order_relaxed);
    const float post = post_.load(std::memory_order_relaxed);
    for (std::int32_t c = 0; c < io.channels; ++c) {
        float* out = io.out[c] + off;
        const float* in = (io.in != nullptr && io.in[c] != nullptr) ? io.in[c] + off : nullptr;
        if (in == nullptr) std::fill_n(out, n, 0.0f);
        else if (pre == 1.0f) { if (in != out) std::copy_n(in, n, out); }
        else for (std::int32_t i = 0; i < n; ++i) out[i] = in[i] * pre;
    }
    if (io.channels <= 0 || n <= 0) return;
    // In place: Airwindows reads each sample before it writes it. A mono
    // track gives both sides the same buffer.
    float* ch[2] = {io.out[0] + off, io.out[io.channels > 1 ? 1 : 0] + off};
    fx_->processReplacing(ch, ch, n);
    if (post != 1.0f)
        for (std::int32_t c = 0; c < io.channels && c < 2; ++c)
            for (std::int32_t i = 0; i < n; ++i) io.out[c][off + i] *= post;
}

// ---------------------------------------------------------------------------

std::shared_ptr<ConsoleNode> makeConsoleHalf(const std::string& flavor, bool buss) {
    const SummingFlavor* f = findSummingFlavor(flavor);
    if (f == nullptr) return nullptr;
    const std::string_view algorithm = buss ? f->buss : f->channel;
    auto fx = makeAlgorithm(algorithm);
    if (!fx) return nullptr;
    return std::make_shared<ConsoleNode>(std::move(fx), std::string(algorithm));
}

double summingUnityGain(const std::string& flavor) {
    const SummingFlavor* f = findSummingFlavor(flavor);
    return f == nullptr ? 1.0 : unityGain(*f);
}

void GroupSumming::sync(const rows::Model& model) {
    problems_.clear();
    const GraphPlan plan = planGraph(model);
    std::map<std::int64_t, std::size_t> nodeOf;
    for (std::size_t i = 0; i < plan.nodes.size(); ++i) nodeOf[plan.nodes[i].trackId] = i;

    std::set<std::int64_t> liveBusses, liveChannels;
    for (const rows::GroupSumming& g : model.summing) {
        const std::string who = "tracks#" + std::to_string(g.trackId);
        const auto at = nodeOf.find(g.trackId);
        if (at == nodeOf.end() || plan.nodes[at->second].kind != PlannedKind::Group) {
            if (g.enabled) problems_.push_back(who + ": summing is on a track that is not a group; it is not played");
            continue;
        }
        if (!g.enabled) continue;
        const SummingFlavor* flavor = findSummingFlavor(g.flavor);
        if (flavor == nullptr) {
            problems_.push_back(who + ": summing flavour '" + g.flavor + "' is unknown; summing is off");
            continue;
        }
        const double drive = std::clamp(g.driveDb, -12.0, 24.0);

        // The buss half, on the group's sum: the drive back off, the flavour's
        // own gain staging made up.
        Half& b = busses_[g.trackId];
        if (!b.node || b.flavor != g.flavor) b = Half{g.flavor, g.trackId, makeConsoleHalf(g.flavor, true)};
        b.node->setGains(1.0f, dbToGain(-drive) * static_cast<float>(unityGain(*flavor)));
        liveBusses.insert(g.trackId);

        // A channel half on every track whose main output is this group.
        for (const PlannedEdge& e : plan.edges) {
            if (e.bus != Bus::Main || e.to != at->second) continue;
            const PlannedNode& child = plan.nodes[e.from];
            if (child.kind == PlannedKind::Vca) continue;
            Half& c = channels_[child.trackId];
            if (!c.node || c.flavor != g.flavor || c.group != g.trackId)
                c = Half{g.flavor, g.trackId, makeConsoleHalf(g.flavor, false)};
            c.node->setGains(dbToGain(drive), 1.0f);
            liveChannels.insert(child.trackId);
        }
    }
    // Gone from the model: dropped here, kept alive by any graph still playing them.
    for (auto it = busses_.begin(); it != busses_.end();)
        it = liveBusses.count(it->first) ? std::next(it) : busses_.erase(it);
    for (auto it = channels_.begin(); it != channels_.end();)
        it = liveChannels.count(it->first) ? std::next(it) : channels_.erase(it);
}

Node* GroupSumming::headFor(std::int64_t trackId) noexcept {
    const auto it = busses_.find(trackId);
    return it == busses_.end() ? nullptr : it->second.node.get();
}

Node* GroupSumming::tailFor(std::int64_t trackId) noexcept {
    const auto it = channels_.find(trackId);
    return it == channels_.end() ? nullptr : it->second.node.get();
}

const ConsoleNode* GroupSumming::bussOf(std::int64_t groupId) const noexcept {
    const auto it = busses_.find(groupId);
    return it == busses_.end() ? nullptr : it->second.node.get();
}

const ConsoleNode* GroupSumming::channelOf(std::int64_t childId) const noexcept {
    const auto it = channels_.find(childId);
    return it == channels_.end() ? nullptr : it->second.node.get();
}

}  // namespace adi::engine
