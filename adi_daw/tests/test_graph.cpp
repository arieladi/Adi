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

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
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

void testSuspendedNodeClearsOnceToCapacity() {
    section("ADR-0043 / ADR-0102 d5 -- a sleeping node clears its buffer once, "
            "to capacity, and costs nothing per block after that");

    // A node that copies its input: the one kind whose buffer can hold stale
    // audio when it falls asleep. TailNode writes zeros and could never leak.
    class PassNode final : public Node {
    public:
        void process(const NodeIo& io) noexcept override {
            for (std::int32_t c = 0; c < io.channels; ++c) {
                const float* in = io.in != nullptr ? io.in[c] + io.blockOffset : nullptr;
                float* o = io.out[c] + io.blockOffset;
                for (std::int32_t i = 0; i < io.frames; ++i) o[i] = in ? in[i] : 0.0f;
            }
        }
        [[nodiscard]] std::int64_t tailSamples() const noexcept override { return 0; }
        [[nodiscard]] const char* name() const noexcept override { return "pass"; }
    };

    ConstNode src(1.0f);
    PassNode pass;
    Graph g;
    const NodeId ns = g.addNode(src), np = g.addNode(pass);
    g.connect(ns, np);
    g.setOutput(np);
    g.prepare(48000.0, 256);

    Out o(256);
    AudioIo big = makeIo(o, 256);
    AudioIo small = makeIo(o, 64);

    auto allZero = [&](std::int32_t frames) {
        for (std::int32_t i = 0; i < frames; ++i)
            if (o.l[static_cast<std::size_t>(i)] != 0.0f || o.r[static_cast<std::size_t>(i)] != 0.0f)
                return false;
        return true;
    };

    // Loud at 256: pass's whole buffer holds ones.
    for (int i = 0; i < 2; ++i) g.process(big);
    check(!allZero(256), "loud first, so the buffer genuinely holds audio");

    // Quiet at 64: pass falls asleep on a SHORT block. A clear sized to that
    // block leaves ones at [64, 256) -- which the next long block would read.
    src.set(0.0f);
    g.process(small);
    const std::int64_t clearsAfterSleep = g.stats().suspendClears;
    check(clearsAfterSleep >= 1, "falling asleep cleared the buffer: " +
                                     std::to_string(clearsAfterSleep));

    g.process(big);
    check(allZero(256), "a longer block after a short sleep reads zeros to the "
                        "end -- the clear was to capacity, not to that block");

    // Asleep for a while: no further clears. This is the cost that scaled with
    // frames times sleeping nodes; a plant that clears every block fails here.
    for (int i = 0; i < 5; ++i) g.process(big);
    check(g.stats().suspendClears == clearsAfterSleep,
          "and five more silent blocks cleared nothing: " +
              std::to_string(g.stats().suspendClears));
    check(g.stats().nodesSuspended >= 6, "while the node stayed suspended");

    // Wake, then sleep again: the flag was dropped when the node wrote, so the
    // second sleep clears again. A plant that never drops the flag leaks the
    // ones from the second loud stretch.
    src.set(1.0f);
    for (int i = 0; i < 2; ++i) g.process(big);
    check(!allZero(256), "awake again, audio flows");
    src.set(0.0f);
    g.process(small);
    g.process(big);
    check(allZero(256), "asleep again, zeros to the end again");
    check(g.stats().suspendClears == clearsAfterSleep + 1,
          "which took exactly one more clear: " +
              std::to_string(g.stats().suspendClears));
}

void testMixSkipsSleepingSources() {
    section("ADR-0102 d5 -- a mix does not read a sleeping source, and reads "
            "everything that is merely quiet");

    // Generators inherit kInfiniteTail and never sleep; a pass-through behind
    // one of them is the node that actually suspends. The mixer is a
    // pass-through too, so the output IS the mix the graph handed it.
    class PassNode final : public Node {
    public:
        void process(const NodeIo& io) noexcept override {
            for (std::int32_t c = 0; c < io.channels; ++c) {
                const float* in = io.in != nullptr ? io.in[c] + io.blockOffset : nullptr;
                float* o = io.out[c] + io.blockOffset;
                for (std::int32_t i = 0; i < io.frames; ++i) o[i] = in ? in[i] : 0.0f;
            }
        }
        [[nodiscard]] std::int64_t tailSamples() const noexcept override { return 0; }
        [[nodiscard]] bool alwaysProcess() const noexcept override { return always; }
        [[nodiscard]] const char* name() const noexcept override { return "pass"; }
        bool always = false;
    };
    auto bits = [](float f) { std::uint32_t u; std::memcpy(&u, &f, sizeof u); return u; };
    Out o(128);
    AudioIo io = makeIo(o, 128);

    // --- 1. three inputs: a generator, the sleeper, a live generator ---------
    {
        ConstNode a(0.25f), b(0.5f), c(0.125f);
        PassNode sleeper, mixer;
        Graph g;
        const NodeId na = g.addNode(a), nb = g.addNode(b), nc = g.addNode(c),
                     ns = g.addNode(sleeper), nm = g.addNode(mixer);
        // The sleeper is the FIRST input on purpose: a skipped first input must
        // still zero the mix, or the previous block's mix is summed into this one.
        g.connect(na, ns);
        g.connect(ns, nm);          // first: the sleeper, fed by a
        g.connect(nc, nm);          // second: live
        g.connect(nb, nm);          // third: live
        g.setOutput(nm);
        g.prepare(48000.0, 128);

        g.process(io);
        check(o.l[0] == 0.875f && o.r[127] == 0.875f, "awake: the mix is the sum of all three");
        check(g.stats().inputsSkipped == 0, "and nothing was skipped");

        a.set(0.0f);
        g.process(io);              // the sleeper runs once more, on zeros
        g.process(io);              // and is now asleep and cleared
        const std::int64_t skipped = g.stats().inputsSkipped;
        check(skipped >= 1, "asleep: the mix skipped the sleeping input: " + std::to_string(skipped));
        check(o.l[0] == 0.625f && o.r[127] == 0.625f,
              "and the mix is exactly the live inputs: " + std::to_string(o.l[0]));
        for (int i = 0; i < 4; ++i) g.process(io);
        check(g.stats().inputsSkipped == skipped + 4, "one skip per block while it sleeps");

        a.set(0.25f);
        g.process(io);
        g.process(io);
        check(o.l[0] == 0.875f, "the sleeper woke and its audio is summed again");
    }

    // --- 2. quiet is not asleep: a -0.0f generator is READ, not skipped -----
    //
    // The scheduler measures -0.0f as silence (it compares with != 0.0f) but
    // a generator never sleeps, so it is never `zeroed` and must be read. With
    // only it and a sleeping input feeding the mix, its sign reaches the
    // output. This also pins the one visible difference the skip makes: the
    // old code added the sleeper's +0.0f and produced +0.0f here.
    {
        ConstNode z(-0.0f), a(0.25f);
        PassNode sleeper, mixer;
        // Every input of the mixer is 'silent' once the sleeper sleeps, so the
        // scheduler would suspend the MIXER too -- correctly. Keep it awake so
        // that what it is handed can be observed.
        mixer.always = true;
        Graph g;
        const NodeId nz = g.addNode(z), na = g.addNode(a), ns = g.addNode(sleeper),
                     nm = g.addNode(mixer);
        g.connect(nz, nm);          // first: -0.0f
        g.connect(na, ns);
        g.connect(ns, nm);          // second: the sleeper
        g.setOutput(nm);
        g.prepare(48000.0, 128);

        g.process(io);
        check(o.l[0] == 0.25f, "awake: -0.0 + 0.25");
        a.set(0.0f);
        for (int i = 0; i < 3; ++i) g.process(io);
        check(g.stats().inputsSkipped >= 1, "the sleeper is skipped");
        check(bits(o.l[0]) == 0x80000000u && bits(o.r[127]) == 0x80000000u,
              "and the -0.0f generator was read, not skipped: its sign survives "
              "(bits " + std::to_string(bits(o.l[0])) + ")");
    }
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

        // EXACTLY ceil(1000 / 256) = 4 blocks. This used to accept "3 to 6",
        // with a comment that the figure "depends on when the counter arms" --
        // and 3 was the bug: the counter was decremented BEFORE the decision,
        // so the block in which the last 232 samples of tail should play was
        // itself skipped. A tolerance wide enough to be safe was wide enough
        // to hide a defect. The tail is judged at the START of a block now.
        eqi(ran, 4, "a 1000-sample tail runs exactly four 256-frame blocks");
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

// The routing fixtures below use the same traversal matrix and byte oracle.
bool sidechainTraversalIsDeterministic();
bool eventTraversalIsDeterministic();

bool sameSamples(const std::vector<float>& a, const std::vector<float>& b) {
    return a.size() == b.size() &&
           std::memcmp(a.data(), b.data(), a.size() * sizeof(float)) == 0;
}

bool sameOutput(const Out& a, const Out& b) {
    return sameSamples(a.l, b.l) && sameSamples(a.r, b.r);
}

void selectTraversal(Graph& g, int variant) {
    // Called only after prepare, before any callback; each render owns a new graph.
    g.setReverseWithinLevel(variant == 1);
    if (variant >= 2) {
        constexpr std::uint32_t seeds[] = {1u, 42u, 0xC0FFEEu};
        g.permuteLevelsForTest(seeds[variant - 2]);
    }
}

void testLevelOrderDoesNotChangeOutput() {
    section("ADR-0056 -- forward, reverse and seeded levels give identical bytes");

    // Multiple independent nodes at THREE levels: sources, sums and gains.
    // Consumer input order stays fixed while traversal changes. The last sum
    // makes every branch observable in both output channels.
    ConstNode a(0.1f), b(0.2f), c(0.3f), d(0.4f);
    SumNode first, second, master;
    GainNode firstGain, secondGain;
    Graph g;
    const NodeId na = g.addNode(a), nb = g.addNode(b);
    const NodeId nc = g.addNode(c), nd = g.addNode(d);
    const NodeId nfirst = g.addNode(first), nsecond = g.addNode(second);
    const NodeId ngfirst = g.addNode(firstGain), ngsecond = g.addNode(secondGain);
    const NodeId nmaster = g.addNode(master);
    g.connect(na, nfirst); g.connect(nb, nfirst);
    g.connect(nc, nsecond); g.connect(nd, nsecond);
    g.connect(nfirst, ngfirst); g.connect(nsecond, ngsecond);
    g.connect(ngfirst, nmaster); g.connect(ngsecond, nmaster);
    g.setOutput(nmaster);
    g.prepare(48000.0, 512);
    check(g.ok(), "prepared: " + g.error());

    const auto normal = g.levels();  // copy from the read-only interface
    check(normal.size() == 4 && normal[0].size() == 4 &&
          normal[1].size() == 2 && normal[2].size() == 2 && normal[3].size() == 1,
          "four levels; three have independently reorderable nodes");

    Out forward(512), reverse(512);
    g.setReverseWithinLevel(false);
    g.process(makeIo(forward, 512));
    g.setReverseWithinLevel(true);
    g.process(makeIo(reverse, 512));
    bool identical = sameOutput(forward, reverse);
    if (!identical) std::puts("  traversal mismatch: reverse");

    bool permutationsValid = true;
    std::vector<bool> changed(normal.size(), false);
    std::vector<std::vector<std::vector<NodeId>>> seen;
    g.setReverseWithinLevel(false);
    for (std::uint32_t seed : {1u, 42u, 0xC0FFEEu}) {
        // All calls to the hook occur before a callback, on the test thread.
        g.permuteLevelsForTest(seed);
        const auto permuted = g.levels();
        permutationsValid &= permuted.size() == normal.size();
        for (std::size_t level = 0; level < normal.size(); ++level) {
            if (level >= permuted.size()) { permutationsValid = false; continue; }
            permutationsValid &= std::is_permutation(
                normal[level].begin(), normal[level].end(),
                permuted[level].begin(), permuted[level].end());
            changed[level] = changed[level] || permuted[level] != normal[level];
        }
        permutationsValid &= std::find(seen.begin(), seen.end(), permuted) == seen.end();
        seen.push_back(permuted);
        g.permuteLevelsForTest(seed ^ 0xA5A5A5A5u);
        g.permuteLevelsForTest(seed);
        permutationsValid &= g.levels() == permuted;

        Out shuffled(512);
        g.process(makeIo(shuffled, 512));
        const bool equal = sameOutput(forward, shuffled);
        if (!equal) std::printf("  traversal mismatch: seed %u\n", static_cast<unsigned>(seed));
        identical &= equal;
    }
    for (std::size_t level = 0; level < normal.size(); ++level)
        if (normal[level].size() > 1) permutationsValid &= changed[level];
    // Re-preparing must discard the test permutation and recover the schedule.
    g.prepare(48000.0, 512);
    permutationsValid &= g.ok() && g.levels() == normal;
    check(permutationsValid,
          "three distinct reproducible seeds preserve membership, exercise every "
          "nontrivial level, and prepare restores normal traversal");

    // Prove the oracle is byte-sensitive, including the right channel. A float
    // comparison would accept these two buffers despite their different bytes.
    Out positiveZero(1), negativeZero(1);
    negativeZero.r[0] = -0.0f;
    const bool byteSensitive = positiveZero.r[0] == negativeZero.r[0] &&
                               !sameOutput(positiveZero, negativeZero);
    const bool sidechain = sidechainTraversalIsDeterministic();
    const bool events = eventTraversalIsDeterministic();
    check(identical && byteSensitive && sidechain && events,
          "forward, reverse and all three seeds match both channels byte-for-byte; "
          "the oracle distinguishes signed zero; sidechain and event fixtures agree");
    check(std::fabs(forward.l[0] - 1.0f) < 1e-6f &&
          std::fabs(forward.r[0] - 1.0f) < 1e-6f,
          "the fixture sums to one in both channels, rather than matching silence");
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

void testATailShorterThanABlockStillPlays() {
    section("ADR-0043 -- a tail no longer than one block is not skipped entirely");

    // The boundary cases of the same off-by-one. With the counter decremented
    // before the decision, a tail of exactly one block, or less, suspended the
    // node on the very block that tail belonged to -- the tail was never heard.
    for (const std::int64_t tail : {std::int64_t{100}, std::int64_t{256}, std::int64_t{257}}) {
        ConstNode src(1.0f);
        TailNode verb(tail);
        Graph g;
        const NodeId ns = g.addNode(src), nv = g.addNode(verb);
        g.connect(ns, nv);
        g.setOutput(nv);
        g.prepare(48000.0, 256);
        Out o(256);
        AudioIo io = makeIo(o, 256);
        g.process(io);
        const int before = verb.calls;
        src.set(0.0f);
        for (int i = 0; i < 6; ++i) g.process(io);
        const long long want = (tail + 255) / 256;
        eqi(verb.calls - before, want,
            "a " + std::to_string(tail) + "-sample tail runs " + std::to_string(want) +
                " block(s) past the silence");
    }
}

void testACompensatedInputIsPlayedOutBeforeItsNodeSleeps() {
    section("ADR-0058 + ADR-0043 -- compensation in flight is flushed, not cut");

    // A dry path compensated 64 samples against a latent one, and the dry path
    // is the LAST thing playing. When it stops, the master's compensation ring
    // still holds its final 64 samples, due out over the next 64 -- but the
    // master's inputs are both silent, its own tail is zero, and ADR-0043 put
    // it to sleep on the spot. Those samples were never heard.
    //
    // At 5120 samples of linear-phase compensation that is the last 107 ms of
    // a dry track, cut, whenever a plugin with latency sits anywhere else.
    ConstNode quiet(0.0f), dry(1.0f);
    LatentNode slow(64);
    SumNode direct, master;
    Graph g;
    const NodeId nq = g.addNode(quiet), ns = g.addNode(slow);
    const NodeId nd = g.addNode(dry), nx = g.addNode(direct), nm = g.addNode(master);
    g.connect(nq, ns);
    g.connect(ns, nm);
    g.connect(nd, nx);
    g.connect(nx, nm);
    g.setOutput(nm);
    g.prepare(48000.0, 256);
    eqi(g.compensationFor(nx, nm), 64, "the dry path is held back 64 samples");

    Out o(256);
    AudioIo io = makeIo(o, 256);
    for (int b = 0; b < 4; ++b) g.process(io);
    check(o.l[255] == 1.0f, "the dry signal reaches the master");

    dry.set(0.0f);                  // the last thing playing stops
    g.process(io);
    check(o.l[0] == 1.0f && o.l[63] == 1.0f,
          "its last 64 samples still come out of the ring: got " +
              std::to_string(o.l[0]) + " .. " + std::to_string(o.l[63]));
    check(o.l[64] == 0.0f && o.l[255] == 0.0f, "and then silence, exactly when it should");

    g.process(io);
    bool silent = true;
    for (float v : o.l) if (v != 0.0f) silent = false;
    check(silent, "after which the master may sleep");
}

/// Writes its SIDECHAIN input to its output, so the key a compressor would
/// see is observable. Tail zero, like any node with no state of its own.
class KeyThrough final : public Node {
public:
    void process(const NodeIo& io) noexcept override {
        for (std::int32_t c = 0; c < io.channels; ++c) {
            float* o = io.out[c] + io.blockOffset;
            const float* k = (io.sidechain != nullptr && io.sidechain[c] != nullptr)
                                 ? io.sidechain[c] + io.blockOffset : nullptr;
            for (std::int32_t i = 0; i < io.frames; ++i) o[i] = (k != nullptr ? k[i] : 0.0f);
        }
    }
    [[nodiscard]] std::int64_t tailSamples() const noexcept override { return 0; }
    [[nodiscard]] const char* name() const noexcept override { return "key"; }
};

void testACompensatedKeyIsPlayedOutToo() {
    section("ADR-0058 d5 + ADR-0043 -- a compensated sidechain is flushed as well");

    // ADR-0058 compensates a sidechain with the rest (decision 5), so a key
    // input has a ring too. If the key is the last input to stop, the node
    // must stay awake to hear the end of it -- otherwise a compressor's gain
    // reduction releases early by exactly the compensation.
    ConstNode quiet(0.0f), key(1.0f);
    LatentNode slow(64);
    SumNode direct;
    KeyThrough comp;
    Graph g;
    const NodeId nq = g.addNode(quiet), ns = g.addNode(slow);
    const NodeId nk = g.addNode(key), nd = g.addNode(direct), nc = g.addNode(comp);
    g.connect(nq, ns);
    g.connect(ns, nc, Bus::Main);
    g.connect(nk, nd);
    g.connect(nd, nc, Bus::Sidechain);
    g.setOutput(nc);
    g.prepare(48000.0, 256);
    eqi(g.compensationFor(nd, nc, Bus::Sidechain), 64, "the key is held back 64 samples");

    Out o(256);
    AudioIo io = makeIo(o, 256);
    for (int b = 0; b < 4; ++b) g.process(io);
    check(o.l[255] == 1.0f, "the key reaches the compressor");

    key.set(0.0f);
    g.process(io);
    check(o.l[0] == 1.0f && o.l[63] == 1.0f,
          "the key's last 64 samples still reach it: got " + std::to_string(o.l[0]) +
              " .. " + std::to_string(o.l[63]));
    check(o.l[64] == 0.0f, "and then it ends");
}

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

/// Test-only compressor shape, NOT production compressor DSP. Buffers are
/// allocated by the caller, before process; captures are private to this node.
class DuckingProbe final : public Node {
public:
    explicit DuckingProbe(std::int32_t frames) : mainSeen(frames), keySeen(frames) {}
    void process(const NodeIo& io) noexcept override {
        for (std::int32_t c = 0; c < io.channels; ++c) {
            auto& main = c == 0 ? mainSeen.l : mainSeen.r;
            auto& key = c == 0 ? keySeen.l : keySeen.r;
            for (std::int32_t f = io.blockOffset; f < io.blockOffset + io.frames; ++f) {
                const auto index = static_cast<std::size_t>(f);
                main[index] = io.in != nullptr ? io.in[c][f] : 0.0f;
                key[index] = io.sidechain != nullptr ? io.sidechain[c][f] : 0.0f;
                io.out[c][f] = main[index] / (1.0f + std::fabs(key[index]));
            }
        }
    }
    Out mainSeen, keySeen;
};

bool sidechainTraversalIsDeterministic() {
    section("ADR-0056/0058 -- traversal preserves compensated compressor keys");
    constexpr std::int32_t frames = 256, blocks = 4, latency = 64;
    Out reference(frames * blocks);
    bool all = true;
    for (int variant = 0; variant < 5; ++variant) {
        RampNode music, key;
        LatentNode slow(latency);
        SumNode fast, master;
        DuckingProbe comp(frames), mirror(frames);
        Graph g;
        const auto nm = g.addNode(music), nk = g.addNode(key);
        const auto ns = g.addNode(slow), nf = g.addNode(fast);
        const auto nc = g.addNode(comp), nx = g.addNode(mirror), no = g.addNode(master);
        bool valid = g.connect(nm, ns) && g.connect(nk, nf) &&
                     g.connect(ns, nc) && g.connect(nf, nc, Bus::Sidechain) &&
                     g.connect(nf, nx) && g.connect(ns, nx, Bus::Sidechain) &&
                     g.connect(nc, no) && g.connect(nx, no);
        g.setOutput(no);
        g.prepare(48000.0, frames);
        valid &= g.ok() && g.compensationFor(nf, nc, Bus::Sidechain) == latency &&
                 g.compensationFor(ns, nc) == 0 &&
                 g.compensationFor(nf, nx) == latency &&
                 g.compensationFor(ns, nx, Bus::Sidechain) == 0;
        selectTraversal(g, variant);
        Out rendered(frames * blocks), expected(frames * blocks);
        for (std::int32_t block = 0; block < blocks; ++block) {
            Out out(frames), aligned(frames);
            g.process(makeIo(out, frames, block * frames));
            for (std::int32_t f = 0; f < frames; ++f) {
                const auto i = static_cast<std::size_t>(f);
                const auto absolute = block * frames + f;
                const auto j = static_cast<std::size_t>(absolute);
                const float sample = absolute < latency ? 0.0f :
                                     static_cast<float>(absolute - latency);
                aligned.l[i] = aligned.r[i] = sample;
                const float ducked = sample / (1.0f + std::fabs(sample));
                expected.l[j] = expected.r[j] = ducked + ducked;
                rendered.l[j] = out.l[i]; rendered.r[j] = out.r[i];
            }
            // Metadata alone cannot prove PDC: inspect the samples the key and
            // main buses actually deliver, including startup and block crossings.
            valid &= sameOutput(comp.mainSeen, aligned) && sameOutput(comp.keySeen, aligned) &&
                     sameOutput(mirror.mainSeen, aligned) && sameOutput(mirror.keySeen, aligned);
            music.advance(); key.advance();
        }
        valid &= sameOutput(rendered, expected);
        if (variant == 0) { reference.l = rendered.l; reference.r = rendered.r; }
        else valid &= sameOutput(rendered, reference);
        if (!valid) std::printf("  FAIL  sidechain traversal variant %d: key/main alignment or output bytes\n", variant);
        all &= valid;
    }
    return all;
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


/// Counts the events its slot was handed. Nothing else -- the question is
/// only whether they arrive.
class EventCounter final : public Node {
public:
    void process(const NodeIo& io) noexcept override {
        seen += io.events.count;
        for (std::int32_t c = 0; c < io.channels; ++c) {
            float* o = io.out[c] + io.blockOffset;
            const float* i = (io.in != nullptr && io.in[c] != nullptr)
                                 ? io.in[c] + io.blockOffset : nullptr;
            for (std::int32_t k = 0; k < io.frames; ++k)
                o[k] = (i != nullptr ? i[k] : 0.0f);
        }
    }
    [[nodiscard]] std::int64_t tailSamples() const noexcept override { return 0; }
    [[nodiscard]] const char* name() const noexcept override { return "count"; }
    std::int64_t seen = 0;
};

/// A NOT-YET, pinned so it cannot be discovered twice.
///
/// ADR-0045 says a track carries audio AND events through one chain, and
/// ADR-0055's node contract gives every node an `EventSpan`. The scheduler
/// accumulates AUDIO along edges and does not accumulate events: a slot's
/// `events` come only from a `pushInputEvent` naming that slot.
///
/// The consequence is not theoretical and it is not small. `inputFor(trackId)`
/// is where a clip reader pushes, and on any track with a device chain it is
/// the MixNode at the head -- so a MIDI clip pushes note-ons at the head and
/// the instrument two nodes downstream never sees one. Measured with Surge XT
/// through `adi_clap_probe --rebuild`: a note at the head is silence, the same
/// note at the tail is 0.21 peak.
///
/// This test asserts the CURRENT behaviour, deliberately. When events learn to
/// travel along edges, it fails, and the failure is the notification.
void testSplitsAreExactAcrossSlots() {
    section("ADR-0042 -- every distinct event frame is a split, whichever slot holds it");

    // The split loop coalesced WHILE it collected, comparing each event with
    // the last split PUSHED rather than the last split in TIME. Slots are
    // walked in index order, so a later slot's earlier event came out
    // negative against the previous slot's later one, fell under the floor,
    // and was dropped: a real, distinct frame with no segment boundary at it.
    //
    // It survived because every test that split a block kept all its events
    // in one slot. Forwarding (ADR-0091) puts the same note in many slots, so
    // this stops being a corner.
    Graph g;
    EventCounter a, b;
    SumNode out;
    const NodeId na = g.addNode(a), nb = g.addNode(b), no = g.addNode(out);
    g.connect(na, no);
    g.connect(nb, no);
    g.setOutput(no);
    g.prepare(48000.0, 256);
    check(g.floorFrames() <= 96, "the floor is under the 100-frame gap used here");

    Event late;  late.type = EventType::ParamValue;  late.frame = 200;
    Event early; early.type = EventType::ParamValue; early.frame = 100;
    check(g.pushInputEvent(na, late), "slot 0 gets frame 200");
    check(g.pushInputEvent(nb, early), "slot 1 gets frame 100 -- EARLIER, in a LATER slot");

    std::vector<float> l(256, 0.0f), r(256, 0.0f);
    float* outp[2] = {l.data(), r.data()};
    AudioIo io;
    io.out = outp; io.numOut = 2; io.frames = 256;
    g.process(io);

    eqi(g.stats().segments, 3,
        "three segments -- [0,100), [100,200), [200,256) -- and not two, which "
        "is what dropping frame 100 gives");

    // AND THE NEXT BLOCK STARTS CLEAN. The marks are per block; one left over
    // splits an empty block at frames that belonged to the previous one. A
    // single-block test cannot see that, so this runs a second, empty block.
    g.process(io);
    eqi(g.stats().segments, 4,
        "an empty block after it is ONE segment, not three -- the previous "
        "block's frames do not carry over");
}

// --- ADR-0091: events travel along edges ------------------------------------

/// Records every event it is handed, with the ABSOLUTE time it arrived and the
/// segment it arrived in, so a delay and a split are both observable.
class EventProbe final : public Node {
public:
    explicit EventProbe(EventFlow flow = EventFlow::Through, std::int32_t latency = 0)
        : flow_(flow), latency_(latency) {
        at.reserve(64); types.reserve(64); segs.reserve(64);
    }
    void process(const NodeIo& io) noexcept override {
        for (const Event& e : io.events) {
            at.push_back(base + e.frame);
            types.push_back(e.type);
            segs.push_back(io.blockOffset);
        }
        for (std::int32_t c = 0; c < io.channels; ++c) {
            float* o = io.out[c] + io.blockOffset;
            const float* i = (io.in != nullptr && io.in[c] != nullptr)
                                 ? io.in[c] + io.blockOffset : nullptr;
            for (std::int32_t k = 0; k < io.frames; ++k)
                o[k] = (i != nullptr ? i[k] : 0.0f);
        }
    }
    [[nodiscard]] EventFlow eventFlow() const noexcept override { return flow_; }
    [[nodiscard]] std::int32_t latencySamples() const noexcept override { return latency_; }
    [[nodiscard]] std::int64_t tailSamples() const noexcept override { return 0; }
    [[nodiscard]] const char* name() const noexcept override { return "probe"; }

    [[nodiscard]] std::size_t notes() const {
        std::size_t n = 0;
        for (EventType t : types) if (isNoteStream(t)) ++n;
        return n;
    }

    std::int64_t base = 0;              ///< the absolute sample this block starts at
    std::vector<std::int64_t> at;
    std::vector<EventType> types;
    std::vector<std::int32_t> segs;

private:
    EventFlow flow_;
    std::int32_t latency_;
};

Event noteOn(std::int32_t frame) {
    Event e;
    e.type = EventType::NoteOn;
    e.frame = frame;
    e.noteId = 1;
    e.dim = 60;
    e.value = 1.0;
    e.channel = 1;
    return e;
}

/// One block through `g`, with every probe told where that block starts.
void runBlock(Graph& g, std::int32_t frames, std::int64_t base,
              std::initializer_list<EventProbe*> probes) {
    for (EventProbe* p : probes) p->base = base;
    std::vector<float> l(static_cast<std::size_t>(frames), 0.0f);
    std::vector<float> r(static_cast<std::size_t>(frames), 0.0f);
    float* outp[2] = {l.data(), r.data()};
    AudioIo io;
    io.out = outp; io.numOut = 2; io.frames = frames;
    g.process(io);
}

/// Test-only instrument: renders note/expression values at the actual event
/// frame, and records explicit fields rather than memcmp-ing Event padding.
/// Fixed storage keeps the callback allocation-free, with a counted overflow.
class EventRenderProbe final : public Node {
public:
    using Record = std::array<std::uint64_t, 10>;
    void process(const NodeIo& io) noexcept override {
        for (const Event& e : io.events) {
            inRange &= e.frame >= io.blockOffset && e.frame < io.blockOffset + io.frames;
            if (count == records.size()) { overflow = true; continue; }
            records[count++] = {
                static_cast<std::uint64_t>(base + e.frame),
                static_cast<std::uint64_t>(e.frame),
                static_cast<std::uint64_t>(io.blockOffset),
                static_cast<std::uint64_t>(io.frames),
                static_cast<std::uint64_t>(e.type), e.channel, e.dim, e.noteId,
                e.paramId, std::bit_cast<std::uint64_t>(e.value)};
        }
        for (std::int32_t f = io.blockOffset; f < io.blockOffset + io.frames; ++f) {
            for (const Event& e : io.events) {
                // Engine events remain block-relative even in a late segment.
                if (e.frame != f) continue;
                if (e.type == EventType::NoteOn) value_ = static_cast<float>(e.value);
                else if (e.type == EventType::NoteExpression) value_ = static_cast<float>(e.value);
                else if (e.type == EventType::NoteOff) value_ = 0.0f;
            }
            for (std::int32_t c = 0; c < io.channels; ++c)
                io.out[c][f] = value_ * static_cast<float>(c + 1);
        }
    }
    [[nodiscard]] EventFlow eventFlow() const noexcept override { return EventFlow::Consume; }
    std::array<Record, 32> records{};
    std::size_t count = 0;
    std::int64_t base = 0;
    bool inRange = true, overflow = false;
private:
    float value_ = 0.0f;
};

bool eventTraversalIsDeterministic() {
    section("ADR-0091/0081 -- traversal preserves routed event bytes and block frames");
    constexpr std::int32_t frames = 256, blocks = 3, latency = 64;
    std::array<Event, 5> input{};
    constexpr std::int32_t offsets[] = {32, 96, 191, 192, 224};
    constexpr double values[] = {0.25, 0.5, 0.75, 0.125, 0.0};
    for (std::size_t i = 0; i < input.size(); ++i) {
        input[i] = noteOn(offsets[i]);
        input[i].noteId = 0x123456789ABCull;
        input[i].channel = 3;
        input[i].value = values[i];
        input[i].type = i == 0 ? EventType::NoteOn :
                        i == input.size() - 1 ? EventType::NoteOff : EventType::NoteExpression;
        input[i].dim = i == 0 || i == input.size() - 1 ? 60 : static_cast<std::uint16_t>(i - 1);
    }
    Out reference(frames * blocks), expected(frames * blocks);
    std::array<EventRenderProbe::Record, 32> referenceRecords{};
    std::size_t referenceCount = 0;
    float value = 0.0f;
    for (std::int32_t f = 0; f < frames * blocks; ++f) {
        for (const auto& e : input) if (f == e.frame + latency) value = static_cast<float>(e.value);
        const auto i = static_cast<std::size_t>(f);
        expected.l[i] = value + value;
        expected.r[i] = (value * 2.0f) + (value * 2.0f);
    }
    bool all = true;
    for (int variant = 0; variant < 5; ++variant) {
        SumNode head, fast, master;
        LatentNode slow(latency);
        EventRenderProbe first, second;
        Graph g;
        const auto nh = g.addNode(head), ns = g.addNode(slow), nf = g.addNode(fast);
        const auto na = g.addNode(first), nb = g.addNode(second), nm = g.addNode(master);
        bool valid = g.connect(nh, ns) && g.connect(nh, nf) &&
                     g.connect(ns, na) && g.connect(nf, na) &&
                     g.connect(ns, nb) && g.connect(nf, nb) &&
                     g.connect(na, nm) && g.connect(nb, nm);
        g.setOutput(nm);
        g.prepare(48000.0, frames);
        valid &= g.ok() && g.compensationFor(nf, na) == latency &&
                 g.compensationFor(nf, nb) == latency;
        selectTraversal(g, variant);
        for (const auto& e : input) valid &= g.pushInputEvent(nh, e);
        Out rendered(frames * blocks);
        for (std::int32_t block = 0; block < blocks; ++block) {
            first.base = second.base = block * frames;
            Out out(frames);
            g.process(makeIo(out, frames, block * frames));
            std::copy(out.l.begin(), out.l.end(), rendered.l.begin() + block * frames);
            std::copy(out.r.begin(), out.r.end(), rendered.r.begin() + block * frames);
        }
        valid &= sameOutput(rendered, expected) && g.stats().eventsDropped == 0 &&
                 g.stats().eventsDeferred > 0 && g.stats().eventsForwarded > 0;
        for (const auto* probe : {&first, &second}) {
            valid &= probe->count == input.size() * 2 && probe->inRange && !probe->overflow;
            // One copy per fan-in path, at 96,160,255,256,288 absolute samples.
            // The two middle expressions straddle a block boundary by ONE sample.
            for (std::size_t i = 0; i < input.size(); ++i) {
                const auto& e = input[i];
                const auto due = static_cast<std::uint64_t>(e.frame + latency);
                for (std::size_t path = 0; path < 2; ++path) {
                    const auto& r = probe->records[2 * i + path];
                    valid &= r[0] == due && r[1] == due % frames &&
                             r[1] >= r[2] && r[1] < r[2] + r[3] &&
                             r[4] == static_cast<std::uint64_t>(e.type) &&
                             r[5] == e.channel && r[6] == e.dim && r[7] == e.noteId &&
                             r[8] == e.paramId && r[9] == std::bit_cast<std::uint64_t>(e.value);
                }
            }
        }
        // Compare the emitted samples and the complete observed event sequence,
        // including segment coordinates, by bytes. Equal wrong schedules also
        // fail the independent expected-frame/output oracle above.
        if (variant == 0) {
            reference.l = rendered.l; reference.r = rendered.r;
            referenceRecords = first.records; referenceCount = first.count;
        }
        valid &= sameOutput(rendered, reference);
        for (const auto* probe : {&first, &second})
            valid &= probe->count == referenceCount &&
                     std::memcmp(probe->records.data(), referenceRecords.data(),
                                 referenceCount * sizeof(EventRenderProbe::Record)) == 0;
        if (!valid) std::printf("  FAIL  event traversal variant %d: routed fields, block frames or output bytes\n", variant);
        all &= valid;
    }
    return all;
}

void testANoteTravelsTheChain() {
    section("ADR-0091 -- a note pushed at the head reaches the instrument at the tail");

    // mac measured this failing with a real plugin: a note pushed at the chain
    // head was silence, the same note pushed at the tail was 0.21 peak. The
    // head is a MixNode on every track with devices (ADR-0077), so that is
    // where a clip reader pushes -- and until now it went nowhere.
    Graph g;
    EventProbe head, mid, synth(EventFlow::Consume);
    const NodeId nh = g.addNode(head), nm = g.addNode(mid), ns = g.addNode(synth);
    g.connect(nh, nm);
    g.connect(nm, ns);
    g.setOutput(ns);
    g.prepare(48000.0, 256);
    check(g.ok(), "prepared: " + g.error());

    check(g.pushInputEvent(nh, noteOn(10)), "a note is pushed at the HEAD");
    runBlock(g, 256, 0, {&head, &mid, &synth});

    eqi(static_cast<long long>(head.notes()), 1, "the head sees it");
    eqi(static_cast<long long>(mid.notes()), 1, "so does the node in between");
    eqi(static_cast<long long>(synth.notes()), 1,
        "and so does the instrument at the tail -- which is the whole point");
    check(!synth.at.empty() && synth.at[0] == 10,
          "at the frame it was pushed, since nothing on the way has latency");
    eqi(g.stats().eventsForwarded, 2, "two hops, two forwards");
}

void testAnInstrumentConsumesTheNote() {
    section("ADR-0091 -- the note stops at the instrument");

    // An effect after a synth has no use for the synth's notes. Without
    // Consume they would flow on to the master through every effect, costing a
    // split in each and meaning something to any second instrument downstream.
    Graph g;
    EventProbe head, synth(EventFlow::Consume), eq;
    const NodeId nh = g.addNode(head), ns = g.addNode(synth), ne = g.addNode(eq);
    g.connect(nh, ns);
    g.connect(ns, ne);
    g.setOutput(ne);
    g.prepare(48000.0, 256);

    g.pushInputEvent(nh, noteOn(10));
    runBlock(g, 256, 0, {&head, &synth, &eq});

    eqi(static_cast<long long>(synth.notes()), 1,
        "the instrument RECEIVES it -- Consume stops onward travel, not delivery");
    eqi(static_cast<long long>(eq.notes()), 0, "the effect after it does not");
}

void testParametersStayWherePushed() {
    section("ADR-0091 -- an addressed event is not forwarded");

    // ParamValue names a parameter OF THE NODE IT WAS PUSHED TO. GainNode
    // applies any ParamValue whose id matches its own, so forwarding one would
    // have every node downstream set its parameter 0 to this node's value.
    Graph g;
    EventProbe head, tail;
    const NodeId nh = g.addNode(head), nt = g.addNode(tail);
    g.connect(nh, nt);
    g.setOutput(nt);
    g.prepare(48000.0, 256);

    Event param;
    param.type = EventType::ParamValue;
    param.frame = 20;
    param.paramId = 0;
    param.value = 0.5;
    g.pushInputEvent(nh, param);
    g.pushInputEvent(nh, noteOn(30));
    runBlock(g, 256, 0, {&head, &tail});

    eqi(static_cast<long long>(head.types.size()), 2, "the head sees both");
    eqi(static_cast<long long>(tail.types.size()), 1, "the tail sees one");
    check(!tail.types.empty() && tail.types[0] == EventType::NoteOn,
          "and it is the note, not the parameter");
}

void testASidechainCarriesNoNotes() {
    section("ADR-0091 -- main edges only; a sidechain is an audio key");

    // A compressor keyed from a kick track has no use for that track's notes,
    // and handing them over would make every keyed plugin a second instrument.
    Graph g;
    EventProbe music, kick, comp;
    const NodeId nm = g.addNode(music), nk = g.addNode(kick), nc = g.addNode(comp);
    g.connect(nm, nc, Bus::Main);
    g.connect(nk, nc, Bus::Sidechain);
    g.setOutput(nc);
    g.prepare(48000.0, 256);

    g.pushInputEvent(nk, noteOn(10));
    runBlock(g, 256, 0, {&music, &kick, &comp});

    eqi(static_cast<long long>(kick.notes()), 1, "the key track has its note");
    eqi(static_cast<long long>(comp.notes()), 0, "and it does not cross the sidechain");

    g.pushInputEvent(nm, noteOn(10));
    runBlock(g, 256, 256, {&music, &kick, &comp});
    eqi(static_cast<long long>(comp.notes()), 1, "while the main input's note does");
}

void testLatencyDelaysTheNote() {
    section("ADR-0091 -- a note is delayed by the latency of what it passes through");

    // The case that makes this necessary. A node with latency L before an
    // instrument: the graph believes the instrument's input is L late
    // (arrival = L) and compensates every OTHER track by L to match. If the
    // note skipped the delay the instrument would play L early -- aligned in
    // the graph's arithmetic and early in the room.
    Graph g;
    EventProbe head(EventFlow::Through, 30), synth(EventFlow::Consume);
    const NodeId nh = g.addNode(head), ns = g.addNode(synth);
    g.connect(nh, ns);
    g.setOutput(ns);
    g.prepare(48000.0, 256);
    eqi(g.arrivalOf(ns), 30, "the graph believes the instrument's input is 30 late");

    g.pushInputEvent(nh, noteOn(10));
    runBlock(g, 256, 0, {&head, &synth});
    check(!synth.at.empty() && synth.at[0] == 40,
          "so the note reaches it at 10 + 30, not 10: got " +
              (synth.at.empty() ? std::string("nothing") : std::to_string(synth.at[0])));
}

void testTwoPathsDeliverTheNoteAligned() {
    section("ADR-0091 -- compensation applies to events exactly as to audio");

    // The note splits, one branch declares 64 samples, both rejoin. PDC holds
    // the direct branch back 64 so the audio meets. The note must be held back
    // the same 64 -- and then it reaches the merge at ONE frame by both paths,
    // which is ADR-0058's alignment property, stated for events.
    Graph g;
    EventProbe src, slow(EventFlow::Through, 64), fast, mix;
    const NodeId ns = g.addNode(src), nl = g.addNode(slow);
    const NodeId nf = g.addNode(fast), nm = g.addNode(mix);
    g.connect(ns, nl);
    g.connect(ns, nf);
    g.connect(nl, nm);
    g.connect(nf, nm);
    g.setOutput(nm);
    g.prepare(48000.0, 256);
    eqi(g.compensationFor(nf, nm), 64, "the fast path's audio is held back 64");

    g.pushInputEvent(ns, noteOn(10));
    runBlock(g, 256, 0, {&src, &slow, &fast, &mix});

    eqi(static_cast<long long>(mix.notes()), 2,
        "the merge receives one copy PER PATH -- which is what a layering rack "
        "needs, and why ADR-0072 puts re-converging paths inside racks");
    check(mix.at.size() == 2 && mix.at[0] == 74 && mix.at[1] == 74,
          "and both arrive at 74: the slow one through its latency, the fast one "
          "through the compensation that meets it");
}

void testADelayCrossesBlockBoundaries() {
    section("ADR-0091 -- a delay longer than the block defers to a later one");

    // ADR-0088 sized compensation for 5120 samples, a block is often 256, so a
    // forwarded note is routinely due several blocks from now. It is held in a
    // fixed-capacity queue keyed by absolute sample, and delivered in the block
    // it falls in, at the frame it falls on.
    Graph g;
    EventProbe head(EventFlow::Through, 300), synth(EventFlow::Consume);
    const NodeId nh = g.addNode(head), ns = g.addNode(synth);
    g.connect(nh, ns);
    g.setOutput(ns);
    g.prepare(48000.0, 256);

    g.pushInputEvent(nh, noteOn(100));
    runBlock(g, 256, 0, {&head, &synth});
    eqi(static_cast<long long>(synth.notes()), 0, "nothing reaches the synth in block 0");
    eqi(g.stats().eventsDeferred, 1, "because the note was deferred");

    runBlock(g, 256, 256, {&head, &synth});
    eqi(static_cast<long long>(synth.notes()), 1, "it arrives in block 1");
    check(!synth.at.empty() && synth.at[0] == 400,
          "at absolute sample 100 + 300 = 400, i.e. frame 144 of block 1: got " +
              (synth.at.empty() ? std::string("nothing") : std::to_string(synth.at[0])));

    runBlock(g, 256, 512, {&head, &synth});
    eqi(static_cast<long long>(synth.notes()), 1, "and exactly once");
}

void testAForwardedNoteGetsItsOwnSegment() {
    section("ADR-0091 + ADR-0042 -- a forwarded note lands ON a segment boundary");

    // Why forwarding happens BEFORE the splits and not while nodes run. The
    // note is pushed at 70 and delayed 80, so it reaches the tail at 150 --
    // a frame no pushed event occupies. Forwarding during the run would hand
    // it over inside a segment chosen without it.
    Graph g;
    EventProbe head(EventFlow::Through, 80), tail;
    const NodeId nh = g.addNode(head), nt = g.addNode(tail);
    g.connect(nh, nt);
    g.setOutput(nt);
    g.prepare(48000.0, 256);

    g.pushInputEvent(nh, noteOn(70));
    runBlock(g, 256, 0, {&head, &tail});
    eqi(g.stats().segments, 3, "three segments: the pushed frame AND the forwarded one");
    check(!tail.segs.empty() && tail.segs[0] == 150,
          "and the tail receives it in the segment that STARTS at 150: got " +
              (tail.segs.empty() ? std::string("nothing") : std::to_string(tail.segs[0])));
}

void testFanInOverflowIsCountedNotAllocated() {
    section("ADR-0091 -- fan-in shares the receiver's capacity, and overflow is counted");

    Graph g;
    g.setEventCapacity(4);
    EventProbe a, b, c, sink;
    const NodeId na = g.addNode(a), nb = g.addNode(b), nc = g.addNode(c);
    const NodeId nk = g.addNode(sink);
    g.connect(na, nk);
    g.connect(nb, nk);
    g.connect(nc, nk);
    g.setOutput(nk);
    g.prepare(48000.0, 256);
    eqi(g.eventCapacity(), 4, "four events per slot");

    for (NodeId n : {na, nb, nc}) {
        g.pushInputEvent(n, noteOn(10));
        g.pushInputEvent(n, noteOn(90));
    }
    runBlock(g, 256, 0, {&a, &b, &c, &sink});
    eqi(static_cast<long long>(sink.notes()), 4, "the sink holds what fits");
    eqi(g.stats().eventsDropped, 2,
        "and the two that did not are COUNTED -- a list that grew would be the "
        "audio thread allocating");
}

void testDeferredOverflowIsCounted() {
    section("ADR-0091 -- the deferral queue is bounded too");

    Graph g;
    g.setEventCapacity(2);
    EventProbe head(EventFlow::Through, 1000), tail;
    const NodeId nh = g.addNode(head), nt = g.addNode(tail);
    g.connect(nh, nt);
    g.setOutput(nt);
    g.prepare(48000.0, 256);

    g.pushInputEvent(nh, noteOn(10));
    g.pushInputEvent(nh, noteOn(80));
    runBlock(g, 256, 0, {&head, &tail});
    eqi(g.stats().eventsDeferred, 2, "two fit the queue");

    g.pushInputEvent(nh, noteOn(10));
    runBlock(g, 256, 256, {&head, &tail});
    eqi(g.stats().eventsDropped, 1,
        "the third does not, and is counted rather than allocated for");
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
        testSuspendedNodeClearsOnceToCapacity();
        testMixSkipsSleepingSources();
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
        testATailShorterThanABlockStillPlays();
        testACompensatedInputIsPlayedOutBeforeItsNodeSleeps();
        testACompensatedKeyIsPlayedOutToo();
        testDelayLineItself();
        testArrivalArithmetic();
        testPhaseAlignment();
        testSidechainIsCompensatedToo();
        testGroupsCompensateAsOne();
        testNoLatencyMeansNoDelayLines();
    testSplitsAreExactAcrossSlots();
        testANoteTravelsTheChain();
        testAnInstrumentConsumesTheNote();
        testParametersStayWherePushed();
        testASidechainCarriesNoNotes();
        testLatencyDelaysTheNote();
        testTwoPathsDeliverTheNoteAligned();
        testADelayCrossesBlockBoundaries();
        testAForwardedNoteGetsItsOwnSegment();
        testFanInOverflowIsCountedNotAllocated();
        testDeferredOverflowIsCounted();
    } catch (const std::exception& e) {
        std::printf("\nFAILED -- exception escaped: %s\n", e.what());
        return 1;
    }
    std::printf("\n%s -- %d checks, %d failure(s)\n",
                g_failures ? "FAILED" : "PASS", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
