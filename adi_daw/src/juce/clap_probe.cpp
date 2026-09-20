// SPDX-License-Identifier: GPL-3.0-or-later
//
// Load a real .clap and answer the question ADR-0084 could not.
//
// CLAP's latency extension says the latency "is only allowed to change during
// plugin->activate", and `clap_host_latency.changed` is documented
// `[main-thread & being-activated]`. If that is literal, then the sequence is
// restart -> deactivate -> activate -> new latency, and ADR-0082's cheap path
// -- re-read the latency WITHOUT reactivating -- reads a stale value on CLAP
// even though it is correct on VST3.
//
// ADR-0084 named this and could not settle it: there was no CLAP plugin
// installed. Surge XT is, so this settles it.
//
// No JUCE. A .clap is a shared library exporting one symbol.

#include "juce/clap_host.hpp"
#include "juce/device_host.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

int g_failures = 0;
int g_checks = 0;
void check(bool c, const std::string& w) {
    ++g_checks;
    std::printf("  %-5s %s\n", c ? "ok" : "FAIL", w.c_str());
    if (!c) ++g_failures;
}

}  // namespace

namespace {

/// ADR-0084's parked question, answered against a real CLAP plugin.
///
/// `ext/latency.h` says the latency "is only allowed to change during
/// plugin->activate" and annotates `clap_host_latency.changed` as
/// `[main-thread & being-activated]`. If that is literal, the sequence is
/// restart -> deactivate -> activate -> new latency, and ADR-0082's cheap
/// path -- re-read WITHOUT reactivating -- reads a stale value on CLAP even
/// though it is correct on VST3.
///
/// Pro-Q 3 is the canonical mover: measured at 5120 samples in linear phase
/// through the VST3 path. This drives the same switch through CLAP.
int answerTheLatencyQuestion(const std::string& want) {
    adi::device::ClapHost host;
    host.scan(adi::device::ClapHost::defaultSearchPaths());

    const adi::device::ClapPluginRef* pick = nullptr;
    for (const auto& r : host.plugins())
        if (r.name.find(want) != std::string::npos) { pick = &r; break; }
    if (pick == nullptr) {
        std::printf("  skip  no CLAP plugin matching '%s'\n", want.c_str());
        return 0;
    }

    std::string err;
    auto dev = host.makeDevice(*pick, 48000.0, 512, err);
    if (dev == nullptr || !dev->loaded()) {
        std::printf("  FAIL  %s did not load: %s\n", pick->name.c_str(), err.c_str());
        return 1;
    }
    std::printf("  plugin   %s %s (CLAP)\n", pick->name.c_str(), pick->version.c_str());
    std::printf("  params   %d\n", dev->paramCount());
    std::printf("  latency  %d samples at rest\n", dev->latencySamples());

    const adi::device::ParamDescriptor* mode = nullptr;
    for (std::int32_t i = 0; i < dev->paramCount(); ++i) {
        const auto* d = dev->paramAt(i);
        if (d == nullptr) continue;
        if (d->name.find("hase") != std::string::npos ||
            d->name.find("rocessing") != std::string::npos) { mode = d; break; }
    }
    if (mode == nullptr) { std::printf("  skip  no phase/processing parameter\n"); return 0; }
    std::printf("  sweeping '%s'  %.3f .. %.3f\n",
                mode->name.c_str(), mode->minReal, mode->maxReal);

    std::vector<float> l(512, 0.0f), r(512, 0.0f);
    float* outp[2] = {l.data(), r.data()};
    adi::engine::NodeIo io;
    io.out = outp; io.channels = 2; io.frames = 512; io.sampleRate = 48000.0;

    const std::int32_t before = dev->latencySamples();
    const std::uint64_t changesBefore = host.glue().latencyChanges();
    const std::uint64_t restartsBefore = host.glue().restartRequests();

    // Walk the parameter's real range. Pro-Q 3's modes are discrete points
    // in it, so stepping finds them without knowing the encoding.
    // EVERY mode, not just the first that moves. 320 samples is a small
    // change that fits any ring; 5120 -- linear phase, measured through the
    // VST3 path -- is the one ADR-0079's escalation exists for, and the
    // plugin may well behave differently for it.
    std::int32_t moved = -1;
    std::int32_t largest = before;
    for (int step = 0; step <= 8; ++step) {
        const double v = mode->minReal
                       + (mode->maxReal - mode->minReal) * (double) step / 8.0;
        const std::uint64_t cBefore = host.glue().latencyChanges();
        const std::uint64_t rBefore = host.glue().restartRequests();
        dev->setParam(mode->id, adi::device::ParamValue::withReal(0.0, v));
        std::int32_t now = dev->latencySamples();
        for (int blk = 0; blk < 20; ++blk) {
            dev->process(io);
            host.glue().dispatchMainThread();
            now = dev->latencySamples();
        }
        std::printf("    mode %.3f -> latency %-6d  changed+%llu restart+%llu\n", v, now,
                    (unsigned long long)(host.glue().latencyChanges() - cBefore),
                    (unsigned long long)(host.glue().restartRequests() - rBefore));
        if (now != before && moved < 0) moved = now;
        if (now > largest) largest = now;
    }
    if (largest != before) moved = largest;

    std::printf("\n  [result]\n");
    std::printf("  latency before          %d\n", before);
    std::printf("  latency after (no re-activate) %s\n",
                moved < 0 ? "UNCHANGED" : std::to_string(moved).c_str());
    std::printf("  latencyChanges()        %llu\n",
                (unsigned long long)(host.glue().latencyChanges() - changesBefore));
    std::printf("  restartRequests()       %llu\n",
                (unsigned long long)(host.glue().restartRequests() - restartsBefore));

    // Now reactivate and look again. If the value only appears HERE, the
    // cheap path is reading stale and ADR-0084 needs a third case.
    dev->prepare(48000.0, 512);
    const std::int32_t afterReactivate = dev->latencySamples();
    std::printf("  latency after re-activate      %d\n", afterReactivate);

    std::printf("\n  [ADR-0084 verdict]\n");
    if (moved >= 0 && moved == afterReactivate) {
        std::printf("  The cheap path is SOUND on CLAP: the new latency was readable\n"
                    "  without reactivating, and matches what reactivating gives.\n");
    } else if (moved < 0 && afterReactivate != before) {
        std::printf("  The cheap path reads STALE on CLAP: the value only appeared\n"
                    "  after a deactivate/activate. ADR-0084 needs a third case --\n"
                    "  a CLAP latency report must drive reactivation before the retap.\n");
    } else if (moved < 0 && afterReactivate == before) {
        std::printf("  INCONCLUSIVE: this plugin did not move its latency at all,\n"
                    "  so the question is untouched. Try another.\n");
    } else {
        std::printf("  MIXED: read %d without reactivating, %d after. Worth a look.\n",
                    moved, afterReactivate);
    }
    return 0;
}

}  // namespace

namespace {

/// The whole chain, with a real plugin in it. ADR-0082 + ADR-0084 + ADR-0079.
///
/// win built the coalescer against synthetic fixtures and asked for this:
/// a real plugin reporting a real latency change, debounced, retapping a
/// real graph, without the audio breaking. Every piece existed; none of them
/// had been run together with a plugin at the front.
int endToEndCoalescer(const std::string& want) {
    adi::device::ClapHost chost;
    chost.scan(adi::device::ClapHost::defaultSearchPaths());

    const adi::device::ClapPluginRef* pick = nullptr;
    for (const auto& r : chost.plugins())
        if (r.name.find(want) != std::string::npos) { pick = &r; break; }
    if (pick == nullptr) { std::printf("  skip  no CLAP '%s'\n", want.c_str()); return 0; }

    std::string err;
    auto dev = chost.makeDevice(*pick, 48000.0, 512, err);
    if (dev == nullptr || !dev->loaded()) { std::printf("  FAIL  %s\n", err.c_str()); return 1; }

    // Find the mode parameter before the device is moved into the host.
    std::string modeId;
    double modeMax = 1.0;
    for (std::int32_t i = 0; i < dev->paramCount(); ++i) {
        const auto* d = dev->paramAt(i);
        if (d != nullptr && (d->name.find("hase") != std::string::npos ||
                             d->name.find("rocessing") != std::string::npos)) {
            modeId = d->id; modeMax = d->maxReal; break;
        }
    }
    if (modeId.empty()) { std::printf("  skip  no mode parameter\n"); return 0; }

    adi::device::DeviceHost host;
    adi::device::DeviceNode& node = host.add(std::move(dev), pick->name);
    host.watchClapGlue(chost.glue(), pick->name);
    auto& inst = host.deviceAt(0);

    // Two paths into one sum, the compensated shape ADR-0058's own test uses.
    // Headroom 8192, which is the MEASURED worst case: Pro-Q 3 swings 5120.
    adi::engine::Graph g;
    g.setLatencyHeadroom(8192);
    adi::engine::SumNode src, mix;
    const adi::engine::NodeId nSrc = g.addNode(src);
    const adi::engine::NodeId nDev = g.addNode(node);
    const adi::engine::NodeId nMix = g.addNode(mix);
    g.connect(nSrc, nDev);
    g.connect(nSrc, nMix);        // the dry path, which must stay aligned
    g.connect(nDev, nMix);
    g.setOutput(nMix);
    g.prepare(48000.0, 512);
    check(g.ok(), "the compensated graph prepares: " + g.error());
    host.attachGraph(g);
    host.coalescer().setQuietPeriodMs(50);

    std::vector<float> l(512, 0.0f), r(512, 0.0f);
    float* outp[2] = {l.data(), r.data()};
    adi::engine::AudioIo aio;
    aio.out = outp; aio.numOut = 2; aio.frames = 512;

    std::printf("  latency at rest   %d\n", inst.latencySamples());
    std::printf("  sources watched   %zu\n", host.coalescer().sourceCount());

    std::int64_t now = 1000;
    for (int i = 0; i < 3; ++i) { g.process(aio); host.tick(now); now += 20; }
    check(host.coalescer().stats().retaps == 0, "nothing retaps while nothing changes");

    // THE SWITCH. Linear phase, the 5120-sample case.
    inst.setParam(modeId, adi::device::ParamValue::withReal(0.0, modeMax));
    for (int i = 0; i < 20; ++i) { g.process(aio); host.tick(now); now += 5; }

    const std::int32_t after = inst.latencySamples();
    std::printf("  latency after     %d\n", after);
    check(after != 0, "the plugin moved its latency, saw " + std::to_string(after));
    check(host.coalescer().stats().reports > 0,
          "and the coalescer SAW it, reports=" +
          std::to_string(host.coalescer().stats().reports));

    // Past the quiet period: the burst closes and the tap moves.
    now += 80;
    host.tick(now);
    const auto& st = host.coalescer().stats();
    std::printf("  reports %lld  bursts %lld  retaps %lld  rebuildsNeeded %lld\n",
                (long long) st.reports, (long long) st.bursts,
                (long long) st.retaps, (long long) st.rebuildsNeeded);
    check(st.retaps >= 1, "the tap MOVED after the quiet period");
    check(st.retaps < st.reports || st.reports == 1,
          "and coalesced -- fewer retaps than reports");
    check(!host.coalescer().rebuildNeeded(),
          "8192 of headroom absorbed a 5120 swing, so no rebuild was needed");

    // And the audio still runs. A retap that broke the graph would show here.
    bool finite = true;
    for (int i = 0; i < 10; ++i) {
        g.process(aio);
        for (float v : l) if (!std::isfinite(v)) finite = false;
    }
    check(finite, "the graph still renders finite samples after the retap");
    check(g.ok(), "and is still ok: " + g.error());
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::printf("adi_clap_probe -- a real .clap, and ADR-0084's open question\n\n");

    std::string path = "/Library/Audio/Plug-Ins/CLAP/Surge XT.clap";
    std::string latencyWant, coalesceWant;
    for (int i = 1; i + 1 < argc; ++i) {
        if (std::string(argv[i]) == "--plugin") path = argv[i + 1];
        if (std::string(argv[i]) == "--latency") latencyWant = argv[i + 1];
        if (std::string(argv[i]) == "--coalesce") coalesceWant = argv[i + 1];
    }
    if (!coalesceWant.empty()) {
        std::printf("[ADR-0082/0084/0079] a real plugin, through the whole chain\n");
        const int rc = endToEndCoalescer(coalesceWant);
        std::printf("\n%s -- %d checks, %d failure(s)\n",
                    g_failures ? "FAILED" : "PASS", g_checks, g_failures);
        return g_failures ? 1 : rc;
    }

    if (!latencyWant.empty()) {
        std::printf("[ADR-0084] does the cheap path read stale on CLAP?\n");
        return answerTheLatencyQuestion(latencyWant);
    }

    // --- the sibling of Vst3Host, end to end ------------------------------
    {
        std::printf("[ClapHost] scan -> makeDevice -> DeviceHost\n");
        adi::device::ClapHost chost;
        const auto paths = adi::device::ClapHost::defaultSearchPaths();
        std::printf("  search paths: %zu\n", paths.size());
        chost.scan(paths);
        std::printf("  found %zu plugin(s) in %zu bundle(s)\n",
                    chost.plugins().size(), chost.libraryCount());
        check(!chost.plugins().empty(), "the scan found at least one CLAP plugin");

        for (const auto& r : chost.plugins())
            std::printf("    %-24s %-22s %s%s\n", r.name.c_str(), r.vendor.c_str(),
                        r.version.c_str(), r.isInstrument ? "  [instrument]" : "");

        if (!chost.plugins().empty()) {
            // Pick an instrument if there is one: an effect handed no input
            // is correctly silent and proves nothing about the event path.
            const adi::device::ClapPluginRef* pick = &chost.plugins().front();
            for (const auto& r : chost.plugins()) if (r.isInstrument) { pick = &r; break; }

            std::string e2;
            auto dev = chost.makeDevice(*pick, 48000.0, 512, e2);
            check(dev != nullptr, "makeDevice never returns null (ADR-0011)");
            check(dev && dev->loaded(), "and it loaded: " + e2);
            check(dev && dev->identity().format == "clap", "the identity says clap");

            // Through the SAME DeviceHost a VST3 goes through. That is the
            // whole point: the contract does not know which format it has.
            adi::device::DeviceHost host;
            adi::device::DeviceNode& node = host.add(std::move(dev), pick->name);
            host.watchClapGlue(chost.glue(), pick->name);
            check(host.deviceCount() == 1, "DeviceHost took it like any other device");
            check(host.coalescer().sourceCount() == 4,
                  "one device source plus the glue's three (ADR-0084), saw " +
                  std::to_string(host.coalescer().sourceCount()));

            adi::engine::Graph g;
            const adi::engine::NodeId n = g.addNode(node);
            g.setOutput(n);
            g.prepare(48000.0, 512);
            check(g.ok(), "and it prepares as a graph node: " + g.error());
            host.attachGraph(g);

            // A note through the graph's own event path, then render.
            adi::engine::Event on;
            on.type = adi::engine::EventType::NoteOn;
            on.noteId = 9; on.dim = 60; on.value = 0.8; on.frame = 0; on.channel = 2;
            check(g.pushInputEvent(n, on), "a note is queued on the graph");

            std::vector<float> l(512, 0.0f), r2(512, 0.0f);
            float* outp[2] = {l.data(), r2.data()};
            adi::engine::AudioIo aio;
            aio.out = outp; aio.numOut = 2; aio.frames = 512;

            double peak = 0.0;
            for (int blk = 0; blk < 40; ++blk) {
                g.process(aio);
                for (std::size_t i = 0; i < l.size(); ++i)
                    peak = std::max(peak, std::abs((double) l[i]));
            }
            std::printf("  peak     %.6f after 40 blocks\n", peak);
            if (pick->isInstrument)
                check(peak > 1e-5,
                      "the plugin made SOUND driven by the graph, through the "
                      "format-agnostic contract");
            else
                std::printf("  skip  %s is not an instrument\n", pick->name.c_str());
        }
        std::printf("\n");
    }

    adi::device::ClapLibrary lib;
    std::string err;
    if (!lib.open(path, err)) {
        std::printf("  skip  %s: %s\n", path.c_str(), err.c_str());
        std::printf("\nPASS -- 0 checks, 0 failure(s)\n");
        return 0;
    }
    check(lib.isOpen(), "the bundle loaded");
    std::printf("  %u plugin(s) in the factory\n", lib.pluginCount());

    const clap_plugin_descriptor_t* desc = lib.descriptorAt(0);
    check(desc != nullptr, "the factory describes its first plugin");
    if (desc == nullptr) return 1;
    std::printf("  %s %s by %s\n", desc->name, desc->version, desc->vendor);

    adi::device::ClapHostGlue glue;
    const clap_plugin_t* plugin = lib.create(glue.host(), desc->id);
    check(plugin != nullptr, "it instantiated through our own factory call");
    if (plugin == nullptr) return 1;

    glue.registerPlugin(plugin);
    {
        adi::device::DeviceIdentity id;
        id.format = "clap";
        id.name = desc->name;
        id.vendor = desc->vendor;
        id.version = desc->version;
        adi::device::ClapDevice dev(plugin, id);

        check(dev.loaded(), "and reached the device contract");
        std::printf("  params   %d\n", dev.paramCount());
        std::printf("  latency  %d samples before activate\n", dev.latencySamples());

        // The panel's finding, on a real CLAP plugin rather than a fake.
        if (dev.paramCount() > 0) {
            const auto* p0 = dev.paramAt(0);
            check(p0 != nullptr && p0->hasRealRange,
                  "a real CLAP parameter carries a REAL range, which VST3 cannot");
            if (p0 != nullptr)
                std::printf("  param0   '%s'  %.3f .. %.3f\n",
                            p0->name.c_str(), p0->minReal, p0->maxReal);
            check(p0 != nullptr && p0->domain == adi::device::ParamDomain::Real,
                  "and its domain is Real, not Normalized");
        }

        dev.prepare(48000.0, 512);
        const std::int32_t afterActivate = dev.latencySamples();
        std::printf("  latency  %d samples after activate\n", afterActivate);

        // --- THE QUESTION -------------------------------------------------
        //
        // Did the plugin report through clap_host_latency.changed during
        // activate? If it did, the host learns the new value through the
        // callback and the cheap path is safe. If it did not, re-reading
        // without reactivating is the only way to learn it -- and then the
        // cheap path is not merely safe, it is the ONLY path.
        std::printf("\n[ADR-0084] what a real CLAP plugin does\n");
        std::printf("  latencyChanges()      %llu\n",
                    (unsigned long long) glue.latencyChanges());
        std::printf("  restartRequests()     %llu\n",
                    (unsigned long long) glue.restartRequests());
        std::printf("  unexplainedRestarts() %llu\n",
                    (unsigned long long) glue.unexplainedRestarts());
        std::printf("  portChanges()         %llu\n",
                    (unsigned long long) glue.portChanges());

        check(glue.unexplainedRestarts() == 0,
              "no unexplained restart during a normal load and activate");

        // State, byte for byte, through our own clap_ostream_t.
        const auto blob = dev.saveState("chunk");
        std::printf("  state    %zu bytes\n", blob.size());
        check(!blob.empty(), "state saves through our own clap_ostream_t");
        if (!blob.empty()) {
            check(dev.loadState("chunk", blob), "and loads back");
            check(dev.saveState("chunk") == blob, "byte-identical round trip");
        }

        glue.dispatchMainThread();
        std::printf("  callbacks drained, %llu requested\n",
                    (unsigned long long) glue.callbackRequests());

        glue.unregisterPlugin(plugin);
    }
    // ClapDevice's destructor called plugin->destroy; the library closes
    // after, which is the order that matters.

    std::printf("\n%s -- %d checks, %d failure(s)\n",
                g_failures ? "FAILED" : "PASS", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
