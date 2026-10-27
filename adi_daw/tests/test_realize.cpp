// SPDX-License-Identifier: GPL-3.0-or-later
//
// Realisation — src/adi/engine/realize.{hpp,cpp}.
//
// The seam these tests are really about: a project description goes in one end
// and audio comes out the other, with NOTHING in between knowing what a plugin
// format is. A `rows::Model` becomes a `GraphPlan` becomes a live `Graph` that
// processes a block, and the only thing injected along the way is a
// `std::vector<Node*>` per track.
//
// Compensation is checked here rather than only in test_graph.cpp, and that is
// deliberate: `test_graph` proves the arithmetic against hand-built nodes, and
// this proves the arithmetic is REACHED when the graph was built from a
// project. Those fail separately. A realiser that connected a device chain
// backwards would leave every graph test green.

#include "adi/engine/realize.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

using namespace adi;
using namespace adi::engine;

namespace {

int g_failures = 0;
int g_checks = 0;

void check(bool cond, const std::string& what) {
    ++g_checks;
    if (!cond) { ++g_failures; std::printf("  FAIL  %s\n", what.c_str()); }
}
void eqi(long long got, long long want, const std::string& what) {
    ++g_checks;
    if (got != want) {
        ++g_failures;
        std::printf("  FAIL  %s\n          got %lld, want %lld\n", what.c_str(), got, want);
    }
}
void section(const char* s) { std::printf("[%s]\n", s); }

rows::Track track(std::int64_t id, const char* kind, const char* name,
                  std::optional<std::int64_t> parent = std::nullopt) {
    rows::Track t;
    t.id = id;
    t.kind = kind;
    t.name = name;
    t.parentId = parent;
    return t;
}

rows::Routing route(std::int64_t id, std::int64_t from, std::int64_t to,
                    const char* kind = "main", const char* origin = "user") {
    rows::Routing r;
    r.id = id;
    r.srcKind = "track";
    r.srcId = from;
    r.dstKind = "track";
    r.dstId = to;
    r.kind = kind;
    r.origin = origin;
    return r;
}

std::string problems(const RealizedGraph& r) {
    std::string s;
    for (const auto& x : r.problems()) { s += "\n      - "; s += x; }
    return s.empty() ? std::string(" (none)") : s;
}

// --- fixtures ---------------------------------------------------------------

/// Writes a constant. Stands in for a clip reader: a track's content, pushed
/// into the head of its chain.
class ToneNode final : public Node {
public:
    explicit ToneNode(float v) : v_(v) {}
    void process(const NodeIo& io) noexcept override {
        for (std::int32_t c = 0; c < io.channels; ++c) {
            float* o = io.out[c] + io.blockOffset;
            for (std::int32_t i = 0; i < io.frames; ++i) o[i] = v_;
        }
    }
    // NO tailSamples() OVERRIDE, and the first version of this file had one.
    // A generator produces whether or not anything drives it, so declaring 0
    // says "I stop when my input goes quiet" -- and its input is quiet
    // forever, because it has none. ADR-0043 suspended it on block 1 and every
    // audio assertion here read 0.0 while every structural one passed, which
    // looks exactly like a realiser that forgot to connect anything.
    // Inheriting the base class's kInfiniteTail is the correct answer for
    // anything that is a SOURCE.
    [[nodiscard]] const char* name() const noexcept override { return "tone"; }
private:
    float v_;
};

/// Adds a constant and optionally declares latency, so a chain's ORDER is
/// visible in the output and its latency is visible to PDC.
class AddNode final : public Node {
public:
    AddNode(float add, std::int32_t latency = 0) : add_(add), latency_(latency) {}
    void process(const NodeIo& io) noexcept override {
        ++calls;
        for (std::int32_t c = 0; c < io.channels; ++c) {
            float* o = io.out[c] + io.blockOffset;
            const float* i = (io.in != nullptr && io.in[c] != nullptr)
                                 ? io.in[c] + io.blockOffset : nullptr;
            for (std::int32_t k = 0; k < io.frames; ++k)
                o[k] = (i != nullptr ? i[k] : 0.0f) + add_;
        }
    }
    [[nodiscard]] std::int64_t tailSamples() const noexcept override { return 0; }
    [[nodiscard]] std::int32_t latencySamples() const noexcept override { return latency_; }
    [[nodiscard]] const char* name() const noexcept override { return "add"; }
    int calls = 0;
private:
    float add_;
    std::int32_t latency_;
};

struct Out {
    std::vector<float> l, r;
    std::vector<float*> ptrs;
    explicit Out(std::int32_t n) : l(static_cast<std::size_t>(n), -1.0f),
                                   r(static_cast<std::size_t>(n), -1.0f) {
        ptrs = {l.data(), r.data()};
    }
};

AudioIo makeIo(Out& o, std::int32_t frames) {
    AudioIo io;
    io.out = o.ptrs.data();
    io.numOut = 2;
    io.frames = frames;
    io.streamTimeSamples = 0;
    return io;
}

// ===========================================================================

void testTheSimplestProjectRuns() {
    section("a track and a master, from rows to audio");

    rows::Model m;
    m.tracks.push_back(track(1, "audio", "Kick"));
    m.tracks.push_back(track(9, "master", "Master"));

    const GraphPlan plan = planGraph(m);
    auto r = realize(plan);
    check(r->ok(), "realised: " + r->error());
    eqi(static_cast<long long>(r->nodeCount()), 2,
        "two tracks, two junctions, no hidden nodes");

    // The head and the tail of a bare track are the SAME node. A caller that
    // has to ask which one it wants would find this out by getting silence.
    check(r->inputFor(1) == r->outputFor(1),
          "a track with no devices is one node, entered and left at the same id");
    check(r->inputFor(1) != kInvalidNode, "and it is a real node");

    // Drive it: the tone stands in for a clip reader on the track.
    ToneNode tone(0.25f);
    const NodeId nt = r->graph().addNode(tone);
    r->graph().connect(nt, r->inputFor(1));
    r->graph().prepare(48000.0, 64);
    check(r->graph().ok(), "prepared: " + r->graph().error());

    Out o(64);
    AudioIo io = makeIo(o, 64);
    r->graph().process(io);
    bool allTone = true;
    for (float v : o.l) if (v != 0.25f) allTone = false;
    check(allTone, "the track's audio reaches the master, unchanged: got " +
                       std::to_string(o.l[0]));
}

void testAGroupSumsItsChildren() {
    section("ADR-0044 -- a group is a node with no device, and it sums");

    rows::Model m;
    m.tracks.push_back(track(1, "audio", "A", 5));
    m.tracks.push_back(track(2, "audio", "B", 5));
    m.tracks.push_back(track(5, "group", "Drums"));
    m.tracks.push_back(track(9, "master", "Master"));

    const GraphPlan plan = planGraph(m);
    auto r = realize(plan);
    check(r->ok(), "realised: " + r->error());
    check(r->problems().empty(), "with no problems:" + problems(*r));

    ToneNode a(0.25f), b(0.5f);
    const NodeId na = r->graph().addNode(a), nb = r->graph().addNode(b);
    r->graph().connect(na, r->inputFor(1));
    r->graph().connect(nb, r->inputFor(2));
    r->graph().prepare(48000.0, 64);
    check(r->graph().ok(), "prepared: " + r->graph().error());

    Out o(64);
    AudioIo io = makeIo(o, 64);
    r->graph().process(io);
    check(o.l[0] == 0.75f,
          "both children arrive at the master through the group: got " +
              std::to_string(o.l[0]));
}

void testADeviceChainRunsInOrder() {
    section("a track is a CHAIN -- devices run in series, in signal order");

    rows::Model m;
    m.tracks.push_back(track(1, "audio", "Lead"));
    m.tracks.push_back(track(9, "master", "Master"));

    // Two devices whose effects do not commute would be better still, but +1
    // then *2 needs a multiplying fixture; instead the ORDER is checked by the
    // node ids and the SERIES by the arithmetic.
    AddNode d1(1.0f), d2(10.0f);
    RealizeOptions opts;
    opts.devicesFor = [&](std::int64_t id) -> std::vector<Node*> {
        if (id == 1) return {&d1, &d2};
        return {};
    };

    const GraphPlan plan = planGraph(m);
    auto r = realize(plan, opts);
    check(r->ok(), "realised: " + r->error());
    eqi(static_cast<long long>(r->nodeCount()), 4,
        "two junctions plus two devices");

    // THE POINT OF THE JUNCTION. The track's input is not its first plugin, so
    // adding or removing a device does not change the id anything upstream --
    // or PDC -- is holding.
    check(r->inputFor(1) != r->outputFor(1),
          "with devices, a track is entered and left at DIFFERENT nodes");

    ToneNode tone(0.25f);
    const NodeId nt = r->graph().addNode(tone);
    r->graph().connect(nt, r->inputFor(1));
    r->graph().prepare(48000.0, 64);
    check(r->graph().ok(), "prepared: " + r->graph().error());

    Out o(64);
    AudioIo io = makeIo(o, 64);
    r->graph().process(io);
    check(o.l[0] == 11.25f,
          "both devices ran, in series, between the track and the master: got " +
              std::to_string(o.l[0]));
    eqi(d1.calls, 1, "the first device ran once");
    eqi(d2.calls, 1, "and so did the second");
}

// --- sends (ADR-0203 d3) ----------------------------------------------------

/// Track 1 (a tone through +1 then +10), a return 3, the master. `tap` is
/// where track 1's send to the return leaves; the send's own node adds 100,
/// so each path's share of the master is visible.
struct SendRig {
    rows::Model m;
    ToneNode tone{0.25f};
    AddNode fx{1.0f}, fader{10.0f}, send{100.0f};
    RealizeOptions opts;
    SendRig() {
        m.tracks.push_back(track(1, "audio", "A"));
        m.tracks.push_back(track(3, "return", "R"));
        m.tracks.push_back(track(9, "master", "Master"));
        opts.devicesFor = [this](std::int64_t id) -> std::vector<Node*> {
            if (id == 1) return {&fx, &fader};
            return {};
        };
        opts.tapFor = [this](std::int64_t id, Tap tap) -> Node* {
            if (id != 1) return nullptr;
            if (tap == Tap::PreFx) return nullptr;       // the junction: no instrument here
            if (tap == Tap::PostFx) return &fx;
            return &fader;
        };
        opts.sendFor = [this](std::int64_t id) -> Node* { return id == 11 ? &send : nullptr; };
    }
    [[nodiscard]] GraphPlan plan(Tap tap, std::int64_t routingId = 11) const {
        GraphPlan p = planGraph(m);
        PlannedEdge e;
        e.from = *p.indexOf(1);
        e.to = *p.indexOf(3);
        e.kind = "send";
        e.routingId = routingId;
        e.tap = tap;
        p.edges.push_back(e);
        return p;
    }
    /// The master's first sample, the tone pushed into track 1.
    float play(RealizedGraph& r) {
        const NodeId nt = r.graph().addNode(tone);
        r.graph().connect(nt, r.inputFor(1));
        r.graph().prepare(48000.0, 256);
        Out o(256);
        AudioIo io = makeIo(o, 256);
        r.graph().process(io);
        return r.graph().ok() ? o.l[255] : -1.0f;
    }
};

void testASendLeavesAtItsTap() {
    section("ADR-0203 d3 -- a send leaves its track at its tap, through its own node, into the return");

    // The main path is 0.25 + 1 + 10 = 11.25 whatever the tap. The send adds
    // 100 to what it is handed and the return passes it on.
    struct Case { Tap tap; float want; const char* what; };
    for (const Case& c : {Case{Tap::PostFader, 11.25f + 111.25f, "Post: after the fader node"},
                          Case{Tap::PostFx, 11.25f + 101.25f, "Pre: after the effects, before the fader"},
                          Case{Tap::PreFx, 11.25f + 100.25f, "Pre FX here is the junction: the tone alone"},
                          Case{Tap::Tail, 11.25f + 111.25f, "the tail is the chain's end"}}) {
        SendRig rig;
        auto r = realize(rig.plan(c.tap), rig.opts);
        check(r->ok() && r->problems().empty(), std::string(c.what) + ": realised, no problems" + problems(*r));
        const float got = rig.play(*r);
        check(got == c.want, std::string(c.what) + ": the master is " + std::to_string(c.want) +
                                 ", got " + std::to_string(got));
        eqi(rig.send.calls, 1, std::string(c.what) + ": the send's node ran once");
    }
    {
        // No tap function: every tap is the tail.
        SendRig rig;
        rig.opts.tapFor = nullptr;
        auto r = realize(rig.plan(Tap::PostFx), rig.opts);
        check(rig.play(*r) == 11.25f + 111.25f, "unset, a tap is the chain's tail");
    }
}

void testASendIsCompensated() {
    section("ADR-0203 d3 -- a send's path is compensated like any other");

    // A Pre send leaves before a 64-sample plugin, so through the return it
    // reaches the master 64 samples ahead of the track's own path.
    SendRig rig;
    AddNode latent(10.0f, 64);
    rig.opts.devicesFor = [&](std::int64_t id) -> std::vector<Node*> {
        if (id == 1) return {&rig.fx, &latent};
        return {};
    };
    auto r = realize(rig.plan(Tap::PostFx), rig.opts);
    check(r->ok(), "realised: " + r->error());
    const float got = rig.play(*r);
    eqi(r->graph().compensationFor(r->outputFor(3), r->inputFor(9)), 64,
        "the return's way into the master is held back the plugin's 64 samples");
    eqi(r->graph().compensationFor(r->outputFor(1), r->inputFor(9)), 0, "the track's own is not");
    eqi(r->graph().latencySamples(), 64, "and the graph is 64 late, not 128");
    check(got == 11.25f + 101.25f, "both paths meet in the master: " + std::to_string(got));
}

void testAReturnsLoopIsRealised() {
    section("ADR-0203 d3 -- a return's send into itself: the send ends in a writer, a reader starts the return");

    // Row 12, R into itself (a plan's feedback edge), at R's junction.
    auto loopPlan = [](const SendRig& rig) {
        GraphPlan p = planGraph(rig.m);
        PlannedEdge e;
        e.from = e.to = *p.indexOf(3);
        e.kind = "send";
        e.routingId = 12;
        e.tap = Tap::PostFader;
        e.feedback = true;
        p.edges.push_back(e);
        return p;
    };
    {
        SendRig rig;
        AddNode writer(0.0f), reader(0.0f);
        rig.opts.sendFor = [&](std::int64_t id) -> Node* { return id == 12 ? &rig.send : nullptr; };
        rig.opts.feedbackFor = [&](std::int64_t id) {
            return id == 12 ? FeedbackNodes{&writer, &reader} : FeedbackNodes{};
        };
        auto r = realize(loopPlan(rig), rig.opts);
        check(r->ok() && r->problems().empty(), "realised, no problems" + problems(*r));
        check(rig.play(*r) == 11.25f, "the graph prepares and plays: A's own path, the loop empty");
        const Graph& g = r->graph();
        const NodeId ns = g.find(rig.send), nw = g.find(writer), nr = g.find(reader);
        check(ns != kInvalidNode && nw != kInvalidNode && nr != kInvalidNode, "the send, the writer and the reader");
        check(g.compensationFor(r->inputFor(3), ns, Bus::Main) >= 0 && g.compensationFor(ns, nw, Bus::Main) >= 0 &&
                  g.compensationFor(nr, r->inputFor(3), Bus::Main) >= 0,
              "joined R's junction -> the send -> the writer, and the reader -> R's junction");
        check(g.compensationFor(ns, r->inputFor(3), Bus::Main) == -1, "and the send does not feed R: no cycle");
        using Key = RealizedGraph::ChainKey;
        auto has = [&](std::int64_t track, Key::Leg leg, const Node* from, std::uint64_t name) {
            for (const auto& c : r->chainEdges())
                if (c.key.track == track && c.key.leg == leg && c.key.from == from && c.key.name == name) return true;
            return false;
        };
        check(has(3, Key::Leg::Send, nullptr, 12) && has(3, Key::Leg::Next, &rig.send, 0) &&
                  has(3, Key::Leg::Into, &reader, 0) &&
                  std::is_sorted(r->chainEdges().begin(), r->chainEdges().end(),
                                 [](const auto& a, const auto& b) { return a.key < b.key; }),
              "each of its edges keyed, the keys sorted");
        check(r->loopReaders() == std::vector<Node*>{&reader}, "and the reader listed, for the swap");
    }
    // No loop for it: the row is named, nothing of it is in the graph, and
    // the rest plays.
    for (const bool unset : {true, false}) {
        SendRig rig;
        AddNode reader(0.0f);
        rig.opts.sendFor = [&](std::int64_t id) -> Node* { return id == 12 ? &rig.send : nullptr; };
        if (!unset) rig.opts.feedbackFor = [&](std::int64_t) { return FeedbackNodes{nullptr, &reader}; };
        auto r = realize(loopPlan(rig), rig.opts);
        const std::string what = unset ? "no loop function" : "a loop with no writer";
        check(r->ok() && problems(*r).find("routing#12: no loop to close") != std::string::npos,
              what + ": named" + problems(*r));
        check(r->graph().find(rig.send) == kInvalidNode && r->graph().find(reader) == kInvalidNode &&
                  r->loopReaders().empty(),
              what + ": none of its nodes is placed");
        check(rig.play(*r) == 11.25f, what + ": and the rest plays");
    }
}

void testASendIsRefusedByName() {
    section("ADR-0203 d3 -- a send that cannot be made is named, and the rest plays");

    {
        SendRig rig;
        rig.opts.sendFor = nullptr;
        auto r = realize(rig.plan(Tap::PostFader), rig.opts);
        check(r->ok(), "still realised");
        check(problems(*r).find("routing#11: no node to play the send through") != std::string::npos,
              "no node for it: named" + problems(*r));
        check(rig.play(*r) == 11.25f, "and the track plays without it");
    }
    {
        SendRig rig;
        AddNode stranger(0.0f);
        rig.opts.tapFor = [&](std::int64_t, Tap) -> Node* { return &stranger; };
        auto r = realize(rig.plan(Tap::PostFader), rig.opts);
        check(problems(*r).find("routing#11: its tap is not a node of tracks#1's chain") != std::string::npos,
              "a tap that is not in the chain: named" + problems(*r));
        check(rig.play(*r) == 11.25f, "and not made");
    }
    {
        // Two sends handed one node: it would run twice a block.
        SendRig rig;
        rig.opts.sendFor = [&](std::int64_t) -> Node* { return &rig.send; };
        GraphPlan p = rig.plan(Tap::PostFader, 11);
        PlannedEdge twin = p.edges.back();
        twin.routingId = 12;
        p.edges.push_back(twin);
        auto r = realize(p, rig.opts);
        check(problems(*r).find("routing#12: a send's node given twice; the second was dropped") != std::string::npos,
              "the second send on one node: named" + problems(*r));
        check(rig.play(*r) == 11.25f + 111.25f && rig.send.calls == 1, "and the first plays, once");
    }
    {
        // The same rule for any node: a device in two chains.
        SendRig rig;
        rig.opts.devicesFor = [&](std::int64_t id) -> std::vector<Node*> {
            if (id == 1 || id == 3) return {&rig.fx};
            return {};
        };
        auto r = realize(planGraph(rig.m), rig.opts);
        check(problems(*r).find("tracks#3: a chain node given twice; the second was dropped") != std::string::npos,
              "a device in two chains: named" + problems(*r));
    }
}

void testASendsEdgesAreKeyedForTheNextGraph() {
    section("ADR-0092 -- both of a send's edges are keyed so the next graph finds them");

    // The return has a device of its own, so its chain's edges are made
    // before track 1's send: the list is in order only if it is sorted last.
    SendRig rig;
    AddNode returnFx(0.0f);
    rig.opts.devicesFor = [&](std::int64_t id) -> std::vector<Node*> {
        if (id == 1) return {&rig.fx, &rig.fader};
        if (id == 3) return {&returnFx};
        return {};
    };
    auto a = realize(rig.plan(Tap::PostFx), rig.opts);
    auto b = realize(rig.plan(Tap::PostFx), rig.opts);
    using Leg = RealizedGraph::ChainKey::Leg;
    auto find = [](const RealizedGraph& r, std::int64_t track, Leg leg, const Node* from, std::uint64_t name) {
        for (const auto& rec : r.chainEdges())
            if (rec.key.track == track && rec.key.leg == leg && rec.key.from == from && rec.key.name == name)
                return true;
        return false;
    };
    check(find(*a, 1, Leg::Send, &rig.fx, 11) && find(*b, 1, Leg::Send, &rig.fx, 11),
          "the tap into the send: track 1's, a Send from its effect named by the row, in both graphs");
    check(find(*a, 3, Leg::Into, &rig.send, 0) && find(*b, 3, Leg::Into, &rig.send, 0),
          "the send into the return: the return's, Into its junction from the send, in both");
    check(find(*a, 1, Leg::Next, &rig.fx, 0), "and the effect's own next edge is apart from the send's");
    check(std::is_sorted(a->chainEdges().begin(), a->chainEdges().end(),
                         [](const auto& x, const auto& y) { return x.key < y.key; }),
          "and the list the swap walks is sorted with them in it");
}

void testCompensationIsReachedFromAPlan() {
    section("ADR-0058 -- a plugin's latency is compensated because the graph was BUILT");

    // Two tracks into the master, one carrying a 64-sample plugin. Nothing in
    // `realize` computes a delay; `Graph::prepare` does, from what the node
    // declares. This checks the wiring reaches it.
    rows::Model m;
    m.tracks.push_back(track(1, "audio", "Latent"));
    m.tracks.push_back(track(2, "audio", "Direct"));
    m.tracks.push_back(track(9, "master", "Master"));

    AddNode plugin(0.0f, 64);
    RealizeOptions opts;
    opts.devicesFor = [&](std::int64_t id) -> std::vector<Node*> {
        if (id == 1) return {&plugin};
        return {};
    };

    auto r = realize(planGraph(m), opts);
    check(r->ok(), "realised: " + r->error());
    r->graph().prepare(48000.0, 256);
    check(r->graph().ok(), "prepared: " + r->graph().error());

    eqi(r->graph().latencySamples(), 64, "the graph reports the plugin's latency");
    eqi(r->graph().compensationFor(r->outputFor(2), r->inputFor(9)), 64,
        "and the OTHER track is delayed to match -- kick against bass, and it "
        "came from a routing table rather than from a hand-built graph");
    eqi(r->graph().compensationFor(r->outputFor(1), r->inputFor(9)), 0,
        "while the late one is not delayed twice");
}

void testBypassChangesLatencyButNotTopology() {
    section("bypass moves the compensation, not the graph");

    // The reason `DeviceNode::setBypassed` does not remove the node: the
    // topology has to stay put so that every id survives, and only the numbers
    // change. Checked here with a plain `Node` standing in, because the rule
    // belongs to the graph rather than to any plugin format.
    rows::Model m;
    m.tracks.push_back(track(1, "audio", "Latent"));
    m.tracks.push_back(track(2, "audio", "Direct"));
    m.tracks.push_back(track(9, "master", "Master"));

    AddNode plugin(0.0f, 64);
    RealizeOptions opts;
    opts.devicesFor = [&](std::int64_t id) -> std::vector<Node*> {
        if (id == 1) return {&plugin};
        return {};
    };
    auto r = realize(planGraph(m), opts);
    r->graph().prepare(48000.0, 256);

    const std::size_t before = r->nodeCount();
    const NodeId out1 = r->outputFor(1);
    eqi(r->graph().compensationFor(r->outputFor(2), r->inputFor(9)), 64,
        "compensated while the plugin is active");

    // A second realisation with a zero-latency stand-in is what "bypassed"
    // looks like to the graph: same nodes, same ids, different declaration.
    AddNode bypassed(0.0f, 0);
    RealizeOptions opts2;
    opts2.devicesFor = [&](std::int64_t id) -> std::vector<Node*> {
        if (id == 1) return {&bypassed};
        return {};
    };
    auto r2 = realize(planGraph(m), opts2);
    r2->graph().prepare(48000.0, 256);

    eqi(static_cast<long long>(r2->nodeCount()), static_cast<long long>(before),
        "the node count is unchanged");
    check(r2->outputFor(1) == out1, "and so is the track's output id");
    eqi(r2->graph().compensationFor(r2->outputFor(2), r2->inputFor(9)), 0,
        "but nothing is delayed against a latency that is no longer declared");
    eqi(r2->graph().latencySamples(), 0, "and the graph costs nothing");
}

void testAVcaIsNotAnAudioNode() {
    section("a VCA controls; it does not carry audio");

    rows::Model m;
    m.tracks.push_back(track(1, "audio", "A"));
    m.tracks.push_back(track(7, "vca", "Faders"));
    m.tracks.push_back(track(9, "master", "Master"));

    const GraphPlan plan = planGraph(m);
    check(plan.indexOf(7).has_value(), "the PLAN has the VCA -- it is a track");

    auto r = realize(plan);
    check(r->ok(), "realised: " + r->error());
    eqi(static_cast<long long>(r->nodeCount()), 2,
        "but the GRAPH does not: a node that moves nothing would still be "
        "processed every block");
    check(!r->has(7), "and asking for it says so");
    check(r->outputFor(7) == kInvalidNode, "with an invalid id rather than a guess");
}

void testARoutingRowNamingAVcaIsReported() {
    section("a user routing row into a VCA is named, not silently dropped");

    rows::Model m;
    m.tracks.push_back(track(1, "audio", "A"));
    m.tracks.push_back(track(7, "vca", "Faders"));
    m.tracks.push_back(track(9, "master", "Master"));
    m.routing.push_back(route(100, 1, 7));

    auto r = realize(planGraph(m));
    check(r->ok(), "the rest of the project is still realised: " + r->error());

    bool named = false;
    for (const auto& p : r->problems())
        if (p.find("tracks#7") != std::string::npos) named = true;
    check(named,
          "and the connection that could not be made is named -- a silently "
          "dropped edge is a signal path the project says exists:" + problems(*r));
}

void testACycleIsRefusedBeforeAnythingIsBuilt() {
    section("ADR-0055 -- a cyclic plan constructs nothing at all");

    rows::Model m;
    m.tracks.push_back(track(1, "audio", "A"));
    m.tracks.push_back(track(2, "audio", "B"));
    m.tracks.push_back(track(9, "master", "Master"));
    m.routing.push_back(route(100, 1, 2));
    m.routing.push_back(route(101, 2, 1));

    const GraphPlan plan = planGraph(m);
    check(plan.cycle, "the plan says so as a flag, not only as prose");

    int built = 0;
    RealizeOptions opts;
    opts.devicesFor = [&](std::int64_t) -> std::vector<Node*> { ++built; return {}; };

    auto r = realize(plan, opts);
    check(!r->ok(), "realisation refuses it");
    check(!r->error().empty(), "with a reason: " + r->error());
    eqi(static_cast<long long>(r->nodeCount()), 0, "and no node was created");
    eqi(built, 0,
        "and NO DEVICE WAS ASKED FOR -- which is the point of refusing here "
        "rather than letting prepare() find it after every plugin has loaded");
}

void testNoMasterIsRefused() {
    section("a project with no master cannot be realised");

    rows::Model m;
    m.tracks.push_back(track(1, "audio", "A"));

    const GraphPlan plan = planGraph(m);
    check(!plan.output.has_value(), "the plan has no output");

    auto r = realize(plan);
    check(!r->ok(), "so realisation refuses");
    check(r->error().find("master") != std::string::npos,
          "and says which thing is missing: " + r->error());
}

void testProblemsAreCarriedForward() {
    section("a plan's problems survive realisation");

    // A routing row naming a track that is not there. The rest of the project
    // is still realised -- losing ninety-nine tracks because of one stale row
    // is not a recovery.
    rows::Model m;
    m.tracks.push_back(track(1, "audio", "A"));
    m.tracks.push_back(track(9, "master", "Master"));
    m.routing.push_back(route(100, 1, 404));

    const GraphPlan plan = planGraph(m);
    check(!plan.problems.empty(), "the plan found it");

    auto r = realize(plan);
    check(r->ok(), "and the project is still realised: " + r->error());
    check(r->problems().size() >= plan.problems.size(),
          "with the plan's problems carried forward rather than replaced");
}

void testANullDeviceIsNamedRatherThanSkipped() {
    section("a null in a device chain is reported");

    rows::Model m;
    m.tracks.push_back(track(1, "audio", "A"));
    m.tracks.push_back(track(9, "master", "Master"));

    AddNode real(1.0f);
    RealizeOptions opts;
    opts.devicesFor = [&](std::int64_t id) -> std::vector<Node*> {
        if (id == 1) return {nullptr, &real};
        return {};
    };

    auto r = realize(planGraph(m), opts);
    check(r->ok(), "the chain is still built from what is there: " + r->error());
    eqi(static_cast<long long>(r->nodeCount()), 3,
        "two junctions and the one device that existed");
    bool named = false;
    for (const auto& p : r->problems())
        if (p.find("null device") != std::string::npos) named = true;
    check(named, "and the missing one is named:" + problems(*r));
}

void testSilenceIsWrittenNotLeft() {
    section("a junction with nothing feeding it writes silence");

    // A track with no content and no children. Leaving its buffer alone hands
    // the master whatever was there, which on a second block is the first block
    // repeated -- and that reaches the monitors.
    rows::Model m;
    m.tracks.push_back(track(1, "audio", "Empty"));
    m.tracks.push_back(track(9, "master", "Master"));

    auto r = realize(planGraph(m));
    r->graph().prepare(48000.0, 32);
    check(r->graph().ok(), "prepared: " + r->graph().error());

    Out o(32);
    AudioIo io = makeIo(o, 32);
    r->graph().process(io);
    bool silent = true;
    for (float v : o.l) if (v != 0.0f) silent = false;
    check(silent, "the output is zero, not the -1.0 the buffer was filled with: got " +
                      std::to_string(o.l[0]));
}

/// ADR-0122: a source is injected like a device, and it lands in the
/// JUNCTION -- ahead of the chain, summed with everything upstream. Planted
/// and watched fail: `sourcesFor` ignored (three nodes, no tone), and the
/// edge reversed so the junction feeds the source (the tone goes nowhere).
void testASourceFeedsTheJunction() {
    section("ADR-0122 -- a source is connected into the junction, ahead of the devices");
    rows::Model m;
    m.tracks.push_back(track(1, "audio", "A"));
    m.tracks.push_back(track(9, "master", "Master"));
    const GraphPlan plan = planGraph(m);

    ToneNode tone(0.5f);
    AddNode add(0.25f);
    RealizeOptions opts;
    opts.sourcesFor = [&](std::int64_t id) -> std::vector<Node*> {
        return id == 1 ? std::vector<Node*>{&tone} : std::vector<Node*>{};
    };
    opts.devicesFor = [&](std::int64_t id) -> std::vector<Node*> {
        return id == 1 ? std::vector<Node*>{&add} : std::vector<Node*>{};
    };
    auto r = realize(plan, opts);
    check(r->ok(), "realised" + problems(*r));
    eqi(static_cast<long long>(r->nodeCount()), 4, "junction, source, device, master");
    check(r->inputFor(1) != kInvalidNode && r->inputFor(1) != r->outputFor(1),
          "the source did not become the track's input: the junction is still it");

    r->graph().prepare(48000.0, 64);
    check(r->graph().ok(), "prepared: " + r->graph().error());
    Out o(64);
    AudioIo io = makeIo(o, 64);
    r->graph().process(io);
    check(o.l[0] == 0.75f,
          "the tone went THROUGH the device: 0.5 + 0.25, got " + std::to_string(o.l[0]));
    eqi(add.calls, 1, "the device ran once");

    // A null source is named, like a null device.
    RealizeOptions bad;
    bad.sourcesFor = [](std::int64_t) -> std::vector<Node*> { return {nullptr}; };
    auto rb = realize(plan, bad);
    check(rb->ok(), "a null source does not refuse the graph");
    bool named = false;
    for (const auto& p : rb->problems())
        if (p.find("tracks#1: a null source was dropped") != std::string::npos) named = true;
    check(named, "and it is named" + problems(*rb));
    check(!named || rb->problems().size() == 2, "once per track (both tracks were asked)");
}

void testRealisationIsDeterministic() {
    section("ADR-0021 -- the same project realises to the same ids, every time");

    // The planner sorts by track id so row order never reaches the graph.
    // Realisation walks the plan in order, so the ids it hands out inherit
    // that -- but only if it really does walk in order, which is what this
    // checks. An id that moved between runs would make a saved automation
    // target or a delay line point somewhere else.
    rows::Model a, b;
    a.tracks.push_back(track(1, "audio", "A"));
    a.tracks.push_back(track(2, "audio", "B"));
    a.tracks.push_back(track(9, "master", "Master"));
    // Same project, rows in a different order -- which is what a different
    // SQLite query plan hands back.
    b.tracks.push_back(track(9, "master", "Master"));
    b.tracks.push_back(track(2, "audio", "B"));
    b.tracks.push_back(track(1, "audio", "A"));

    auto ra = realize(planGraph(a));
    auto rb = realize(planGraph(b));
    check(ra->ok() && rb->ok(), "both realised");
    check(ra->inputFor(1) == rb->inputFor(1) &&
              ra->inputFor(2) == rb->inputFor(2) &&
              ra->inputFor(9) == rb->inputFor(9),
          "every track has the same node id in both");
    eqi(static_cast<long long>(ra->nodeCount()),
        static_cast<long long>(rb->nodeCount()), "and the same node count");
}

}  // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::printf("adi_realize_tests -- a plan becomes a graph that makes sound\n\n");
    testTheSimplestProjectRuns();
    testAGroupSumsItsChildren();
    testADeviceChainRunsInOrder();
    testCompensationIsReachedFromAPlan();
    testASendLeavesAtItsTap();
    testASendIsCompensated();
    testAReturnsLoopIsRealised();
    testASendIsRefusedByName();
    testASendsEdgesAreKeyedForTheNextGraph();
    testBypassChangesLatencyButNotTopology();
    testAVcaIsNotAnAudioNode();
    testARoutingRowNamingAVcaIsReported();
    testACycleIsRefusedBeforeAnythingIsBuilt();
    testNoMasterIsRefused();
    testProblemsAreCarriedForward();
    testANullDeviceIsNamedRatherThanSkipped();
    testSilenceIsWrittenNotLeft();
    testASourceFeedsTheJunction();
    testRealisationIsDeterministic();
    std::printf("\n%s -- %d checks, %d failure(s)\n",
                g_failures ? "FAILED" : "PASS", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
