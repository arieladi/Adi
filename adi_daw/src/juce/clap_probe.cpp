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

#include "adi/engine/host.hpp"
#include "juce/clap_host.hpp"
#include "juce/device_host.hpp"
#include "juce/probe_audio.hpp"
#include "juce/probe_surge.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <utility>
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


/// A constant. With DC in, the master's level is a number a test can name,
/// and a delay line that lost its history is a STEP in that number.
class Dc final : public adi::engine::Node {
public:
    explicit Dc(float v) : v_(v) {}
    void process(const adi::engine::NodeIo& io) noexcept override {
        for (std::int32_t c = 0; c < io.channels; ++c) {
            float* o = io.out[c] + io.blockOffset;
            for (std::int32_t i = 0; i < io.frames; ++i) o[i] = v_;
        }
    }
    [[nodiscard]] const char* name() const noexcept override { return "dc"; }
private:
    float v_;
};

/// ADR-0090, against a real plugin. win asked for exactly this: force a port
/// rescan, show the rebuild happens, show the audio does not break, show the
/// retired graph is reclaimed.
///
/// WHAT IS REAL HERE AND WHAT IS NOT. The plugin is real, its declared bus
/// layout is real, the graph is realised from a model the way a session
/// realises one, and the rebuild runs the whole plan-realise-prepare-publish
/// path. The TRIGGER is synthesised: no installed plugin changes its port
/// count on demand, so the rescan is delivered through the same
/// `clap_host_audio_ports.rescan` entry a plugin would call, on the real
/// glue the real plugin is registered with. That is the honest split, and
/// saying it here is cheaper than someone later believing the plugin moved
/// its own ports.
int rebuildAgainstARealPlugin(const std::string& want) {
    adi::device::ClapHost chost;
    chost.scan(adi::device::ClapHost::defaultSearchPaths());

    const adi::device::ClapPluginRef* pick = nullptr;
    for (const auto& r : chost.plugins())
        if (r.name.find(want) != std::string::npos) { pick = &r; break; }
    if (pick == nullptr) { std::printf("  skip  no CLAP '%s'\n", want.c_str()); return 0; }

    std::string err;
    auto dev = chost.makeDevice(*pick, 48000.0, 512, err);
    if (dev == nullptr || !dev->loaded()) { std::printf("  FAIL  %s\n", err.c_str()); return 1; }
    const bool instrument = pick->isInstrument;

    adi::engine::GraphHost gh;
    gh.setLatencyHeadroom(8192);
    gh.setFadeFrames(0);   // ADR-0089's fade would scale what this measures

    adi::device::DeviceHost host;
    host.add(std::move(dev), pick->name, /*trackId=*/1);
    host.watchClapGlue(chost.glue(), pick->name);

    // Two audio tracks and a master: the smallest model a session has.
    adi::rows::Model model;
    for (const auto& t : {std::pair<std::int64_t, const char*>{1, "audio"},
                          {2, "audio"}, {9, "master"}}) {
        adi::rows::Track row;
        row.id = t.first;
        row.kind = t.second;
        row.name = t.second;
        model.tracks.push_back(row);
    }

    adi::device::DeviceHost::RebuildSpec spec;
    spec.model = [&model] { return &model; };
    spec.sampleRate = 48000.0;
    spec.maxFrames = 512;
    host.attachHost(gh, spec);
    host.coalescer().setQuietPeriodMs(50);

    check(host.rebuildNow(), "the first graph realises with the plugin in it: " +
                                 host.lastRebuildError());
    std::printf("  plugin            %s%s\n", pick->name.c_str(),
                instrument ? "  [instrument]" : "");

    std::vector<float> l(512, 0.0f), r(512, 0.0f);
    float* outp[2] = {l.data(), r.data()};
    adi::engine::AudioIo aio;
    aio.out = outp; aio.numOut = 2; aio.frames = 512;

    // A held note, so an instrument is actually producing something across
    // the seam rather than being silent on both sides of it.
    //
    // PUSHED AT THE CHAIN HEAD, which is where a clip reader pushes. This
    // used to push at the TAIL, because events did not travel along edges and
    // a note at the head reached the MixNode and nothing else. ADR-0091 made
    // them travel; pushing at `inputFor` is now both correct and the thing
    // this probe proves.
    auto noteAt = [&](adi::engine::NodeId where) {
        adi::engine::RealizedGraph* rg = gh.current();
        if (rg == nullptr || where == adi::engine::kInvalidNode) return;
        adi::engine::Event on;
        on.type = adi::engine::EventType::NoteOn;
        on.noteId = 7; on.dim = 60; on.value = 0.8; on.frame = 0; on.channel = 2;
        rg->graph().pushInputEvent(where, on);
    };
    auto noteOn = [&] {
        adi::engine::RealizedGraph* rg = gh.current();
        if (rg != nullptr) noteAt(rg->inputFor(1));
    };

    auto render = [&](int blocks) {
        double peak = 0.0;
        bool finite = true;
        for (int b = 0; b < blocks; ++b) {
            gh.process(aio);
            for (std::size_t i = 0; i < l.size(); ++i) {
                if (!std::isfinite(l[i])) finite = false;
                peak = std::max(peak, std::abs((double) l[i]));
            }
        }
        return std::pair<double, bool>{peak, finite};
    };

    // THE GAP, MEASURED BEFORE IT IS WORKED AROUND. A note at the chain head
    // is what win's handoff describes a clip reader doing, and on a track
    // with a device chain it produces silence.
    if (instrument) {
        adi::engine::RealizedGraph* rg0 = gh.current();
        noteAt(rg0->inputFor(1));
        auto [headPeak, headFinite] = render(20);
        (void) headFinite;
        std::printf("  note at HEAD      peak %.6f  (inputFor -- a clip reader's push)\n",
                    headPeak);
        std::printf("  note at TAIL      the instrument itself\n");
        check(headPeak <= 1e-5,
              "ADR-0045 IS NOT IMPLEMENTED: events do not travel along edges, so "
              "a note pushed at the chain head never reaches the instrument. If "
              "this check FAILS, someone fixed it and this line should go");
    }

    noteOn();
    auto [peakBefore, finiteBefore] = render(40);
    std::printf("  before rebuild    peak %.6f\n", peakBefore);
    check(finiteBefore, "the graph renders finite samples before the rebuild");
    if (instrument)
        check(peakBefore > 1e-5, "and the instrument is sounding");

    adi::engine::Graph* before = gh.currentGraph();
    const std::int64_t publishedBefore = gh.stats().published;

    // --- THE PORT RESCAN --------------------------------------------------
    const clap_host_t* h = chost.glue().host();
    const auto* ports = static_cast<const clap_host_audio_ports_t*>(
        h->get_extension(h, CLAP_EXT_AUDIO_PORTS));
    if (ports == nullptr) { std::printf("  FAIL  no audio-ports host extension\n"); return 1; }
    ports->rescan(h, CLAP_AUDIO_PORTS_RESCAN_CHANNEL_COUNT);

    std::int64_t now = 1000;
    host.tick(now);
    check(host.stats().rebuilds == 0, "nothing rebuilds while the burst is open");
    now += 60;
    host.tick(now);

    const auto& st = host.coalescer().stats();
    std::printf("  shapeReports %lld  rebuildsNeeded %lld  rebuilds %lld  failed %lld\n",
                (long long) st.shapeReports, (long long) st.rebuildsNeeded,
                (long long) host.stats().rebuilds,
                (long long) host.stats().rebuildsFailed);

    check(host.stats().rebuilds == 1, "the rescan produced exactly one rebuild");
    check(host.stats().rebuildsFailed == 0,
          "which succeeded: " + host.lastRebuildError());
    check(gh.stats().published == publishedBefore + 1, "a new graph is published");
    check(gh.currentGraph() != before, "and it is a different graph");
    check(!host.coalescer().rebuildNeeded(), "the flag is answered");

    // The SAME plugin instance, re-injected rather than reloaded.
    check(host.chainFor(1).size() == 1 && host.chainFor(1)[0] == &host.nodeAt(0),
          "the same device node is in the new graph -- no plugin was reloaded");

    // --- DOES THE PLUGIN'S OWN STATE SURVIVE THE SWAP? -------------------
    //
    // Not re-triggered. `Graph::prepare` calls `prepare` on every node, and
    // a `DeviceNode`'s prepare activates the plugin -- so a rebuild
    // re-activates every plugin in the project whether or not anything about
    // it changed. If a held note is gone here, ADR-0042 d5's "a rebuild is
    // not a reason to reload a plugin" holds for LOADING and not for STATE.
    if (instrument) {
        auto [heldPeak, heldFinite] = render(20);
        (void) heldFinite;
        std::printf("  held note, not retriggered, after swap: peak %.6f\n", heldPeak);
        check(heldPeak > 1e-5,
              "the note held across the rebuild -- the plugin was not reset");
    }

    // --- the audio across the seam ---------------------------------------
    noteOn();
    auto [peakAfter, finiteAfter] = render(40);
    std::printf("  after rebuild     peak %.6f\n", peakAfter);
    check(finiteAfter, "the new graph renders finite samples");
    if (instrument)
        check(peakAfter > 1e-5, "and the instrument is still sounding after the swap");

    check(gh.stats().swaps >= 2, "the audio thread picked up the new graph, swaps=" +
                                     std::to_string(gh.stats().swaps));

    // --- reclamation ------------------------------------------------------
    check(gh.stats().reclaimed == 0,
          "nothing was freed while the reader might still have held it");
    now += 60;
    host.tick(now);

    // --- AND DOES IT SURVIVE THE RECLAMATION? ----------------------------
    //
    // The retired graph is freed here, and a Graph's teardown calls
    // `release()` on every node it holds -- including the devices it SHARES
    // with the graph that is currently rendering.
    if (instrument) {
        auto [afterFree, freeFinite] = render(20);
        (void) freeFinite;
        std::printf("  still sounding after the old graph was freed: peak %.6f\n",
                    afterFree);
        check(afterFree > 1e-5,
              "freeing the RETIRED graph did not release the device the LIVE "
              "graph is using");
    }

    std::printf("  published %lld  swaps %lld  reclaimed %lld  refused %lld\n",
                (long long) gh.stats().published, (long long) gh.stats().swaps,
                (long long) gh.stats().reclaimed, (long long) gh.stats().refused);
    check(gh.stats().reclaimed == 1, "the tick reclaimed the retired graph");

    return 0;
}


/// win asked one question and named it as a real cost he did not fix: a
/// rebuild resets EVERY edge's compensation history, including the edges
/// whose routing did not change. Is that audible?
///
/// This measures it rather than arguing it. Two tracks into a master, a
/// plugin with 5120 samples of latency on one of them, DC on both so the
/// master's level is a number. PDC delays the DRY path to match the wet one;
/// a rebuild hands that edge a fresh ring, and a fresh ring is empty.
int measureTheSeam(const std::string& want, bool wetOnly) {
    adi::device::ClapHost chost;
    chost.scan(adi::device::ClapHost::defaultSearchPaths());

    const adi::device::ClapPluginRef* pick = nullptr;
    for (const auto& r : chost.plugins())
        if (r.name.find(want) != std::string::npos) { pick = &r; break; }
    if (pick == nullptr) { std::printf("  skip  no CLAP '%s'\n", want.c_str()); return 0; }

    std::string err;
    auto dev = chost.makeDevice(*pick, 48000.0, 512, err);
    if (dev == nullptr || !dev->loaded()) { std::printf("  FAIL  %s\n", err.c_str()); return 1; }

    std::string modeId;
    double modeMax = 1.0;
    for (std::int32_t i = 0; i < dev->paramCount(); ++i) {
        const auto* d = dev->paramAt(i);
        if (d != nullptr && (d->name.find("hase") != std::string::npos ||
                             d->name.find("rocessing") != std::string::npos)) {
            modeId = d->id; modeMax = d->maxReal; break;
        }
    }
    if (modeId.empty()) { std::printf("  skip  no mode parameter on %s\n",
                                      pick->name.c_str()); return 0; }

    adi::engine::GraphHost gh;
    gh.setLatencyHeadroom(8192);
    gh.setFadeFrames(0);   // the fade is ADR-0089's and would mask the seam

    adi::device::DeviceHost host;
    host.add(std::move(dev), pick->name, /*trackId=*/1);
    host.watchClapGlue(chost.glue(), pick->name);
    auto& inst = host.deviceAt(0);

    adi::rows::Model model;
    for (const auto& t : {std::pair<std::int64_t, const char*>{1, "audio"},
                          {2, "audio"}, {9, "master"}}) {
        adi::rows::Track row;
        row.id = t.first; row.kind = t.second; row.name = t.second;
        model.tracks.push_back(row);
    }

    adi::device::DeviceHost::RebuildSpec spec;
    spec.model = [&model] { return &model; };
    spec.sampleRate = 48000.0;
    spec.maxFrames = 512;
    host.attachHost(gh, spec);
    host.coalescer().setQuietPeriodMs(50);

    check(host.rebuildNow(), "the compensated graph realises: " + host.lastRebuildError());

    // DC on both tracks: the master's level is then a number, and a lost
    // delay line is a step in it rather than a subtle smear.
    // DISTINGUISHABLE LEVELS, so the hole says WHICH path is missing rather
    // than only that one is. Settled is 1.0; a hole at 0.75 is the wet path
    // gone, at 0.25 the dry path's ring, at 0.0 both.
    Dc wet(0.25f), dry(wetOnly ? 0.0f : 0.75f);
    auto feedBoth = [&] {
        adi::engine::RealizedGraph* rg = gh.current();
        adi::engine::Graph& g = rg->graph();
        g.connect(g.addNode(wet), rg->inputFor(1));
        g.connect(g.addNode(dry), rg->inputFor(2));
        g.prepare(48000.0, 512);
        return g.ok();
    };
    check(feedBoth(), "both tracks are fed");

    std::vector<float> l(512, 0.0f), r(512, 0.0f);
    float* outp[2] = {l.data(), r.data()};
    adi::engine::AudioIo aio;
    aio.out = outp; aio.numOut = 2; aio.frames = 512;

    // LINEAR PHASE, AFTER ACTIVATION. Setting it before the first rebuild
    // sets it on a plugin that has not been activated, and a CLAP plugin's
    // latency may not be read before activate (ADR-0087) -- so the graph
    // would be built compensating a zero it was not allowed to ask for.
    std::int64_t now = 1000;
    for (int b = 0; b < 4; ++b) { gh.process(aio); host.tick(now); now += 20; }
    inst.setParam(modeId, adi::device::ParamValue::withReal(0.0, modeMax));
    for (int b = 0; b < 40; ++b) { gh.process(aio); host.tick(now); now += 5; }
    now += 120;
    host.tick(now);

    std::printf("  plugin latency    %d samples\n", inst.latencySamples());
    std::printf("  retaps            %lld\n",
                (long long) host.coalescer().stats().retaps);
    if (inst.latencySamples() <= 0) {
        std::printf("  skip  %s reports no latency in this mode; nothing to compensate\n",
                    pick->name.c_str());
        return 0;
    }

    // Settle: past the plugin's own latency, the master is at its steady
    // level and that level is what a seam has to be measured against.
    for (int b = 0; b < 40; ++b) gh.process(aio);
    const double settled = std::abs((double) l[511]);
    std::printf("  settled level     %.6f\n", settled);

    // --- the rebuild ------------------------------------------------------
    const clap_host_t* h = chost.glue().host();
    const auto* ports = static_cast<const clap_host_audio_ports_t*>(
        h->get_extension(h, CLAP_EXT_AUDIO_PORTS));
    ports->rescan(h, CLAP_AUDIO_PORTS_RESCAN_CHANNEL_COUNT);
    host.tick(now);
    now += 60;
    host.tick(now);
    check(host.stats().rebuilds == 1, "one rebuild");
    check(feedBoth(), "the new graph is fed the same two sources");

    // --- how long is the hole, and how deep -------------------------------
    double worst = settled;
    std::int64_t belowFor = 0;
    bool stillBelow = true;
    for (int b = 0; b < 60; ++b) {
        gh.process(aio);
        for (std::size_t i = 0; i < l.size(); ++i) {
            const double v = std::abs((double) l[i]);
            if (v < settled * 0.99) {
                worst = std::min(worst, v);
                if (stillBelow) ++belowFor;
            } else {
                stillBelow = false;
            }
        }
    }

    std::printf("  worst after swap  %.6f  (%.1f%% of settled)\n",
                worst, settled > 0.0 ? 100.0 * worst / settled : 0.0);
    // wet 0.25 + dry 0.75 = 1.0, so the level DURING the hole names what
    // survived: 0.25 is the wet path alone, 0.75 the dry path alone.
    if (!wetOnly)
        std::printf("  attribution       %s\n",
                    worst < 0.1 ? "BOTH paths went silent"
                    : (worst < 0.4 ? "the DRY path is missing (its ring)"
                                   : "the WET path is missing (the plugin)"));
    std::printf("  below settled for %lld samples  (%.1f ms at 48k)\n",
                (long long) belowFor, (double) belowFor / 48.0);
    std::printf("  plugin latency    %d samples\n", inst.latencySamples());

    // Two findings, and they are different findings.
    if (wetOnly) {
        // Nothing but the plugin's own path. A hole here would mean the
        // rebuild re-primed the plugin, which it did until `ClapDevice::
        // prepare` learned to do nothing when nothing changed.
        check(belowFor == 0,
              "the plugin is NOT re-primed by a rebuild: no hole with no ring "
              "in the path, was 5120 samples before prepare became idempotent");
    } else {
        // This used to assert belowFor > 0 -- a pin on the cost ADR-0089
        // named and did not fix, written so that fixing it would make this
        // line fail. ADR-0092 carries ring history across the swap, so the
        // dry path no longer drops for its compensation delay.
        check(belowFor == 0,
              "the ring history survives the rebuild: the DRY path does not "
              "drop, where it dropped for exactly its compensation delay "
              "(5120 samples on Pro-Q 3) before ADR-0092");
    }
    return 0;
}

}  // namespace

/// ADR-0098/0099: per-note expression through the CLAP host, measured on a
/// real synth, in EVERY dialect the plugin declares. The CLAP twin of
/// adi_vst3_probe --mpe, with the same pass rule: each scene sounds bent
/// exactly as sent, or unbent -- never at a wrong pitch, never dragging
/// another note -- and at least one dialect delivers.
///
///   A  C4 bent +12                       B  C4 +7 on ch 2, E4 on ch 3
///   C  B with both notes on ONE channel   D  (MidiMpe) a reused channel is reset
int clapMpeAcceptance(const std::string& want) {
    using adi::engine::Event;
    using adi::engine::EventType;
    using adi::device::ClapDialect;
    using adi::device::ClapDialectChoice;
    namespace pr = adi::probe;

    double worst = 0.0;
    const bool trusted = pr::selfTest(&worst);
    check(trusted, "the pitch and level measurements pass their own self-test (worst " +
                       std::to_string(worst) + " cents)");
    if (!trusted) return 1;

    adi::device::ClapHost chost;
    chost.scan(adi::device::ClapHost::defaultSearchPaths());
    const adi::device::ClapPluginRef* pick = nullptr;
    for (const auto& r : chost.plugins())
        if (r.isInstrument && r.name.find(want) != std::string::npos) { pick = &r; break; }
    check(pick != nullptr, "a CLAP INSTRUMENT matching '" + want + "' is installed");
    if (pick == nullptr) return 1;
    std::printf("  plugin  %s by %s %s\n", pick->name.c_str(), pick->vendor.c_str(),
                pick->version.c_str());

    const double sr = 48000.0;
    const std::size_t from = 32768 - 16384, n = 16384;
    auto note = [](std::uint64_t id, int key, int chan) {
        Event e; e.type = EventType::NoteOn; e.noteId = id; e.dim = static_cast<std::uint16_t>(key);
        e.channel = static_cast<std::uint8_t>(chan); e.value = 0.8; return e;
    };
    auto bend = [](std::uint64_t id, double semis, int chan) {
        Event e; e.type = EventType::NoteExpression; e.noteId = id; e.channel = static_cast<std::uint8_t>(chan);
        e.dim = static_cast<std::uint16_t>(adi::ExpressionDim::Pitch); e.value = semis; return e;
    };
    auto off = [](std::uint64_t id, int key, std::int32_t frame) {
        Event e; e.type = EventType::NoteOff; e.noteId = id; e.dim = static_cast<std::uint16_t>(key);
        e.channel = 2; e.frame = frame; return e;
    };

    enum class Heard { Bent, Unbent, Wrong };
    auto name = [](Heard h) { return h == Heard::Bent ? "bent" : (h == Heard::Unbent ? "unbent" : "WRONG"); };
    auto single = [&](const std::vector<float>& a) {
        const double hz = pr::estimateHz(a, from, n, sr, 80.0, 1200.0);
        if (std::abs(pr::centsBetween(hz, pr::midiHz(72))) < 15.0) return Heard::Bent;
        if (std::abs(pr::centsBetween(hz, pr::midiHz(60))) < 15.0) return Heard::Unbent;
        return Heard::Wrong;
    };
    auto pair = [&](const std::vector<float>& a) {
        const double g4 = pr::toneLevel(a, from, n, sr, 392.0);
        const double e4 = pr::toneLevel(a, from, n, sr, pr::midiHz(64));
        const double b4 = pr::toneLevel(a, from, n, sr, pr::midiHz(71));
        const double c4 = pr::toneLevel(a, from, n, sr, pr::midiHz(60));
        const double top = std::max({g4, e4, b4, c4});
        auto present = [&](double l) { return pr::dbRel(l, top) > -12.0; };
        auto absent = [&](double l) { return pr::dbRel(l, top) < -30.0; };
        if (present(g4) && present(e4) && absent(b4) && absent(c4)) return Heard::Bent;
        if (present(c4) && present(e4) && absent(g4) && absent(b4)) return Heard::Unbent;
        return Heard::Wrong;
    };

    struct Row { const char* label; ClapDialectChoice choice; ClapDialect used{}; Heard a{}, b{}, c{};
                 double hzD = 0; bool haveD = false; };
    Row rows[] = {{"Auto", ClapDialectChoice::Auto}, {"Clap", ClapDialectChoice::Clap},
                  {"MidiMpe", ClapDialectChoice::MidiMpe}, {"Midi", ClapDialectChoice::Midi}};

    for (Row& row : rows) {
        for (int scene = 0; scene < 4; ++scene) {
            std::string err;
            auto dev = chost.makeDevice(*pick, sr, 512, err);
            auto* cd = dynamic_cast<adi::device::ClapDevice*>(dev.get());
            if (cd == nullptr || !cd->loaded()) { check(false, "the plugin loads: " + err); return 1; }
            if (scene == 0 && row.choice == ClapDialectChoice::Auto) {
                const auto& np = cd->notePorts();
                std::printf("  note ports  extension=%s input=%s supported=0x%x preferred=0x%x\n",
                            np.extension ? "yes" : "no", np.input ? "yes" : "no",
                            static_cast<unsigned>(np.supported), static_cast<unsigned>(np.preferred));
            }
            cd->setNoteDialect(row.choice);
            cd->prepare(sr, 512);
            // Scene D is MidiMpe's reset. The dialect is read AFTER rendering:
            // makeDevice already activated the plugin, so this prepare changes
            // nothing and the request is applied by the first process call.
            if (scene == 3 && row.choice != ClapDialectChoice::MidiMpe) continue;
            std::vector<Event> ev;
            if (scene == 0) ev = {note(1, 60, 2), bend(1, 12.0, 2)};
            if (scene == 1) ev = {note(1, 60, 2), bend(1, 7.0, 2), note(2, 64, 3)};
            if (scene == 2) ev = {note(1, 60, 1), bend(1, 7.0, 1), note(2, 64, 1)};
            if (scene == 3) {
                ev = {note(1, 60, 2), bend(1, 12.0, 2), off(1, 60, 10)};
                for (int k = 2; k <= 15; ++k) {
                    Event on = note(static_cast<std::uint64_t>(k), 40 + k, 2);
                    on.frame = 20 + 4 * k;
                    ev.push_back(on);
                    ev.push_back(off(static_cast<std::uint64_t>(k), 40 + k, 22 + 4 * k));
                }
                Event last = note(16, 64, 2);
                last.frame = 120;
                ev.push_back(last);
            }
            const auto audio = pr::render(*cd, ev, 64, 512, sr);
            row.used = cd->noteDialect();
            if (scene == 0) row.a = single(audio);
            if (scene == 1) row.b = pair(audio);
            if (scene == 2) row.c = pair(audio);
            if (scene == 3) { row.hzD = pr::estimateHz(audio, from, n, sr, 80.0, 1200.0); row.haveD = true; }
            check(cd->eventsDropped() == 0 && cd->eventsOutOfRange() == 0,
                  "nothing dropped or out of range in the scene");
        }
    }

    auto dialectName = [](ClapDialect d) {
        return d == ClapDialect::Clap ? "Clap" : d == ClapDialect::MidiMpe ? "MidiMpe"
             : d == ClapDialect::Midi ? "Midi" : "None";
    };
    // Measured 2026-09-21 (ADR-0099). Scene A per dialect asked for.
    static const pr::Baseline kBase[] = {
        {"Surge XT", "1.3.4", "Auto", "bent"},
        {"Surge XT", "1.3.4", "Clap", "bent"},
        {"Surge XT", "1.3.4", "MidiMpe", "bent"},
        {"Surge XT", "1.3.4", "Midi", "unbent"},
    };
    const std::size_t nBase = sizeof kBase / sizeof kBase[0];
    if (!pr::hasBaseline(kBase, nBase, pick->name, pick->version))
        std::printf("  (no baseline for %s %s: judged by the general rule only)\n",
                    pick->name.c_str(), pick->version.c_str());
    std::printf("\n");
    bool anyDelivers = false;
    for (const Row& row : rows) {
        std::printf("  %-8s -> %-8s A %-7s B %-7s C %-7s", row.label, dialectName(row.used),
                    name(row.a), name(row.b), name(row.c));
        if (row.haveD)
            std::printf(" D %.2f Hz (%+.1f c from E4)", row.hzD, pr::centsBetween(row.hzD, pr::midiHz(64)));
        std::printf("\n");
        const std::string who = std::string(row.label) + " (" + dialectName(row.used) + ")";
        check(row.a != Heard::Wrong && row.b != Heard::Wrong && row.c != Heard::Wrong,
              who + ": every scene bent exactly or not at all -- never a wrong pitch, never dragged");
        check(row.a == row.b && row.b == row.c, who + ": the plugin reads it in every scene or in none");
        if (row.haveD)
            check(std::abs(pr::centsBetween(row.hzD, pr::midiHz(64))) < 15.0,
                  who + " D: a reused member channel starts in tune");
        if (row.used == ClapDialect::Midi)
            check(row.a == Heard::Unbent, who + ": plain MIDI carries no per-note pitch, so nothing bends");
        if (row.a == Heard::Bent) anyDelivers = true;
        if (const char* expected = pr::baselineFor(kBase, nBase, pick->name, pick->version, row.label))
            check(std::string(expected) == name(row.a),
                  who + ": " + name(row.a) + ", as measured before (" + expected + ")");
    }
    check(anyDelivers, "per-note pitch reaches this synth in at least one dialect");
    return 0;
}

/// ADR-0100: pressure and timbre through the CLAP host, measured on Surge XT
/// with a patch that makes each audible (probe_surge.hpp). Every dialect, both
/// dimensions: delivered PER NOTE, or not delivered -- never globally.
int clapDimensions(const std::string& want) {
    using adi::device::ClapDialect;
    using adi::device::ClapDialectChoice;
    namespace pr = adi::probe;

    adi::device::ClapHost chost;
    chost.scan(adi::device::ClapHost::defaultSearchPaths());
    const adi::device::ClapPluginRef* pick = nullptr;
    for (const auto& r : chost.plugins())
        if (r.isInstrument && r.name.find(want) != std::string::npos) { pick = &r; break; }
    check(pick != nullptr, "a CLAP INSTRUMENT matching '" + want + "' is installed");
    if (pick == nullptr) return 1;

    const double sr = 48000.0;
    const std::size_t from = 32768 - 16384, n = 16384;
    struct Dialect { const char* label; ClapDialectChoice choice; };
    const Dialect dialects[] = {{"Clap", ClapDialectChoice::Clap}, {"MidiMpe", ClapDialectChoice::MidiMpe},
                                {"Midi", ClapDialectChoice::Midi}};
    const adi::ExpressionDim dims[] = {adi::ExpressionDim::Pressure, adi::ExpressionDim::Timbre};
    bool delivered[2] = {false, false};
    // Measured 2026-09-21 (ADR-0100).
    static const pr::Baseline kBase[] = {
        {"Surge XT", "1.3.4", "Clap pressure", "per note"},
        {"Surge XT", "1.3.4", "Clap timbre", "per note"},
        {"Surge XT", "1.3.4", "MidiMpe pressure", "per note"},
        {"Surge XT", "1.3.4", "MidiMpe timbre", "per note"},
        {"Surge XT", "1.3.4", "Midi pressure", "per note"},
        {"Surge XT", "1.3.4", "Midi timbre", "not delivered"},
    };
    const std::size_t nBase = sizeof kBase / sizeof kBase[0];
    if (!pr::hasBaseline(kBase, nBase, pick->name, pick->version))
        std::printf("  (no baseline for %s %s: judged by the general rule only)\n",
                    pick->name.c_str(), pick->version.c_str());

    std::printf("\n  %-8s %-9s %9s %9s  verdict\n", "dialect", "dimension", "single", "pair");
    for (const Dialect& dl : dialects) {
        for (int di = 0; di < 2; ++di) {
            const adi::ExpressionDim d = dims[di];
            bool routed = true;
            auto render = [&](const std::vector<adi::engine::Event>& ev) {
                std::string err;
                auto dev = chost.makeDevice(*pick, sr, 512, err);
                auto* cd = dynamic_cast<adi::device::ClapDevice*>(dev.get());
                if (cd == nullptr || !cd->loaded()) { routed = false; return std::vector<float>{}; }
                auto state = cd->saveState("chunk");
                std::string why;
                if (!pr::routeSurgePatch(state, d, why) || !cd->loadState("chunk", state)) {
                    std::printf("  could not route the patch: %s\n", why.c_str());
                    routed = false;
                    return std::vector<float>{};
                }
                cd->setNoteDialect(dl.choice);
                cd->prepare(sr, 512);
                pr::render(*cd, {}, 16, 512, sr);          // let the new patch land first
                return pr::render(*cd, ev, 64, 512, sr);
            };
            // MPE's timbre neutral is CC74 64 -- see dimScenes.
            const double low = (d == adi::ExpressionDim::Timbre && dl.choice == ClapDialectChoice::MidiMpe)
                                   ? 0.5 : 0.0;
            const pr::DimHeard h = pr::measureDimension(d, render, from, n, sr, low);
            const char* dname = d == adi::ExpressionDim::Pressure ? "pressure" : "timbre";
            std::printf("  %-8s %-9s %+8.1f %+8.1f  %s\n", dl.label, dname, h.single, h.pair,
                        pr::verdictName(h.verdict));
            std::printf("           C4 alone %+.1f/%+.1f   pair A: C4 %+.1f E4 %+.1f   pair B: C4 %+.1f E4 %+.1f\n",
                        h.c4Hi, h.c4Lo, h.c4A, h.e4A, h.c4B, h.e4B);
            check(routed, std::string(dl.label) + " " + dname + ": the patch was routed and loaded");
            check(h.verdict != pr::DimHeard::Verdict::Wrong,
                  std::string(dl.label) + " " + dname +
                      ": per note or not at all -- never applied to every note");
            if (h.verdict == pr::DimHeard::Verdict::PerNote) delivered[di] = true;
            const std::string what = std::string(dl.label) + " " + dname;
            if (const char* expected = pr::baselineFor(kBase, nBase, pick->name, pick->version, what))
                check(std::string(expected) == pr::verdictName(h.verdict),
                      what + ": " + pr::verdictName(h.verdict) + ", as measured before (" + expected + ")");
        }
    }
    std::printf("\n");
    check(delivered[0], "pressure reaches this synth per note in at least one dialect");
    check(delivered[1], "timbre reaches this synth per note in at least one dialect");
    return 0;
}

int main(int argc, char** argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::printf("adi_clap_probe -- a real .clap, and ADR-0084's open question\n\n");

    std::string path = "/Library/Audio/Plug-Ins/CLAP/Surge XT.clap";
    std::string latencyWant, coalesceWant, rebuildWant, seamWant, mpeWant, dimWant;
    bool wetOnly = false;
    for (int i = 1; i < argc; ++i)
        if (std::string(argv[i]) == "--wet-only") wetOnly = true;
    for (int i = 1; i + 1 < argc; ++i) {
        if (std::string(argv[i]) == "--plugin") path = argv[i + 1];
        if (std::string(argv[i]) == "--latency") latencyWant = argv[i + 1];
        if (std::string(argv[i]) == "--coalesce") coalesceWant = argv[i + 1];
        if (std::string(argv[i]) == "--rebuild") rebuildWant = argv[i + 1];
        if (std::string(argv[i]) == "--seam") seamWant = argv[i + 1];
        if (std::string(argv[i]) == "--mpe") mpeWant = argv[i + 1];
        if (std::string(argv[i]) == "--dimensions") dimWant = argv[i + 1];
    }
    if (!dimWant.empty()) {
        std::printf("[ADR-0100] pressure and timbre through the CLAP host, measured\n");
        const int rc = clapDimensions(dimWant);
        std::printf("\n%s -- %d checks, %d failure(s)\n",
                    g_failures ? "FAILED" : "PASS", g_checks, g_failures);
        return g_failures ? 1 : rc;
    }
    // --dump-state <plugin> <file>: the plugin's opaque state, as our host
    // saves it -- for reading a format before a test edits it (ADR-0100).
    for (int i = 1; i + 2 < argc; ++i) {
        if (std::string(argv[i]) != "--dump-state") continue;
        adi::device::ClapHost chost;
        chost.scan(adi::device::ClapHost::defaultSearchPaths());
        for (const auto& r : chost.plugins()) {
            if (r.name != argv[i + 1]) continue;
            std::string err;
            auto dev = chost.makeDevice(r, 48000.0, 512, err);
            if (!dev || !dev->loaded()) break;
            const auto st = dev->saveState("chunk");
            std::ofstream f(argv[i + 2], std::ios::binary);
            f.write(reinterpret_cast<const char*>(st.data()), static_cast<std::streamsize>(st.size()));
            std::printf("dumped %zu bytes of %s\n", st.size(), r.name.c_str());
            return f.good() ? 0 : 1;
        }
        std::printf("no CLAP plugin named '%s'\n", argv[i + 1]);
        return 1;
    }
    if (!mpeWant.empty()) {
        std::printf("[ADR-0098] per-note expression through the CLAP host, measured\n");
        const int rc = clapMpeAcceptance(mpeWant);
        std::printf("\n%s -- %d checks, %d failure(s)\n",
                    g_failures ? "FAILED" : "PASS", g_checks, g_failures);
        return g_failures ? 1 : rc;
    }
    if (!seamWant.empty()) {
        std::printf("[ADR-0090] does a rebuild's lost compensation history show?\n");
        const int rc = measureTheSeam(seamWant, wetOnly);
        std::printf("\n%s -- %d checks, %d failure(s)\n",
                    g_failures ? "FAILED" : "PASS", g_checks, g_failures);
        return g_failures ? 1 : rc;
    }

    if (!rebuildWant.empty()) {
        std::printf("[ADR-0089/0090] a real plugin, a port rescan, and a new graph\n");
        const int rc = rebuildAgainstARealPlugin(rebuildWant);
        std::printf("\n%s -- %d checks, %d failure(s)\n",
                    g_failures ? "FAILED" : "PASS", g_checks, g_failures);
        return g_failures ? 1 : rc;
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
        // THE REAL COUNTERS, not a literal. This used to print a hard-coded
        // "PASS -- 0 checks, 0 failure(s)" and return 0 -- after the scan
        // section above had already recorded a FAIL. The skip is honest about
        // this one bundle; the summary has to stay honest about the whole run.
        std::printf("\n%s -- %d checks, %d failure(s)\n",
                    g_failures ? "FAILED" : "PASS", g_checks, g_failures);
        return g_failures ? 1 : 0;
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
