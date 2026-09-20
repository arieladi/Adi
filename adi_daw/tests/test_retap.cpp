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

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>
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
        const std::int32_t before = line_.cursor();
        for (std::int32_t c = 0; c < io.channels; ++c) {
            line_.setCursor(before);
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
    d.endGlide();
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
    std::printf("\n%s -- %d checks, %d failure(s)\n",
                g_failures ? "FAILED" : "PASS", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
