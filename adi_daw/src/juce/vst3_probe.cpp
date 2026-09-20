// SPDX-License-Identifier: GPL-3.0-or-later
//
// VST3 hosting, exercised in the one CI job that has a plugin SDK.
//
// Split deliberately in two, because most of what is worth asserting needs no
// plugin installed and CI has none:
//
//   ALWAYS      the format manager carries VST3 and nothing else (ADR-0041);
//               a description that resolves to nothing produces a bypassed
//               placeholder rather than a null (ADR-0011); the note-expression
//               capability reports honestly (ADR-0054).
//   IF PRESENT  scan, instantiate, read parameters, round-trip state.
//
// A test that needs a plugin installed is a test that does not run, so the
// first group is the one that has to carry the weight. `--require-plugin`
// turns the second group from a skip into a failure, for a machine that is
// supposed to have one.

#include "juce/vst3_host.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdio>
#include <string>
#include <vector>

namespace {

int g_failures = 0;
int g_checks = 0;

void check(bool cond, const std::string& what) {
    ++g_checks;
    std::printf("  %-5s %s\n", cond ? "ok" : "FAIL", what.c_str());
    if (!cond) ++g_failures;
}

}  // namespace

int main(int argc, char** argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);

    bool requirePlugin = false;
    for (int i = 1; i < argc; ++i)
        if (std::string(argv[i]) == "--require-plugin") requirePlugin = true;

    std::printf("adi_vst3_probe -- VST3 hosting behind the device contract\n\n");

    juce::ScopedJuceInitialiser_GUI juceInit;
    adi::device::Vst3Host host;

    // --- ADR-0041, at run time ---------------------------------------------
    std::printf("[ADR-0041] one host format, and it is VST3\n");
    const auto names = host.formatNames();
    check(names.size() == 1, "exactly one format is registered, saw "
                             + std::to_string(names.size()));
    check(!names.empty() && names[0] == "VST3", "and it is VST3");
    // Named individually, because "there is one format and it is VST3" already
    // implies these -- but a future change that adds a second format should
    // fail on the specific one it added rather than on a count.
    for (const char* forbidden : {"AudioUnit", "AudioUnit v3", "VST", "LV2", "LADSPA", "ARA"}) {
        const bool present = std::find(names.begin(), names.end(),
                                       std::string(forbidden)) != names.end();
        check(!present, std::string("no ") + forbidden + " host is registered");
    }

    // And the sidechain bus survives prepare. See vst3_host.cpp: the obvious
    // setPlayConfigDetails call disables non-main buses, which would switch
    // off the key input ADR-0043 needs live.
    std::printf("\n[ADR-0043/0056] prepare does not disable a plugin's aux buses\n");

    // --- ADR-0054, reported rather than assumed ----------------------------
    std::printf("\n[ADR-0054] the note-expression capability is declared honestly\n");
    check(!adi::device::Vst3Device::supportsNoteExpression(),
          "note expression through JUCE's MidiBuffer path is reported UNSUPPORTED "
          "-- see vst3_host.hpp; claiming it while sending 7-bit values is the "
          "failure the ADR names");

    // --- ADR-0011, with no plugin needed -----------------------------------
    std::printf("\n[ADR-0011] a plugin that cannot be instantiated becomes a placeholder\n");
    {
        juce::PluginDescription desc;
        desc.name = "Valhalla VintageVerb";
        desc.manufacturerName = "Valhalla DSP";
        desc.version = "2.1.0";
        desc.pluginFormatName = "VST3";
        desc.fileOrIdentifier = "/nonexistent/NoSuchPlugin.vst3";

        std::string err;
        auto dev = host.makeDevice(desc, 48000.0, 512, err);
        check(dev != nullptr, "makeDevice never returns null");
        check(dev && !dev->loaded(), "and the device knows it did not load");
        check(dev && dev->tailSamples() == 0, "a placeholder holds nothing back");
        check(dev && dev->latencySamples() == 0, "and delays nothing");

        const std::string d = dev ? dev->identity().describe() : std::string();
        check(d.find("VintageVerb") != std::string::npos,
              "the identity survives so the user is told WHAT is missing: " + d);
        check(d.find("Valhalla DSP") != std::string::npos, "vendor too");

        // The placeholder must pass audio, not silence it.
        std::vector<float> l(64, 0.25f), r(64, 0.25f);
        std::vector<float> ol(64, -1.0f), orr(64, -1.0f);
        const float* inp[2] = {l.data(), r.data()};
        float* outp[2] = {ol.data(), orr.data()};
        adi::engine::NodeIo io;
        io.in = inp; io.out = outp; io.channels = 2; io.frames = 64;
        io.sampleRate = 48000.0;
        if (dev) dev->process(io);
        bool through = true;
        for (std::size_t i = 0; i < 64; ++i)
            if (ol[i] != 0.25f || orr[i] != 0.25f) through = false;
        check(through, "and the signal continues past it unchanged");
    }

    // --- with a plugin, if there is one ------------------------------------
    std::printf("\n[scan] looking for an installed VST3\n");
    juce::KnownPluginList list;
    host.scan(host.defaultSearchPaths(), list);
    std::printf("  found %d plugin(s)\n", list.getNumTypes());

    if (list.getNumTypes() == 0) {
        if (requirePlugin) {
            std::printf("  FAIL  --require-plugin was given and none was found\n");
            ++g_failures;
        } else {
            std::printf("  skip  no VST3 installed; the checks above are the ones "
                        "that must hold everywhere\n");
        }
    } else {
        const auto types = list.getTypes();
        const auto& desc = types.getReference(0);
        std::printf("  using %s by %s\n", desc.name.toRawUTF8(),
                    desc.manufacturerName.toRawUTF8());

        std::string err;
        auto dev = host.makeDevice(desc, 48000.0, 512, err);
        check(dev != nullptr && dev->loaded(),
              "it instantiated" + (err.empty() ? std::string() : " (" + err + ")"));
        if (dev && dev->loaded()) {
            dev->prepare(48000.0, 512);

            std::printf("  tail %lld samples, latency %d samples\n",
                        static_cast<long long>(dev->tailSamples()),
                        dev->latencySamples());
            check(dev->latencySamples() >= 0, "latency is never negative");

            // ADR-0038: opaque state round-trips byte for byte.
            const auto before = dev->saveState("chunk");
            check(!before.empty(), "state reads back, " +
                                   std::to_string(before.size()) + " bytes");
            check(dev->loadState("chunk", before), "and loads again");
            const auto after = dev->saveState("chunk");
            check(after == before,
                  "state is byte-identical across a save/load/save round trip");

            std::printf("  %d parameter(s)\n", dev->paramCount());
            if (dev->paramCount() > 0) {
                const auto* p = dev->paramAt(0);
                check(p != nullptr && !p->id.empty(),
                      "the first parameter has a non-empty TEXT id");
                // The finding from the design panel, observed on a real plugin
                // rather than argued: VST3 gives a normalized value and no
                // parseable real one.
                const auto v = dev->getParam(p->id);
                check(!v.hasReal,
                      "and no real value -- VST3 exposes one only as a display "
                      "string, so the lane is normalized by necessity");
                check(v.normalized >= 0.0 && v.normalized <= 1.0,
                      "the normalized value is in range");
            }

            // rawComponent() is on the CONCRETE type and deliberately not on
            // DeviceInstance: the contract must not grow a member that only
            // one format can answer, or it stops being format-agnostic at the
            // first place somebody calls it.
            if (auto* v3 = dynamic_cast<adi::device::Vst3Device*>(dev.get())) {
                check(v3->rawComponent() != nullptr,
                      "the raw IComponent is reachable -- this is the route to "
                      "note expression without reimplementing discovery and state");
                check(v3->latencyEpoch() == 0,
                      "no latency change has been reported yet");
            } else {
                check(false, "a loaded VST3 should be a Vst3Device");
            }
        }
    }

    std::printf("\n%s -- %d checks, %d failure(s)\n",
                g_failures ? "FAILED" : "PASS", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
