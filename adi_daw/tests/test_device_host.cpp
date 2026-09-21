// SPDX-License-Identifier: GPL-3.0-or-later
//
// Who owns the coalescer, and what ticks it. ADR-0082 left both open.
//
// No JUCE here, so this runs on every ABI the suite runs on -- the tick is a
// plain call taking a millisecond clock, and the only JUCE-shaped part is a
// timer that calls it.

#include "juce/device_host.hpp"

#include <cstdio>
#include <functional>
#include <vector>
#include <memory>
#include <string>

namespace {

using namespace adi;
using namespace adi::device;

int g_failures = 0;
int g_checks = 0;
void check(bool c, const std::string& w) {
    ++g_checks;
    if (!c) { ++g_failures; std::printf("  FAIL  %s\n", w.c_str()); }
}
void section(const char* s) { std::printf("[%s]\n", s); }
void eqi(std::int64_t got, std::int64_t want, const std::string& what) {
    ++g_checks;
    if (got != want) {
        ++g_failures;
        std::printf("  FAIL  %s\n          got %lld, want %lld\n", what.c_str(),
                    static_cast<long long>(got), static_cast<long long>(want));
    }
}

/// A device that reports a latency change on demand.
class Reporter final : public DeviceInstance {
public:
    [[nodiscard]] const DeviceIdentity& identity() const noexcept override { return id_; }
    [[nodiscard]] bool loaded() const noexcept override { return true; }
    void process(const engine::NodeIo& io) noexcept override { passThrough(io); }
    [[nodiscard]] std::int64_t tailSamples() const noexcept override { return 0; }
    [[nodiscard]] std::int32_t latencySamples() const noexcept override { return lat_; }
    [[nodiscard]] std::uint64_t latencyEpoch() const noexcept override { return epoch_; }

    void report(std::int32_t newLatency) { lat_ = newLatency; ++epoch_; }

private:
    DeviceIdentity id_;
    std::int32_t lat_ = 0;
    std::uint64_t epoch_ = 0;
};

/// The smallest clap_plugin_t that can run a main-thread callback.
struct Fake {
    clap_plugin_t plugin{};
    std::function<void()> onMainThread;
    Fake() {
        plugin.plugin_data = this;
        plugin.on_main_thread = [](const clap_plugin_t* p) {
            auto& f = *static_cast<Fake*>(p->plugin_data);
            if (f.onMainThread) f.onMainThread();
        };
    }
};

void testOwnershipAndLifetime() {
    section("ADR-0082 -- the coalescer outlives the graph, and lives beside the devices");

    DeviceHost host;
    auto dev = std::make_unique<Reporter>();
    auto* raw = dev.get();
    DeviceNode& node = host.add(std::move(dev), "Reporter");

    check(host.deviceCount() == 1, "the host owns the device");
    check(&host.nodeAt(0) == &node, "and the node");
    check(host.coalescer().sourceCount() == 1, "which registered one source");

    // THE LIFETIME ARGUMENT, made concrete. A graph is replaced -- ADR-0019
    // swaps one and ADR-0085 rebuilds one -- so a coalescer living inside a
    // Graph would be destroyed by the swap it exists to cause. Two graphs,
    // one host, and the device and its registration survive both.
    {
        engine::Graph g1;
        const engine::NodeId n1 = g1.addNode(node);
        g1.setOutput(n1);
        g1.prepare(48000.0, 256);
        host.attachGraph(g1);
        check(g1.ok(), "the first graph prepares");
    }
    engine::Graph g2;
    const engine::NodeId n2 = g2.addNode(node);
    g2.setOutput(n2);
    g2.prepare(48000.0, 256);
    host.attachGraph(g2);

    check(host.deviceCount() == 1, "the device survived the graph it was in");
    check(host.coalescer().sourceCount() == 1, "and so did its registration");
    check(raw->latencyEpoch() == 0, "with no spurious reports");
}

void testTheTickCoalesces() {
    section("a tick debounces a burst into one retap");

    DeviceHost host;
    auto dev = std::make_unique<Reporter>();
    auto* raw = dev.get();
    DeviceNode& node = host.add(std::move(dev), "Reporter");

    engine::Graph g;
    g.setLatencyHeadroom(8192);        // measured: Pro-Q 3 swings 5120
    const engine::NodeId src = g.addNode(node);
    engine::SumNode mix;
    const engine::NodeId out = g.addNode(mix);
    g.connect(src, out);
    g.setOutput(out);
    g.prepare(48000.0, 256);
    check(g.ok(), "the graph prepares: " + g.error());
    host.attachGraph(g);
    host.coalescer().setQuietPeriodMs(50);

    std::int64_t t = 1000;
    check(!host.tick(t), "a quiet tick does nothing");

    // A burst, at the cadence MEASURED from FabFilter Pro-Q 3: five reports
    // with a largest gap of 26 ms. Against a 50 ms quiet period that is one
    // burst, not five.
    for (int i = 0; i < 5; ++i) {
        raw->report(320 + i * 1200);
        t += 26;
        host.tick(t);
    }
    check(host.coalescer().pending(), "the burst is still open mid-stream");
    check(host.coalescer().stats().retaps == 0, "and nothing retapped yet");

    t += 60;                           // longer than the quiet period
    const bool did = host.tick(t);
    check(did, "the tick after the quiet period retaps");
    check(host.coalescer().stats().retaps == 1,
          "ONCE for the whole burst, not five times -- saw " +
          std::to_string(host.coalescer().stats().retaps));
    check(host.coalescer().stats().reports == 5, "though all five reports were seen");
    check(!host.coalescer().pending(), "and the burst is closed");
}

void testAShapeReportEscalates() {
    section("ADR-0084/0085 -- a shape report escalates instead of retapping");

    DeviceHost host;
    ClapHostGlue glue;
    host.watchClapGlue(glue, "Surge");
    check(host.coalescer().sourceCount() == 3,
          "a CLAP glue registers three sources: latency, ports, bare");

    engine::Graph g;
    engine::SumNode a;
    const engine::NodeId n = g.addNode(a);
    g.setOutput(n);
    g.prepare(48000.0, 256);
    host.attachGraph(g);
    host.coalescer().setQuietPeriodMs(50);

    const clap_host_t* h = glue.host();
    const auto* ports = static_cast<const clap_host_audio_ports_t*>(
        h->get_extension(h, CLAP_EXT_AUDIO_PORTS));
    ports->rescan(h, CLAP_AUDIO_PORTS_RESCAN_CHANNEL_COUNT);

    std::int64_t t = 2000;
    host.tick(t);
    t += 60;
    host.tick(t);
    check(host.coalescer().rebuildNeeded(),
          "a channel-count change demands a rebuild, not a tap move");
}

void testCallbacksAreDrainedByTheSameTick() {
    section("the tick drains deferred plugin work before polling");

    DeviceHost host;
    ClapHostGlue glue;
    host.watchClapGlue(glue, "Surge");

    engine::Graph g;
    engine::SumNode a;
    g.setOutput(g.addNode(a));
    g.prepare(48000.0, 256);
    host.attachGraph(g);

    glue.host()->request_callback(glue.host());
    check(glue.mainThreadWorkPending(), "a plugin deferred work");
    host.tick(3000);
    check(!glue.mainThreadWorkPending(),
          "and the same tick drained it -- a second timer for this would be a "
          "second thing to forget to start");

    // THE ORDER, which the check above cannot see. Deferred work is often
    // the thing that then reports a latency change; polling first would
    // miss it and cost a whole quiet period for nothing.
    //
    // A device whose epoch only moves when the main-thread callback runs
    // makes the ordering observable: drain-then-poll sees the report in the
    // SAME tick, poll-then-drain does not. Planting the swap passed every
    // check until this existed.
    DeviceHost h2;
    ClapHostGlue g2;
    auto dev = std::make_unique<Reporter>();
    auto* raw = dev.get();
    DeviceNode& node = h2.add(std::move(dev), "Deferred");
    h2.watchClapGlue(g2, "Surge");

    engine::Graph gr;
    gr.setLatencyHeadroom(8192);
    const engine::NodeId nDev = gr.addNode(node);
    engine::SumNode mix2;
    const engine::NodeId nMix = gr.addNode(mix2);
    gr.connect(nDev, nMix);
    gr.setOutput(nMix);
    gr.prepare(48000.0, 256);
    h2.attachGraph(gr);
    h2.coalescer().setQuietPeriodMs(50);

    // Stand in for "the plugin reports its new latency from on_main_thread".
    Fake fake;
    fake.onMainThread = [raw] { raw->report(5120); };
    g2.registerPlugin(&fake.plugin);
    g2.host()->request_callback(g2.host());

    h2.tick(4000);
    check(h2.coalescer().stats().reports == 1,
          "the report raised by the deferred work is seen in the SAME tick, "
          "saw " + std::to_string(h2.coalescer().stats().reports));
}


// ---------------------------------------------------------------------------
// ADR-0090 -- the loop closes: a shape report becomes a new published graph.
// ---------------------------------------------------------------------------

rows::Track track(std::int64_t id, const char* kind, const char* name) {
    rows::Track t;
    t.id = id;
    t.kind = kind;
    t.name = name;
    return t;
}

rows::Model twoTracks() {
    rows::Model m;
    m.tracks.push_back(track(1, "audio", "A"));
    m.tracks.push_back(track(2, "audio", "B"));
    m.tracks.push_back(track(9, "master", "Master"));
    return m;
}

/// A device that HALVES. The point is that its presence in the chain is a
/// different number at the master, which `Reporter` -- a pass-through -- can
/// never be. Omitting `devicesFor` from the rebuild passed every check in the
/// first version of this test, because nothing downstream could tell a graph
/// containing a pass-through device from a graph containing no device.
class HalfDevice final : public DeviceInstance {
public:
    [[nodiscard]] const DeviceIdentity& identity() const noexcept override { return id_; }
    [[nodiscard]] bool loaded() const noexcept override { return true; }
    [[nodiscard]] std::int64_t tailSamples() const noexcept override { return 0; }
    void process(const engine::NodeIo& io) noexcept override {
        for (std::int32_t c = 0; c < io.channels; ++c) {
            float* o = io.out[c] + io.blockOffset;
            const float* i = (io.in != nullptr && io.in[c] != nullptr)
                                 ? io.in[c] + io.blockOffset : nullptr;
            for (std::int32_t k = 0; k < io.frames; ++k)
                o[k] = (i != nullptr ? i[k] : 0.0f) * 0.5f;
        }
    }
private:
    DeviceIdentity id_;
};

/// A track's content. The clip reader's stand-in: it is pushed into whichever
/// graph is current, which is the point -- a rebuild means pushing again.
class ToneNode final : public engine::Node {
public:
    explicit ToneNode(float v) : v_(v) {}
    void process(const engine::NodeIo& io) noexcept override {
        for (std::int32_t c = 0; c < io.channels; ++c) {
            float* o = io.out[c] + io.blockOffset;
            for (std::int32_t i = 0; i < io.frames; ++i) o[i] = v_;
        }
    }
    [[nodiscard]] const char* name() const noexcept override { return "tone"; }
private:
    float v_;
};

struct Out {
    std::vector<float> l, r;
    std::vector<float*> ptrs;
    explicit Out(std::int32_t n) : l(static_cast<std::size_t>(n), -7.0f),
                                   r(static_cast<std::size_t>(n), -7.0f) {
        ptrs = {l.data(), r.data()};
    }
};

engine::AudioIo makeIo(Out& o, std::int32_t frames) {
    engine::AudioIo io;
    io.out = o.ptrs.data();
    io.numOut = 2;
    io.frames = frames;
    io.streamTimeSamples = 0;
    return io;
}

/// Feed a track's chain head in the CURRENT graph. A rebuild retires the
/// graph this touched, so it has to be called again -- which is a real
/// consequence of ADR-0089 and not a wart of the test.
bool feed(engine::GraphHost& host, engine::Node& src, std::int64_t trackId,
          double sr, std::int32_t frames) {
    engine::RealizedGraph* rg = host.current();
    if (rg == nullptr) return false;
    engine::Graph& g = rg->graph();
    const engine::NodeId n = g.addNode(src);
    if (!g.connect(n, rg->inputFor(trackId))) return false;
    g.prepare(sr, frames);
    return g.ok();
}

void testTheRebuildLoopCloses() {
    section("ADR-0090 -- a shape report becomes a new graph, and the audio survives");

    engine::GraphHost gh;
    gh.setFadeFrames(0);  // a fade is ADR-0089's and would scale what we assert

    DeviceHost host;
    auto dev = std::make_unique<HalfDevice>();
    auto* raw = dev.get();
    host.add(std::move(dev), "OnTrackOne", /*trackId=*/1);

    ClapHostGlue glue;
    host.watchClapGlue(glue, "Surge");

    rows::Model model = twoTracks();
    DeviceHost::RebuildSpec spec;
    spec.model = [&model] { return &model; };
    spec.sampleRate = 48000.0;
    spec.maxFrames = 64;
    host.attachHost(gh, spec);
    host.coalescer().setQuietPeriodMs(50);

    // --- the device reaches the graph through devicesFor, not by hand ------
    check(host.rebuildNow(), "first graph: " + host.lastRebuildError());
    eqi(gh.stats().published, 1, "one graph published");

    const std::vector<engine::Node*> chain = host.chainFor(1);
    eqi(static_cast<std::int64_t>(chain.size()), 1, "track 1 has one device");
    check(!chain.empty() && chain[0] == &host.nodeAt(0),
          "and it is the node the host owns, not a copy");
    check(host.chainFor(2).empty(), "track 2 has none");

    ToneNode tone(0.5f);
    check(feed(gh, tone, 1, 48000.0, 64), "track 1 is fed");

    Out o1(64);
    engine::AudioIo io1 = makeIo(o1, 64);
    gh.process(io1);
    check(o1.l[0] == 0.25f,
          "audio reaches the master THROUGH the injected device -- 0.5 halved: got " +
              std::to_string(o1.l[0]));

    engine::Graph* before = gh.currentGraph();

    // --- a port rescan, which no amount of retapping can fix --------------
    const clap_host_t* h = glue.host();
    const auto* ports = static_cast<const clap_host_audio_ports_t*>(
        h->get_extension(h, CLAP_EXT_AUDIO_PORTS));
    ports->rescan(h, CLAP_AUDIO_PORTS_RESCAN_CHANNEL_COUNT);

    std::int64_t t = 1000;
    host.tick(t);
    check(host.stats().rebuilds == 0, "nothing rebuilds while the burst is open");
    t += 60;
    host.tick(t);

    eqi(host.stats().rebuilds, 1, "the quiet burst produced exactly one rebuild");
    eqi(host.stats().rebuildsFailed, 0, "and it succeeded: " + host.lastRebuildError());
    eqi(gh.stats().published, 2, "a second graph is published");
    check(gh.currentGraph() != before, "and it is a DIFFERENT graph");
    check(!host.coalescer().rebuildNeeded(), "the flag is answered, not left raised");

    // The same device object is in the new graph: a rebuild re-injects, it
    // does not reload the plugin (ADR-0042 d5).
    check(host.chainFor(1)[0] == &host.nodeAt(0),
          "the SAME device node is in the new graph");
    check(&host.nodeAt(0).instance() == raw, "and the same plugin instance behind it");

    // --- the audio survives the swap --------------------------------------
    check(feed(gh, tone, 1, 48000.0, 64), "the new graph is fed");
    Out o2(64);
    engine::AudioIo io2 = makeIo(o2, 64);
    gh.process(io2);
    eqi(gh.stats().swaps, 2, "the audio thread picked up the new graph");
    check(o2.l[0] == 0.25f,
          "and the device is STILL IN THE CHAIN across the seam -- a rebuild "
          "that dropped devicesFor would read 0.5: got " + std::to_string(o2.l[0]));

    // --- the retired graph is reclaimed, by the tick, not by hand ---------
    eqi(gh.stats().reclaimed, 0, "nothing is freed while the reader may still hold it");
    t += 60;
    host.tick(t);
    eqi(gh.stats().reclaimed, 1, "the tick collects the retired graph");
    eqi(host.stats().rebuilds, 1, "and a quiet tick rebuilds nothing");
}

void testAFailedRebuildLeavesTheSessionPlaying() {
    section("ADR-0090 -- a rebuild that cannot be realised does not retry, and does not silence");

    engine::GraphHost gh;
    gh.setFadeFrames(0);

    DeviceHost host;
    ClapHostGlue glue;
    host.watchClapGlue(glue, "Surge");

    rows::Model model = twoTracks();
    DeviceHost::RebuildSpec spec;
    spec.model = [&model] { return &model; };
    spec.sampleRate = 48000.0;
    spec.maxFrames = 64;
    host.attachHost(gh, spec);
    host.coalescer().setQuietPeriodMs(50);

    check(host.rebuildNow(), "first graph: " + host.lastRebuildError());
    ToneNode tone(0.25f);
    check(feed(gh, tone, 1, 48000.0, 64), "track 1 is fed");
    engine::Graph* live = gh.currentGraph();

    // The model loses its master between the report and the rebuild, which is
    // what a bad edit looks like from here.
    model.tracks.clear();

    const clap_host_t* h = glue.host();
    const auto* ports = static_cast<const clap_host_audio_ports_t*>(
        h->get_extension(h, CLAP_EXT_AUDIO_PORTS));
    ports->rescan(h, CLAP_AUDIO_PORTS_RESCAN_CHANNEL_COUNT);

    std::int64_t t = 5000;
    host.tick(t);
    t += 60;
    host.tick(t);

    eqi(host.stats().rebuildsFailed, 1, "the rebuild was refused");
    eqi(host.stats().rebuilds, 0, "and nothing was published");
    check(!host.lastRebuildError().empty(), "the reason is named, not swallowed");
    check(gh.currentGraph() == live, "the running graph is untouched");

    Out o(64);
    engine::AudioIo io = makeIo(o, 64);
    gh.process(io);
    check(o.l[0] == 0.25f,
          "and the session is still playing what it was playing: got " +
              std::to_string(o.l[0]));

    // THE STORM CHECK. Leaving the flag raised on failure would rebuild on
    // every tick from here to the end of the session.
    for (int i = 0; i < 20; ++i) { t += 60; host.tick(t); }
    eqi(host.stats().rebuildsFailed, 1,
        "twenty more ticks attempt nothing: one report, one attempt");

    // ...and a NEW report is still answered, so clearing is not deafness.
    model = twoTracks();
    ports->rescan(h, CLAP_AUDIO_PORTS_RESCAN_CHANNEL_COUNT);
    t += 10;
    host.tick(t);
    t += 60;
    host.tick(t);
    eqi(host.stats().rebuilds, 1, "the next report rebuilds");
}

void testChainOrderAndPlacement() {
    section("ADR-0090 -- devicesFor hands each track its own chain, in order");

    DeviceHost host;
    host.add(std::make_unique<Reporter>(), "one", 1);
    host.add(std::make_unique<Reporter>(), "two", 2);
    host.add(std::make_unique<Reporter>(), "three", 1);
    host.add(std::make_unique<Reporter>(), "unplaced");

    const std::vector<engine::Node*> t1 = host.chainFor(1);
    eqi(static_cast<std::int64_t>(t1.size()), 2, "track 1 has two");
    check(t1.size() == 2 && t1[0] == &host.nodeAt(0) && t1[1] == &host.nodeAt(2),
          "in the order they were added, and not the other track's");
    eqi(static_cast<std::int64_t>(host.chainFor(2).size()), 1, "track 2 has one");
    check(host.chainFor(0).empty(), "track 0 is not a track: the unplaced one is not in it");
    eqi(static_cast<std::int64_t>(host.deviceCount()), 4, "all four are owned");

    // The supplier is the same answer, reached the way realize() reaches it.
    const engine::DeviceChainFn fn = host.chainSupplier();
    eqi(static_cast<std::int64_t>(fn(1).size()), 2, "through the supplier too");
}

}  // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::printf("adi_device_host_tests -- ownership, the tick, and the rebuild loop (ADR-0082/0090)\n\n");
    testOwnershipAndLifetime();
    testTheTickCoalesces();
    testAShapeReportEscalates();
    testCallbacksAreDrainedByTheSameTick();
    testTheRebuildLoopCloses();
    testAFailedRebuildLeavesTheSessionPlaying();
    testChainOrderAndPlacement();
    std::printf("\n%s -- %d checks, %d failure(s)\n",
                g_failures ? "FAILED" : "PASS", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
