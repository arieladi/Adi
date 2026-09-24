// SPDX-License-Identifier: GPL-3.0-or-later
//
// The rebuild path — src/adi/engine/host.{hpp,cpp}, ADR-0089.
//
// `rebuildNeeded()` has been raised and counted since ADR-0084 and nothing has
// ever answered it. These are the answer's tests: a model goes in, a new graph
// comes out, the audio thread picks it up at a block boundary, and the one it
// was using is freed only once it has demonstrably moved past.
//
// Its own binary because it replaces global `operator new`: the claim that the
// audio thread allocates nothing across a graph swap is the claim most worth
// observing rather than asserting, and it is the wrong thing to impose on the
// suites that do not make it.

#include "adi/engine/host.hpp"
#include "adi/engine/latency.hpp"

#include <atomic>
#include <cmath>
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

using namespace adi;
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

rows::Track track(std::int64_t id, const char* kind, const char* name,
                  std::optional<std::int64_t> parent = std::nullopt) {
    rows::Track t;
    t.id = id;
    t.kind = kind;
    t.name = name;
    t.parentId = parent;
    return t;
}

rows::Routing route(std::int64_t id, std::int64_t from, std::int64_t to) {
    rows::Routing r;
    r.id = id;
    r.srcKind = "track";
    r.srcId = from;
    r.dstKind = "track";
    r.dstId = to;
    r.kind = "main";
    r.origin = "user";
    return r;
}

/// A track's content: a constant, so what reaches the master is a number the
/// test can name.
class ToneNode final : public Node {
public:
    explicit ToneNode(float v) : v_(v) {}
    void process(const NodeIo& io) noexcept override {
        for (std::int32_t c = 0; c < io.channels; ++c) {
            float* o = io.out[c] + io.blockOffset;
            for (std::int32_t i = 0; i < io.frames; ++i) o[i] = v_;
        }
    }
    void set(float v) noexcept { v_ = v; }
    // No tailSamples() override: a SOURCE inherits kInfiniteTail (ADR-0043).
    [[nodiscard]] const char* name() const noexcept override { return "tone"; }
private:
    float v_;
};

class LatentNode final : public Node {
public:
    explicit LatentNode(std::int32_t l) : latency_(l) {}
    void process(const NodeIo& io) noexcept override {
        for (std::int32_t c = 0; c < io.channels; ++c) {
            float* o = io.out[c] + io.blockOffset;
            const float* i = (io.in != nullptr && io.in[c] != nullptr)
                                 ? io.in[c] + io.blockOffset : nullptr;
            for (std::int32_t k = 0; k < io.frames; ++k)
                o[k] = (i != nullptr ? i[k] : 0.0f);
        }
    }
    [[nodiscard]] std::int64_t tailSamples() const noexcept override { return 0; }
    [[nodiscard]] std::int32_t latencySamples() const noexcept override { return latency_; }
    void setLatency(std::int32_t l) noexcept { latency_ = l; ++epoch_; }
    [[nodiscard]] std::uint64_t epoch() const noexcept { return epoch_; }
    [[nodiscard]] const char* name() const noexcept override { return "latent"; }
private:
    std::int32_t latency_;
    std::uint64_t epoch_ = 0;
};

struct Out {
    std::vector<float> l, r;
    std::vector<float*> ptrs;
    explicit Out(std::int32_t n) : l(static_cast<std::size_t>(n), -7.0f),
                                   r(static_cast<std::size_t>(n), -7.0f) {
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

rows::Model twoTracks() {
    rows::Model m;
    m.tracks.push_back(track(1, "audio", "A"));
    m.tracks.push_back(track(2, "audio", "B"));
    m.tracks.push_back(track(9, "master", "Master"));
    return m;
}

// ===========================================================================

void testSilenceBeforeAnythingIsPublished() {
    section("ADR-0089 -- a host with no graph writes silence, it does not return");

    GraphHost host;
    check(host.currentGraph() == nullptr, "nothing is live yet");

    Out o(64);
    AudioIo io = makeIo(o, 64);
    host.process(io);

    bool silent = true;
    for (float v : o.l) if (v != 0.0f) silent = false;
    check(silent,
          "the buffer is ZEROED, not left at the -7 it was filled with -- a "
          "callback that returns without writing hands the driver the previous "
          "block, repeated: got " + std::to_string(o.l[0]));
    eqi(host.stats().blocks, 1, "and the block still counted");
}

void testAModelBecomesAudio() {
    section("ADR-0089 -- plan, realise, prepare, publish, render");

    GraphHost host;
    host.setFadeFrames(0);          // no seam to hide on a first publish
    const rows::Model m = twoTracks();

    check(host.rebuild(m, RealizeOptions{}, 48000.0, 64),
          "rebuilt: " + host.error());
    eqi(host.stats().published, 1, "one graph published");
    check(host.currentGraph() != nullptr, "and it is live");

    // The realised graph exposes each track's chain head, which is where a
    // clip reader pushes (ADR-0077). Driving them is what makes this a test of
    // audio rather than of bookkeeping.
    ToneNode a(0.25f), b(0.5f);
    RealizedGraph& rg = *host.current();
    Graph& g = rg.graph();
    const NodeId na = g.addNode(a), nb = g.addNode(b);
    check(g.connect(na, rg.inputFor(1)), "track A is fed");
    check(g.connect(nb, rg.inputFor(2)), "track B is fed");
    g.prepare(48000.0, 64);
    check(g.ok(), "prepared: " + g.error());

    Out o(64);
    AudioIo io = makeIo(o, 64);
    host.process(io);
    eqi(host.stats().swaps, 1, "the audio thread picked it up");
    eqi(host.stats().blocks, 1, "in one block");

    check(o.l[0] == 0.75f,
          "and both tracks reach the master, summed: got " +
              std::to_string(o.l[0]));
    check(o.r[0] == 0.75f, "on both channels");
}

void testARebuildReplacesTheGraphTheAudioThreadUses() {
    section("ADR-0089 -- the swap reaches the audio thread at a block boundary");

    GraphHost host;
    host.setFadeFrames(0);

    rows::Model m = twoTracks();
    check(host.rebuild(m, RealizeOptions{}, 48000.0, 64), "first build");
    Graph* first = host.currentGraph();

    // A third track. This is a TOPOLOGY change -- exactly what a port rescan
    // produces and what a retap cannot answer.
    m.tracks.push_back(track(3, "audio", "C"));
    check(host.rebuild(m, RealizeOptions{}, 48000.0, 64), "second build");
    Graph* second = host.currentGraph();

    check(first != second, "a different graph object is live");
    eqi(static_cast<long long>(second->nodeCount()), 4,
        "with the new track in it -- three tracks and a master");
    eqi(host.stats().published, 2, "two published");

    Out o(64);
    AudioIo io = makeIo(o, 64);
    host.process(io);
    eqi(host.stats().swaps, 1,
        "and the audio thread took the NEWEST, not the one it missed -- it "
        "never rendered the first at all");
}

void testAFailedRebuildLeavesTheSessionPlaying() {
    section("ADR-0089 -- a model that cannot be realised changes nothing");

    GraphHost host;
    host.setFadeFrames(0);
    check(host.rebuild(twoTracks(), RealizeOptions{}, 48000.0, 64), "first build");
    Graph* good = host.currentGraph();
    const std::int64_t published = host.stats().published;

    // A cycle. The planner finds it and realisation refuses to construct
    // anything (ADR-0077 d4).
    rows::Model bad = twoTracks();
    bad.routing.push_back(route(100, 1, 2));
    bad.routing.push_back(route(101, 2, 1));
    check(!host.rebuild(bad, RealizeOptions{}, 48000.0, 64), "the cycle is refused");
    check(!host.error().empty(), "with a reason: " + host.error());
    eqi(host.stats().refused, 1, "counted as refused");
    eqi(host.stats().published, published, "and NOTHING new was published");
    check(host.currentGraph() == good,
          "the session is still playing the graph it was playing -- swapping "
          "first and discovering second turns a bad edit into silence");

    // A model with no master at all: the same rule, a different reason.
    rows::Model headless;
    headless.tracks.push_back(track(1, "audio", "A"));
    check(!host.rebuild(headless, RealizeOptions{}, 48000.0, 64), "no master is refused");
    eqi(host.stats().refused, 2, "counted");
    check(host.currentGraph() == good, "and still playing");

    Out o(64);
    AudioIo io = makeIo(o, 64);
    host.process(io);
    eqi(host.stats().swaps, 1, "the audio thread saw one graph, not three");
}

void testAGraphThatCannotBePreparedIsNotPublished() {
    section("ADR-0089 -- realisation succeeding is not the same as prepare succeeding");

    // The two failure gates are separate and both have to hold. Realisation
    // refuses a cycle or a missing master; `prepare` refuses everything that
    // depends on the RUN, and a block size of zero is the one a driver can
    // actually hand us (ADR-0049). Publishing between the two would put a
    // graph with no buffers in front of the audio thread.
    GraphHost host;
    host.setFadeFrames(0);
    const rows::Model m = twoTracks();
    check(host.rebuild(m, RealizeOptions{}, 48000.0, 64), "a good build first");
    Graph* good = host.currentGraph();
    const std::int64_t published = host.stats().published;

    check(!host.rebuild(m, RealizeOptions{}, 48000.0, 0),
          "a block size of zero is refused");
    check(!host.error().empty(), "with prepare's reason: " + host.error());
    eqi(host.stats().published, published, "and nothing was published");
    check(host.currentGraph() == good, "the session is still on the good graph");

    Out o(64);
    AudioIo io = makeIo(o, 64);
    host.process(io);
    eqi(host.stats().swaps, 1, "the audio thread never saw the unprepared one");
}

void testTheHeadroomIsATime() {
    section("ADR-0157 d3 -- the latency headroom is a time: 8192 samples at 48 kHz, the same 171 ms at every rate");
    const struct { double rate; std::int32_t want; } cases[] = {
        {44100.0, 8192}, {48000.0, 8192}, {96000.0, 16384},
        {192000.0, 32768}, {384000.0, 65536}, {768000.0, 131072},
    };
    for (const auto& c : cases) {
        GraphHost host;
        const bool built = host.rebuild(twoTracks(), RealizeOptions{}, c.rate, 64);
        const RealizedGraph* rg = host.current();
        const std::int32_t got = rg != nullptr ? rg->graph().latencyHeadroom() : -1;
        check(built && got == c.want,
              std::to_string(static_cast<int>(c.rate)) + " Hz: headroom " + std::to_string(got) +
                  ", want " + std::to_string(c.want));
    }
    GraphHost off;
    off.setLatencyHeadroom(0);
    check(off.latencyHeadroomAt(192000.0) == 0, "zero stays zero: switching compensation headroom off is not scaled back on");
}

void testTheSwapIsFadedIn() {
    section("ADR-0089 -- the incoming graph is ramped up, not cut in");

    // A constant makes the ramp visible as a ramp. The fade is on the HOST's
    // output, after the graph has rendered, so it does not care what produced
    // the signal.
    GraphHost host;
    host.setFadeFrames(16);

    rows::Model m;
    m.tracks.push_back(track(9, "master", "Master"));
    check(host.rebuild(m, RealizeOptions{}, 48000.0, 64), "first build");

    // THE FIRST GRAPH IS NOT FADED, and that has to be observable or the rule
    // is untested: fading silence looks exactly like not fading it. So the
    // master is fed before the first block, and the first block must be at
    // full level from sample 0.
    ToneNode first(1.0f);
    {
        RealizedGraph& rg0 = *host.current();
        Graph& g0 = rg0.graph();
        const NodeId n0 = g0.addNode(first);
        check(g0.connect(n0, rg0.inputFor(9)), "the master is fed before the first block");
        g0.prepare(48000.0, 64);
    }
    Out o(64);
    AudioIo io = makeIo(o, 64);
    host.process(io);
    check(o.l[0] == 1.0f && o.l[63] == 1.0f,
          "the FIRST graph plays at full level from sample 0 -- there is "
          "nothing to fade from, and ramping the opening of every session is "
          "an artefact rather than the absence of one: got " +
              std::to_string(o.l[0]));

    // Rebuild, then hand the new master a tone so there is something to ramp.
    check(host.rebuild(m, RealizeOptions{}, 48000.0, 64), "second build");
    ToneNode tone(1.0f);
    RealizedGraph& rg = *host.current();
    Graph& g = rg.graph();
    const NodeId nt = g.addNode(tone);
    check(g.connect(nt, rg.inputFor(9)), "the master is fed a constant");
    g.prepare(48000.0, 64);

    Out o2(64);
    AudioIo io2 = makeIo(o2, 64);
    host.process(io2);

    // The graph is a bare master fed by a tone, so its output is 1.0 -- and the
    // first 16 samples are scaled by the ramp.
    check(o2.l[0] < o2.l[8] && o2.l[8] < o2.l[15],
          "the first samples rise: " + std::to_string(o2.l[0]) + ", " +
              std::to_string(o2.l[8]) + ", " + std::to_string(o2.l[15]));
    check(o2.l[0] < 0.2f,
          "starting near silence rather than at full level: got " +
              std::to_string(o2.l[0]));
    check(o2.l[16] == o2.l[63],
          "and past the fade it is flat again: " + std::to_string(o2.l[16]) +
              " vs " + std::to_string(o2.l[63]));

    // The block AFTER the swap is not faded again. Restarting the ramp per
    // block would mean it never finishes.
    Out o3(64);
    AudioIo io3 = makeIo(o3, 64);
    host.process(io3);
    check(o3.l[0] == o3.l[63], "the next block is not ramped at all");
}

void testAFadeLongerThanTheBlockCarriesAcross() {
    section("ADR-0089 -- a fade spanning several blocks finishes once");

    GraphHost host;
    host.setFadeFrames(96);        // longer than the 32-frame block

    rows::Model m;
    m.tracks.push_back(track(9, "master", "Master"));
    check(host.rebuild(m, RealizeOptions{}, 48000.0, 32), "first build");
    Out warm(32);
    AudioIo wio = makeIo(warm, 32);
    host.process(wio);

    check(host.rebuild(m, RealizeOptions{}, 48000.0, 32), "second build");
    ToneNode tone(1.0f);
    RealizedGraph& rg = *host.current();
    Graph& g = rg.graph();
    const NodeId nt = g.addNode(tone);
    check(g.connect(nt, rg.inputFor(9)), "the master is fed a constant");
    g.prepare(48000.0, 32);

    float last = -1.0f;
    bool rising = true;
    for (int b = 0; b < 3; ++b) {
        Out o(32);
        AudioIo io = makeIo(o, 32);
        host.process(io);
        for (float v : o.l) { if (v < last) rising = false; last = v; }
    }
    check(rising, "the ramp rises monotonically across all three blocks");
    check(last > 0.99f, "and reaches full level by the end: " + std::to_string(last));

    Out o(32);
    AudioIo io = makeIo(o, 32);
    host.process(io);
    check(o.l[0] == o.l[31] && o.l[0] > 0.99f,
          "the fourth block is flat at full level");
}

void testTheAudioThreadAllocatesNothingAcrossASwap() {
    section("ADR-0010 -- building is the message thread's job, rendering is not");

    GraphHost host;
    host.setFadeFrames(32);
    rows::Model m = twoTracks();
    check(host.rebuild(m, RealizeOptions{}, 48000.0, 64), "first build");

    Out o(64);
    AudioIo io = makeIo(o, 64);
    host.process(io);

    m.tracks.push_back(track(3, "audio", "C"));
    check(host.rebuild(m, RealizeOptions{}, 48000.0, 64), "second build");

    // The swap block, the fade, and two after it.
    g_allocs.store(0, std::memory_order_relaxed);
    g_counting.store(true, std::memory_order_relaxed);
    host.process(io);
    host.process(io);
    host.process(io);
    g_counting.store(false, std::memory_order_relaxed);
    eqi(g_allocs.load(std::memory_order_relaxed), 0,
        "picking up a new graph, fading it in and rendering allocate nothing");
    eqi(host.stats().swaps, 2, "and the swap did happen");

    // The counter has to be able to fail, or the check above is a comment.
    g_allocs.store(0, std::memory_order_relaxed);
    g_counting.store(true, std::memory_order_relaxed);
    { std::vector<float> leak(1024, 0.0f); (void)leak.size(); }
    g_counting.store(false, std::memory_order_relaxed);
    check(g_allocs.load(std::memory_order_relaxed) > 0, "and the counter is watching");
}

void testTheRetiredGraphIsFreedOnlyOnceTheReaderHasMovedPast() {
    section("ADR-0019 -- reclamation is strictly-greater, and this is why");

    GraphHost host;
    host.setFadeFrames(0);
    rows::Model m = twoTracks();
    check(host.rebuild(m, RealizeOptions{}, 48000.0, 64), "first build");

    Out o(64);
    AudioIo io = makeIo(o, 64);
    host.process(io);                       // the reader is now on graph 1

    m.tracks.push_back(track(3, "audio", "C"));
    check(host.rebuild(m, RealizeOptions{}, 48000.0, 64), "second build");

    eqi(static_cast<long long>(host.collect()), 0,
        "graph 1 is NOT freed yet -- the reader has not been seen past it, and "
        "freeing on >= would free the one it is inside");

    host.process(io);                       // the reader moves to graph 2
    eqi(static_cast<long long>(host.collect()), 1, "now graph 1 is freed");
    eqi(host.stats().reclaimed, 1, "and counted");
    eqi(static_cast<long long>(host.collect()), 0, "a second collect frees nothing");
}

void testTheCoalescerSurvivesARebuild() {
    section("ADR-0089 -- attaching to the HOST, because a graph is replaced");

    // The bug this prevents: LatencyCoalescer::attach(Graph&) stores a raw
    // pointer, GraphHost::collect() frees that graph, and the next poll reads
    // freed memory. Attaching to the host re-reads the graph every poll.
    GraphHost host;
    host.setFadeFrames(0);
    rows::Model m = twoTracks();
    check(host.rebuild(m, RealizeOptions{}, 48000.0, 64), "first build");

    std::atomic<std::uint64_t> epoch{0};
    LatencyCoalescer c;
    c.attach(host);
    c.addSource("p", [&] { return epoch.load(std::memory_order_acquire); });
    c.setQuietPeriodMs(0);

    Out o(64);
    AudioIo io = makeIo(o, 64);
    host.process(io);

    epoch.fetch_add(1, std::memory_order_release);
    check(c.poll(0), "a report before the rebuild is acted on");
    eqi(c.stats().retaps, 1, "once");

    // Rebuild and free the graph the coalescer would have been holding.
    m.tracks.push_back(track(3, "audio", "C"));
    check(host.rebuild(m, RealizeOptions{}, 48000.0, 64), "rebuilt");
    host.process(io);
    eqi(static_cast<long long>(host.collect()), 1, "the old graph is gone");

    epoch.fetch_add(1, std::memory_order_release);
    check(c.poll(100), "and a report AFTER it is still acted on");
    eqi(c.stats().retaps, 2, "twice, against the graph that now exists");
    check(!c.rebuildNeeded(), "with no rebuild demanded by a latency report");
}

void testRebuildNeededFinallyHasAnAnswer() {
    section("ADR-0084 + ADR-0089 -- a shape report now ends in a rebuild");

    // The loop this closes. A port rescan raises rebuildNeeded(); the owner
    // rebuilds and clears it. Before this existed the flag was raised, counted,
    // and ignored.
    GraphHost host;
    host.setFadeFrames(0);
    rows::Model m = twoTracks();
    check(host.rebuild(m, RealizeOptions{}, 48000.0, 64), "first build");

    std::atomic<std::uint64_t> ports{0};
    LatencyCoalescer c;
    c.attach(host);
    c.addSource("ports", [&] { return ports.load(std::memory_order_acquire); },
                LatencyCoalescer::Kind::Shape);
    c.setQuietPeriodMs(0);

    Out o(64);
    AudioIo io = makeIo(o, 64);
    host.process(io);

    ports.fetch_add(1, std::memory_order_release);
    check(c.poll(0), "the shape report is acted on");
    check(c.rebuildNeeded(), "and demands a rebuild");

    const std::int64_t before = host.stats().published;
    if (c.rebuildNeeded()) {
        check(host.rebuild(m, RealizeOptions{}, 48000.0, 64), "which the host performs");
        c.clearRebuildNeeded();
    }
    eqi(host.stats().published, before + 1, "one more graph published");
    check(!c.rebuildNeeded(), "and the flag is cleared by whoever answered it");

    host.process(io);
    eqi(host.stats().swaps, 2, "the audio thread is on the new graph");
}

void testProblemsSurviveWithoutHoldingTheGraph() {
    section("ADR-0089 -- what could not be planned is reported by the host");

    GraphHost host;
    rows::Model m = twoTracks();
    m.routing.push_back(route(100, 1, 404));     // a track that is not there

    check(host.rebuild(m, RealizeOptions{}, 48000.0, 64),
          "the rest of the project is still realised: " + host.error());
    check(!host.problems().empty(),
          "and the dangling route is named without the caller holding the graph");
}

// === ADR-0092: a rebuild keeps the ring history ===========================

/// A source INSIDE a chain: ignores its input, writes a constant. Injected as
/// a device, so a rebuild re-injects the same object and the model alone
/// decides the graph -- nothing is added by hand after each swap.
class DcDevice final : public Node {
public:
    explicit DcDevice(float v) : v_(v) {}
    void process(const NodeIo& io) noexcept override {
        for (std::int32_t c = 0; c < io.channels; ++c) {
            float* o = io.out[c] + io.blockOffset;
            for (std::int32_t i = 0; i < io.frames; ++i) o[i] = v_;
        }
    }
    // A SOURCE: inherits kInfiniteTail (ADR-0043).
    [[nodiscard]] const char* name() const noexcept override { return "dc"; }
private:
    float v_;
};

/// Delays by `latency` and says so -- and, like `ClapDevice` since ADR-0090,
/// does NOT reset its own history when prepared again at the same rate and
/// size. Without that the rebuild would re-prime it, and the seam would be the
/// PLUGIN's: the other cause, which mac had to separate before this one showed.
class LatentDevice final : public Node {
public:
    explicit LatentDevice(std::int32_t l) : latency_(l) {}
    void prepare(double sr, std::int32_t frames) override {
        if (sr == sr_ && frames == frames_) return;
        sr_ = sr;
        frames_ = frames;
        line_.prepare(2, latency_);
        line_.setDelay(latency_);
        ++primes;
    }
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
        line_.endEdge();
    }
    [[nodiscard]] std::int32_t latencySamples() const noexcept override { return latency_; }
    [[nodiscard]] std::int64_t tailSamples() const noexcept override { return kInfiniteTail; }
    [[nodiscard]] const char* name() const noexcept override { return "latent"; }
    int primes = 0;
private:
    std::int32_t latency_;
    double sr_ = 0.0;
    std::int32_t frames_ = 0;
    DelayLine line_;
};

struct Seam {
    double settled = 0.0;
    std::int64_t below = 0;      ///< samples after the swap under 99% of settled
    double worst = 0.0;
    std::int64_t carried = 0;
    int primes = 0;
};

/// mac's `adi_clap_probe --seam`, rebuilt from fixtures: a wet track through
/// 5120 samples of latency and a dry track compensated to meet it, DC on both
/// at DISTINGUISHABLE levels -- 0.25 wet, 0.75 dry -- so the level during a
/// hole names which path went missing.
Seam measureSeam(bool keepHistory, bool wetOnly, int rebuildsInARow,
                 bool collectBetween = false, int cycles = 1) {
    GraphHost host;
    host.setKeepHistory(keepHistory);
    DcDevice wet(0.25f), dry(wetOnly ? 0.0f : 0.75f);
    LatentDevice latent(5120);

    RealizeOptions opts;
    opts.devicesFor = [&](std::int64_t id) -> std::vector<Node*> {
        if (id == 1) return {&wet, &latent};
        if (id == 2) return {&dry};
        return {};
    };
    const rows::Model m = twoTracks();

    Seam s;
    if (!host.rebuild(m, opts, 48000.0, 512)) return s;
    Out o(512);
    AudioIo io = makeIo(o, 512);
    for (int b = 0; b < 30; ++b) host.process(io);     // well past 5120
    s.settled = std::fabs(static_cast<double>(o.l[511]));
    s.worst = s.settled;

    for (int c = 0; c < cycles; ++c) {
        for (int r = 0; r < rebuildsInARow; ++r) host.rebuild(m, opts, 48000.0, 512);
        for (int b = 0; b < 30; ++b) {
            host.process(io);
            if (collectBetween) host.collect();
            for (float v : o.l) {
                const double a = std::fabs(static_cast<double>(v));
                if (a < s.settled * 0.99) {
                    ++s.below;
                    if (a < s.worst) s.worst = a;
                }
            }
        }
    }
    s.carried = host.stats().historyCarried;
    s.primes = latent.primes;
    return s;
}

void testWithoutHistoryTheSeamIsExactlyTheCompensation() {
    section("ADR-0092 -- the BEFORE: mac's 5120 samples, reproduced in the suite");

    // The measurement has to be able to see the defect or the next test proves
    // nothing. So first, with history carrying switched OFF, the number mac
    // measured on Pro-Q 3 must come out -- exactly, because the dry edge's
    // ring is 5120 samples of silence and nothing else is wrong.
    const Seam s = measureSeam(false, false, 1);
    check(s.settled > 0.99 && s.settled < 1.01,
          "settled at 0.25 wet + 0.75 dry = 1.0: got " + std::to_string(s.settled));
    eqi(s.below, 5120,
        "the master sits below settled for EXACTLY the dry edge's 5120 samples");
    check(s.worst > 0.24 && s.worst < 0.26,
          "and at 0.25 while it does -- the WET path alone, so it is the dry ring "
          "that emptied: got " + std::to_string(s.worst));
    eqi(s.primes, 1,
        "the latent plugin was primed ONCE -- the rebuild did not re-prime it, so "
        "this seam is the ring's and not the plugin's (ADR-0090)");
}

void testARebuildKeepsTheRingHistory() {
    section("ADR-0092 -- the AFTER: a rebuild that changes nothing audible is seamless");

    const Seam s = measureSeam(true, false, 1);
    eqi(s.below, 0,
        "not one sample below settled -- the dry edge took the old ring's history");
    check(s.carried >= 1, "and the host says it carried: " + std::to_string(s.carried));
}

void testTheControlHasNoSeamEitherWay() {
    section("ADR-0092 -- the CONTROL: with no dry path there was never a seam");

    // mac's --wet-only. If this showed a hole the fix would be aimed at the
    // wrong thing; that it shows none with history OFF is what makes the 5120
    // above the dry ring's and nobody else's.
    eqi(measureSeam(false, true, 1).below, 0, "wet only, history off: no hole");
    eqi(measureSeam(true, true, 1).below, 0, "wet only, history on: still none");
}

void testHistoryComesFromTheGraphThatActuallyRan() {
    section("ADR-0092 -- two rebuilds between blocks carry from the graph that RAN");

    // The audio thread goes from graph 1 straight to graph 3; graph 2 was
    // published and superseded before any block rendered it, so its rings hold
    // nothing. Carrying from "the last one published" would carry silence.
    // Carrying from the last one RENDERED carries the music.
    eqi(measureSeam(true, false, 2).below, 0, "no seam across a skipped graph");
    eqi(measureSeam(true, false, 5).below, 0, "nor across four skipped graphs");
}

void testHistoryIsCarriedAcrossRepeatedRebuilds() {
    section("ADR-0092 -- rebuild, render, collect, repeat: every swap is seamless");

    // Collecting between swaps is what frees each retired graph, so a host
    // that kept carrying from a stale pointer would read freed memory here --
    // the defect that only appears on the SECOND swap, never the first.
    eqi(measureSeam(true, false, 1, true, 4).below, 0,
        "four rebuilds, each collected, none audible");
}

void testTheSameEdgeIsMatchedHoweverTheRowSaysIt() {
    section("ADR-0092 -- an edge is matched by what it IS, not by how the model spells it");

    // The planner emits explicit routing rows first and ADR-0065's defaults
    // after, so edge order depends on how each route happens to be spelled.
    // Here the DRY track (id 1, the compensated one) routes by default, and
    // the wet track (id 2) by an explicit row naming the same master its
    // default would -- identical audio, different emission order.
    //
    // The audio thread matches two graphs' edges in one merge walk, which is
    // only correct over lists SORTED by key. Unsorted, the old graph lists
    // (2->9, 1->9) and the new one (1->9, 2->9); the walk passes 1->9 in the
    // new list before reaching it in the old, and the one edge that has
    // history to carry is the one it skips.
    GraphHost host;
    DcDevice dry(0.75f), wet(0.25f);
    LatentDevice latent(5120);
    RealizeOptions opts;
    opts.devicesFor = [&](std::int64_t id) -> std::vector<Node*> {
        if (id == 1) return {&dry};
        if (id == 2) return {&wet, &latent};
        return {};
    };

    rows::Model spelled = twoTracks();
    spelled.routing.push_back(route(100, 2, 9));     // says what the default says
    const rows::Model plain = twoTracks();

    check(host.rebuild(spelled, opts, 48000.0, 512), "built with the explicit row");
    Out o(512);
    AudioIo io = makeIo(o, 512);
    for (int b = 0; b < 30; ++b) host.process(io);
    const double settled = std::fabs(static_cast<double>(o.l[511]));
    check(settled > 0.99 && settled < 1.01, "settled at 1.0: " + std::to_string(settled));

    check(host.rebuild(plain, opts, 48000.0, 512), "rebuilt with the row removed");
    std::int64_t below = 0;
    for (int b = 0; b < 30; ++b) {
        host.process(io);
        for (float v : o.l)
            if (std::fabs(static_cast<double>(v)) < settled * 0.99) ++below;
    }
    eqi(below, 0,
        "no seam: 1->9 is the same edge whether a row or the default produced it");
}

}  // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::printf("adi_host_tests -- the rebuild path\n\n");
    testSilenceBeforeAnythingIsPublished();
    testAModelBecomesAudio();
    testTheHeadroomIsATime();
    testARebuildReplacesTheGraphTheAudioThreadUses();
    testAFailedRebuildLeavesTheSessionPlaying();
    testAGraphThatCannotBePreparedIsNotPublished();
    testTheSwapIsFadedIn();
    testAFadeLongerThanTheBlockCarriesAcross();
    testTheAudioThreadAllocatesNothingAcrossASwap();
    testTheRetiredGraphIsFreedOnlyOnceTheReaderHasMovedPast();
    testTheCoalescerSurvivesARebuild();
    testRebuildNeededFinallyHasAnAnswer();
    testProblemsSurviveWithoutHoldingTheGraph();
    testWithoutHistoryTheSeamIsExactlyTheCompensation();
    testARebuildKeepsTheRingHistory();
    testTheControlHasNoSeamEitherWay();
    testHistoryComesFromTheGraphThatActuallyRan();
    testHistoryIsCarriedAcrossRepeatedRebuilds();
    testTheSameEdgeIsMatchedHoweverTheRowSaysIt();
    std::printf("\n%s -- %d checks, %d failure(s)\n",
                g_failures ? "FAILED" : "PASS", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
