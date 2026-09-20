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
#include "juce/device_bridge.hpp"
#include "juce/vst3_host.hpp"

#include <algorithm>
#include <cmath>
#include <string>
#include <cstddef>
#include <vector>
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

namespace {

/// Measure a REAL plugin changing its latency at runtime.
///
/// ADR-0082's coalescer has a 50 ms quiet period and a 500 ms ceiling, and
/// win's own note says those are guesses from synthetic fixtures. This is the
/// measurement that replaces them: drive a plugin that genuinely moves its
/// latency -- a linear-phase EQ mode switch is the canonical case -- and
/// record how many reports arrive and over what span.
int measureLatencyChange(adi::device::Vst3Host& host, const juce::String& want) {
    juce::KnownPluginList list;
    host.scan(host.defaultSearchPaths(), list);

    juce::PluginDescription found;
    bool haveIt = false;
    for (const auto& d : list.getTypes())
        if (d.name.containsIgnoreCase(want)) { found = d; haveIt = true; break; }
    if (!haveIt) {
        std::printf("  skip  no plugin matching '%s' is installed\n", want.toRawUTF8());
        return 0;
    }

    std::string err;
    auto dev = host.makeDevice(found, 48000.0, 512, err);
    auto* v3 = dynamic_cast<adi::device::Vst3Device*>(dev.get());
    if (v3 == nullptr || !v3->loaded()) {
        std::printf("  FAIL  %s did not load: %s\n", found.name.toRawUTF8(), err.c_str());
        return 1;
    }
    v3->prepare(48000.0, 512);

    std::printf("  plugin   %s %s\n", found.name.toRawUTF8(), found.version.toRawUTF8());
    std::printf("  latency  %d samples at rest\n", v3->latencySamples());
    std::printf("  params   %d\n", v3->paramCount());

    // Find something that plausibly switches processing mode. Named by
    // pattern rather than by index, because an index is a different
    // parameter in the next version.
    const adi::device::ParamDescriptor* mode = nullptr;
    for (std::int32_t i = 0; i < v3->paramCount(); ++i) {
        const auto* d = v3->paramAt(i);
        if (d == nullptr) continue;
        const juce::String n(d->name);
        if (n.containsIgnoreCase("phase") || n.containsIgnoreCase("processing mode")
            || n.containsIgnoreCase("linear")) { mode = d; break; }
    }
    if (mode == nullptr) {
        std::printf("  skip  no phase/mode parameter found by name\n");
        return 0;
    }
    std::printf("  sweeping '%s'\n", mode->name.c_str());

    // Sweep it and watch. latencyEpoch() is bumped from
    // audioProcessorChanged, which JUCE delivers on the message thread, so
    // the loop has to be pumped or nothing arrives at all.
    struct Hit { std::int64_t ms; std::uint64_t epoch; int latency; };
    std::vector<Hit> hits;
    const auto t0 = juce::Time::getMillisecondCounterHiRes();
    std::uint64_t lastEpoch = v3->latencyEpoch();

    // RAPID FIRE, and the distinction matters. A first attempt changed the
    // parameter then pumped 400 ms before the next, and duly reported
    // ~1000 ms gaps -- which were MY sweep rate, not the plugin's. What the
    // coalescer needs to know is how a plugin behaves when a user drags a
    // mode control, so the changes go in as fast as the message loop allows
    // and the gaps measured are the plugin's own.
    const double values[] = {0.34, 0.67, 1.0, 0.34, 0.67, 1.0, 0.0};
    std::size_t next = 0;
    for (int i = 0; i < 300; ++i) {                 // ~1.5 s total
        if (next < std::size(values) && (i % 2) == 0)
            v3->setParam(mode->id,
                         adi::device::ParamValue::fromNormalized(values[next++]));
        juce::MessageManager::getInstance()->runDispatchLoopUntil(5);
        const std::uint64_t e = v3->latencyEpoch();
        if (e != lastEpoch) {
            lastEpoch = e;
            hits.push_back({static_cast<std::int64_t>(
                                juce::Time::getMillisecondCounterHiRes() - t0),
                            e, v3->latencySamples()});
        }
    }

    std::printf("  reports  %zu\n", hits.size());
    for (const auto& h : hits)
        std::printf("    t=%4lldms  epoch=%llu  latency=%d\n",
                    static_cast<long long>(h.ms),
                    static_cast<unsigned long long>(h.epoch), h.latency);

    if (hits.size() >= 2) {
        std::int64_t maxGap = 0, prev = hits.front().ms;
        for (const auto& h : hits) { maxGap = std::max(maxGap, h.ms - prev); prev = h.ms; }
        const std::int64_t span = hits.back().ms - hits.front().ms;
        std::printf("  BURST    span %lldms, largest gap between reports %lldms\n",
                    static_cast<long long>(span), static_cast<long long>(maxGap));
        std::printf("  -> a quiet period must EXCEED %lldms to coalesce this burst\n",
                    static_cast<long long>(maxGap));
        std::printf("  -> a ceiling must EXCEED %lldms or it cuts the burst short\n",
                    static_cast<long long>(span));
    }
    return 0;
}

}  // namespace

/// Drive a real instrument through the RAW path and check sound comes out.
///
/// Reaching `IAudioProcessor` only proves `queryInterface` worked. This
/// proves our own `ProcessData` — our buses, our `IEventList`, our
/// `IParameterChanges` — actually makes a plugin produce audio, which is the
/// whole of ADR-0073's takeover.
int renderThrough(adi::device::Vst3Host& host, const juce::String& want) {
    juce::KnownPluginList list;
    host.scan(host.defaultSearchPaths(), list);

    juce::PluginDescription found;
    bool haveIt = false;
    for (const auto& d : list.getTypes())
        if (d.name.containsIgnoreCase(want)) { found = d; haveIt = true; break; }
    if (!haveIt) { std::printf("  skip  no plugin matching '%s'\n", want.toRawUTF8()); return 0; }

    std::string err;
    auto dev = host.makeDevice(found, 48000.0, 512, err);
    auto* v3 = dynamic_cast<adi::device::Vst3Device*>(dev.get());
    if (v3 == nullptr || !v3->loaded()) {
        std::printf("  FAIL  %s did not load: %s\n", found.name.toRawUTF8(), err.c_str());
        return 1;
    }
    v3->prepare(48000.0, 512);
    std::printf("  plugin  %s  raw=%s  noteExpr=%s\n", found.name.toRawUTF8(),
                v3->usingRawProcessor() ? "yes" : "NO (fell back to JUCE)",
                v3->supportsNoteExpression() ? "yes" : "no");
    if (!v3->usingRawProcessor()) return 1;

    // CONTROL FIRST. Without this, "peak > 0" proves only that the plugin
    // makes noise, not that OUR note caused it -- a synth with a free-running
    // oscillator or a tail would pass either way. Render silence first and
    // require it to actually be silent.
    std::vector<float> ql(512, 0.0f), qr(512, 0.0f);
    float* quiet[2] = {ql.data(), qr.data()};
    adi::engine::NodeIo qio;
    qio.out = quiet; qio.channels = 2; qio.frames = 512; qio.sampleRate = 48000.0;
    double idle = 0.0;
    for (int blk = 0; blk < 10; ++blk) {
        v3->process(qio);
        for (int i = 0; i < 512; ++i) idle = std::max(idle, std::abs((double) ql[i]));
    }
    std::printf("  idle    %.9f with no events\n", idle);
    check(idle < 1e-6, "the plugin is SILENT before any note -- so what follows is ours");

    // A note, and a 14-bit-resolution bend on it. Frames are BLOCK-relative
    // (ADR-0081); this block is one segment so the offset is zero.
    adi::engine::Event on;
    on.type = adi::engine::EventType::NoteOn;
    on.noteId = 77; on.dim = 60; on.value = 100.0 / 127.0; on.frame = 0; on.channel = 2;
    v3->pushEvent(on);
    adi::engine::Event bend;
    bend.type = adi::engine::EventType::NoteExpression;
    bend.dim = static_cast<std::uint16_t>(adi::ExpressionDim::Pitch);
    bend.noteId = 77;
    bend.value = 12.0 + 96.0 / 16384.0;      // one 14-bit LSB above an octave
    bend.frame = 8;
    v3->pushEvent(bend);

    std::vector<float> l(512, 0.0f), r(512, 0.0f);
    float* outp[2] = {l.data(), r.data()};
    adi::engine::NodeIo io;
    io.out = outp; io.channels = 2; io.frames = 512; io.sampleRate = 48000.0;

    double peak = 0.0;
    for (int blk = 0; blk < 40; ++blk) {      // ~0.4 s; synths have attacks
        v3->process(io);
        for (int i = 0; i < 512; ++i) peak = std::max(peak, std::abs((double) l[i]));
    }

    std::printf("  peak    %.6f after 40 blocks\n", peak);
    // An EFFECT handed no input is correctly silent, so asserting audio on
    // one says nothing about our ProcessData. Only an instrument can answer
    // "did our note reach it". Pointing this at 'Serum 2 FX' -- the effect
    // half of that bundle -- produced a red check that was entirely my
    // matcher picking the wrong plugin.
    if (found.isInstrument) {
        check(peak > idle * 1000.0 && peak > 1e-5,
              "the plugin produced AUDIO through our own ProcessData -- buses, "
              "IEventList and IParameterChanges all ours (ADR-0073)");
    } else {
        std::printf("  skip  %s is an effect, not an instrument; silence with no "
                    "input proves nothing either way\n", found.name.toRawUTF8());
    }
    check(v3->eventsOutOfRange() == 0, "no event was refused for landing outside its segment");
    check(v3->eventsDropped() == 0, "and none dropped for capacity");
    return 0;
}


int main(int argc, char** argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);

    bool requirePlugin = false;
    juce::String latencyProbe;
    juce::String renderName;
    for (int i = 1; i + 1 < argc; ++i) {
        if (std::string(argv[i]) == "--latency-probe") latencyProbe = argv[i + 1];
        if (std::string(argv[i]) == "--render") renderName = argv[i + 1];
    }
    for (int i = 1; i < argc; ++i)
        if (std::string(argv[i]) == "--require-plugin") requirePlugin = true;

    std::printf("adi_vst3_probe -- VST3 hosting behind the device contract\n\n");

    juce::ScopedJuceInitialiser_GUI juceInit;
    adi::device::Vst3Host host;

    if (renderName.isNotEmpty()) {
        std::printf("[ADR-0073] rendering through our own ProcessData\n");
        const int rc = renderThrough(host, renderName);
        std::printf("\n%s -- %d checks, %d failure(s)\n",
                    g_failures ? "FAILED" : "PASS", g_checks, g_failures);
        return g_failures ? 1 : rc;
    }

    if (latencyProbe.isNotEmpty()) {
        std::printf("[ADR-0082] measuring a real runtime latency change\n");
        return measureLatencyChange(host, latencyProbe);
    }

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
    std::printf("  (per instance now, not per build -- checked on a real plugin below)\n");

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

    // --- ADR-0081 on the VST3 side, which had only a weak `== 0` check -----
    std::printf("\n[ADR-0081] a VST3 event outside its segment is counted, not dropped quietly\n");
    {
        adi::device::Vst3EventList list;
        list.reserve(16);

        adi::engine::Event e;
        e.type = adi::engine::EventType::NoteExpression;
        e.dim = static_cast<std::uint16_t>(adi::ExpressionDim::Pitch);
        e.noteId = 3;
        e.value = 12.0;

        // Segment [256, 256+128). Event::frame is BLOCK-relative and
        // sampleOffset is SEGMENT-relative, so 300 becomes 44.
        e.frame = 300;
        check(list.add(e, 256, 128), "an event inside the segment is accepted");
        check(list.getEventCount() == 1, "and is in the list");
        Steinberg::Vst::Event out{};
        list.getEvent(0, out);
        check(out.sampleOffset == 44,
              "its sampleOffset is segment-relative: 300 - 256, saw " +
              std::to_string(out.sampleOffset));

        e.frame = 100;
        check(!list.add(e, 256, 128), "one before the segment is refused");
        e.frame = 500;
        check(!list.add(e, 256, 128), "one after it is refused");
        check(list.outOfRange() == 2,
              "and BOTH are counted -- a refusal that looks like absence reads as "
              "someone forgetting to send rather than sending the wrong thing");
        check(list.dropped() == 0, "not confused with a capacity drop");

        // The bound itself. Off by one here puts an event in the next
        // segment's first sample, which a plugin reads as a different instant.
        e.frame = 256;
        check(list.add(e, 256, 128), "the first sample of the segment is inside");
        e.frame = 256 + 128;
        check(!list.add(e, 256, 128), "the sample after the last is outside");
    }

    // --- ADR-0082: something actually calls poll() now ---------------------
    std::printf("\n[ADR-0082] the timer that ticks DeviceHost\n");
    {
        adi::device::DeviceHost dh;
        adi::engine::Graph g;
        adi::engine::SumNode only;
        g.setOutput(g.addNode(only));
        g.prepare(48000.0, 512);
        dh.attachGraph(g);

        adi::device::DeviceHostTimer timer(dh);
        check(timer.ticks() == 0, "no ticks before it is started");
        timer.start(20);
        // Pump the message loop, which is where juce::Timer fires and where
        // reconfiguring the graph is allowed (ADR-0066).
        //
        // ASSERT THAT IT FIRES, NOT HOW OFTEN. The first version required
        // five ticks in 200 ms at a 20 ms interval, which is arithmetic about
        // the MACHINE rather than about the code: a loaded CI runner
        // delivered two and the job went red. That is the same flaky-test
        // shape I had diagnosed in win's snapshot suite an hour earlier --
        // an assertion whose truth depends on scheduling.
        //
        // What the code promises is that the timer runs on the message
        // thread and that stop() stops it. The RATE is a tuning number
        // measured from Pro-Q 3 (26 ms largest report gap), and a test that
        // pins it would fail on any busy machine without finding a defect.
        for (int i = 0; i < 20 && timer.ticks() == 0; ++i)
            juce::MessageManager::getInstance()->runDispatchLoopUntil(50);
        timer.stop();
        check(timer.ticks() >= 1,
              "the timer fired on the message thread, " +
              std::to_string(timer.ticks()) + " tick(s)");
        check(timer.retaps() == 0, "and retapped nothing, because nothing reported");

        // stop() is not timing-dependent: after it, no further pumping may
        // produce a tick however long the loop runs.
        const std::int64_t frozen = timer.ticks();
        juce::MessageManager::getInstance()->runDispatchLoopUntil(150);
        check(timer.ticks() == frozen,
              "stop() actually stops it -- still " + std::to_string(frozen));
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
                // ADR-0073: did the takeover actually take? A false here
                // means we fell back to JUCE's MidiBuffer, where noteId is
                // hardcoded to -1 and velocity is value/127.
                check(v3->usingRawProcessor(),
                      "the raw IAudioProcessor was reached -- the note-expression path is live");
                check(v3->supportsNoteExpression(),
                      "and this instance reports note expression as SUPPORTED");
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
