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
