// SPDX-License-Identifier: GPL-3.0-or-later
//
// A latency change while the graph is running — ADR-0079, amending ADR-0066.
//
// Its own binary rather than more cases in test_graph.cpp, because it replaces
// global `operator new` to observe ADR-0010. That is the right tool for the one
// claim this file exists to check and the wrong thing to impose on a hundred
// unrelated assertions.
//
// The claim: when a plugin changes its reported latency, the compensation
// follows it, the output does not break, and NOTHING ALLOCATES on the audio
// thread. The first two are arithmetic. The third is why the delay line is a
// ring with a movable tap instead of a buffer that gets replaced.

#include "adi/engine/graph.hpp"
#include "adi/engine/latency.hpp"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <cstring>
#include <limits>
#include <new>
#include <thread>
#include <string>
#include <vector>

// --- allocation counter -----------------------------------------------------
namespace {
std::atomic<long> g_allocs{0};
std::atomic<bool> g_counting{false};
}  // namespace

void* operator new(std::size_t n) {
    if (g_counting.load(std::memory_order_relaxed))
        g_allocs.fetch_add(1, std::memory_order_relaxed);
    void* p = std::malloc(n == 0 ? 1 : n);
    if (p == nullptr) throw std::bad_alloc();
    return p;
}
void* operator new[](std::size_t n) { return operator new(n); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }

namespace {

using namespace adi::engine;

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

/// Declares a latency AND incurs it, and both can change at runtime -- which
/// is a plugin switching to linear phase. Its ring is sized once, for the
/// largest latency it will ever report, exactly as a real device would have to.
class SwitchableNode final : public Node {
public:
    SwitchableNode(std::int32_t latency, std::int32_t maxLatency)
        : latency_(latency), max_(maxLatency) {}

    void prepare(double, std::int32_t) override {
        line_.prepare(2, max_);
        line_.setDelay(latency_.load(std::memory_order_relaxed));
    }

    /// Message thread: the mode switch itself.
    void setLatency(std::int32_t n) noexcept {
        latency_.store(n, std::memory_order_relaxed);
        line_.setDelay(n);
        ++epoch_;
    }
    [[nodiscard]] std::int64_t epoch() const noexcept { return epoch_; }

    void process(const NodeIo& io) noexcept override {
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

    [[nodiscard]] std::int32_t latencySamples() const noexcept override {
        return latency_.load(std::memory_order_relaxed);
    }
    [[nodiscard]] std::int64_t tailSamples() const noexcept override {
        return kInfiniteTail;
    }
    [[nodiscard]] const char* name() const noexcept override { return "switchable"; }

private:
    std::atomic<std::int32_t> latency_;
    std::int32_t max_;
    std::int64_t epoch_ = 0;
    DelayLine line_;
};

class SumNode final : public Node {
public:
    void process(const NodeIo& io) noexcept override {
        for (std::int32_t c = 0; c < io.channels; ++c) {
            float* o = io.out[c] + io.blockOffset;
            const float* i = (io.in != nullptr && io.in[c] != nullptr)
                                 ? io.in[c] + io.blockOffset : nullptr;
            for (std::int32_t k = 0; k < io.frames; ++k) o[k] = (i != nullptr ? i[k] : 0.0f);
        }
        ++calls;
    }
    [[nodiscard]] std::int64_t tailSamples() const noexcept override { return 0; }
    [[nodiscard]] bool alwaysProcess() const noexcept override { return always; }
    [[nodiscard]] const char* name() const noexcept override { return "sum"; }
    bool always = false;   ///< a mix kept awake, for a test that needs its edges run
    int calls = 0;
};

struct Out {
    std::vector<float> l, r;
    std::vector<float*> ptrs;
    explicit Out(std::int32_t n) : l(static_cast<std::size_t>(n), 0.0f),
                                   r(static_cast<std::size_t>(n), 0.0f) {
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

void testTheTapMovesInsideItsRing() {
    section("ADR-0079 -- the delay is a tap, and the tap can move");

    DelayLine d;
    d.prepare(1, 8);              // capacity, not delay
    eqi(d.capacity(), 8, "the ring holds up to eight samples of delay");
    eqi(d.delay(), 0, "and prepare does NOT choose a delay -- setDelay does");

    d.setDelay(2);
    eqi(d.delay(), 2, "which it now has");

    std::vector<float> in{1, 2, 3, 4, 5, 6, 7, 8};
    std::vector<float> out(in.size(), -1.0f);
    d.process(0, in.data(), out.data(), 8);
    check(out[0] == 0.0f && out[1] == 0.0f && out[2] == 1.0f && out[7] == 6.0f,
          "delayed by exactly two: got " + std::to_string(out[2]) + ", " +
              std::to_string(out[7]));

    // A tap at the full capacity must still be a real delay. With a ring of
    // exactly `capacity` the write would land on the slot the tap reads and the
    // longest delay would silently become none, which is why the ring is one
    // longer than the capacity.
    DelayLine f;
    f.prepare(1, 4);
    f.setDelay(4);
    std::vector<float> fin{1, 2, 3, 4, 5, 6};
    std::vector<float> fout(6, -1.0f);
    f.process(0, fin.data(), fout.data(), 6);
    check(fout[0] == 0.0f && fout[4] == 1.0f && fout[5] == 2.0f,
          "a tap at the full capacity delays by the full capacity: got " +
              std::to_string(fout[4]) + " (want 1)");

    check(!d.beginGlide(9), "a move beyond the ring is REFUSED, not clamped");
    eqi(d.delay(), 2, "and nothing changed");
    check(d.beginGlide(5), "a move that fits is accepted");
    check(d.gliding(), "and is pending until a block has run");
    eqi(d.target(), 5, "with the target recorded");
}

void testTheGlideCrossfadesAndSettles() {
    section("ADR-0079 -- a tap move crossfades, then holds");

    // A constant makes the crossfade's endpoints identical, which proves
    // nothing. A ramp makes them differ by exactly the change in delay.
    DelayLine d;
    d.prepare(1, 16);
    d.setDelay(2);

    std::vector<float> a(16), out(16, -1.0f);
    for (int i = 0; i < 16; ++i) a[static_cast<std::size_t>(i)] = static_cast<float>(i);
    d.process(0, a.data(), out.data(), 16);      // prime the ring

    std::vector<float> b(16), out2(16, -1.0f);
    for (int i = 0; i < 16; ++i) b[static_cast<std::size_t>(i)] = static_cast<float>(16 + i);
    check(d.beginGlide(6), "asked to move from 2 to 6");
    d.process(0, b.data(), out2.data(), 16);
    d.endEdge();
    eqi(d.delay(), 6, "and the move has taken effect");
    check(!d.gliding(), "with nothing still pending");

    // During the fade the output sits BETWEEN the two taps. At the very start
    // it is essentially the old one and at the end essentially the new one.
    // Checked as a bound rather than a formula: the exact ramp shape is an
    // implementation choice, being bracketed by the two taps is the contract.
    bool bracketed = true;
    for (int i = 0; i < 16; ++i) {
        const float want2 = static_cast<float>(16 + i - 2);   // tap at 2
        const float want6 = static_cast<float>(16 + i - 6);   // tap at 6
        const float got = out2[static_cast<std::size_t>(i)];
        const float lo = want6 - 0.001f, hi = want2 + 0.001f;
        if (got < lo || got > hi) bracketed = false;
    }
    check(bracketed, "every sample of the fade lies between the old tap and the new");
    check(out2[15] < static_cast<float>(16 + 15 - 2),
          "and by the end it has actually moved off the old tap: got " +
              std::to_string(out2[15]));

    // BOTH ENDS, because "bracketed" alone does not distinguish a crossfade
    // from a hard switch -- a jump straight to the new tap is inside the
    // bracket at every sample. The start has to still be the OLD tap, and
    // that is the assertion a missing fade fails.
    check(out2[0] > static_cast<float>(16 - 2) - 0.5f,
          "and at the START it is still essentially the old tap -- a hard "
          "switch would sit at the new one from sample 0, which is the click "
          "the fade exists to avoid: got " + std::to_string(out2[0]) +
              ", old tap is " + std::to_string(16 - 2));

    // After the fade it is the new tap exactly, with no residue.
    std::vector<float> c(16), out3(16, -1.0f);
    for (int i = 0; i < 16; ++i) c[static_cast<std::size_t>(i)] = static_cast<float>(32 + i);
    d.process(0, c.data(), out3.data(), 16);
    bool exact = true;
    for (int i = 0; i < 16; ++i)
        if (out3[static_cast<std::size_t>(i)] != static_cast<float>(32 + i - 6)) exact = false;
    check(exact, "the block after the fade is the new delay exactly: got " +
                     std::to_string(out3[0]) + ", want " + std::to_string(32 - 6));
}

void testWithoutHeadroomARetapIsRefused() {
    section("ADR-0079 -- headroom is opt-in, and its absence is reported");

    RampNode src;
    SwitchableNode plugin(0, 512);
    SumNode direct, mix;
    Graph g;
    const NodeId ns = g.addNode(src), np = g.addNode(plugin);
    const NodeId nd = g.addNode(direct), nm = g.addNode(mix);
    g.connect(ns, np);
    g.connect(ns, nd);
    g.connect(np, nm);
    g.connect(nd, nm);
    g.setOutput(nm);

    // THE DEFAULT IS NOT ZERO ANY MORE (ADR-0088), so this test has to opt
    // OUT to say what it is about. Zero is still a legal setting and still
    // means "compensation is fixed at prepare" -- it is just no longer what
    // you get by accident, because what you got by accident was the cheap
    // path never running.
    eqi(g.latencyHeadroom(), 8192, "8192 by default, measured on a real plugin");
    g.setLatencyHeadroom(0);
    eqi(g.latencyHeadroom(), 0, "and zero when explicitly asked for");
    g.prepare(48000.0, 256);
    check(g.ok(), "prepared: " + g.error());

    plugin.setLatency(128);
    check(!g.retapLatency(),
          "the retap is REFUSED -- the ring was sized for the latency it had, "
          "and making it bigger means allocating");
    eqi(g.compensationFor(nd, nm), 0,
        "and nothing moved, so the graph is still internally consistent with "
        "the compensation it has");
}

void testTheHeadroomCostIsMeasuredNotGuessed() {
    section("ADR-0088 -- what the default actually costs, in bytes");

    // The default was wrong for a year of comments because the comment said
    // "low thousands" and nothing measured it. This asserts the arithmetic so
    // the next person choosing a number is choosing against a fact.
    RampNode src;
    SumNode a, b, mix;
    Graph g;
    const NodeId ns = g.addNode(src), na = g.addNode(a);
    const NodeId nb = g.addNode(b), nm = g.addNode(mix);
    g.connect(ns, na);
    g.connect(ns, nb);
    g.connect(na, nm);
    g.connect(nb, nm);
    g.setOutput(nm);

    g.setLatencyHeadroom(0);
    g.prepare(48000.0, 256);
    eqi(static_cast<long long>(g.compensationBytes()), 0,
        "with no headroom and no latency, the rings cost nothing at all");

    // Four edges, stereo, (8192 + 1) floats each: the +1 is the ring's extra
    // slot, which is why this is measured rather than multiplied out.
    g.setLatencyHeadroom(8192);
    g.prepare(48000.0, 256);
    const long long want = 4LL * 2 * (8192 + 1) * 4;
    eqi(static_cast<long long>(g.compensationBytes()), want,
        "and 8192 samples of headroom on four stereo edges is " +
            std::to_string(want / 1024) + " KB");

    // Which scales linearly, so a 200-edge project is fifty times this and
    // still single-digit megabytes. That is the trade the default makes.
    check(g.compensationBytes() < 300u * 1024u,
          "under 300 KB for this graph, so a 200-edge project is ~16 MB");
}

void testARetapFollowsTheNewLatency() {
    section("ADR-0079 -- with headroom, the compensation follows the plugin");

    RampNode src;
    SwitchableNode plugin(64, 512);
    SumNode direct, mix;
    Graph g;
    const NodeId ns = g.addNode(src), np = g.addNode(plugin);
    const NodeId nd = g.addNode(direct), nm = g.addNode(mix);
    g.connect(ns, np);
    g.connect(ns, nd);
    g.connect(np, nm);
    g.connect(nd, nm);
    g.setOutput(nm);
    g.setLatencyHeadroom(512);
    g.prepare(48000.0, 256);
    check(g.ok(), "prepared: " + g.error());

    eqi(g.compensationFor(nd, nm), 64, "compensated for the initial 64");
    eqi(g.latencySamples(), 64, "and reporting it");

    Out o(256);
    AudioIo io = makeIo(o, 256);
    g.process(io);
    src.advance();

    // The mode switch.
    plugin.setLatency(192);
    check(g.retapLatency(), "the retap fits the headroom");
    eqi(g.latencySamples(), 192, "and the graph reports the new cost at once");

    g.process(io);            // the crossfade block
    src.advance();
    eqi(g.compensationFor(nd, nm), 192,
        "after one block the direct path carries the new compensation");

    // And it is genuinely aligned afterwards, not merely renumbered.
    g.process(io);
    src.advance();
    g.process(io);
    src.advance();
    bool aligned = true;
    std::size_t bad = 0;
    for (std::size_t i = 0; i < o.l.size(); ++i) {
        // Both paths now delay by 192, so the sum is 2x the ramp at t-192.
        const float want = 2.0f * (static_cast<float>(3 * 256 + i) - 192.0f);
        if (o.l[i] != want) { aligned = false; bad = i; break; }
    }
    check(aligned,
          "and two blocks later the paths sum in phase again" +
              (aligned ? std::string()
                       : " -- sample " + std::to_string(bad) + " is " +
                             std::to_string(o.l[bad])));
}

void testARetapAllocatesNothingOnTheAudioThread() {
    section("ADR-0010 + ADR-0066 -- the switch does not allocate");

    RampNode src;
    SwitchableNode plugin(64, 512);
    SumNode direct, mix;
    Graph g;
    const NodeId ns = g.addNode(src), np = g.addNode(plugin);
    const NodeId nd = g.addNode(direct), nm = g.addNode(mix);
    g.connect(ns, np);
    g.connect(ns, nd);
    g.connect(np, nm);
    g.connect(nd, nm);
    g.setOutput(nm);
    g.setLatencyHeadroom(512);
    g.prepare(48000.0, 256);

    Out o(256);
    AudioIo io = makeIo(o, 256);
    g.process(io);

    plugin.setLatency(192);
    g.retapLatency();

    // The crossfade block and two after it. This is the window ADR-0066's
    // original design would have had to allocate in -- a new schedule carrying
    // new delay buffers. A tap move needs none of it.
    g_allocs.store(0, std::memory_order_relaxed);
    g_counting.store(true, std::memory_order_relaxed);
    g.process(io);
    g.process(io);
    g.process(io);
    g_counting.store(false, std::memory_order_relaxed);
    eqi(g_allocs.load(std::memory_order_relaxed), 0,
        "no allocation across the switch, the crossfade, or the blocks after");

    // The counter itself has to be able to fail, or the check above is a
    // comment. A vector that grows is the same thing the old design did.
    g_allocs.store(0, std::memory_order_relaxed);
    g_counting.store(true, std::memory_order_relaxed);
    { std::vector<float> leak(4096, 1.0f); (void)leak.size(); }
    g_counting.store(false, std::memory_order_relaxed);
    check(g_allocs.load(std::memory_order_relaxed) > 0,
          "and the counter is watching -- an allocation in that window IS seen");
}

void testAZeroEdgeCanStillBeGivenADelay() {
    section("ADR-0079 -- the fast path does not swallow a pending glide");

    // An edge sitting at zero is the one most likely to need a delay next: it
    // is the direct path everything else is compensated against. The memcpy
    // fast path in accumulate() must therefore check for a pending glide
    // BEFORE it takes the shortcut, or the move is silently dropped.
    RampNode src;
    SwitchableNode plugin(0, 512);
    SumNode direct, mix;
    Graph g;
    const NodeId ns = g.addNode(src), np = g.addNode(plugin);
    const NodeId nd = g.addNode(direct), nm = g.addNode(mix);
    g.connect(ns, np);
    g.connect(ns, nd);
    g.connect(np, nm);
    g.connect(nd, nm);
    g.setOutput(nm);
    g.setLatencyHeadroom(512);
    g.prepare(48000.0, 256);

    eqi(g.compensationFor(nd, nm), 0, "nothing is compensated to begin with");

    Out o(256);
    AudioIo io = makeIo(o, 256);
    g.process(io);
    src.advance();

    plugin.setLatency(100);
    check(g.retapLatency(), "the retap fits");
    // The fast path left the edge's ring unwritten, so the glide first waits
    // a block for 100 samples of the edge's real past (ADR-0079 d2), then
    // crossfades: it is not dropped, and it does not read a stale ring.
    int blocks = 0;
    while (g.compensationFor(nd, nm) != 100 && blocks < 4) {
        g.process(io);       // must NOT take the memcpy path
        src.advance();
        ++blocks;
    }
    eqi(g.compensationFor(nd, nm), 100,
        "an edge that was at zero now carries 100 -- the glide was not dropped");
    eqi(blocks, 2, "one block held while its history is written, then the crossfade");
}

// === the coalescer (ADR-0066 d2) ==========================================

/// A graph with one switchable plugin, so a coalescer has something to retap.
struct Rig {
    RampNode src;
    SwitchableNode plugin{64, 512};
    SumNode direct, mix;
    Graph g;
    NodeId ns{}, np{}, nd{}, nm{};

    Rig() {
        ns = g.addNode(src); np = g.addNode(plugin);
        nd = g.addNode(direct); nm = g.addNode(mix);
        g.connect(ns, np);
        g.connect(ns, nd);
        g.connect(np, nm);
        g.connect(nd, nm);
        g.setOutput(nm);
        g.setLatencyHeadroom(512);
        g.prepare(48000.0, 256);
    }
};

void testABurstBecomesOneRetap() {
    section("ADR-0066 d2 -- many reports in a burst, one retap");

    Rig r;
    std::atomic<std::uint64_t> epoch{0};
    LatencyCoalescer c;
    c.attach(r.g);
    c.addSource("plugin", [&] { return epoch.load(std::memory_order_acquire); });
    c.setQuietPeriodMs(50);
    c.setMaxWaitMs(500);

    check(!c.poll(0), "nothing reported, nothing done");

    // A mode switch that reports five times over 20ms, as a real one does.
    for (int i = 0; i < 5; ++i) {
        r.plugin.setLatency(128 + i);
        epoch.fetch_add(1, std::memory_order_release);
        check(!c.poll(10 + i * 5),
              "still inside the burst at t=" + std::to_string(10 + i * 5));
    }
    eqi(c.stats().reports, 5, "all five reports were seen");
    eqi(c.stats().bursts, 1, "as ONE burst");
    check(c.pending(), "which is still open");

    check(!c.poll(60), "49ms after the last report is not yet quiet");
    check(c.poll(80), "50ms after it is");
    eqi(c.stats().retaps, 1,
        "and the graph was retapped exactly once for the whole burst");
    check(!c.pending(), "with nothing left open");

    eqi(r.g.latencySamples(), 132, "against the LAST value reported, not the first");
    check(!c.poll(200), "and a quiet poll afterwards does nothing");
    eqi(c.stats().retaps, 1, "still once");
}

void testAContinuousReporterIsNotStarved() {
    section("ADR-0066 d2 -- a plugin that never goes quiet is still acted on");

    // The failure mode a pure debounce has: report on every block, never be
    // quiet, never be acted on, and the compensation stays wrong forever while
    // the coalescer looks busy.
    Rig r;
    std::atomic<std::uint64_t> epoch{0};
    LatencyCoalescer c;
    c.attach(r.g);
    c.addSource("chatty", [&] { return epoch.load(std::memory_order_acquire); });
    c.setQuietPeriodMs(50);
    c.setMaxWaitMs(200);

    r.plugin.setLatency(200);
    bool acted = false;
    for (std::int64_t t = 0; t <= 300; t += 10) {
        epoch.fetch_add(1, std::memory_order_release);   // never quiet
        if (c.poll(t)) { acted = true; break; }
    }
    check(acted, "the ceiling fired even though the burst never went quiet");
    eqi(c.stats().maxWaitTrips, 1, "and it is counted as such, not as a normal retap");
    eqi(r.g.latencySamples(), 200, "the compensation followed");
}

void testTheQuietPeriodIsTheThingBeingTested() {
    section("ADR-0066 d2 -- the boundary, to the millisecond");

    // The clock is an argument, which is what makes this assertable at all.
    // A coalescer that read std::chrono itself could only be tested by
    // sleeping, and a sleep cannot reliably distinguish 49ms from 50.
    Rig r;
    std::atomic<std::uint64_t> epoch{0};
    LatencyCoalescer c;
    c.attach(r.g);
    c.addSource("p", [&] { return epoch.load(std::memory_order_acquire); });
    c.setQuietPeriodMs(50);

    r.plugin.setLatency(100);
    epoch.fetch_add(1, std::memory_order_release);
    check(!c.poll(1000), "the report arrives at t=1000");
    check(!c.poll(1049), "t+49 is not enough");
    check(c.poll(1050), "t+50 is");

    // Zero means now, and that is what an offline render wants.
    LatencyCoalescer imm;
    imm.attach(r.g);
    imm.addSource("p", [&] { return epoch.load(std::memory_order_acquire); });
    imm.setQuietPeriodMs(0);
    epoch.fetch_add(1, std::memory_order_release);
    check(imm.poll(2000), "with a quiet period of zero it acts on the first poll");
}

void testSourcesAreSeededSoJoiningIsNotAReport() {
    section("a device that already reported does not retap when it is registered");

    Rig r;
    std::atomic<std::uint64_t> epoch{7};      // it has lived a life already
    LatencyCoalescer c;
    c.attach(r.g);
    c.addSource("veteran", [&] { return epoch.load(std::memory_order_acquire); });

    check(!c.poll(0), "adding it reported nothing");
    eqi(c.stats().reports, 0, "because the source was SEEDED, not zeroed");
    eqi(c.stats().retaps, 0, "so loading a project does not retap once per plugin");
}

void testTwoSourcesCoalesceIntoOne() {
    section("ADR-0066 d2 -- two plugins reporting at once is still one retap");

    Rig r;
    std::atomic<std::uint64_t> a{0}, b{0};
    LatencyCoalescer c;
    c.attach(r.g);
    c.addSource("a", [&] { return a.load(std::memory_order_acquire); });
    c.addSource("b", [&] { return b.load(std::memory_order_acquire); });
    c.setQuietPeriodMs(50);
    eqi(static_cast<long long>(c.sourceCount()), 2, "two sources");

    r.plugin.setLatency(96);
    a.fetch_add(1, std::memory_order_release);
    b.fetch_add(1, std::memory_order_release);
    check(!c.poll(0), "both reported on the same poll");
    eqi(c.stats().reports, 2, "and BOTH were counted -- neither was skipped");
    eqi(c.stats().bursts, 1, "inside one burst");
    check(c.poll(60), "acted once");
    eqi(c.stats().retaps, 1, "once, for two reporters");
}

void testAReporterOnAnotherThread() {
    section("the producer has no thread affinity, and that is the whole design");

    // mac's constraint, stated plainly: CLAP's request_restart is called from
    // whatever thread the plugin picked. So the reporter does ONE thing -- bump
    // an atomic -- and the coalescer reads it on the thread it chose.
    //
    // This is also why ClapHostGlue's counters had to become atomic: they were
    // a plain ++ under a comment saying "the plugin may call this from any
    // thread", which is a data race whatever the width, and a torn 64-bit read
    // on the 32-bit CI target besides.
    Rig r;
    std::atomic<std::uint64_t> epoch{0};
    std::atomic<bool> done{false};

    LatencyCoalescer c;
    c.attach(r.g);
    c.addSource("elsewhere", [&] { return epoch.load(std::memory_order_acquire); });
    c.setQuietPeriodMs(0);

    std::thread producer([&] {
        for (int i = 0; i < 20000; ++i)
            epoch.fetch_add(1, std::memory_order_release);
        done.store(true, std::memory_order_release);
    });

    // POLL UNTIL THE PRODUCER IS DONE, not a fixed number of times. The first
    // version ran 500 polls and saw nothing at all: 500 polls of trivial work
    // finish long before a thread has even started, so the test asserted
    // "reports from another thread were seen" against a thread that had not
    // yet run. It failed for the one reason that says nothing about the code.
    std::int64_t t = 0;
    long long acted = 0;
    while (!done.load(std::memory_order_acquire))
        if (c.poll(t++)) ++acted;
    producer.join();

    if (c.poll(t++)) ++acted;    // drain whatever arrived after the last poll
    c.poll(t++);

    check(acted > 0, "reports from another thread were seen: " + std::to_string(acted));
    check(c.stats().retaps <= c.stats().reports,
          "and never more retaps than reports -- coalescing only ever reduces");
    eqi(r.g.latencySamples(), 64,
        "the graph is untouched in value, because only the COUNTER moved: a "
        "report is a hint to re-read, never the new number itself");
}

// === growing the ring (ADR-0085) ==========================================

void testTheRingGrowsWithoutLosingAudio() {
    section("ADR-0085 -- a bigger ring is primed against the old one, never swapped cold");

    // The arithmetic is spelled out because the whole claim is continuity, and
    // continuity is a statement about specific samples.
    DelayLine d;
    d.prepare(1, 4);
    d.setDelay(2);

    auto run = [&d](std::vector<float> in) {
        std::vector<float> out(in.size(), -1.0f);
        d.process(0, in.data(), out.data(), static_cast<std::int32_t>(in.size()));
        d.endEdge();
        return out;
    };

    std::vector<float> o = run({1, 2, 3, 4, 5, 6, 7, 8});
    check(o[2] == 1.0f && o[7] == 6.0f,
          "delayed by two to start with: got " + std::to_string(o[2]) + ", " +
              std::to_string(o[7]));

    // A delay of 10 does not fit a capacity of 4. The history for it was never
    // stored, so there is nothing to copy and the wait is not an implementation
    // shortcut -- it is the data not existing.
    check(!d.beginGlide(10), "a tap at 10 does not fit a ring of 4");
    check(d.offerRing(std::vector<float>(17, 0.0f), 16, 10),
          "so it is handed a bigger ring instead");
    check(d.growing(), "which is now in flight");
    eqi(d.primeRemaining(), 10, "needing ten samples of history");
    eqi(d.delay(), 2, "while STILL DELAYING BY TWO -- the old tap keeps serving");

    // Priming. The output must be the old compensation throughout: continuous,
    // stale by the difference, and above all not silence. Silence here would be
    // a dropout every time a plugin went linear-phase.
    o = run({9, 10, 11, 12});
    check(o[0] == 7.0f && o[3] == 10.0f,
          "block 1 of priming is still the old tap: got " + std::to_string(o[0]) +
              ".." + std::to_string(o[3]));
    eqi(d.primeRemaining(), 6, "six samples still needed");

    o = run({13, 14, 15, 16});
    check(o[0] == 11.0f && o[3] == 14.0f, "block 2, unbroken");
    o = run({17, 18, 19, 20});
    check(o[0] == 15.0f && o[3] == 18.0f, "block 3, unbroken");
    eqi(d.primeRemaining(), 0, "and now the new ring holds ten real samples");

    // The crossfade block. The two taps are eight samples apart, which is what
    // a latency change IS, so this is bracketed rather than equal to either.
    o = run({21, 22, 23, 24});
    bool bracketed = true;
    for (int i = 0; i < 4; ++i) {
        const float hi = 19.0f + static_cast<float>(i);   // old tap, delay 2
        const float lo = 11.0f + static_cast<float>(i);   // new tap, delay 10
        if (o[static_cast<std::size_t>(i)] > hi + 0.001f ||
            o[static_cast<std::size_t>(i)] < lo - 0.001f) bracketed = false;
    }
    check(bracketed, "the fade lies between the two taps");
    // NOT "equals the old tap": with a four-sample fade the first sample is
    // already a quarter of the way across, so the honest claim is that it
    // starts NEARER the old tap than the new one. A cold swap sits exactly on
    // the new tap at sample 0, so this still fails for the defect it is here
    // to catch.
    check(std::fabs(o[0] - 19.0f) < std::fabs(o[0] - 11.0f),
          "and starts nearer the OLD tap than the new -- a cold swap would sit "
          "exactly on the new one at sample 0: got " + std::to_string(o[0]));

    eqi(d.delay(), 10, "after the fade the new delay is in effect");
    eqi(d.capacity(), 16, "in the new ring");

    o = run({25, 26, 27, 28});
    check(o[0] == 15.0f && o[3] == 18.0f,
          "and the block after it is the new delay exactly: got " +
              std::to_string(o[0]) + ", want 15");
}

void testAnOfferIsRefusedWhileOneIsInFlight() {
    section("ADR-0085 -- one handover at a time");

    DelayLine d;
    d.prepare(1, 4);
    d.setDelay(2);
    check(d.offerRing(std::vector<float>(17, 0.0f), 16, 10), "the first offer is taken");
    check(!d.offerRing(std::vector<float>(33, 0.0f), 32, 20),
          "the second is refused rather than queued -- the caller retries after "
          "collecting, and two rings in flight would need three buffers to be "
          "correct");
    check(d.collectRing().empty(),
          "and nothing can be collected until the audio thread has finished");
}

void testTheAudioThreadNeverDeallocates() {
    section("ADR-0010 + ADR-0085 -- the swap allocates and frees NOTHING on the audio thread");

    DelayLine d;
    d.prepare(1, 4);
    d.setDelay(2);
    std::vector<float> in{1, 2, 3, 4}, out(4, 0.0f);
    d.process(0, in.data(), out.data(), 4);
    d.endEdge();

    // The allocation is the caller's, on the message thread. That split is the
    // entire design: the audio thread receives a ready-made buffer, writes into
    // it, and swaps two vectors -- which moves pointers and touches no
    // allocator.
    check(d.offerRing(std::vector<float>(17, 0.0f), 16, 10), "offered");

    g_allocs.store(0, std::memory_order_relaxed);
    g_counting.store(true, std::memory_order_relaxed);
    for (int b = 0; b < 6; ++b) {       // priming, the fade, and past the swap
        d.process(0, in.data(), out.data(), 4);
        d.endEdge();
    }
    g_counting.store(false, std::memory_order_relaxed);
    eqi(g_allocs.load(std::memory_order_relaxed), 0,
        "priming, the crossfade and the swap itself allocate nothing");
    eqi(d.delay(), 10, "and the handover did happen -- this is not zero because "
                       "nothing ran");

    // The old buffer is parked, not freed, and the message thread takes it.
    std::vector<float> old = d.collectRing();
    check(!old.empty(), "the retired ring comes back to the message thread");
    eqi(static_cast<long long>(old.size()), 5, "and it is the OLD one: 4 + 1");
    check(!d.growing(), "with the line free for another offer");
    check(d.offerRing(std::vector<float>(65, 0.0f), 64, 40), "which it now accepts");
}

void testAMisfitGrowsTheRingThroughTheCoalescer() {
    section("ADR-0085 -- the coalescer escalates a misfit instead of giving up");

    RampNode src;
    SwitchableNode plugin(0, 4096);
    SumNode direct, mix;
    Graph g;
    const NodeId ns = g.addNode(src), np = g.addNode(plugin);
    const NodeId nd = g.addNode(direct), nm = g.addNode(mix);
    g.connect(ns, np);
    g.connect(ns, nd);
    g.connect(np, nm);
    g.connect(nd, nm);
    g.setOutput(nm);
    g.setLatencyHeadroom(64);          // deliberately far too small
    g.prepare(48000.0, 256);

    std::atomic<std::uint64_t> epoch{0};
    LatencyCoalescer c;
    c.attach(g);
    c.addSource("linear-phase", [&] { return epoch.load(std::memory_order_acquire); });
    c.setQuietPeriodMs(0);
    check(c.autoEscalate(), "escalation is on by default");

    plugin.setLatency(2048);           // thirty-two times the headroom
    epoch.fetch_add(1, std::memory_order_release);
    check(c.poll(0), "the retap ran");
    eqi(c.stats().escalations, 1, "one edge was handed a bigger ring");
    eqi(static_cast<long long>(g.growingEdges()), 1, "and has an offer in flight");
    check(!c.rebuildNeeded(),
          "and NO rebuild was demanded -- a size problem is fixed by growing, "
          "which is the whole of ADR-0085");
    check(c.lastReporter() == std::string("linear-phase"), "the culprit is named");

    // Run it out. Priming is 2048 samples = eight blocks of 256, then one
    // block of crossfade.
    Out o(256);
    AudioIo io = makeIo(o, 256);
    int blocks = 0;
    auto run = [&](int n) { for (int b = 0; b < n; ++b) { g.process(io); src.advance(); ++blocks; } };
    run(12);

    eqi(g.compensationFor(nd, nm), 2048,
        "the direct path now carries the full 2048 samples");
    eqi(g.latencySamples(), 2048, "and the graph reports it");

    eqi(static_cast<long long>(c.stats().ringsReclaimed), 0, "nothing reclaimed yet");
    c.poll(1000);
    eqi(c.stats().ringsReclaimed, 1, "a poll reclaims the retired ring");
    eqi(static_cast<long long>(g.growingEdges()), 0, "and the edge is settled");

    // THE NEW RING CARRIES HEADROOM TOO, and that is not decoration. A plugin
    // that steps its latency up in stages -- which a mode switch with an
    // oversampling option does -- would otherwise escalate on every step, and
    // every escalation costs a priming window during which the compensation is
    // stale. Growing to exactly the requirement guarantees the next sample of
    // movement misses again.
    plugin.setLatency(2080);
    epoch.fetch_add(1, std::memory_order_release);
    check(c.poll(2000), "a further small step is retapped");
    eqi(c.stats().escalations, 1,
        "and does NOT escalate again -- it fits the headroom the grown ring "
        "was given");
    eqi(static_cast<long long>(g.growingEdges()), 0, "so nothing is in flight");

    // Alignment, once both paths have filled. This is the point of all of it:
    // a plugin went linear-phase mid-session and the kick still lines up.
    run(16);
    // Settled at 2080 now, after the second step above.
    // `o` holds the LAST block rendered, so its first sample is at
    // (blocks - 1) * 256. Counting the blocks rather than writing the product
    // by hand, because the first version of this line was one block out and
    // the failure read as a misalignment rather than as arithmetic.
    const std::int64_t t0 = static_cast<std::int64_t>(blocks - 1) * 256;
    bool aligned = true;
    std::size_t bad = 0;
    for (std::size_t i = 0; i < o.l.size(); ++i) {
        const float want = 2.0f * (static_cast<float>(t0 + static_cast<std::int64_t>(i)) - 2080.0f);
        if (o.l[i] != want) { aligned = false; bad = i; break; }
    }
    check(aligned, "and the two paths sum in phase again" +
                       (aligned ? std::string()
                                : " -- sample " + std::to_string(bad) + " is " +
                                      std::to_string(o.l[bad])));
}

void testAShapeChangeEscalatesStraightToARebuild() {
    section("ADR-0084 -- CLAP says WHY it wants a restart, and the two are not the same");

    // clap_host_latency.changed and clap_host_audio_ports.rescan are different
    // notifications. Treating both as "re-read the latency" throws away the
    // only information that says a retap cannot possibly help: a port-layout
    // change is a different graph, not a number that moved.
    Rig r;
    std::atomic<std::uint64_t> latency{0}, ports{0};
    LatencyCoalescer c;
    c.attach(r.g);
    c.addSource("latency", [&] { return latency.load(std::memory_order_acquire); },
                LatencyCoalescer::Kind::Latency);
    c.addSource("ports", [&] { return ports.load(std::memory_order_acquire); },
                LatencyCoalescer::Kind::Shape);
    c.setQuietPeriodMs(0);

    // A latency report alone: the cheap path, and no rebuild.
    r.plugin.setLatency(96);
    latency.fetch_add(1, std::memory_order_release);
    check(c.poll(0), "the latency report is acted on");
    eqi(r.g.latencySamples(), 96, "and the compensation followed");
    check(!c.rebuildNeeded(), "with no rebuild demanded");
    eqi(c.stats().shapeReports, 0, "and nothing counted as a shape change");

    // A port report: no retap can fix a different topology.
    ports.fetch_add(1, std::memory_order_release);
    check(c.poll(100), "the port report is acted on too");
    eqi(c.stats().shapeReports, 1, "counted as a shape change");
    check(c.rebuildNeeded(),
          "and it escalates straight to a rebuild -- retapping a graph whose "
          "shape moved is answering the wrong question");
    eqi(c.stats().rebuildsNeeded, 1, "once, not twice");

    c.clearRebuildNeeded();
    check(!c.rebuildNeeded(), "cleared by whoever rebuilds");

    // And a burst carrying BOTH still demands the rebuild: the expensive
    // answer wins, because the cheap one cannot be sufficient.
    r.plugin.setLatency(128);
    latency.fetch_add(1, std::memory_order_release);
    ports.fetch_add(1, std::memory_order_release);
    check(c.poll(200), "a mixed burst is acted on");
    check(c.rebuildNeeded(), "and the shape half wins");
    eqi(r.g.latencySamples(), 128,
        "while the latency half is still applied -- a partial correction is "
        "closer to right than none, and the rebuild may be a frame away");

    // AND THE FLAG DOES NOT LEAK INTO THE NEXT BURST. Without this the whole
    // distinction collapses after the first port change: every later latency
    // report would demand a rebuild, and the cheap path would exist but never
    // be taken again. Asserting the shape case alone cannot see that -- it
    // takes a LATENCY-ONLY burst afterwards.
    c.clearRebuildNeeded();
    const std::int64_t before = c.stats().rebuildsNeeded;
    r.plugin.setLatency(160);
    latency.fetch_add(1, std::memory_order_release);
    check(c.poll(300), "a later latency-only burst is acted on");
    check(!c.rebuildNeeded(),
          "and demands NO rebuild -- the shape flag belonged to its own burst");
    eqi(c.stats().rebuildsNeeded, before, "and nothing new was counted");
    eqi(r.g.latencySamples(), 160, "with the cheap path still working");
}

void testEscalationCanBeDeclined() {
    section("ADR-0085 -- an offline render can decline the machinery");

    RampNode src;
    SwitchableNode plugin(0, 4096);
    SumNode direct, mix;
    Graph g;
    const NodeId ns = g.addNode(src), np = g.addNode(plugin);
    const NodeId nd = g.addNode(direct), nm = g.addNode(mix);
    g.connect(ns, np);
    g.connect(ns, nd);
    g.connect(np, nm);
    g.connect(nd, nm);
    g.setOutput(nm);
    g.setLatencyHeadroom(64);
    g.prepare(48000.0, 256);

    std::atomic<std::uint64_t> epoch{0};
    LatencyCoalescer c;
    c.attach(g);
    c.addSource("p", [&] { return epoch.load(std::memory_order_acquire); });
    c.setQuietPeriodMs(0);
    c.setAutoEscalate(false);

    plugin.setLatency(2048);
    epoch.fetch_add(1, std::memory_order_release);
    check(c.poll(0), "the retap ran");
    eqi(c.stats().escalations, 0, "nothing was grown");
    check(c.rebuildNeeded(),
          "and the misfit is reported instead -- a bounce has no real-time "
          "constraint and can simply rebuild from the top (ADR-0066 d5)");
    eqi(static_cast<long long>(g.growingEdges()), 0, "with no offer in flight");
}

// === ADR-0092: a ring adopts another's history ============================

void testAdoptedHistoryContinuesTheStream() {
    section("ADR-0092 -- after adopting, the new ring says exactly what the old would have");

    // The whole contract in one comparison: feed both lines the same next
    // samples, and the outputs must be identical. Not "close" -- identical,
    // because this is a copy and not an estimate.
    DelayLine old;
    old.prepare(1, 8);
    old.setDelay(3);
    std::vector<float> warm(20), sink(20);
    for (int i = 0; i < 20; ++i) warm[static_cast<std::size_t>(i)] = static_cast<float>(i + 1);
    old.process(0, warm.data(), sink.data(), 20);

    // A BIGGER ring, as a rebuild with headroom produces, at the same delay.
    DelayLine fresh;
    fresh.prepare(1, 16);
    fresh.setDelay(3);
    fresh.adoptHistory(old);

    std::vector<float> next{21, 22, 23, 24}, a(4), b(4);
    old.process(0, next.data(), a.data(), 4);
    fresh.process(0, next.data(), b.data(), 4);
    check(a == b, "identical output: old " + std::to_string(a[0]) + ".." +
                      std::to_string(a[3]) + ", new " + std::to_string(b[0]) + ".." +
                      std::to_string(b[3]));
    check(b[0] == 18.0f && b[3] == 21.0f,
          "and it is the right thing: three samples late, so 18..21");

    // Without adoption the fresh ring would have said four zeros -- the seam.
    DelayLine cold;
    cold.prepare(1, 16);
    cold.setDelay(3);
    std::vector<float> c(4);
    cold.process(0, next.data(), c.data(), 4);
    check(c[0] == 0.0f && c[2] == 0.0f,
          "whereas a ring that adopted nothing starts in silence -- which is the "
          "5120-sample hole, at a size small enough to read");
}

void testAdoptionCopiesOnlyWhatTheTapReaches() {
    section("ADR-0092 -- a swap costs in proportion to compensation, not headroom");

    // A ring holds `delay + 8192` samples by default (ADR-0088). Copying all of
    // it at every swap would make a rebuild cost scale with headroom. Only
    // what the new tap reads is copied, so a tap moved further back afterwards
    // reads silence rather than stale history -- the observable edge of the
    // rule, pinned so nobody "helpfully" copies the lot.
    DelayLine old;
    old.prepare(1, 16);
    old.setDelay(10);
    std::vector<float> warm(40), sink(40);
    for (int i = 0; i < 40; ++i) warm[static_cast<std::size_t>(i)] = static_cast<float>(i + 1);
    old.process(0, warm.data(), sink.data(), 40);

    DelayLine fresh;
    fresh.prepare(1, 16);
    fresh.setDelay(3);
    fresh.adoptHistory(old);        // carries 3, not 16

    fresh.setDelay(10);             // reach back past what was carried
    std::vector<float> one{41}, out(1, -1.0f);
    fresh.process(0, one.data(), out.data(), 1);
    check(out[0] == 0.0f,
          "a tap moved beyond the carried history reads silence, not a stale "
          "copy -- only three samples crossed: got " + std::to_string(out[0]));
}

void testAdoptionRefusesWhatItCannotDoHonestly() {
    section("ADR-0092 -- no history is invented");

    DelayLine old;
    old.prepare(2, 8);
    old.setDelay(4);
    std::vector<float> w(8, 1.0f), s(8);
    old.process(0, w.data(), s.data(), 8);
    old.process(1, w.data(), s.data(), 8);

    // A mono ring cannot take stereo history: which channel would it be?
    DelayLine mono;
    mono.prepare(1, 8);
    mono.setDelay(4);
    mono.adoptHistory(old);
    std::vector<float> z{0.0f}, o(1, -1.0f);
    mono.process(0, z.data(), o.data(), 1);
    check(o[0] == 0.0f, "a channel-count mismatch adopts nothing");

    // A zero tap reads the sample just written, so there is no history to take.
    DelayLine zero;
    zero.prepare(2, 8);
    zero.setDelay(0);
    zero.adoptHistory(old);
    std::vector<float> seven{7.0f}, oz(1, -1.0f);
    zero.process(0, seven.data(), oz.data(), 1);
    check(oz[0] == 7.0f, "a zero-delay ring passes the current sample, untouched by adoption");
}

// --- a grow on a live graph: one snapshot per edge --------------------------
//
// `Graph::accumulate` drives a stereo edge as below: one `beginEdge`, every
// channel, one `endEdge`. The message thread offers rings and asks for glides
// WHILE it does, so an offer can land between the left channel and the right.
// These run that interleaving on one thread, at the exact point a second
// thread could put it, which a stress run under a race detector can only hope
// to hit.

/// A stereo edge fed a stream whose every sample says where it came from:
/// left k+1, right -(k+1) at stream position k, so any output names the
/// delay it was read at, and a zero is a sample nobody stored.
struct LiveEdge {
    explicit LiveEdge(DelayLine& line) : d(line) {}
    DelayLine& d;
    std::int64_t n = 0;
    std::vector<float> l, r;

    template <typename Between>
    void block(std::int32_t frames, Between between) {
        const auto f = static_cast<std::size_t>(frames);
        std::vector<float> inL(f), inR(f);
        for (std::size_t i = 0; i < f; ++i) {
            inL[i] = static_cast<float>(n + static_cast<std::int64_t>(i) + 1);
            inR[i] = -inL[i];
        }
        l.assign(f, -9.0f);
        r.assign(f, -9.0f);
        const DelayLine::Edge e = d.beginEdge();
        d.process(e, 0, inL.data(), l.data(), frames);
        between();
        d.process(e, 1, inR.data(), r.data(), frames);
        d.endEdge(e);
        n += frames;
    }
    void block(std::int32_t frames) { block(frames, [] {}); }

    /// Every sample of the last block, both channels, lies between the
    /// stream read `nearer` and `further` back: a crossfade between taps.
    [[nodiscard]] bool between(std::int32_t nearer, std::int32_t further) const {
        const std::int64_t first = n - static_cast<std::int64_t>(l.size());
        for (std::size_t i = 0; i < l.size(); ++i) {
            const std::int64_t p = first + static_cast<std::int64_t>(i);
            const auto hi = static_cast<float>(p - nearer + 1);
            const auto lo = static_cast<float>(p - further + 1);
            if (l[i] > hi + 0.001f || l[i] < lo - 0.001f || r[i] != -l[i]) return false;
        }
        return true;
    }
    /// The last block is the crossfade from the tap `from` back to the tap
    /// `to` back, to the sample: the same arithmetic `DelayLine` does (a step
    /// of 1/frames, summed), so a tap reading a slot nobody stored shows even
    /// where the fade gives it little weight.
    [[nodiscard]] bool faded(std::int32_t from, std::int32_t to) const {
        const std::int64_t first = n - static_cast<std::int64_t>(l.size());
        const float step = 1.0f / static_cast<float>(l.size());
        float t = 0.0f;
        for (std::size_t i = 0; i < l.size(); ++i) {
            const std::int64_t p = first + static_cast<std::int64_t>(i);
            t += step;
            const auto a = static_cast<float>(p - from + 1);
            const auto b = static_cast<float>(p - to + 1);
            const float want = a + (b - a) * t;
            if (std::fabs(l[i] - want) > 0.001f || std::fabs(r[i] + want) > 0.001f) return false;
        }
        return true;
    }
    /// No sample of the last block is one nobody stored. The stream is never
    /// zero, so a zero is a slot of a ring that was never written.
    [[nodiscard]] bool noHoles() const {
        for (std::size_t i = 0; i < l.size(); ++i)
            if (l[i] == 0.0f || r[i] == 0.0f) return false;
        return true;
    }

    /// Both channels of the last block read exactly `delay` samples back.
    [[nodiscard]] bool delayedBy(std::int32_t delay) const {
        const std::int64_t first = n - static_cast<std::int64_t>(l.size());
        for (std::size_t i = 0; i < l.size(); ++i) {
            const std::int64_t at = first + static_cast<std::int64_t>(i) - delay;
            const float want = at >= 0 ? static_cast<float>(at + 1) : 0.0f;
            if (l[i] != want || r[i] != -want) return false;
        }
        return true;
    }
};

void testAnOfferBetweenTwoChannelsKeepsItsPrime() {
    section("ADR-0085 -- an offer that lands mid-edge keeps its prime count");

    DelayLine d;
    d.prepare(2, 16);
    d.setDelay(4);
    LiveEdge e(d);
    for (int b = 0; b < 3; ++b) e.block(8);

    bool taken = false;
    e.block(8, [&] { taken = d.offerRing(std::vector<float>(2 * 65, 0.0f), 64, 40); });
    check(taken, "a ring for a delay of 40 is offered between the left channel and the right");
    check(e.delayedBy(4), "that edge plays the old delay on both channels");
    // THE DEFECT. The edge's own cursors, written back after its right
    // channel, used to include the incoming ring's prime count as it stood
    // when the edge began: zero. The grow then swapped after one block, to a
    // ring that held one block of history, and played its zeros.
    eqi(d.primeRemaining(), 40, "and the offer's prime count survives the edge it landed in");

    int blocks = 0;
    bool stale = true, fade = false;
    while (d.growing() && d.delay() != 40 && blocks < 20) {
        e.block(8);
        ++blocks;
        if (d.delay() != 40 && !e.delayedBy(4)) stale = false;
        if (d.delay() == 40) fade = e.faded(4, 40);   // the block that swapped
    }
    check(stale, "priming keeps the old delay exactly, block after block");
    // Where a grow begun by the right channel alone would show: its left
    // channel never wrote the offer's block into the new ring, so its new tap
    // would read slots nobody stored there and its right channel's would not.
    check(fade, "the fade block crossfades 4 to 40 to the sample, on both channels: the "
                "edge's channels began the grow together");
    eqi(blocks, 6, "five blocks to prime forty samples, then the one that fades");
    eqi(d.delay(), 40, "and the new delay is in effect");

    e.block(8);
    check(e.delayedBy(40), "the block after the swap reads forty back on both channels, "
                           "every sample one that was stored, not a zero");
    e.block(8);
    check(e.delayedBy(40), "and the next one too");
    check(d.reclaim(), "the old ring is collected");
}

void testAnOfferAfterACollectionMidEdgeKeepsItsPrime() {
    section("ADR-0085 -- a collection and a second offer mid-edge keep the prime count");

    // The other half of the rule: after a swap the line is Spent, and the
    // message thread's next poll collects the old ring (back to Idle) and may
    // offer the next one at once. An edge that began in Spent must not write
    // back the incoming cursors either, or the second grow is the first
    // defect again.
    DelayLine d;
    d.prepare(2, 16);
    d.setDelay(4);
    LiveEdge e(d);
    for (int b = 0; b < 3; ++b) e.block(8);
    check(d.offerRing(std::vector<float>(2 * 65, 0.0f), 64, 40), "a ring for 40 is offered");
    int blocks = 0;
    while (d.capacity() != 64 && blocks < 20) { e.block(8); ++blocks; }
    e.block(8);
    check(d.growing(), "the swap is done and the old ring waits to be collected");

    bool again = false;
    e.block(8, [&] {
        again = d.reclaim() &&
                d.offerRing(std::vector<float>(2 * 201, 0.0f), 200, 100);
    });
    check(again, "between that edge's channels it is collected and a ring for 100 offered");
    eqi(d.primeRemaining(), 100, "and the second offer's prime count survives the edge it landed in");

    bool sound = true;
    blocks = 0;
    while (d.delay() != 100 && blocks < 30) {
        e.block(8);
        ++blocks;
        if (!e.noHoles()) sound = false;
    }
    check(sound, "no sample on the way to 100 is one nobody stored");
    e.block(8);
    check(e.delayedBy(100), "and the edge reads a hundred back exactly");
}

void testAGlideAskedForMidEdgeIsNotLost() {
    section("ADR-0079 -- a glide asked for while an edge glides is the next one");

    DelayLine d;
    d.prepare(2, 16);
    d.setDelay(2);
    LiveEdge e(d);
    e.block(8);
    e.block(8);

    check(d.beginGlide(6), "a glide from 2 to 6");
    bool asked = false;
    e.block(8, [&] { asked = d.beginGlide(10); });
    check(asked, "and, between that edge's channels, another to 10");
    eqi(d.delay(), 6, "the edge ends at the delay it glided to");
    check(d.gliding(), "with the newer request still pending: ending a glide used to "
                       "clear the flag whatever had been asked for meanwhile");
    eqi(d.target(), 10, "for 10");

    e.block(8);
    eqi(d.delay(), 10, "the next edge glides on to it");
    check(!d.gliding(), "and settles");
    e.block(8);
    check(e.delayedBy(10), "both channels then read ten back exactly");
}

void testAGlidePendingAtAnOfferIsSuperseded() {
    section("ADR-0085 -- an offer supersedes a glide still pending");

    DelayLine d;
    d.prepare(2, 16);
    d.setDelay(4);
    LiveEdge e(d);
    for (int b = 0; b < 3; ++b) e.block(8);

    // The latency service retaps (6 fits), the plug-in reports again before
    // a block has run, and this time the delay does not fit: a ring is offered.
    check(d.beginGlide(6), "a glide to 6 is pending");
    check(d.offerRing(std::vector<float>(2 * 65, 0.0f), 64, 40), "when a ring for 40 is offered");
    eqi(d.target(), 40, "the delay asked for is now the offer's");

    bool held = true;
    int blocks = 0;
    while (d.delay() != 40 && blocks < 20) {
        e.block(8);
        ++blocks;
        if (d.delay() != 40 && !e.delayedBy(4)) held = false;
    }
    check(held, "the old ring keeps its delay of 4 while the new one primes: the stale "
                "glide is not played as a jump on a ring that is on its way out");
    check(!d.gliding(), "and at the swap the glide is gone, not resumed");
    e.block(8);
    e.block(8);
    eqi(d.delay(), 40, "so the edge stays at the offer's delay");
    check(e.delayedBy(40), "reading forty back exactly");
}

void testAGlideDuringAGrowWaitsForTheNewRing() {
    section("ADR-0085 -- a glide asked for during a grow fits the ring that is coming");

    DelayLine d;
    d.prepare(2, 16);
    d.setDelay(4);
    LiveEdge e(d);
    for (int b = 0; b < 3; ++b) e.block(8);
    check(d.offerRing(std::vector<float>(2 * 65, 0.0f), 64, 40), "a ring for 40 is offered");
    e.block(8);
    e.block(8);

    // 8 fits both rings. It still waits: played now it would be a jump on the
    // old ring and then another at the swap, two moves for one request.
    check(d.beginGlide(8), "a glide to 8, mid-grow");
    e.block(8);
    check(e.delayedBy(4), "waits: the old ring still plays its delay of 4");

    // 50 does not fit the old ring of 16 and does fit the new one of 64, so
    // this is a tap move after the swap, not a second grow and not a rebuild.
    check(d.beginGlide(50), "a glide to 50, mid-grow, is accepted against the new ring");
    check(!d.beginGlide(65), "one past the new ring is still refused");
    check(d.beginGlide(50), "(50 again: the last request is the one that holds)");

    bool held = true;
    int blocks = 0;
    while (d.capacity() != 64 && blocks < 20) {
        e.block(8);
        ++blocks;
        if (d.capacity() != 64 && !e.delayedBy(4)) held = false;
    }
    check(held, "the old ring plays its delay of 4 until the swap, every sample");
    eqi(d.delay(), 40, "the swap lands on the offer's delay");
    check(d.gliding(), "with the glide to 50 still to come");

    // The new ring holds what was written since the offer: 48 samples at the
    // swap. A tap at 50 waits a block for them rather than reading two that
    // were never stored.
    bool sound = true;
    blocks = 0;
    while (d.delay() != 50 && blocks < 20) {
        e.block(8);
        ++blocks;
        if (!e.noHoles() || !(e.delayedBy(40) || e.faded(40, 50))) sound = false;
    }
    check(sound, "every block until it lands is the delay of 40 or the fade from it to 50, "
                 "and no sample of either is one nobody stored");
    eqi(blocks, 2, "one block held, then the fade");
    check(!d.gliding(), "and settles");
    e.block(8);
    check(e.delayedBy(50), "reading fifty back exactly: the new ring held them");
}

void testAGlideWaitsForTheHistoryItReads() {
    section("ADR-0085 -- a glide past what a fresh ring holds waits for it, held");

    // A plug-in that steps its latency twice inside one prime window: the
    // first step grows the ring for 40, the second asks for 64, which fits
    // the new ring of 64 and is far more than the 48 samples it will hold at
    // the swap. Played then, the tap would read 16 slots nobody wrote: a
    // fade into silence, then a hole. It waits until the ring holds them.
    DelayLine d;
    d.prepare(2, 16);
    d.setDelay(4);
    LiveEdge e(d);
    for (int b = 0; b < 3; ++b) e.block(8);
    check(d.offerRing(std::vector<float>(2 * 65, 0.0f), 64, 40), "a ring for 40 is offered");
    e.block(8);
    check(d.beginGlide(64), "and, a block into priming, a glide to 64 is accepted");

    bool sound = true;
    int blocks = 0;
    while (d.delay() != 64 && blocks < 30) {
        e.block(8);
        ++blocks;
        if (!e.noHoles() || !(e.delayedBy(4) || e.faded(4, 40) || e.delayedBy(40) ||
                              e.faded(40, 64)))
            sound = false;
    }
    check(sound, "not one sample on the way is one nobody stored: the old delay, the fade "
                 "to 40, 40 held, the fade to 64");
    eqi(d.delay(), 64, "and it lands");
    e.block(8);
    check(e.delayedBy(64), "reading sixty-four back exactly");
}

void testAGlideBackDuringAnOfferIsNotLost() {
    section("ADR-0085 -- a glide back to the old delay, asked for as a grow is offered");

    // Two polls inside one edge. The first asks for 40, which does not fit,
    // so a ring is offered for it. The second, the plug-in having gone back,
    // asks for 6 -- and that edge, which began gliding to 6, ends at 6. The
    // swap then lands on the offer's 40, and the request for 6 must still
    // stand: it is the last thing asked for.
    DelayLine d;
    d.prepare(2, 16);
    d.setDelay(4);
    LiveEdge e(d);
    for (int b = 0; b < 3; ++b) e.block(8);
    check(d.beginGlide(6), "a glide to 6 is pending");
    bool refused = false, offered = false, back = false;
    e.block(8, [&] {
        refused = !d.beginGlide(40);
        offered = d.offerRing(std::vector<float>(2 * 65, 0.0f), 64, 40);
        back = d.beginGlide(6);
    });
    check(refused && offered && back, "between that edge's channels: 40 refused, a ring "
                                      "offered for it, and 6 asked for again");
    eqi(d.delay(), 6, "the edge ends at 6");

    int blocks = 0;
    while (d.capacity() != 64 && blocks < 20) { e.block(8); ++blocks; }
    eqi(d.delay(), 40, "the swap lands on the offer's delay");
    check(d.gliding(), "with the glide back to 6 still standing: the swap used to see the "
                       "flag down and lose it");
    e.block(8);
    eqi(d.delay(), 6, "the next edge makes it");
    e.block(8);
    check(e.delayedBy(6), "reading six back exactly");
    eqi(d.target(), 6, "and the line says so");
}

void testALineWithNoRingIsNotOfferedOne() {
    section("ADR-0088 -- a line with no ring is rebuilt, not grown");

    // Headroom 0 gives an edge at zero no ring at all. A grow offered to it
    // would prime forever: the line is never run, so nothing counts down.
    DelayLine bare;
    bare.prepare(2, 0);
    check(!bare.offerRing(std::vector<float>(2 * 129, 0.0f), 128, 128),
          "an offer to a line with no ring is refused");
    check(!bare.growing(), "and nothing is left in flight");

    RampNode src;
    SwitchableNode plugin(0, 512);
    SumNode direct, mix;
    Graph g;
    const NodeId ns = g.addNode(src), np = g.addNode(plugin);
    const NodeId nd = g.addNode(direct), nm = g.addNode(mix);
    g.connect(ns, np);
    g.connect(ns, nd);
    g.connect(np, nm);
    g.connect(nd, nm);
    g.setOutput(nm);
    g.setLatencyHeadroom(0);
    g.prepare(48000.0, 256);
    plugin.setLatency(128);
    check(!g.retapLatency(), "with no headroom, the move does not fit");
    eqi(static_cast<long long>(g.escalateLatency()), 0,
        "and no ring is grown for the edge that has none: the caller rebuilds instead");
    eqi(static_cast<long long>(g.growingEdges()), 0, "nothing stuck in flight");
}

void testAnAdoptedLineHoldsOnlyWhatItWasHanded() {
    section("ADR-0092 -- after a handover a glide past the carried history waits for it");

    // The next graph's line, compensating the same edge by 40 after a
    // rebuild, adopts the old line's history (GraphHost::handover): forty
    // samples, in a ring of 64. A glide to 60 asked for soon after must wait
    // until the ring holds sixty real samples, not read twenty slots that
    // were zeroed when the line was prepared.
    DelayLine full;
    full.prepare(2, 64);
    full.setDelay(40);
    LiveEdge f(full);
    for (int b = 0; b < 8; ++b) f.block(8);
    DelayLine after;
    after.prepare(2, 64);
    after.setDelay(40);
    after.adoptHistory(full);
    LiveEdge g(after);
    g.n = f.n;                         // the same stream, carried on
    g.block(8);
    check(g.delayedBy(40), "the next graph's line continues the stream, forty back exactly");

    check(after.beginGlide(60), "it is asked for 60");
    bool sound = true;
    int blocks = 0;
    while (after.delay() != 60 && blocks < 20) {
        g.block(8);
        ++blocks;
        if (!g.noHoles() || !(g.delayedBy(40) || g.faded(40, 60))) sound = false;
    }
    check(sound, "every block until it lands is forty back or the fade to sixty, and no "
                 "sample of either is one nobody stored");
    eqi(blocks, 3, "two blocks held until the ring holds sixty, then the fade");
    g.block(8);
    check(g.delayedBy(60), "then sixty back exactly");
}

void testAHandoverMidGrowTakesTheRingBeingGrown() {
    section("ADR-0092 -- a handover in the middle of a grow takes the ring that holds the past");

    // The old line is growing from 16 to a ring for 40, primed and fading,
    // when a rebuild lands with the edge at 40. Its small ring holds 16
    // samples of the edge's past; the one it is growing into holds 40.
    DelayLine old;
    old.prepare(2, 16);
    old.setDelay(4);
    LiveEdge e(old);
    for (int b = 0; b < 3; ++b) e.block(8);
    check(old.offerRing(std::vector<float>(2 * 65, 0.0f), 64, 40), "a ring for 40 is offered");
    int blocks = 0;
    while (old.primeRemaining() > 0 && blocks < 20) { e.block(8); ++blocks; }
    check(old.growing() && old.capacity() == 16, "primed, not yet swapped: the old ring still plays");

    DelayLine next;
    next.prepare(2, 64);
    next.setDelay(40);
    next.adoptHistory(old);
    LiveEdge g(next);
    g.n = e.n;
    g.block(8);
    check(g.noHoles() && g.delayedBy(40),
          "the next graph's line reads forty back at once, every sample stored: it took the "
          "ring being grown, where the small one would have left twenty-four holes");
}

void testARinglessEdgeBesideAGrownOneIsRebuilt() {
    section("ADR-0088 -- an edge with no ring is rebuilt even when another edge grows");

    // Headroom 0: an edge at zero has no ring. P1 has 64 of latency, so its
    // edge into the mix is at zero and ringless; P2's and the direct path's
    // carry 64. P2 goes to 128: the direct edge is grown, and P1's needs 64
    // and has nothing to grow. The coalescer used to see "something grew"
    // and stop there, leaving P1's path 64 samples early for good.
    RampNode src;
    SwitchableNode p1(64, 512), p2(0, 512);
    SumNode direct, mix;
    Graph g;
    const NodeId ns = g.addNode(src), n1 = g.addNode(p1), n2 = g.addNode(p2);
    const NodeId nd = g.addNode(direct), nm = g.addNode(mix);
    g.connect(ns, n1);
    g.connect(ns, n2);
    g.connect(ns, nd);
    g.connect(n1, nm);
    g.connect(n2, nm);
    g.connect(nd, nm);
    g.setOutput(nm);
    g.setLatencyHeadroom(0);
    g.prepare(48000.0, 256);

    std::atomic<std::uint64_t> epoch{0};
    LatencyCoalescer c;
    c.attach(g);
    c.addSource("p2", [&] { return epoch.load(std::memory_order_acquire); });
    c.setQuietPeriodMs(0);

    p2.setLatency(128);
    epoch.fetch_add(1, std::memory_order_release);
    check(c.poll(0), "the retap ran");
    eqi(c.stats().escalations, 1, "the direct edge was grown");
    check(c.rebuildNeeded(), "and a rebuild is demanded for the edge that had nothing to grow");
}

void testAGrowTooSmallIsGrownAgain() {
    section("ADR-0085 -- a grow in flight for less than the edge now needs is grown again");

    // The plug-in goes to 1000 and the direct edge is grown for it. While that
    // ring is still priming, it goes to 3000. The edge cannot be offered a
    // second ring until the first is collected, so the coalescer waits for
    // that and grows it again -- rather than giving up and asking for a
    // rebuild, which costs every edge in the graph its history.
    RampNode src;
    SwitchableNode plugin(0, 4096);
    SumNode direct, mix;
    Graph g;
    const NodeId ns = g.addNode(src), np = g.addNode(plugin);
    const NodeId nd = g.addNode(direct), nm = g.addNode(mix);
    g.connect(ns, np);
    g.connect(ns, nd);
    g.connect(np, nm);
    g.connect(nd, nm);
    g.setOutput(nm);
    g.setLatencyHeadroom(64);
    g.prepare(48000.0, 256);

    std::atomic<std::uint64_t> epoch{0};
    LatencyCoalescer c;
    c.attach(g);
    c.addSource("plugin", [&] { return epoch.load(std::memory_order_acquire); });
    c.setQuietPeriodMs(0);
    Out o(256);
    AudioIo io = makeIo(o, 256);
    std::int64_t now = 0;
    auto step = [&] { g.process(io); src.advance(); now += 5; c.poll(now); };

    plugin.setLatency(1000);
    epoch.fetch_add(1, std::memory_order_release);
    c.poll(now);
    eqi(c.stats().escalations, 1, "the direct edge is grown for 1000");
    step();
    plugin.setLatency(3000);
    epoch.fetch_add(1, std::memory_order_release);
    c.poll(now);
    check(!c.rebuildNeeded(), "the second step asks for no rebuild");
    check(c.stats().regrowsDeferred >= 1, "it waits for the first ring instead");

    int blocks = 0;
    while (g.compensationFor(nd, nm) != 3000 && blocks < 60) { step(); ++blocks; }
    eqi(g.compensationFor(nd, nm), 3000, "the edge reaches 3000");
    eqi(c.stats().escalations, 2, "grown twice, once per step");
    check(!c.rebuildNeeded(), "and nothing was rebuilt");
}

// --- an edge at zero delay, and a glide away from it (ADR-0079 d2) ----------
//
// An edge at zero takes `Graph::accumulate`'s fast path: the source is copied
// and the edge's ring is not written, because zero compensation must cost
// nothing. So the ring stops holding the edge's history, and a glide away
// from zero used to read whatever it held the last time the edge had a delay.

/// A ramp straight into a mix, beside a path whose plug-in's latency the test
/// moves and whose own signal is silence: the mix is then exactly the ramp's
/// edge, so every output sample names the delay that edge read it at. The
/// ramp's value is its stream position.
struct ZeroEdgeRig {
    RampNode src;
    SumNode silent;                    // never fed: a source of zeros
    SwitchableNode plugin{128, 512};
    SumNode direct, mix;
    Graph g;
    NodeId ns{}, nq{}, np{}, nd{}, nm{};
    std::int32_t frames;
    Out o;
    AudioIo io;
    std::int64_t at = 0;               // the stream position after the last block

    ZeroEdgeRig(std::int32_t blockFrames, bool directFirst, Precision p)
        : frames(blockFrames), o(blockFrames), io(makeIo(o, blockFrames)) {
        ns = g.addNode(src); nq = g.addNode(silent); np = g.addNode(plugin);
        nd = g.addNode(direct); nm = g.addNode(mix);
        g.connect(ns, nd);
        g.connect(nq, np);
        // Both orders: the last live input of a double sum is added in its own
        // pass (`accumulateLast`), every other one in `accumulate`.
        if (directFirst) { g.connect(nd, nm); g.connect(np, nm); }
        else             { g.connect(np, nm); g.connect(nd, nm); }
        g.setOutput(nm);
        g.setLatencyHeadroom(512);
        g.setPrecision(p);
        g.prepare(48000.0, blockFrames);
    }
    void block() {
        g.process(io);
        src.advance();
        at += frames;
    }
    void retap(std::int32_t latency) {
        plugin.setLatency(latency);
        (void)g.retapLatency();
    }
    [[nodiscard]] std::int32_t delay() const { return g.compensationFor(nd, nm); }
    [[nodiscard]] bool delayedBy(std::int32_t d) const {
        for (std::int32_t i = 0; i < frames; ++i) {
            const auto want = static_cast<float>(at - frames + i - d);
            if (o.l[static_cast<std::size_t>(i)] != want || o.r[static_cast<std::size_t>(i)] != want)
                return false;
        }
        return true;
    }
    /// The last block is the crossfade from the tap `from` back to `to` back,
    /// both reading the ramp's REAL past: a tap reading a stale slot misses
    /// by thousands, so a tolerance for the double path's rounding is safe.
    [[nodiscard]] bool faded(std::int32_t from, std::int32_t to) const {
        const float step = 1.0f / static_cast<float>(frames);
        float t = 0.0f;
        for (std::int32_t i = 0; i < frames; ++i) {
            t += step;
            const auto p = static_cast<float>(at - frames + i);
            const float a = p - static_cast<float>(from), b = p - static_cast<float>(to);
            const float want = a + (b - a) * t;
            const float l = o.l[static_cast<std::size_t>(i)];
            if (std::fabs(l - want) > 0.05f || o.r[static_cast<std::size_t>(i)] != l) return false;
        }
        return true;
    }
};

void testAGlideFromZeroReadsTheEdgesRealPast() {
    section("ADR-0079 d2 -- a glide away from zero reads the edge's real past, never an old one");

    for (const Precision p : {Precision::Float32, Precision::Float64}) {
        for (const bool directFirst : {true, false}) {
            for (const std::int32_t frames : {256, 64}) {
                const std::string where = std::string(p == Precision::Float64 ? "at 64" : "at 32") +
                                          (directFirst ? ", summed first" : ", summed last") +
                                          ", blocks of " + std::to_string(frames);
                ZeroEdgeRig r(frames, directFirst, p);
                for (int b = 0; b < 2048 / frames; ++b) r.block();
                check(r.delayedBy(128), where + ": compensated by 128 to begin with");

                // To zero, and long enough there that the ring, had it been
                // read, would still hold audio from its 128 days.
                r.retap(0);
                for (int b = 0; b < 40 * 256 / frames; ++b) r.block();
                check(r.delayedBy(0), where + ": at zero, the ramp straight through");

                // THE DEFECT: the new tap read the ring as it was 40 blocks
                // ago -- mid-glide the output fell thousands of positions
                // behind both taps, old audio crossfaded in, and at blocks
                // shorter than the delay the block after played it outright.
                r.retap(128);
                bool real = true;
                int blocks = 0;
                while (r.delay() != 128 && blocks < 40) {
                    r.block();
                    ++blocks;
                    if (!(r.delayedBy(0) || r.faded(0, 128))) real = false;
                }
                check(real, where + ": every block on the way is the ramp at 0 or the fade "
                                    "from 0 to 128 over its real past");
                // Exactly: held until the ring holds 128 written since the
                // glide was asked for, then the one block of the fade.
                eqi(blocks, (128 + frames - 1) / frames + 1,
                    where + ": and it waits only for that past to be written, blocks");
                r.block();
                check(r.delayedBy(128), where + ": then 128 exactly");
            }
        }
    }
}

class ConstNode final : public Node {
public:
    explicit ConstNode(float v) : v_(v) {}
    void process(const NodeIo& io) noexcept override {
        for (std::int32_t c = 0; c < io.channels; ++c) {
            float* o = io.out[c] + io.blockOffset;
            for (std::int32_t i = 0; i < io.frames; ++i) o[i] = v_;
        }
    }
    void set(float v) noexcept { v_ = v; }
    [[nodiscard]] const char* name() const noexcept override { return "const"; }
private:
    float v_;
};

/// A generator into a pass-through that sleeps when it goes quiet (test_graph's
/// testMixSkipsSleepingSources), into a mix beside a switchable silent path.
struct SleepRig {
    ConstNode gen{0.25f};
    SumNode sleeper;                   // tail 0: it suspends
    SumNode silent, mix;
    SwitchableNode plugin;
    Graph g;
    NodeId ng{}, ns{}, nq{}, np{}, nm{};
    Out o{64};
    AudioIo io;

    SleepRig(std::int32_t latency, bool mixAwake) : plugin(latency, 512), io(makeIo(o, 64)) {
        mix.always = mixAwake;
        ng = g.addNode(gen); ns = g.addNode(sleeper); nq = g.addNode(silent);
        np = g.addNode(plugin); nm = g.addNode(mix);
        g.connect(ng, ns);
        g.connect(ns, nm);
        g.connect(nq, np);
        g.connect(np, nm);
        g.setOutput(nm);
        g.setLatencyHeadroom(512);
        g.prepare(48000.0, 64);
    }
    void retap(std::int32_t latency) {
        plugin.setLatency(latency);
        (void)g.retapLatency();
    }
    [[nodiscard]] bool all(float v) const {
        for (std::size_t i = 0; i < 64; ++i) if (o.l[i] != v || o.r[i] != v) return false;
        return true;
    }
};

void testASleepingSourceEdgeGlidesThroughSilence() {
    section("ADR-0102 d5 -- a sleeping source's edge is not read, and a glide on it reads silence");

    // The mix stays awake; its edge at zero from the sleeper is skipped,
    // unread and unwritten. The edge's real past is silence. A ring left as it
    // was when the edge last had a delay still holds the 0.25s.
    SleepRig r(128, true);
    for (int b = 0; b < 16; ++b) r.g.process(r.io);
    check(r.o.l[60] == 0.25f, "compensated by 128, the generator's level comes through");
    r.retap(0);
    for (int b = 0; b < 8; ++b) r.g.process(r.io);
    r.gen.set(0.0f);
    const std::int64_t skipped = r.g.stats().inputsSkipped;
    for (int b = 0; b < 32; ++b) r.g.process(r.io);
    check(r.g.stats().inputsSkipped - skipped >= 30, "the source sleeps, and the mix skips it");

    r.retap(128);
    bool silent = true;
    for (int b = 0; b < 6; ++b) {
        r.g.process(r.io);
        if (!r.all(0.0f)) silent = false;
    }
    check(silent, "every block of the glide to 128 and after is silence, the edge's real past");
    eqi(r.g.compensationFor(r.ns, r.nm), 128, "and the edge is at 128");

    // The same, with the source falling asleep in the very block its edge
    // glides to zero: the edge never takes the fast path, only the sleeping
    // source's skip, and that skip must leave the ring as unwritten too.
    SleepRig q(128, true);
    for (int b = 0; b < 16; ++b) q.g.process(q.io);
    q.retap(0);
    q.gen.set(0.0f);
    for (int b = 0; b < 32; ++b) q.g.process(q.io);
    q.retap(128);
    silent = true;
    for (int b = 0; b < 6; ++b) {
        q.g.process(q.io);
        if (!q.all(0.0f)) silent = false;
    }
    check(silent, "asleep from the block it reached zero: the glide back reads silence too");
}

void testAWakingConsumerGlidesThroughWhatItReallyHeard() {
    section("ADR-0043 -- a consumer that slept, its edge at a delay, glides past it through real history");

    // Here the EDGE has a delay, 128, so its consumer drains 128 samples of
    // silence and then sleeps, and the edge is not run at all while it does:
    // its ring keeps the 0.25s from before, 128 samples further back than
    // the drained silence. The mix wakes with the generator at 0.5 and the
    // plug-in at 400 in the same block, which fits the ring: a tap 400 back
    // must not reach past the drained silence into the 0.3s.
    SleepRig r(128, false);
    r.gen.set(0.3f);   // a level no fade between the 0.5 and silence lands on
    for (int b = 0; b < 16; ++b) r.g.process(r.io);
    check(r.o.l[60] == 0.3f, "compensated by 128, the generator's level comes through");
    r.gen.set(0.0f);
    for (int b = 0; b < 4; ++b) r.g.process(r.io);   // the drain
    const int calls = r.mix.calls;
    for (int b = 0; b < 30; ++b) r.g.process(r.io);
    check(r.mix.calls == calls, "the mix slept, its edges unrun");

    r.retap(400);
    r.gen.set(0.5f);
    r.g.process(r.io);
    check(r.all(0.0f), "the block it wakes in plays the edge's past, which is silence: "
                       "sample 63 = " + std::to_string(r.o.l[63]));
    bool sound = true;
    for (int b = 0; b < 16; ++b) {
        r.g.process(r.io);
        for (std::size_t i = 0; i < 64; ++i) {
            const float v = r.o.l[i];
            // Silence, the 0.5, or a fade between them, which at 64 frames
            // is always a whole number of 128ths: any part of a 0.3 is not.
            const float k = v * 128.0f;
            if (v < 0.0f || v > 0.5f || k != std::floor(k) || r.o.r[i] != v) sound = false;
        }
    }
    check(sound, "and nothing after it plays the level from before the sleep");
    eqi(r.g.compensationFor(r.ns, r.nm), 400, "the edge lands on 400");
    r.g.process(r.io);
    check(r.all(0.5f), "playing the 0.5, 400 samples late");
}

void testAHandoverCarriesOnlyWhatTheOldRingHolds() {
    section("ADR-0092 -- a handover carries only the history the old line really holds");

    // The next graph's line, compensating the same edge by 40 after a
    // rebuild, adopts the old line's history (GraphHost::handover). The old
    // line had a delay of 40, then sat at zero: its ring still holds the
    // stream from then, which is not this edge's past any more.
    DelayLine old;
    old.prepare(2, 64);
    old.setDelay(40);
    LiveEdge e(old);
    for (int b = 0; b < 8; ++b) e.block(8);
    check(old.beginGlide(0), "the old line glides to zero");
    e.block(8);
    for (int b = 0; b < 20; ++b) old.forget();   // the fast path: unwritten, every block

    DelayLine next;
    next.prepare(2, 64);
    next.setDelay(40);
    next.adoptHistory(old);
    std::vector<float> zl(40, 0.0f), zr(40, 0.0f), ol(40, -1.0f), orr(40, -1.0f);
    const DelayLine::Cursors c0 = next.cursors();
    next.process(0, zl.data(), ol.data(), 40);
    next.setCursors(c0);
    next.process(1, zr.data(), orr.data(), 40);
    bool silent = true;
    for (std::size_t i = 0; i < 40; ++i) if (ol[i] != 0.0f || orr[i] != 0.0f) silent = false;
    check(silent, "nothing is carried: the new line plays silence for its 40, not the stream "
                  "from when the old one last had a delay: sample 0 = " + std::to_string(ol[0]));
}

/// A source that is its stream position + 1 while on and 0 while off, so a
/// nonzero sample names the position it was generated at.
class GateNode final : public Node {
public:
    void process(const NodeIo& io) noexcept override {
        for (std::int32_t c = 0; c < io.channels; ++c) {
            float* o = io.out[c] + io.blockOffset;
            for (std::int32_t i = 0; i < io.frames; ++i)
                o[i] = on ? static_cast<float>(at + io.blockOffset + i + 1) : 0.0f;
        }
    }
    [[nodiscard]] const char* name() const noexcept override { return "gate"; }
    bool on = true;
    std::int64_t at = 0;               ///< the stream position of this block's first sample
};

/// Plays its SIDECHAIN input, so the compensation of a key is heard.
class KeyNode final : public Node {
public:
    void process(const NodeIo& io) noexcept override {
        for (std::int32_t c = 0; c < io.channels; ++c) {
            float* o = io.out[c] + io.blockOffset;
            const float* k = (io.sidechain != nullptr && io.sidechain[c] != nullptr)
                                 ? io.sidechain[c] + io.blockOffset : nullptr;
            for (std::int32_t i = 0; i < io.frames; ++i) o[i] = (k != nullptr ? k[i] : 0.0f);
        }
        ++calls;
    }
    [[nodiscard]] std::int64_t tailSamples() const noexcept override { return 0; }
    [[nodiscard]] const char* name() const noexcept override { return "key"; }
    int calls = 0;
};

/// The gate into a pass-through that sleeps when it goes quiet, into a mix
/// beside a switchable silent path that sets the gate edge's compensation.
/// With `key`, the gate edge is the mix's sidechain, and the mix plays it.
struct WakeRig {
    GateNode gate;
    SumNode sleeper, silent, sum;
    KeyNode keyed;
    SwitchableNode plugin;
    Graph g;
    NodeId ng{}, ns{}, nq{}, np{}, nm{};
    bool key;
    std::int32_t frames;
    Out o;
    AudioIo io;
    std::int64_t offAt = std::numeric_limits<std::int64_t>::max();
    std::int64_t onAt = std::numeric_limits<std::int64_t>::max();

    WakeRig(std::int32_t latency, std::int32_t blockFrames, bool keyed_)
        : plugin(latency, 4096), key(keyed_), frames(blockFrames), o(blockFrames),
          io(makeIo(o, blockFrames)) {
        ng = g.addNode(gate); ns = g.addNode(sleeper); nq = g.addNode(silent);
        np = g.addNode(plugin);
        nm = key ? g.addNode(keyed) : g.addNode(sum);
        g.connect(ng, ns);
        g.connect(nq, np);
        g.connect(np, nm);
        g.connect(ns, nm, key ? Bus::Sidechain : Bus::Main);
        g.setOutput(nm);
        g.setLatencyHeadroom(2048);
        g.prepare(48000.0, blockFrames);
    }
    [[nodiscard]] int calls() const { return key ? keyed.calls : sum.calls; }
    void block() {
        g.process(io);
        gate.at += frames;
    }
    void off() { gate.on = false; offAt = gate.at; }
    void on() { gate.on = true; onAt = gate.at; }
    void retap(std::int32_t latency) {
        plugin.setLatency(latency);
        (void)g.retapLatency();
    }
    /// Every sample of the last block is the gate read exactly `d` back: what
    /// it played then, or the silence it played. A fade, a stale sample, a
    /// lost one and a late onset all miss.
    [[nodiscard]] bool exact(std::int32_t d) const {
        const std::int64_t first = gate.at - frames;
        for (std::int32_t i = 0; i < frames; ++i) {
            const std::int64_t x = first + i - d;
            const bool played = x >= 0 && (x < offAt || x >= onAt);
            const float want = played ? static_cast<float>(x + 1) : 0.0f;
            if (o.l[static_cast<std::size_t>(i)] != want || o.r[static_cast<std::size_t>(i)] != want)
                return false;
        }
        return true;
    }
    [[nodiscard]] bool quiet() const {
        for (std::int32_t i = 0; i < frames; ++i)
            if (o.l[static_cast<std::size_t>(i)] != 0.0f || o.r[static_cast<std::size_t>(i)] != 0.0f)
                return false;
        return true;
    }
};

void testAGlideAskedForWhileItsConsumerSleepsSparesTheOnset() {
    section("ADR-0043/0079 -- a glide asked for while its consumer sleeps never lands on the onset");

    // While a mix sleeps its edges are not run, so a retap -- a plug-in's
    // latency switched while the project is stopped -- leaves their glides
    // pending. The silence the mix drained is the edge's real past: a glide
    // reading no further than it goes on at the wake as if the ring had been
    // written all along. And given a block of silence before the sound, the
    // mix runs the glide there, so the wake plays at the new alignment from
    // its first sample, where at the wake it would fade the onset in, or
    // play it at the old alignment while waiting for history.
    struct Case {
        const char* what;
        std::int32_t from, to, frames;
        int gap;
    };
    const Case cases[] = {
        {"down, asked for in the block the sound comes back", 1024, 300, 256, 0},
        {"down, a block of silence before the sound", 400, 100, 256, 1},
        {"up within the drained silence, a block of it before the sound", 100, 400, 512, 1},
        {"up past the drained silence, silence enough to write it", 128, 400, 64, 8},
    };
    for (const bool key : {false, true}) {
        for (const Case& k : cases) {
            const std::string where = std::string(key ? "a key, " : "") + k.what;
            WakeRig r(k.from, k.frames, key);
            for (int b = 0; b < 16; ++b) r.block();
            check(r.exact(k.from), where + ": compensated by " + std::to_string(k.from));
            r.off();
            for (int b = 0; b < 60; ++b) r.block();
            const int asleep = r.calls();
            for (int b = 0; b < 4; ++b) r.block();
            check(r.calls() == asleep, where + ": the mix sleeps");

            r.retap(k.to);
            bool quiet = true;
            for (int b = 0; b < k.gap; ++b) {
                r.block();
                if (!r.quiet()) quiet = false;
            }
            if (k.gap > 0) {
                check(quiet, where + ": the silence before the sound stays silent");
                check(r.calls() > asleep, where + ": the mix runs the glide in it");
            }
            if (k.gap > 1)
                check(r.calls() - asleep < k.gap, where + ": and sleeps again once it has landed, "
                                                  "after " + std::to_string(r.calls() - asleep) + " blocks");

            r.on();
            bool exact = true;
            const int n = (k.to + 2 * k.frames) / k.frames + 1;
            for (int b = 0; b < n; ++b) {
                r.block();
                if (!r.exact(k.to)) exact = false;
            }
            check(exact, where + ": from the wake every sample is the sound exactly " +
                             std::to_string(k.to) + " late, its onset whole");
        }
    }
}

void testAGlideUpWhileAMixDrainsPlaysWhatIsStillDue() {
    section("ADR-0058 -- a glide up while a mix drains plays what is still due, and the wake replays nothing");

    // The mix's tail is armed on its last loud block, with the reach of that
    // moment: 128. A plug-in moving to 400 one block into the drain leaves
    // 272 more samples of the sound in the ring. The mix slept on the old
    // count, cutting them there, and its next wake, however much later,
    // played them.
    for (const bool key : {false, true}) {
        const std::string where = key ? "a key" : "an input";
        WakeRig r(128, 64, key);
        for (int b = 0; b < 16; ++b) r.block();
        r.off();
        r.block();
        r.retap(400);
        r.block();                          // the fade from 128 to 400, over the sound's real past
        bool due = true;
        for (int b = 0; b < 20; ++b) {
            r.block();
            if (!r.exact(400)) due = false;
        }
        check(due, where + ": every block after the glide is the sound 400 late, to its last sample");
        const int asleep = r.calls();
        for (int b = 0; b < 30; ++b) r.block();
        check(r.calls() == asleep, where + ": and then the mix sleeps");

        r.on();
        bool exact = true;
        for (int b = 0; b < 10; ++b) {
            r.block();
            if (!r.exact(400)) exact = false;
        }
        check(exact, where + ": the wake plays the new sound 400 late, and nothing from before the sleep");
    }
}

/// What a return's feedback loop plays back into it (ADR-0203 d3): a source
/// whose edge is the loop's own, never the project's compensation.
class LoopSource final : public Node {
public:
    void process(const NodeIo& io) noexcept override {
        for (std::int32_t c = 0; c < io.channels; ++c)
            std::memset(io.out[c] + io.blockOffset, 0, static_cast<std::size_t>(io.frames) * sizeof(float));
    }
    [[nodiscard]] bool closesFeedback() const noexcept override { return true; }
    [[nodiscard]] const char* name() const noexcept override { return "loop"; }
};

void testAFeedbackSourceIsNeverCompensated() {
    section("ADR-0203 d3 -- a feedback loop's edge is never delayed, glided or grown");

    RampNode src;
    SwitchableNode plugin(64, 4096);
    LoopSource loop;
    SumNode mix;
    Graph g;
    const NodeId ns = g.addNode(src), np = g.addNode(plugin);
    const NodeId nl = g.addNode(loop), nm = g.addNode(mix);
    g.connect(ns, np);
    g.connect(np, nm);
    g.connect(nl, nm);
    g.setOutput(nm);
    g.setLatencyHeadroom(512);
    g.prepare(48000.0, 256);
    check(g.ok(), "prepared: " + g.error());
    eqi(g.arrivalOf(nm), 64, "the return's junction arrives with its latent input");
    eqi(g.compensationFor(nl, nm), 0, "and the loop's edge is not delayed to meet it");
    check(g.edgeLine(nl, nm, Bus::Main) != nullptr && g.edgeLine(nl, nm, Bus::Main)->capacity() == 0,
          "nor given a ring: not even the headroom");

    Out o(256);
    AudioIo io = makeIo(o, 256);
    g.process(io);
    src.advance();
    plugin.setLatency(192);
    check(g.retapLatency(), "a retap to 192 fits: nothing asks the ringless loop's edge to move");
    g.process(io);
    src.advance();
    g.process(io);
    eqi(g.compensationFor(nl, nm), 0, "after a retap to 192, the loop's edge is still not delayed");

    // Past the headroom: the latent edge is the only one that moves, and it
    // takes no delay of its own, so nothing needs a bigger ring.
    plugin.setLatency(2048);
    g.retapLatency();
    Graph::Unresolved left;
    eqi(static_cast<long long>(g.escalateLatency(&left)), 0, "an escalation offers the loop's edge no ring");
    eqi(static_cast<long long>(left.ringless), 0, "nor finds it wanting one");
    eqi(static_cast<long long>(g.growingEdges()), 0, "and nothing is growing");
}

/// Says it is `latency` late, and delays nothing: a strip's track delay,
/// played as latency (ADR-0172), negative for a track played late.
class SaysLatency final : public Node {
public:
    explicit SaysLatency(std::int32_t latency) : latency_(latency) {}
    void set(std::int32_t latency) noexcept { latency_.store(latency, std::memory_order_relaxed); }
    void process(const NodeIo& io) noexcept override {
        for (std::int32_t c = 0; c < io.channels; ++c)
            for (std::int32_t i = 0; i < io.frames; ++i)
                io.out[c][io.blockOffset + i] = io.in != nullptr ? io.in[c][io.blockOffset + i] : 0.0f;
    }
    [[nodiscard]] std::int32_t latencySamples() const noexcept override {
        return latency_.load(std::memory_order_relaxed);
    }
    [[nodiscard]] const char* name() const noexcept override { return "says"; }

private:
    std::atomic<std::int32_t> latency_;
};

/// Where a return's send into itself ends (ADR-0203 d3): a sink whose input
/// is the loop's own.
class LoopSink final : public Node {
public:
    void process(const NodeIo&) noexcept override {}
    [[nodiscard]] bool closesFeedback() const noexcept override { return true; }
    [[nodiscard]] const char* name() const noexcept override { return "loop.write"; }
};

void testALoopsWriterIsNeverCompensated() {
    section("ADR-0203 d3 -- the edge into a feedback loop's writer is never delayed, glided or grown");

    // A send reports its return's track delay as latency, -64: a plain
    // consumer is delayed 64 to play it; the loop's writer is not.
    RampNode src;
    SaysLatency send(-64);
    SumNode plain, out;
    LoopSink writer;
    Graph g;
    const NodeId ns = g.addNode(src), nd = g.addNode(send);
    const NodeId np = g.addNode(plain), nw = g.addNode(writer), no = g.addNode(out);
    g.connect(ns, nd);
    g.connect(nd, np);
    g.connect(nd, nw);
    g.connect(np, no);
    g.setOutput(no);
    g.setLatencyHeadroom(512);
    g.prepare(48000.0, 256);
    check(g.ok(), "prepared: " + g.error());
    eqi(g.compensationFor(nd, np), 64, "a plain consumer plays the 64 as a delay");
    eqi(g.compensationFor(nd, nw), 0, "the loop's writer does not");
    check(g.edgeLine(nd, nw, Bus::Main) != nullptr && g.edgeLine(nd, nw, Bus::Main)->capacity() == 0, "and its edge has no ring");

    Out o(256);
    AudioIo io = makeIo(o, 256);
    g.process(io);
    src.advance();
    send.set(-192);
    check(g.retapLatency(), "a retap to -192 fits: nothing asks the writer's ringless edge to move");
    for (int b = 0; b < 3; ++b) {
        g.process(io);
        src.advance();
    }
    eqi(g.compensationFor(nd, np), 192, "after a retap to -192 the plain consumer delays 192");
    eqi(g.compensationFor(nd, nw), 0, "and the writer still nothing");

    send.set(-2048);
    g.retapLatency();
    Graph::Unresolved left;
    g.escalateLatency(&left);
    eqi(static_cast<long long>(g.growingEdges()), 1, "past the headroom only the plain consumer's edge grows");
    eqi(static_cast<long long>(left.ringless), 0, "and the writer's is not found wanting a ring");
}

}  // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::printf("adi_retap_tests -- a latency change while the graph runs\n\n");
    testAFeedbackSourceIsNeverCompensated();
    testALoopsWriterIsNeverCompensated();
    testTheTapMovesInsideItsRing();
    testTheGlideCrossfadesAndSettles();
    testWithoutHeadroomARetapIsRefused();
    testTheHeadroomCostIsMeasuredNotGuessed();
    testARetapFollowsTheNewLatency();
    testARetapAllocatesNothingOnTheAudioThread();
    testAZeroEdgeCanStillBeGivenADelay();
    testABurstBecomesOneRetap();
    testAContinuousReporterIsNotStarved();
    testTheQuietPeriodIsTheThingBeingTested();
    testSourcesAreSeededSoJoiningIsNotAReport();
    testTwoSourcesCoalesceIntoOne();
    testAReporterOnAnotherThread();
    testTheRingGrowsWithoutLosingAudio();
    testAnOfferIsRefusedWhileOneIsInFlight();
    testTheAudioThreadNeverDeallocates();
    testAMisfitGrowsTheRingThroughTheCoalescer();
    testAShapeChangeEscalatesStraightToARebuild();
    testEscalationCanBeDeclined();
    testAdoptedHistoryContinuesTheStream();
    testAdoptionCopiesOnlyWhatTheTapReaches();
    testAdoptionRefusesWhatItCannotDoHonestly();
    testAnOfferBetweenTwoChannelsKeepsItsPrime();
    testAnOfferAfterACollectionMidEdgeKeepsItsPrime();
    testAGlideAskedForMidEdgeIsNotLost();
    testAGlidePendingAtAnOfferIsSuperseded();
    testAGlideDuringAGrowWaitsForTheNewRing();
    testAGlideWaitsForTheHistoryItReads();
    testAGlideBackDuringAnOfferIsNotLost();
    testALineWithNoRingIsNotOfferedOne();
    testAnAdoptedLineHoldsOnlyWhatItWasHanded();
    testAHandoverMidGrowTakesTheRingBeingGrown();
    testARinglessEdgeBesideAGrownOneIsRebuilt();
    testAGrowTooSmallIsGrownAgain();
    testAGlideFromZeroReadsTheEdgesRealPast();
    testASleepingSourceEdgeGlidesThroughSilence();
    testAWakingConsumerGlidesThroughWhatItReallyHeard();
    testAHandoverCarriesOnlyWhatTheOldRingHolds();
    testAGlideAskedForWhileItsConsumerSleepsSparesTheOnset();
    testAGlideUpWhileAMixDrainsPlaysWhatIsStillDue();
    std::printf("\n%s -- %d checks, %d failure(s)\n",
                g_failures ? "FAILED" : "PASS", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
