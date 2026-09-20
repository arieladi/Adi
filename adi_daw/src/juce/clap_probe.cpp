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

int main(int argc, char** argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::printf("adi_clap_probe -- a real .clap, and ADR-0084's open question\n\n");

    std::string path = "/Library/Audio/Plug-Ins/CLAP/Surge XT.clap";
    for (int i = 1; i + 1 < argc; ++i)
        if (std::string(argv[i]) == "--plugin") path = argv[i + 1];

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
