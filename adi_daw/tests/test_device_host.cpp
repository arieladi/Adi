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

}  // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::printf("adi_device_host_tests -- ownership and the tick (ADR-0082)\n\n");
    testOwnershipAndLifetime();
    testTheTickCoalesces();
    testAShapeReportEscalates();
    testCallbacksAreDrainedByTheSameTick();
    std::printf("\n%s -- %d checks, %d failure(s)\n",
                g_failures ? "FAILED" : "PASS", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
