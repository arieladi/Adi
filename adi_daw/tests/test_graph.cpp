// SPDX-License-Identifier: GPL-3.0-or-later
//
// Tests for the audio graph — src/adi/engine/graph.{hpp,cpp}.
//
// Five ADRs meet in that file and each of them has a check here that FAILS
// when the decision is removed, not merely one that passes while it is
// present. The ramp test is ADR-0042's own: without sub-block splitting the
// output is a staircase, and the check is written so the staircase is what
// trips it.
//
// No JUCE, no device, no sound card. The graph is driven by a synthetic
// caller, which is the property ADR-0036 bought and ADR-0045 extends.

#include "adi/engine/graph.hpp"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

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

// --- fixtures ---------------------------------------------------------------

/// Writes a constant, so a downstream node has something to work on and the
/// silence machinery has something to stop.
class ConstNode final : public Node {
public:
    explicit ConstNode(float v) : v_(v) {}
    void process(const NodeIo& io) noexcept override {
        for (std::int32_t c = 0; c < io.channels; ++c) {
            float* o = io.out[c] + io.blockOffset;
            for (std::int32_t i = 0; i < io.frames; ++i) o[i] = v_;
        }
        ++calls;
    }
    void set(float v) noexcept { v_ = v; }
    // NO tailSamples() override. A generator produces whether or not anything
    // drives it, so it inherits the base class's conservative kInfiniteTail.
    // Declaring 0 here told the scheduler "I stop when nothing drives me" and
    // it obeyed, which is the design working and the fixture lying.
    [[nodiscard]] const char* name() const noexcept override { return "const"; }
    int calls = 0;
private:
    float v_ = 0.0f;
};

/// Declares a tail and writes nothing, so "was it called" is the only question.
class TailNode final : public Node {
public:
    explicit TailNode(std::int64_t tail) : tail_(tail) {}
    void process(const NodeIo& io) noexcept override {
        for (std::int32_t c = 0; c < io.channels; ++c)
            for (std::int32_t i = 0; i < io.frames; ++i)
                io.out[c][io.blockOffset + i] = 0.0f;
        ++calls;
    }
    [[nodiscard]] std::int64_t tailSamples() const noexcept override { return tail_; }
    [[nodiscard]] bool alwaysProcess() const noexcept override { return always_; }
    [[nodiscard]] const char* name() const noexcept override { return "tail"; }
    void setAlways(bool v) noexcept { always_ = v; }
    int calls = 0;
private:
    std::int64_t tail_ = 0;
    bool always_ = false;
};

/// Records the order nodes ran in, so a topological claim is observed.
class OrderNode final : public Node {
public:
    OrderNode(std::vector<int>& log, int id) : log_(&log), id_(id) {}
    void process(const NodeIo& io) noexcept override {
        if (io.blockOffset == 0) log_->push_back(id_);
        for (std::int32_t c = 0; c < io.channels; ++c)
            for (std::int32_t i = 0; i < io.frames; ++i)
                io.out[c][io.blockOffset + i] = 1.0f;
    }
    // A generator, so it inherits kInfiniteTail like ConstNode does. Declaring
    // 0 here says "I stop when nothing drives me", and a source has nothing
    // driving it -- so it was suspended on block 1 and logged nothing.
private:
    std::vector<int>* log_;
    int id_;
};

struct Out {
    std::vector<float> l, r;
    std::vector<float*> ptrs;
    explicit Out(std::int32_t n) : l(static_cast<std::size_t>(n), 0.0f),
                                   r(static_cast<std::size_t>(n), 0.0f) {
        ptrs = {l.data(), r.data()};
    }
    float* const* out() { return ptrs.data(); }
};

AudioIo makeIo(Out& o, std::int32_t frames, std::int64_t t = 0) {
    AudioIo io;
    io.out = o.out();
    io.numOut = 2;
    io.frames = frames;
    io.streamTimeSamples = t;
    return io;
}

// ===========================================================================

void testTopologicalOrder() {
    section("a diamond runs in dependency order, deterministically");

    std::vector<int> log;
    OrderNode src(log, 0), a(log, 1), b(log, 2);
    SumNode sink;

    Graph g;
    const NodeId ns = g.addNode(src), na = g.addNode(a), nb = g.addNode(b);
    const NodeId nk = g.addNode(sink);
    g.connect(ns, na);
    g.connect(ns, nb);
    g.connect(na, nk);
    g.connect(nb, nk);
    g.setOutput(nk);
    g.prepare(48000.0, 256);
    check(g.ok(), "the diamond prepares: " + g.error());

    Out o(256);
    AudioIo io = makeIo(o, 256);
    g.process(io);

    check(log.size() == 3, "three ordered nodes ran");
    check(!log.empty() && log[0] == 0, "the source ran first");
    check(log.size() == 3 && ((log[1] == 1 && log[2] == 2) || (log[1] == 2 && log[2] == 1)),
          "then both middles, before the sink");

    // Both branches write 1.0 and the sink sums, so the output says whether
    // summing actually happened rather than one branch winning.
    check(std::fabs(o.l[0] - 2.0f) < 1e-6f,
          "two branches summed into the sink: got " + std::to_string(o.l[0]));
}

void testSinkNodeRunsWithoutFeedingTheOutput() {
    section("ADR-0074 -- a node may consume audio and produce nothing downstream");

    // A broadcast tap, a meter, a recorder: all the same shape -- audio in, no
    // edge out. ADR-0074 needs this and the mandate behind it asked whether
    // the graph supports it. It does, and this is the check that says so
    // rather than the ADR asserting it.
    //
    // It works because `process` walks `levels_`, which `topoSort` fills from
    // EVERY node, and `prepare` refuses only a cycle -- never an unreachable
    // node. That is a property worth pinning: a future reachability check
    // added to prune dead nodes would silently stop every tap in the project.

    std::vector<int> log;
    OrderNode src(log, 0), tap(log, 1);
    SumNode master;

    Graph g;
    const NodeId ns = g.addNode(src);
    const NodeId nt = g.addNode(tap);
    const NodeId nm = g.addNode(master);
    g.connect(ns, nm);
    g.connect(ns, nt);      // the tap consumes, and feeds nothing
    g.setOutput(nm);
    g.prepare(48000.0, 256);
    check(g.ok(), "a graph with a sink node prepares: " + g.error());

    Out o(256);
    AudioIo io = makeIo(o, 256);
    g.process(io);

    check(log.size() == 2, "both the source and the tap ran, saw " +
                           std::to_string(log.size()));
    bool tapRan = false;
    for (int id : log) if (id == 1) tapRan = true;
    check(tapRan, "the tap ran even though nothing consumes it");

    // And it did not disturb the master. A tap that changed the mix would be
    // worse than one that did not run.
    check(std::fabs(o.l[0] - 1.0f) < 1e-6f,
          "the master output is unchanged by the tap: got " + std::to_string(o.l[0]));

    // The tap is in the levelled schedule too, so a future thread pool runs it
    // like any other node rather than forgetting it (ADR-0056).
    std::size_t inLevels = 0;
    for (const auto& lvl : g.levels()) inLevels += lvl.size();
    check(inLevels == 3, "all three nodes are in the levelled schedule, saw " +
                         std::to_string(inLevels));
}

void testCycleIsRefused() {
    section("a feedback loop is refused, not run and not hung");

    SumNode a, b;
    Graph g;
    const NodeId na = g.addNode(a), nb = g.addNode(b);
    g.connect(na, nb);
    g.connect(nb, na);
    g.setOutput(nb);
    g.prepare(48000.0, 256);

    check(!g.ok(), "prepare refuses a cycle");
    check(g.error().find("cycle") != std::string::npos,
          "and says so: " + g.error());

    // A refused graph must still be safe to call: a device callback does not
    // ask whether prepare succeeded before the driver starts.
    Out o(256);
    for (auto& v : o.l) v = 0.5f;
    AudioIo io = makeIo(o, 256);
    g.process(io);
    check(o.l[0] == 0.0f, "and a refused graph outputs silence rather than garbage");
}

// --- ADR-0042's own test ----------------------------------------------------

void testRampIsNotAStaircase() {
    section("ADR-0042 -- a ramp across one block is a ramp, not a staircase");

    ConstNode one(1.0f);
    GainNode gain(7);
    Graph g;
    const NodeId nc = g.addNode(one), ng = g.addNode(gain);
    g.connect(nc, ng);
    g.setOutput(ng);
    g.prepare(48000.0, 4096);
    check(g.ok(), "prepared: " + g.error());
    check(g.floorFrames() <= 96,
          "the floor respects ADR-0054's bound at 48 kHz: " +
              std::to_string(g.floorFrames()));

    // A gain automation sweep across one 4096-frame block, at 500 Hz -- the
    // Continuum's rate, so this is ADR-0042 and ADR-0054 exercised together.
    // 4096 frames at 48 kHz is 85.3 ms, which is 42 updates.
    const std::int32_t step = 96;                     // 48000/500
    for (std::int32_t f = 0; f < 4096; f += step) {
        Event e;
        e.frame = f;
        e.type = EventType::ParamValue;
        e.paramId = 7;
        e.value = static_cast<double>(f) / 4096.0;    // 0 .. ~1
        g.pushInputEvent(ng, e);
    }

    Out o(4096);
    AudioIo io = makeIo(o, 4096);
    g.process(io);

    // How many distinct output levels are there? A staircase at block
    // resolution has ONE. A correct ramp has one per segment.
    int distinct = 1;
    for (std::size_t i = 1; i < o.l.size(); ++i)
        if (o.l[i] != o.l[i - 1]) ++distinct;

    check(distinct >= 40,
          "the block was split into steps that follow the automation: " +
              std::to_string(distinct) + " distinct levels (a staircase has 1)");
    check(o.l[0] < 0.01f, "it starts near zero");
    check(o.l[4095] > 0.9f, "and ends near one");

    // Monotonic, because the ramp is. A split in the wrong place would show up
    // as a value going backwards.
    bool monotonic = true;
    for (std::size_t i = 1; i < o.l.size(); ++i)
        if (o.l[i] < o.l[i - 1] - 1e-6f) monotonic = false;
    check(monotonic, "and never goes backwards");

    eqi(g.stats().blocks, 1, "one block");
    check(g.stats().segments >= 40,
          "segments: " + std::to_string(g.stats().segments));
}

void testFloorCoalesces() {
    section("ADR-0042 -- events closer than the floor coalesce");

    ConstNode one(1.0f);
    GainNode gain(1);
    Graph g;
    const NodeId nc = g.addNode(one), ng = g.addNode(gain);
    g.connect(nc, ng);
    g.setOutput(ng);
    g.prepare(48000.0, 1024);

    // Ten events one frame apart. Without a floor that is ten segments; with
    // a 96-frame floor they all land in the first, and the block is not shredded
    // by an instrument that decided to send faster than anyone can hear.
    for (std::int32_t f = 1; f <= 10; ++f) {
        Event e;
        e.frame = f;
        e.type = EventType::ParamValue;
        e.paramId = 1;
        e.value = 0.5;
        g.pushInputEvent(ng, e);
    }
    Out o(1024);
    AudioIo io = makeIo(o, 1024);
    g.process(io);

    check(g.stats().segments <= 2,
          "ten events inside one floor make at most two segments, got " +
              std::to_string(g.stats().segments));
}

void testFloorBound() {
    section("ADR-0054 -- the floor never exceeds sample_rate/500");

    eqi(Graph::maxFloorFor(48000.0), 96, "48 kHz allows 96");
    eqi(Graph::maxFloorFor(96000.0), 192, "96 kHz allows 192");
    eqi(Graph::maxFloorFor(192000.0), 384, "192 kHz allows 384");

    // A graph prepared at 48 kHz must clamp, whatever the default was, or a
    // 500 Hz MPE+ stream loses every second frame -- the mandate broken in
    // time rather than in bit depth.
    ConstNode one(1.0f);
    Graph g;
    const NodeId n = g.addNode(one);
    g.setOutput(n);
    g.prepare(48000.0, 4096);
    check(g.floorFrames() <= Graph::maxFloorFor(48000.0),
          "the prepared floor is within the bound: " + std::to_string(g.floorFrames()));
    check(g.floorFrames() >= 1, "and is positive");
}

// --- ADR-0043 ---------------------------------------------------------------

void testSilenceSuspends() {
    section("ADR-0043 -- a silent chain is skipped and its output stays silent");

    ConstNode src(0.0f);              // silent from the first block
    TailNode fx(0);                   // no tail
    Graph g;
    const NodeId ns = g.addNode(src), nf = g.addNode(fx);
    g.connect(ns, nf);
    g.setOutput(nf);
    g.prepare(48000.0, 256);

    Out o(256);
    AudioIo io = makeIo(o, 256);

    // LOUD FIRST. Asserting that something stopped, without it ever having
    // started, passes for the wrong reason -- which is what the first draft of
    // this test did.
    src.set(1.0f);
    for (int i = 0; i < 3; ++i) g.process(io);
    const int whileLoud = fx.calls;
    check(whileLoud == 3, "the effect runs while signal reaches it: " +
                              std::to_string(whileLoud));

    src.set(0.0f);
    g.process(io);                    // src writes zeros; now known silent
    const int afterFirstQuiet = fx.calls;
    for (int i = 0; i < 3; ++i) g.process(io);

    check(fx.calls == afterFirstQuiet,
          "and stops once its input has gone silent: " +
              std::to_string(fx.calls) + " calls");
    check(g.stats().nodesSuspended > 0,
          "and the scheduler counted the suspension");
}

void testEventsAreNotSilence() {
    section("ADR-0043 -- a node with a pending event runs, silent input or not");

    TailNode inst(0);                 // no audio input at all: an instrument
    Graph g;
    const NodeId ni = g.addNode(inst);
    g.setOutput(ni);
    g.prepare(48000.0, 256);

    Out o(256);
    AudioIo io = makeIo(o, 256);
    g.process(io);
    const int quiet = inst.calls;
    g.process(io);
    check(inst.calls == quiet, "with nothing to do it is suspended");

    Event e;
    e.frame = 0;
    e.type = EventType::NoteOn;
    e.noteId = 1;
    g.pushInputEvent(ni, e);
    g.process(io);
    check(inst.calls == quiet + 1,
          "but a note-on wakes it -- otherwise every synth in the project is "
          "suspended, which is the bug this feature ships once");
}

void testTailKeepsRunning() {
    section("ADR-0043 -- a tail is honoured, and an infinite one is never skipped");

    {
        ConstNode src(0.0f);
        TailNode verb(1000);          // 1000 samples of tail
        Graph g;
        const NodeId ns = g.addNode(src), nv = g.addNode(verb);
        g.connect(ns, nv);
        g.setOutput(nv);
        g.prepare(48000.0, 256);

        Out o(256);
        AudioIo io = makeIo(o, 256);
        src.set(1.0f);
        g.process(io);                       // signal reaches it, tail is armed
        const int atSilence = verb.calls;

        src.set(0.0f);
        for (int i = 0; i < 12; ++i) g.process(io);
        const int ran = verb.calls - atSilence;

        // 1000 samples of tail at 256 frames a block is four more blocks, and
        // the exact figure depends on when the counter arms. What matters is
        // that it is neither 0 (cut off, the complaint ADR-0043 exists to
        // prevent) nor 12 (never stops, and the feature does nothing).
        check(ran >= 3 && ran <= 6,
              "it ran on past the silence and then stopped: " +
                  std::to_string(ran) + " blocks of tail, expected about 4");
    }
    {
        ConstNode src(1.0f);
        TailNode delay(kInfiniteTail);
        Graph g;
        const NodeId ns = g.addNode(src), nd = g.addNode(delay);
        g.connect(ns, nd);
        g.setOutput(nd);
        g.prepare(48000.0, 256);

        Out o(256);
        AudioIo io = makeIo(o, 256);
        src.set(1.0f);
        g.process(io);
        src.set(0.0f);
        for (int i = 0; i < 9; ++i) g.process(io);
        eqi(delay.calls, 10,
            "kInfiniteTail is never suspended -- a feedback delay has no point "
            "at which it is safe to stop");
    }
}

void testAlwaysProcess() {
    section("ADR-0043 -- always_process overrides everything");

    ConstNode src(0.0f);
    TailNode liar(0);                 // claims no tail, and then lies about it
    liar.setAlways(true);
    Graph g;
    const NodeId ns = g.addNode(src), nl = g.addNode(liar);
    g.connect(ns, nl);
    g.setOutput(nl);
    g.prepare(48000.0, 256);

    Out o(256);
    AudioIo io = makeIo(o, 256);
    for (int i = 0; i < 6; ++i) g.process(io);
    eqi(liar.calls, 6,
        "the escape hatch keeps a plugin running that reports no tail and then "
        "produces one");
}

// --- ADR-0044 ---------------------------------------------------------------

void testGroupSumsAndPropagates() {
    section("ADR-0044 -- a group is a summing node, and silence climbs it");

    ConstNode a(0.25f), b(0.5f);
    SumNode group;
    Graph g;
    const NodeId na = g.addNode(a), nb = g.addNode(b), ng = g.addNode(group);
    g.connect(na, ng);
    g.connect(nb, ng);
    g.setOutput(ng);
    g.prepare(48000.0, 128);

    Out o(128);
    AudioIo io = makeIo(o, 128);
    g.process(io);
    check(std::fabs(o.l[0] - 0.75f) < 1e-6f,
          "children sum into the group: got " + std::to_string(o.l[0]));

    // Both children fall silent. The group has no code for this case -- it is
    // an ordinary node, so ADR-0043 reaches it for free, which is the whole
    // argument for making a group a node.
    a.set(0.0f);
    b.set(0.0f);
    g.process(io);
    const std::int64_t before = g.stats().nodesSuspended;
    g.process(io);
    g.process(io);
    check(g.stats().nodesSuspended > before,
          "and when they go quiet the group is suspended too, with no special "
          "case written for groups");
}

// --- ADR-0045 ---------------------------------------------------------------

void testPortCarriesBoth() {
    section("ADR-0045 -- a port is audio AND events, so hybrid needs no case");

    // One node receives a constant AND a note-on in the same block. Nothing in
    // the graph distinguishes an "audio track" from a "MIDI track", which is
    // the point: hybrid is the absence of a restriction.
    class Both final : public Node {
    public:
        void process(const NodeIo& io) noexcept override {
            sawAudio = io.in != nullptr && io.in[0] != nullptr &&
                       io.in[0][io.blockOffset] != 0.0f;
            for (const Event& e : io.events)
                if (e.type == EventType::NoteOn) ++notes;
            for (std::int32_t c = 0; c < io.channels; ++c)
                for (std::int32_t i = 0; i < io.frames; ++i)
                    io.out[c][io.blockOffset + i] = 1.0f;
        }
        [[nodiscard]] std::int64_t tailSamples() const noexcept override { return 0; }
        bool sawAudio = false;
        int notes = 0;
    } both;

    ConstNode src(0.5f);
    Graph g;
    const NodeId ns = g.addNode(src), nb = g.addNode(both);
    g.connect(ns, nb);
    g.setOutput(nb);
    g.prepare(48000.0, 256);

    Event e;
    e.frame = 0;
    e.type = EventType::NoteOn;
    e.noteId = 42;
    g.pushInputEvent(nb, e);

    Out o(256);
    AudioIo io = makeIo(o, 256);
    g.process(io);

    check(both.sawAudio, "audio arrived");
    eqi(both.notes, 1, "and a note arrived, on the same port, in the same block");
}

// --- ADR-0054 ---------------------------------------------------------------

void testExpressionKeepsItsPrecision() {
    section("ADR-0054 -- a 14-bit value survives the graph unrounded");

    class Capture final : public Node {
    public:
        void process(const NodeIo& io) noexcept override {
            for (const Event& e : io.events)
                if (e.type == EventType::NoteExpression) values.push_back(e.value);
            for (std::int32_t c = 0; c < io.channels; ++c)
                for (std::int32_t i = 0; i < io.frames; ++i)
                    io.out[c][io.blockOffset + i] = 0.0f;
        }
        [[nodiscard]] bool alwaysProcess() const noexcept override { return true; }
        std::vector<double> values;
    } cap;

    Graph g;
    const NodeId n = g.addNode(cap);
    g.setOutput(n);
    g.prepare(48000.0, 1024);

    // Two adjacent 14-bit codes. Anything that rounds to 7 bits collapses them
    // into one value, which is the mandate broken silently -- nothing errors,
    // the Continuum's extra bits simply stop arriving.
    const double a = 8191.0 / 16383.0;
    const double b = 8192.0 / 16383.0;
    for (double v : {a, b}) {
        Event e;
        e.frame = 0;
        e.type = EventType::NoteExpression;
        e.dim = 2;                       // timbre / Y
        e.noteId = 7;
        e.value = v;
        g.pushInputEvent(n, e);
    }

    Out o(1024);
    AudioIo io = makeIo(o, 1024);
    g.process(io);

    check(cap.values.size() == 2, "both arrived");
    check(cap.values.size() == 2 && cap.values[0] != cap.values[1],
          "and they are still distinct -- a 7-bit path would have merged them");
    check(cap.values.size() == 2 && std::fabs(cap.values[1] - b) < 1e-15,
          "at full double precision, not rounded in transit");
}

void testEventOverflowIsCounted() {
    section("events beyond capacity are counted, never silently dropped");

    ConstNode src(1.0f);
    Graph g;
    const NodeId n = g.addNode(src);
    g.setOutput(n);
    g.setEventCapacity(8);
    g.prepare(48000.0, 256);

    for (int i = 0; i < 20; ++i) {
        Event e;
        e.frame = 0;
        e.type = EventType::NoteOn;
        g.pushInputEvent(n, e);
    }
    Out o(256);
    AudioIo io = makeIo(o, 256);
    g.process(io);

    check(g.stats().eventsDropped == 12,
          "twelve of twenty were refused and counted: " +
              std::to_string(g.stats().eventsDropped));
}

void testShortAndVaryingBlocks() {
    section("a driver may hand over fewer frames than it prepared");

    ConstNode src(0.5f);
    GainNode gain(0);
    Graph g;
    const NodeId ns = g.addNode(src), ng = g.addNode(gain);
    g.connect(ns, ng);
    g.setOutput(ng);
    g.prepare(48000.0, 4096);

    // The buffer is bigger than any block this loop passes, so that reading
    // one frame PAST the block is still inside it. The first version of this
    // allocated exactly 4096 and then read index 4096 to prove nothing was
    // written past the end -- which is the same fixture bug I had just fixed
    // in mac's device test, written by me, two hours later. It hung rather
    // than crashed, which is worse.
    for (std::int32_t n : {4096, 1, 512, 4095, 4096, 64}) {
        Out o(8192);
        AudioIo io = makeIo(o, n);
        g.process(io);
        check(std::fabs(o.l[0] - 0.5f) < 1e-6f,
              "a " + std::to_string(n) + "-frame block is correct");
        check(o.l[static_cast<std::size_t>(n)] == 0.0f,
              "and nothing was written past it");
    }

    // More than prepared: refused, and silent, never processed.
    Out o(8192);
    AudioIo io = makeIo(o, 8192);
    g.process(io);
    check(o.l[0] == 0.0f, "an oversize block is refused and silenced");
}


// --- ADR-0056 ---------------------------------------------------------------

void testSidechainKeepsItAwake() {
    section("ADR-0043/0056 -- a live sidechain prevents suspension");

    ConstNode main(0.0f);              // main input silent
    ConstNode key(1.0f);               // key input playing
    TailNode comp(0);                  // no tail of its own

    Graph g;
    const NodeId nm = g.addNode(main), nk = g.addNode(key), nc = g.addNode(comp);
    g.connect(nm, nc, Bus::Main);
    g.connect(nk, nc, Bus::Sidechain);
    g.setOutput(nc);
    g.prepare(48000.0, 256);
    check(g.ok(), "prepared with two buses: " + g.error());

    Out o(256);
    AudioIo io = makeIo(o, 256);
    for (int i = 0; i < 5; ++i) g.process(io);

    eqi(comp.calls, 5,
        "a compressor whose key input is playing keeps working, however quiet "
        "its main input is -- suspending it would release the gain reduction");

    // And when the key stops too, it is suspended like anything else.
    key.set(0.0f);
    g.process(io);
    const int afterKeyStops = comp.calls;
    for (int i = 0; i < 3; ++i) g.process(io);
    eqi(comp.calls, afterKeyStops, "and stops once the key input stops as well");
}

void testSidechainIsSeparateFromMain() {
    section("ADR-0056 -- the two buses arrive separately, not summed together");

    class Probe final : public Node {
    public:
        void process(const NodeIo& io) noexcept override {
            sawMain = io.in != nullptr ? io.in[0][io.blockOffset] : -1.0f;
            sawSide = io.sidechain != nullptr ? io.sidechain[0][io.blockOffset] : -1.0f;
            mainSilent = io.inputSilent;
            sideSilent = io.sidechainSilent;
            for (std::int32_t c = 0; c < io.channels; ++c)
                for (std::int32_t i = 0; i < io.frames; ++i)
                    io.out[c][io.blockOffset + i] = 0.0f;
        }
        [[nodiscard]] bool alwaysProcess() const noexcept override { return true; }
        float sawMain = -1.0f, sawSide = -1.0f;
        bool mainSilent = false, sideSilent = false;
    } probe;

    ConstNode a(0.25f), b(0.75f);
    Graph g;
    const NodeId na = g.addNode(a), nb = g.addNode(b), np = g.addNode(probe);
    g.connect(na, np, Bus::Main);
    g.connect(nb, np, Bus::Sidechain);
    g.setOutput(np);
    g.prepare(48000.0, 128);

    Out o(128);
    AudioIo io = makeIo(o, 128);
    g.process(io);

    check(std::fabs(probe.sawMain - 0.25f) < 1e-6f,
          "main carries only the main input: " + std::to_string(probe.sawMain));
    check(std::fabs(probe.sawSide - 0.75f) < 1e-6f,
          "and the sidechain only the key: " + std::to_string(probe.sawSide));
    check(!probe.mainSilent && !probe.sideSilent, "both are reported live");
}

void testLevelOrderDoesNotChangeOutput() {
    section("ADR-0056 -- any order within a level gives identical bytes");

    // Four independent sources into a sum, then a gain. Levels are
    // {sources}, {sum}, {gain} -- so level 0 has four members whose order a
    // thread pool would not fix.
    ConstNode a(0.1f), b(0.2f), c(0.3f), d(0.4f);
    SumNode mix;
    GainNode gain(3);

    Graph g;
    const NodeId na = g.addNode(a), nb = g.addNode(b);
    const NodeId nc = g.addNode(c), nd = g.addNode(d);
    const NodeId nm = g.addNode(mix), ng = g.addNode(gain);
    for (NodeId src : {na, nb, nc, nd}) g.connect(src, nm);
    g.connect(nm, ng);
    g.setOutput(ng);
    g.prepare(48000.0, 512);
    check(g.ok(), "prepared: " + g.error());

    check(g.levels().size() == 3,
          "three dependency levels, got " + std::to_string(g.levels().size()));
    check(!g.levels().empty() && g.levels()[0].size() == 4,
          "four independent sources share level 0");

    Out forward(512);
    AudioIo io1 = makeIo(forward, 512);
    g.setReverseWithinLevel(false);
    g.process(io1);

    Out reverse(512);
    AudioIo io2 = makeIo(reverse, 512);
    g.setReverseWithinLevel(true);
    g.process(io2);

    // BYTE for byte, not within a tolerance. ADR-0021's oracle compares bytes,
    // and "close enough" is exactly the answer that lets a reordered float sum
    // through -- which is the one thing that would make a thread pool unsafe.
    bool identical = true;
    for (std::size_t i = 0; i < forward.l.size(); ++i)
        if (forward.l[i] != reverse.l[i] || forward.r[i] != reverse.r[i])
            identical = false;
    check(identical,
          "running level 0 backwards changes nothing: this is the property a "
          "thread pool would depend on, and it is why one is safe to add");
    check(std::fabs(forward.l[0] - 1.0f) < 1e-6f,
          "and the sum is right: " + std::to_string(forward.l[0]));
}

void testEventCapacityFitsTheContinuum() {
    section("ADR-0056 -- the derived capacity survives MPE+ at full polyphony");

    // The arithmetic the default has to clear: 500 Hz across a 4096-frame
    // block is 42.7 update frames, three dimensions each, per note. A fixed
    // 1024 drops from ten notes onwards -- on exactly the instrument ADR-0054
    // was written for, and silently apart from a counter nobody reads.
    const std::int32_t derived = Graph::deriveEventCapacity(48000.0, 4096, 16);
    check(derived >= 2048,
          "16 notes of MPE+ at 4096 frames needs >= 2048, derived " +
              std::to_string(derived));
    check(Graph::deriveEventCapacity(48000.0, 64, 16) >= 256,
          "and a 64-frame block still has room for a chord arriving at once");

    ConstNode src(1.0f);
    Graph g;
    const NodeId n = g.addNode(src);
    g.setOutput(n);
    g.setMaxPolyphony(16);
    g.prepare(48000.0, 4096);
    check(g.eventCapacity() == derived,
          "prepare uses the derived figure: " + std::to_string(g.eventCapacity()));

    // A full Continuum block: 16 notes x 3 dimensions x 42 update frames.
    int pushed = 0;
    for (int frame = 0; frame < 4032; frame += 96)
        for (int note = 0; note < 16; ++note)
            for (int dim = 0; dim < 3; ++dim) {
                Event e;
                e.frame = frame;
                e.type = EventType::NoteExpression;
                e.dim = static_cast<std::uint16_t>(dim);
                e.noteId = static_cast<std::uint64_t>(note);
                e.value = 0.5;
                if (g.pushInputEvent(n, e)) ++pushed;
            }

    Out o(4096);
    AudioIo io = makeIo(o, 4096);
    g.process(io);

    eqi(g.stats().eventsDropped, 0,
        "not one packet dropped at 16-note MPE+ polyphony (" +
            std::to_string(pushed) + " events in one block)");
}

// --- ADR-0058 decisions 2-5: plugin delay compensation ----------------------

/// Declares latency and passes audio through unchanged. The latency is a
/// CLAIM, not a delay the node performs -- which is exactly a plugin: it
/// reports what it costs and the host aligns everything else to it. If the node
/// also delayed, the test would measure the node rather than the compensation.
class LatentNode final : public Node {
public:
    explicit LatentNode(std::int32_t latency) : latency_(latency) {}

    void prepare(double, std::int32_t) override {
        line_.prepare(2, latency_);
        line_.setDelay(latency_);
    }

    void process(const NodeIo& io) noexcept override {
        // It REPORTS latency and it ALSO INCURS IT. The first version only
        // reported: it passed audio through instantly while claiming to be 64
        // samples late, so compensating the other path by 64 CREATED the
        // misalignment the test was checking for. A fixture that does not model
        // the thing turns a correct implementation into a failing test, which
        // is the most expensive kind of test bug -- it argues for changing
        // working code.
        const DelayLine::Cursors before = line_.cursors();
        for (std::int32_t c = 0; c < io.channels; ++c) {
            line_.setCursors(before);
            float* o = io.out[c] + io.blockOffset;
            const float* i = (io.in != nullptr && io.in[c] != nullptr)
                                 ? io.in[c] + io.blockOffset : nullptr;
            if (i == nullptr) {
                std::memset(o, 0, static_cast<std::size_t>(io.frames) * sizeof(float));
                continue;
            }
            line_.process(c, i, o, io.frames);
        }
    }

    [[nodiscard]] std::int32_t latencySamples() const noexcept override { return latency_; }
    [[nodiscard]] std::int64_t tailSamples() const noexcept override { return kInfiniteTail; }
    [[nodiscard]] const char* name() const noexcept override { return "latent"; }

private:
    std::int32_t latency_ = 0;
    DelayLine line_;
};

/// A ramp, so a misalignment is visible in the values rather than only in a sum.
class RampNode final : public Node {
public:
    void process(const NodeIo& io) noexcept override {
        for (std::int32_t c = 0; c < io.channels; ++c) {
            float* o = io.out[c] + io.blockOffset;
            for (std::int32_t i = 0; i < io.frames; ++i)
                o[i] = static_cast<float>(t_ + io.blockOffset + i);
        }
        if (io.blockOffset + io.frames > last_) last_ = io.blockOffset + io.frames;
    }
    void advance() noexcept { t_ += last_; last_ = 0; }
    [[nodiscard]] const char* name() const noexcept override { return "ramp"; }
private:
    std::int64_t t_ = 0;
    std::int32_t last_ = 0;
};

void testDelayLineItself() {
    section("ADR-0058 -- the delay line delays by exactly what it says");

    DelayLine d;
    d.prepare(1, 3);
    d.setDelay(3);
    std::vector<float> in{1, 2, 3, 4, 5, 6, 7, 8};
    std::vector<float> out(in.size(), -1.0f);
    d.process(0, in.data(), out.data(), static_cast<std::int32_t>(in.size()));

    check(out[0] == 0.0f && out[1] == 0.0f && out[2] == 0.0f,
          "three samples of history come out first");
    check(out[3] == 1.0f && out[4] == 2.0f && out[7] == 5.0f,
          "then the input, three late: got " + std::to_string(out[3]) + ", " +
              std::to_string(out[7]));

    // In place, because `accumulate` does exactly that on a second input.
    DelayLine e;
    e.prepare(1, 2);
    e.setDelay(2);
    std::vector<float> both{9, 8, 7, 6};
    e.process(0, both.data(), both.data(), 4);
    check(both[0] == 0.0f && both[2] == 9.0f && both[3] == 8.0f,
          "and src == dst is safe: the input is read before it is overwritten");

    DelayLine z;
    z.prepare(2, 0);
    z.setDelay(0);
    std::vector<float> pass{5, 6};
    z.process(0, pass.data(), pass.data(), 2);
    check(pass[0] == 5.0f, "zero delay is a pass-through, not a one-sample shift");
}

void testArrivalArithmetic() {
    section("ADR-0058 d2 -- arrival is the max over inputs, and the delay is the gap");

    RampNode src;
    LatentNode slow(128);
    SumNode direct, mix;

    Graph g;
    const NodeId ns = g.addNode(src);
    const NodeId nl = g.addNode(slow);
    const NodeId nd = g.addNode(direct);
    const NodeId nm = g.addNode(mix);
    g.connect(ns, nl);      // the long path: through 128 samples of declared latency
    g.connect(ns, nd);      // the short path
    g.connect(nl, nm);
    g.connect(nd, nm);
    g.setOutput(nm);
    g.prepare(48000.0, 256);
    check(g.ok(), "prepared: " + g.error());

    eqi(g.arrivalOf(ns), 0, "the source arrives at zero");
    eqi(g.arrivalOf(nl), 0, "and so does the node that merely CLAIMS latency");
    eqi(g.arrivalOf(nm), 128, "the sum waits for the later of its two feeds");

    eqi(g.compensationFor(nl, nm), 0, "the late path is not delayed further");
    eqi(g.compensationFor(nd, nm), 128,
        "the early path is delayed by the gap -- which is the whole of PDC");

    eqi(g.latencySamples(), 128, "and the graph reports what it costs");
}

void testPhaseAlignment() {
    section("ADR-0058 -- two paths to one sum, aligned to the sample, block after block");

    // The test ADR-0058 named. A ramp splits, one branch both declares AND
    // incurs latency, and both sum.
    //
    // THREE BLOCKS, NOT ONE, and 100 samples of latency rather than a round 64.
    // Both of those are load-bearing, and a one-block/64-sample version of this
    // test passed with the cursor carry completely broken:
    //
    //   - A delay ring is self-consistent WITHIN a call. Past the first `delay`
    //     samples, the output is read from what this same call wrote, whatever
    //     the cursor started at. Only the carry ACROSS a block boundary depends
    //     on each channel resuming where it actually left off.
    //   - 512 frames over a 64-sample ring wraps exactly eight times, so any
    //     cursor error is a whole number of ring sizes and lands back on
    //     itself. 512 % 100 is 12, so an error of one block shows as twelve.
    //
    // The comparison skips the graph's own reported latency, and that is the
    // property rather than a fudge: PDC guarantees the branches are aligned
    // WITH EACH OTHER, not that the graph is instantaneous. A graph containing
    // a 100-sample plugin genuinely produces nothing for 100 samples, reports
    // exactly that, and the transport compensates it -- which is what
    // `latencySamples()` is for. Comparing from sample 0 would be asserting
    // that latency does not exist.
    //
    // If the compensation is wrong the branches are apart, and on a ramp that
    // is a constant offset in the sum: easy to see, impossible to argue with.
    constexpr std::int32_t kBlock = 512;
    constexpr int kBlocks = 3;

    auto run = [](std::int32_t latency, std::vector<float>& out,
                  bool& stereoMatches, std::int32_t& reported) {
        RampNode src;
        LatentNode branch(latency);
        SumNode direct, mix;
        Graph g;
        const NodeId ns = g.addNode(src), nb = g.addNode(branch);
        const NodeId nd = g.addNode(direct), nm = g.addNode(mix);
        g.connect(ns, nb);
        g.connect(ns, nd);
        g.connect(nb, nm);
        g.connect(nd, nm);
        g.setOutput(nm);
        g.prepare(48000.0, kBlock);
        reported = g.latencySamples();

        out.clear();
        stereoMatches = true;
        for (int b = 0; b < kBlocks; ++b) {
            Out o(kBlock);
            AudioIo io = makeIo(o, kBlock, static_cast<std::int64_t>(b) * kBlock);
            g.process(io);
            src.advance();
            for (std::size_t i = 0; i < o.l.size(); ++i)
                if (o.l[i] != o.r[i]) stereoMatches = false;
            out.insert(out.end(), o.l.begin(), o.l.end());
        }
    };

    std::vector<float> withLatency, without;
    bool stereoA = false, stereoB = false;
    std::int32_t reported = 0, none = 0;
    run(100, withLatency, stereoA, reported);
    run(0, without, stereoB, none);

    eqi(reported, 100, "the graph reports what it costs");
    eqi(none, 0, "and reports nothing when nothing costs anything");

    bool aligned = true;
    std::size_t firstDiff = 0;
    for (std::size_t i = 0; i + 100 < withLatency.size(); ++i)
        if (withLatency[i + 100] != without[i]) {
            aligned = false;
            firstDiff = i;
            break;
        }

    check(aligned,
          "past the reported latency the sum is bit-identical to the graph with "
          "no latency at all, across every block" +
              (aligned ? std::string()
                       : " -- first difference at sample " +
                             std::to_string(firstDiff) + " (block " +
                             std::to_string(firstDiff / kBlock) + "): " +
                             std::to_string(withLatency[firstDiff + 100]) + " vs " +
                             std::to_string(without[firstDiff])));

    // And it really is the doubled ramp rather than two zeroes agreeing.
    check(without[600] == 1200.0f,
          "the reference really is 2x the ramp, past the first block: got " +
              std::to_string(without[600]));

    check(stereoA && stereoB,
          "both channels of the edge carry the same signal -- this holds even "
          "with the cursor carry broken, because every channel drifts by the "
          "same (channels-1)*frames, so it is a symmetry check and NOT a test "
          "of the per-channel cursor");
}

void testSidechainIsCompensatedToo() {
    section("ADR-0058 d5 -- a sidechain is aligned with the audio it controls");

    RampNode src;
    LatentNode slowKey(96);
    SumNode chain, comp;

    Graph g;
    const NodeId ns = g.addNode(src);
    const NodeId nk = g.addNode(slowKey);
    const NodeId nc = g.addNode(chain);
    const NodeId nx = g.addNode(comp);
    g.connect(ns, nk);
    g.connect(ns, nc);
    g.connect(nc, nx, Bus::Main);
    g.connect(nk, nx, Bus::Sidechain);
    g.setOutput(nx);
    g.prepare(48000.0, 256);
    check(g.ok(), "prepared with a latent key: " + g.error());

    eqi(g.arrivalOf(nx), 96, "the compressor waits for its key");
    eqi(g.compensationFor(nc, nx, Bus::Main), 96,
        "the main input is delayed to meet it -- a key that arrives early ducks "
        "early, and that is the same defect as a misaligned kick");
    eqi(g.compensationFor(nk, nx, Bus::Sidechain), 0, "the key itself is not delayed");
}

void testGroupsCompensateAsOne() {
    section("ADR-0058 d3 -- a group aligns its children, then itself");

    // Two children into a group, one of them latent; the group and a bare track
    // into the master. The rule is applied at both levels by the same code,
    // which is the point -- nothing here knows what a group is.
    RampNode a, b, c;
    LatentNode latent(32);
    SumNode group, master;

    Graph g;
    const NodeId na = g.addNode(a), nb = g.addNode(b), nc = g.addNode(c);
    const NodeId nl = g.addNode(latent);
    const NodeId ng = g.addNode(group), nm = g.addNode(master);
    g.connect(na, nl);        // child A through 32 samples
    g.connect(nl, ng);
    g.connect(nb, ng);        // child B direct
    g.connect(ng, nm);        // the group into the master
    g.connect(nc, nm);        // and a bare track alongside it
    g.setOutput(nm);
    g.prepare(48000.0, 256);
    check(g.ok(), "prepared: " + g.error());

    eqi(g.arrivalOf(ng), 32, "inside the group, B waits for A");
    eqi(g.compensationFor(nb, ng), 32, "so B is delayed by 32");
    eqi(g.arrivalOf(nm), 32, "and the master waits for the group");
    eqi(g.compensationFor(nc, nm), 32,
        "so the bare track is delayed by the same 32 -- the group's latency "
        "propagated upward with no code that knows what a group is");
    eqi(g.compensationFor(ng, nm), 0, "and the group itself is not delayed twice");
}

void testNoLatencyMeansNoDelayLines() {
    section("ADR-0058 -- the common case costs nothing");

    RampNode src;
    SumNode a, b, out;
    Graph g;
    const NodeId ns = g.addNode(src), na = g.addNode(a);
    const NodeId nb = g.addNode(b), no = g.addNode(out);
    g.connect(ns, na);
    g.connect(ns, nb);
    g.connect(na, no);
    g.connect(nb, no);
    g.setOutput(no);
    g.prepare(48000.0, 256);

    eqi(g.latencySamples(), 0, "a project with no latency reports none");
    eqi(g.compensationFor(na, no), 0, "and inserts no delay");
    eqi(g.compensationFor(nb, no), 0, "on either edge");
}

}  // namespace

int main() {
    // Unbuffered: a crash must not take its own diagnosis with it.
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::printf("adi_graph_tests -- the audio graph\n\n");
    try {
        testTopologicalOrder();
        testSinkNodeRunsWithoutFeedingTheOutput();
        testCycleIsRefused();
        testRampIsNotAStaircase();
        testFloorCoalesces();
        testFloorBound();
        testSilenceSuspends();
        testEventsAreNotSilence();
        testTailKeepsRunning();
        testAlwaysProcess();
        testGroupSumsAndPropagates();
        testPortCarriesBoth();
        testExpressionKeepsItsPrecision();
        testEventOverflowIsCounted();
        testShortAndVaryingBlocks();
        testSidechainKeepsItAwake();
        testSidechainIsSeparateFromMain();
        testLevelOrderDoesNotChangeOutput();
        testEventCapacityFitsTheContinuum();
        testDelayLineItself();
        testArrivalArithmetic();
        testPhaseAlignment();
        testSidechainIsCompensatedToo();
        testGroupsCompensateAsOne();
        testNoLatencyMeansNoDelayLines();
    } catch (const std::exception& e) {
        std::printf("\nFAILED -- exception escaped: %s\n", e.what());
        return 1;
    }
    std::printf("\n%s -- %d checks, %d failure(s)\n",
                g_failures ? "FAILED" : "PASS", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
