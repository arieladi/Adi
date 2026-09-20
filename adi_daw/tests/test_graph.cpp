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

}  // namespace

int main() {
    // Unbuffered: a crash must not take its own diagnosis with it.
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::printf("adi_graph_tests -- the audio graph\n\n");
    try {
        testTopologicalOrder();
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
    } catch (const std::exception& e) {
        std::printf("\nFAILED -- exception escaped: %s\n", e.what());
        return 1;
    }
    std::printf("\n%s -- %d checks, %d failure(s)\n",
                g_failures ? "FAILED" : "PASS", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
