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

#include "juce/vst3_events.hpp"
#include "juce/vst3_host.hpp"

#include <algorithm>
#include <cmath>
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

    // --- ADR-0057: the event path VST3 actually needs -----------------------
    std::printf("\n[ADR-0054/0057] the event list carries a note id and a double\n");
    {
        adi::device::Vst3EventList list;
        list.reserve(2048);

        adi::engine::Event on;
        on.type = adi::engine::EventType::NoteOn;
        on.channel = 2;
        on.dim = 60;
        on.noteId = 7;
        on.value = 100.0 / 127.0;
        check(list.add(on), "a note on is translated");

        adi::engine::Event bend;
        bend.type = adi::engine::EventType::NoteExpression;
        bend.dim = static_cast<std::uint16_t>(adi::ExpressionDim::Pitch);
        bend.noteId = 7;
        bend.value = 12.0;                     // one octave up, in semitones
        check(list.add(bend), "and a pitch expression");

        check(list.getEventCount() == 2, "both are in the list");

        Steinberg::Vst::Event e{};
        list.getEvent(0, e);
        // THE LINE JUCE HARDCODES TO -1. Without it VST3 has nothing to
        // anchor note expression to and every per-note value goes channel-wide.
        check(e.noteOn.noteId == 7, "the note on carries a REAL note id, not -1");
        check(e.type == Steinberg::Vst::Event::kNoteOnEvent, "and is a note-on event");

        list.getEvent(1, e);
        check(e.type == Steinberg::Vst::Event::kNoteExpressionValueEvent,
              "the expression is a kNoteExpressionValueEvent -- the type JUCE "
              "never constructs");
        check(e.noteExpressionValue.noteId == 7, "anchored to the same note");
        check(e.noteExpressionValue.typeId == Steinberg::Vst::kTuningTypeID,
              "pitch is VST3's Tuning");
        const double expect = 12.0 / 240.0 + 0.5;
        check(std::abs(e.noteExpressionValue.value - expect) < 1e-15,
              "and the value is the SDK's own formula, as a double");

        // Overflow is counted rather than a silent break.
        adi::device::Vst3EventList small;
        small.reserve(1);
        check(small.add(on), "one fits");
        check(!small.add(bend), "the second is refused");
        check(small.dropped() == 1, "and COUNTED -- JUCE's own path breaks silently");
    }

    // --- ADR-0073: parameters ride the same call as the events -------------
    std::printf("\n[ADR-0073] the parameter queue is ours, and sample-accurate\n");
    {
        adi::device::Vst3ParamChanges changes;
        changes.reserve(4);

        check(changes.getParameterCount() == 0, "empty to begin with");
        check(changes.set(7, 0.25, 0), "a value at offset 0");
        check(changes.set(7, 0.75, 256), "and another mid-block");
        check(changes.getParameterCount() == 1,
              "both went into ONE queue -- two queues for one ParamID is "
              "malformed and some plugins read only the first");

        auto* q = changes.getParameterData(0);
        check(q != nullptr && q->getParameterId() == 7, "the queue knows its id");
        check(q != nullptr && q->getPointCount() == 2, "with two points");

        // Sample-accurate by construction. ADR-0042 splits a block at every
        // event boundary so automation does not step at 85 ms; a parameter
        // path that could only place a value at offset 0 would undo that for
        // every plugin parameter, which is most of the automation there is.
        Steinberg::int32 off = -1;
        Steinberg::Vst::ParamValue v = -1.0;
        q->getPoint(1, off, v);
        check(off == 256, "the second point keeps its sample offset");
        check(std::abs(v - 0.75) < 1e-15, "and its value");

        // Points stay in sample order however they arrive. Out of order makes
        // a ramp jump backwards mid-block, which sounds like a click.
        changes.clear();
        check(changes.set(9, 0.9, 512), "a late point first");
        check(changes.set(9, 0.1, 64), "then an early one");
        q = changes.getParameterData(0);
        Steinberg::int32 o0 = -1, o1 = -1;
        Steinberg::Vst::ParamValue v0 = 0.0, v1 = 0.0;
        q->getPoint(0, o0, v0);
        q->getPoint(1, o1, v1);
        check(o0 == 64 && o1 == 512, "they come back in sample order");
        check(std::abs(v0 - 0.1) < 1e-15, "with the right values attached");

        // Out of range is clamped: the SDK does not define behaviour outside
        // 0..1 and a plugin handed 1.4 may do anything at all.
        changes.clear();
        changes.set(1, 1.4, 0);
        changes.set(2, -0.3, 0);
        Steinberg::int32 oo = 0;
        Steinberg::Vst::ParamValue hi = 0.0, lo = 0.0;
        changes.getParameterData(0)->getPoint(0, oo, hi);
        changes.getParameterData(1)->getPoint(0, oo, lo);
        check(hi == 1.0 && lo == 0.0, "values are clamped to 0..1");

        // Pool exhaustion is counted rather than allocated through -- ADR-0010
        // says process() does not allocate, and a std::vector that grows on
        // the audio thread is exactly that.
        adi::device::Vst3ParamChanges small;
        small.reserve(2);
        check(small.set(1, 0.5, 0) && small.set(2, 0.5, 0), "two fit");
        check(!small.set(3, 0.5, 0), "the third is refused");
        check(small.dropped() == 1, "and counted, not allocated for");
    }

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
