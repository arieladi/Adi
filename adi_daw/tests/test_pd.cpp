// SPDX-License-Identifier: GPL-3.0-or-later
//
// The libpd latency protocol and the Pd device — src/juce/pd_device.{hpp,cpp},
// ADR-0095.
//
// No libpd in sight, on purpose. The engine that would wrap libpd is replaced
// by a fake patch that does what a lookahead limiter patch does: it delays its
// audio by its lookahead, and it reports that delay by sending a float to
// `$0-report_latency` -- through the SAME `PdReceiverTable::dispatchFloat` that
// libpd's float hook will call. What is left for libpd to supply is the five
// calls in pd_device.hpp's header comment, and none of them carries logic.
//
// Its own binary because it replaces global `operator new`: the dispatch path
// runs on the audio thread and its claim not to allocate is observed here.

#include "juce/device_host.hpp"
#include "juce/pd_device.hpp"

#include "adi/engine/host.hpp"

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <new>
#include <string>
#include <thread>
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
using namespace adi::device;
using V = PdLatencyReceiver::Verdict;

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

// --- a fake lookahead-limiter patch -----------------------------------------

/// Behaves like the limiter patch ADR-0096 describes, as far as latency goes:
/// it delays audio by its lookahead and reports that delay in SAMPLES at the
/// rate it is running at -- and, like any patch that answers from [loadbang],
/// it reports once on open at Pd's default 44.1 kHz, before anyone has told
/// it the real rate.
class FakeLimiterPatch final : public PdPatchEngine {
public:
    FakeLimiterPatch(PdReceiverTable& table, int dollarZero, double lookaheadMs,
                     bool openOk = true)
        : table_(table), d0_(dollarZero), ms_(lookaheadMs), openOk_(openOk) {}

    bool open(PdLatencyReceiver& latency, std::string& error) override {
        if (!openOk_) { error = "no such patch"; return false; }
        if (!table_.bind(d0_, latency)) { error = "could not bind"; return false; }
        name_ = PdLatencyReceiver::nameFor(d0_);
        sayLatency();            // [loadbang], at whatever rate Pd has now
        return true;
    }
    void close() noexcept override { table_.unbind(d0_); }

    void prepare(double sampleRate, std::int32_t) override {
        sr_ = sampleRate;
        line_.prepare(2, samples() + 8192);
        line_.setDelay(samples());
    }
    void requestLatencyReport() override { sayLatency(); }

    /// A parameter change inside the patch: the lookahead switch.
    void setLookaheadMs(double ms) {
        ms_ = ms;
        line_.setDelay(samples());   // the audio delay moves...
        sayLatency();                // ...and the patch says so
    }

    void process(const engine::NodeIo& io) noexcept override {
        const engine::DelayLine::Cursors before = line_.cursors();
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

    [[nodiscard]] std::int32_t samples() const {
        return static_cast<std::int32_t>(std::lround(ms_ * sr_ / 1000.0));
    }

private:
    /// `[s $0-report_latency]`: a float to a receiver name, through the
    /// table, exactly as libpd's hook would deliver it.
    void sayLatency() { table_.dispatchFloat(name_.c_str(), static_cast<float>(samples())); }

    PdReceiverTable& table_;
    int d0_;
    double ms_;
    bool openOk_;
    double sr_ = 44100.0;     // Pd's rate before libpd_init_audio is called
    std::string name_;
    engine::DelayLine line_;
};

}  // namespace

namespace adi::engine {
/// A constant, for a track that has to be playing. A SOURCE: it inherits
/// kInfiniteTail (ADR-0043).
class ConstSource final : public Node {
public:
    explicit ConstSource(float v) : v_(v) {}
    void process(const NodeIo& io) noexcept override {
        for (std::int32_t c = 0; c < io.channels; ++c) {
            float* o = io.out[c] + io.blockOffset;
            for (std::int32_t i = 0; i < io.frames; ++i) o[i] = v_;
        }
    }
    [[nodiscard]] const char* name() const noexcept override { return "const"; }
private:
    float v_;
};
}  // namespace adi::engine

namespace {

DeviceIdentity pdId(const char* name) {
    DeviceIdentity id;
    id.format = "pd";
    id.uid = name;
    id.name = name;
    return id;
}

// ===========================================================================

void testTheReceiverChecksWhatItIsTold() {
    section("ADR-0095 -- a report is validated before the graph believes it");

    PdLatencyReceiver r;
    eqi(r.latencySamples(), 0, "nothing reported, nothing believed");
    eqi(static_cast<long long>(r.epoch()), 0, "and no epoch");

    check(r.report(72.0) == V::Accepted, "72 samples is accepted");
    eqi(r.latencySamples(), 72, "and believed");
    eqi(static_cast<long long>(r.epoch()), 1, "and the epoch moved once");

    // The same value again must NOT move the epoch: a patch that re-sends its
    // latency on every parameter touch would otherwise retap the graph on
    // every knob turn.
    check(r.report(72.0) == V::Unchanged, "the same value again is Unchanged");
    eqi(static_cast<long long>(r.epoch()), 1, "and the epoch did not move");

    // A Pd float is single precision; ms * sr / 1000 can come out as 288.00002.
    check(r.report(288.00002) == V::Accepted,
          "float noise on a whole number is a whole number, not a rounding");
    eqi(r.latencySamples(), 288, "288");

    check(r.report(144.4) == V::Rounded, "a genuine fraction is rounded, and SAYS so");
    eqi(r.latencySamples(), 144, "to the nearest sample");
    eqi(r.stats().rounded, 1, "and counted, so a patch that forgot to round shows up");

    check(r.report(-3.0) == V::RejectedNegative, "a negative delay is refused");
    check(r.report(std::numeric_limits<double>::quiet_NaN()) == V::RejectedNonFinite,
          "NaN is refused");
    check(r.report(std::numeric_limits<double>::infinity()) == V::RejectedNonFinite,
          "infinity is refused");
    r.setMaxSamples(1000);
    check(r.report(5000.0) == V::RejectedTooLarge, "past the bound is refused");
    eqi(r.latencySamples(), 144,
        "and every refusal KEPT the last good value rather than zeroing it");
    eqi(r.stats().rejected, 4, "four refusals counted");

    check(PdLatencyReceiver::nameFor(1003) == "1003-report_latency", "the report name");
    check(PdLatencyReceiver::queryFor(1003) == "1003-query_latency", "the query name");
}

void testTwoPatchesAreToldApart() {
    section("ADR-0095 -- $0, because Pd's names are global");

    // THE BUG THE BARE NAME HAS. Two limiters on two tracks, both sending to
    // `report_latency`, put two numbers on one name. With $0 each patch has
    // its own, and the table routes each report to its own device.
    PdReceiverTable table;
    PdLatencyReceiver a, b;
    check(table.bind(1001, a), "patch A binds 1001-report_latency");
    check(table.bind(1002, b), "patch B binds 1002-report_latency");
    check(!table.bind(1001, b), "a second LIVE binding of one $0 is refused");

    check(table.dispatchFloat("1001-report_latency", 72.0f), "A reports 72");
    check(table.dispatchFloat("1002-report_latency", 288.0f), "B reports 288");
    eqi(a.latencySamples(), 72, "A believes 72");
    eqi(b.latencySamples(), 288, "B believes 288 -- neither overwrote the other");

    check(!table.dispatchFloat("report_latency", 9999.0f),
          "a bare `report_latency` reaches nobody, which is the point of $0");
    check(!table.dispatchFloat("1001-cutoff", 0.5f),
          "every OTHER receiver in a patch arrives through the same hook and is ignored");
    eqi(a.latencySamples(), 72, "and changed nothing");
}

void testUnbindingLeavesATombstone() {
    section("ADR-0095 -- closing a patch never reuses its slot");

    PdReceiverTable table;
    PdLatencyReceiver a, again;
    table.bind(1001, a);
    eqi(table.bound(), 1, "one live");
    check(table.unbind(1001), "closed");
    eqi(table.bound(), 0, "none live");
    eqi(table.used(), 1, "but the slot is still used -- a tombstone, not a reuse");
    check(!table.dispatchFloat("1001-report_latency", 64.0f),
          "a closed patch's report reaches nobody");
    eqi(a.latencySamples(), 0, "and the old receiver heard nothing");

    // Pd recycles $0 numbers. The same name bound again is a new entry.
    check(table.bind(1001, again), "a reopened patch with a recycled $0 binds");
    check(table.dispatchFloat("1001-report_latency", 64.0f), "and is heard");
    eqi(again.latencySamples(), 64, "by the NEW receiver");
    check(!table.unbind(4242), "unbinding something never bound is refused");
}

void testDispatchAllocatesNothing() {
    section("ADR-0095 -- the dispatch path is on the audio thread");

    // Why the table exists at all. Looking a `const char*` up in an
    // unordered_map<std::string, ...> constructs a std::string -- an
    // allocation, per message, on the thread that must never allocate.
    PdReceiverTable table;
    PdLatencyReceiver a, b;
    table.bind(1001, a);
    table.bind(1002, b);

    g_allocs.store(0, std::memory_order_relaxed);
    g_counting.store(true, std::memory_order_relaxed);
    for (int i = 0; i < 1000; ++i) {
        table.dispatchFloat("1002-report_latency", static_cast<float>(i % 7));
        table.dispatchFloat("1003-some-other-receiver", 1.0f);
    }
    g_counting.store(false, std::memory_order_relaxed);
    eqi(g_allocs.load(std::memory_order_relaxed), 0,
        "two thousand dispatches, hits and misses, allocate nothing");

    g_allocs.store(0, std::memory_order_relaxed);
    g_counting.store(true, std::memory_order_relaxed);
    { std::string s = PdLatencyReceiver::nameFor(123456789); (void)s.size(); }
    g_counting.store(false, std::memory_order_relaxed);
    check(g_allocs.load(std::memory_order_relaxed) > 0,
          "and the counter is watching -- building a name DOES allocate, which is "
          "why names are built at bind time and never at dispatch");
}

void testBindingWhileTheAudioThreadDispatches() {
    section("ADR-0095 -- a patch can be opened while audio runs");

    // A new slot is written in full before the count that exposes it is
    // published, so a dispatch never reads a half-written name.
    PdReceiverTable table;
    PdLatencyReceiver first;
    table.bind(1, first);
    std::vector<PdLatencyReceiver> later(100);
    std::atomic<bool> done{false};
    std::atomic<bool> running{false};

    std::thread audio([&] {
        while (!done.load(std::memory_order_acquire)) {
            table.dispatchFloat("1-report_latency", 7.0f);
            running.store(true, std::memory_order_release);
        }
    });
    // WAIT FOR THE THREAD TO BE DISPATCHING before binding a thing. The first
    // version did not, and binding a hundred entries finished before the
    // thread had even started -- so nothing overlapped and the test asserted
    // concurrency it never had. The same mistake this project already wrote
    // down once, made again by the agent who wrote it down.
    while (!running.load(std::memory_order_acquire)) std::this_thread::yield();
    for (int i = 0; i < 100; ++i) table.bind(100 + i, later[static_cast<std::size_t>(i)]);
    done.store(true, std::memory_order_release);
    audio.join();

    eqi(table.bound(), 101, "every binding landed");
    eqi(first.latencySamples(), 7, "and the patch that was reporting throughout was heard");
    check(table.dispatchFloat("199-report_latency", 11.0f), "the last one bound is reachable");
    eqi(later[99].latencySamples(), 11, "and routed correctly");
}

void testAPatchReportsAtTheRealRate() {
    section("ADR-0095 -- the query after prepare corrects what [loadbang] said");

    // THE SECOND BUG THE NAIVE PROTOCOL HAS. A patch reporting from [loadbang]
    // reports at Pd's default 44.1 kHz, because nobody has told libpd the real
    // rate yet: 1.5 ms is 66 samples there, and 72 in a 48 kHz session.
    PdReceiverTable table;
    auto patch = std::make_unique<FakeLimiterPatch>(table, 1001, 1.5);
    PdDevice dev(std::move(patch), pdId("limiter"));
    check(dev.loaded(), "the patch opened: " + dev.openError());
    eqi(dev.latencySamples(), 66,
        "straight after open it claims 66 -- 1.5 ms at 44.1 kHz, the WRONG rate");

    dev.prepare(48000.0, 256);
    eqi(dev.latencySamples(), 72, "after prepare and the query: 72, the right one");
    dev.prepare(96000.0, 256);
    eqi(dev.latencySamples(), 144, "and 144 at 96 kHz");
    eqi(dev.latency().maxSamples(), 960000, "with the sanity bound at ten seconds of 96 kHz");
}

void testAPatchThatFailedToOpenClaimsNothing() {
    section("ADR-0095 -- a patch that did not open delays nothing, so it claims nothing");

    PdReceiverTable table;
    auto patch = std::make_unique<FakeLimiterPatch>(table, 1001, 6.0, /*openOk=*/false);
    PdDevice dev(std::move(patch), pdId("missing"));
    check(!dev.loaded(), "not loaded");
    check(dev.openError() == "no such patch", "with the reason: " + dev.openError());

    // A late report for its $0 must not make it claim a latency either.
    dev.latency().report(288.0);
    eqi(dev.latencySamples(), 0,
        "still 0 -- it passes audio straight through (ADR-0011), and compensating "
        "against a delay that is not happening moves everything else");

    std::vector<float> in(32, 0.5f), out(32, -1.0f);
    const float* ip[2] = {in.data(), in.data()};
    float* op[2] = {out.data(), out.data()};
    engine::NodeIo io;
    io.in = ip; io.out = op; io.channels = 1; io.frames = 32; io.sampleRate = 48000.0;
    dev.process(io);
    check(out[0] == 0.5f && out[31] == 0.5f, "and its audio passes through untouched");
}

void testTheLatencyReachesTheGraph() {
    section("ADR-0095 -- through DeviceHost and the coalescer, with no new plumbing");

    // The claim ADR-0095 makes: a Pd patch is compensated by the SAME
    // machinery as a VST3 or a CLAP, because DeviceHost registers every
    // device's latencyEpoch() through the contract. A dry track feeds the
    // master beside the Pd limiter's track; the dry edge's compensation is the
    // observable.
    PdReceiverTable table;
    auto patchOwned = std::make_unique<FakeLimiterPatch>(table, 1001, 1.5);
    FakeLimiterPatch* patch = patchOwned.get();

    DeviceHost host;
    host.add(std::make_unique<PdDevice>(std::move(patchOwned), pdId("limiter")),
             "Pd limiter", /*trackId=*/1);

    rows::Model model;
    rows::Track t1; t1.id = 1; t1.kind = "audio"; t1.name = "Limited";
    rows::Track t2; t2.id = 2; t2.kind = "audio"; t2.name = "Dry";
    rows::Track m;  m.id = 9;  m.kind = "master"; m.name = "Master";
    model.tracks = {t1, t2, m};

    engine::GraphHost gh;
    DeviceHost::RebuildSpec spec;
    spec.model = [&model] { return &model; };
    spec.sampleRate = 48000.0;
    spec.maxFrames = 256;
    host.attachHost(gh, spec);
    host.coalescer().setQuietPeriodMs(20);
    check(host.rebuildNow(), "the graph realises: " + host.lastRebuildError());

    engine::RealizedGraph* rg = gh.current();
    check(rg != nullptr, "and is live");

    // AUDIO ON BOTH TRACKS. Without it every input to the master is silent,
    // ADR-0043 puts the master to sleep, and a sleeping node never finishes a
    // tap move -- so the compensation would read 72 forever and the test would
    // be measuring suspension, not the protocol. (Chasing exactly that is what
    // found the two suspension bugs fixed alongside this.)
    engine::ConstSource wet(0.25f), dry(0.75f);
    {
        engine::Graph& g = rg->graph();
        g.connect(g.addNode(wet), rg->inputFor(1));
        g.connect(g.addNode(dry), rg->inputFor(2));
        g.prepare(48000.0, 256);
        check(g.ok(), "fed and re-prepared: " + g.error());
    }
    auto dryComp = [&] {
        engine::RealizedGraph* g = gh.current();
        return g->graph().compensationFor(g->outputFor(2), g->inputFor(9));
    };
    eqi(dryComp(), 72,
        "the dry track is held back 72 samples -- the patch's 1.5 ms at 48 kHz, "
        "learned through the query after prepare");

    std::vector<float> l(256, 0.0f), r(256, 0.0f);
    float* outp[2] = {l.data(), r.data()};
    engine::AudioIo io;
    io.out = outp; io.numOut = 2; io.frames = 256;
    gh.process(io);

    // The lookahead switch, inside the patch: 1.5 ms -> 6 ms.
    patch->setLookaheadMs(6.0);
    std::int64_t now = 1000;
    for (int i = 0; i < 6; ++i) { gh.process(io); host.tick(now); now += 20; }

    eqi(host.coalescer().stats().reports, 1, "the coalescer saw ONE report");
    check(host.coalescer().stats().retaps >= 1, "and retapped");
    eqi(dryComp(), 288,
        "the dry track now carries 288 -- 6 ms at 48 kHz -- and nothing but the "
        "ordinary DeviceInstance contract carried it there");
}

}  // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::printf("adi_pd_tests -- the libpd latency protocol\n\n");
    testTheReceiverChecksWhatItIsTold();
    testTwoPatchesAreToldApart();
    testUnbindingLeavesATombstone();
    testDispatchAllocatesNothing();
    testBindingWhileTheAudioThreadDispatches();
    testAPatchReportsAtTheRealRate();
    testAPatchThatFailedToOpenClaimsNothing();
    testTheLatencyReachesTheGraph();
    std::printf("\n%s -- %d checks, %d failure(s)\n",
                g_failures ? "FAILED" : "PASS", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
