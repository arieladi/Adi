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
    }
    [[nodiscard]] std::int64_t tailSamples() const noexcept override { return 0; }
    [[nodiscard]] const char* name() const noexcept override { return "sum"; }
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
    eqi(g.latencyHeadroom(), 0, "a graph has no headroom unless asked");
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
    g.process(io);           // the crossfade block -- must NOT take the memcpy path
    src.advance();
    eqi(g.compensationFor(nd, nm), 100,
        "an edge that was at zero now carries 100 -- the glide was not dropped");
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

}  // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::printf("adi_retap_tests -- a latency change while the graph runs\n\n");
    testTheTapMovesInsideItsRing();
    testTheGlideCrossfadesAndSettles();
    testWithoutHeadroomARetapIsRefused();
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
    std::printf("\n%s -- %d checks, %d failure(s)\n",
                g_failures ? "FAILED" : "PASS", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
