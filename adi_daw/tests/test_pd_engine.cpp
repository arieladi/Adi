// SPDX-License-Identifier: GPL-3.0-or-later
//
// libpd, actually running -- ADR-0035, ADR-0183.
//
// `test_pd.cpp` proves the CONTRACT against a fake engine, with no libpd
// present. This proves the other half: that Pd is embedded, that a real patch
// from `pd/` opens in its own instance, and that audio goes in one end and
// comes out the other having been processed by Pd's DSP and not by us.
//
// THE PATCH IS A FIXTURE, NOT A DEVICE, and that is a finding rather than a
// convenience. Everything in `pd/` is an ABSTRACTION -- `inlet~` and `outlet~`,
// no `adc~`, no `dac~`. libpd renders the patch it opens as a TOP-LEVEL canvas,
// where `inlet~` is inert and connects to nothing, so those patches render
// silence through this engine and report no error doing it. Measured, not
// assumed: adi-rmsc.pd opens, four Pd blocks render, and every output sample is
// zero. `tests/pd/adi-proof.pd` is therefore top-level, and what to do about
// the device patches is ADR-0183's question, not this test's.
//
// The fixture answers two ways, because a pass-through test alone would also
// pass if the engine quietly copied its input and never called Pd:
//
//   * gain 1  -> the output IS the input, sample for sample, delayed by the
//                adapter and by nothing else;
//   * gain 0  -> silence.
//
// Both cannot pass unless Pd rendered them. It also carries the latency
// protocol: [loadbang] reports 99 at Pd's default rate, and the query after
// prepare reports 64 -- ADR-0095 decision 2, end to end for the first time.

#include "juce/pd_engine.hpp"
#include "adi/engine/graph.hpp"

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

namespace {

using namespace adi;
using adi::device::LibPdEngine;
using adi::device::PdLatencyReceiver;

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

/// Where `pd/` is. The test binary runs from the build directory, so the path
/// comes from the build rather than from a guess about the working directory.
const char* patchDir() { return ADI_PD_FIXTURE_DIR; }
const char* devicePatchDir() { return ADI_PD_PATCH_DIR; }

/// Planar buffers shaped the way `NodeIo` wants them.
struct Buffers {
    std::vector<std::vector<float>> in, sc, out;
    std::vector<const float*> inP, scP;
    std::vector<float*> outP;

    Buffers(int channels, int frames) {
        in.assign(static_cast<std::size_t>(channels), std::vector<float>(static_cast<std::size_t>(frames), 0.f));
        sc.assign(static_cast<std::size_t>(channels), std::vector<float>(static_cast<std::size_t>(frames), 0.f));
        out.assign(static_cast<std::size_t>(channels), std::vector<float>(static_cast<std::size_t>(frames), 0.f));
        for (auto& v : in) inP.push_back(v.data());
        for (auto& v : sc) scP.push_back(v.data());
        for (auto& v : out) outP.push_back(v.data());
    }
    engine::NodeIo io(int channels, int frames, int offset, int count) {
        engine::NodeIo n;
        n.in = inP.data();
        n.sidechain = scP.data();
        n.out = outP.data();
        n.channels = channels;
        n.frames = count;
        n.blockOffset = offset;
        (void)frames;
        return n;
    }
};

// ---------------------------------------------------------------------------

void testPdOpensARealPatch() {
    section("ADR-0035 -- libpd is embedded, and a patch from pd/ opens in it");

    std::string err;
    check(device::PdRuntime::initialise(err), "libpd initialises: " + err);
    eqi(device::PdRuntime::blockSize(), 64,
        "Pd's block is 64 frames, read from libpd rather than assumed -- the "
        "whole reason the engine needs a block adapter at all");

    LibPdEngine eng(patchDir(), "adi-proof.pd", 2, 2);
    PdLatencyReceiver latency;
    err.clear();
    check(eng.open(latency, err), "adi-proof.pd opens: " + err);
    check(eng.dollarZero() != 0,
          "the patch has a $0, which is what makes its receive names its own");

    LibPdEngine second(patchDir(), "adi-proof.pd", 2, 2);
    PdLatencyReceiver latency2;
    err.clear();
    check(second.open(latency2, err), "a SECOND instance of the same patch opens: " + err);

    // $0 is per INSTANCE, not per process: both of these are the same number,
    // which is why the routing table is per instance. ADR-0095 decision 1
    // assumed one instance and is amended by ADR-0183, not contradicted -- the
    // name still does the routing, inside the instance it belongs to.
    check(second.dollarZero() == eng.dollarZero(),
          "and gets the SAME $0 -- canvas_getdollarzero() is per-instance "
          "state, so two devices cannot be told apart by $0 alone");

    // The proof that they are told apart anyway: query ONE of them and watch
    // only that one's receiver move. Both patches answer on the same name,
    // `<$0>-report_latency` with the same $0, so a router that kept one table
    // would deliver this to whichever bound first.
    eng.requestLatencyReport();
    eqi(latency.latencySamples(), 64, "the queried patch answered its own receiver");
    eqi(latency2.latencySamples(), 0,
        "and the other receiver heard nothing, though both patches answer on "
        "the identical name -- which is what the per-instance table buys");
}

void testTheAdapterCostsOneBlockAndSaysSo() {
    section("ADR-0183 -- the block adapter's delay is reported, not hidden");

    LibPdEngine eng(patchDir(), "adi-proof.pd", 2, 2);
    PdLatencyReceiver latency;
    std::string err;
    if (!eng.open(latency, err)) { check(false, "open: " + err); return; }

    eqi(eng.adapterLatencySamples(), 0,
        "before prepare there is no adapter and so no delay to claim");
    eng.prepare(48000.0, 512);
    eqi(eng.adapterLatencySamples(), 64,
        "after prepare it is exactly one Pd block -- constant, because the "
        "output is primed with a block of silence so every segment can pop as "
        "many frames as it pushed");
}

/// Runs `frames` of a known signal through the patch in SEGMENTS of awkward
/// length, and returns the output of channel 0.
std::vector<float> runSegmented(LibPdEngine& eng, const std::vector<float>& sig,
                                float key, const std::vector<int>& segments) {
    const int total = static_cast<int>(sig.size());
    Buffers b(2, total);
    for (int f = 0; f < total; ++f) {
        b.in[0][static_cast<std::size_t>(f)] = sig[static_cast<std::size_t>(f)];
        b.in[1][static_cast<std::size_t>(f)] = sig[static_cast<std::size_t>(f)];
        b.sc[0][static_cast<std::size_t>(f)] = key;
        b.sc[1][static_cast<std::size_t>(f)] = key;
    }
    int at = 0;
    std::size_t s = 0;
    while (at < total) {
        const int n = std::min(segments[s % segments.size()], total - at);
        auto io = b.io(2, total, at, n);
        eng.process(io);
        at += n;
        ++s;
    }
    return b.out[0];
}

void testAudioGoesThroughPdAndComesBack() {
    section("ADR-0183 -- audio through Pd, in segments Pd cannot take directly");

    LibPdEngine eng(patchDir(), "adi-proof.pd", 2, 2);
    PdLatencyReceiver latency;
    std::string err;
    if (!eng.open(latency, err)) { check(false, "open: " + err); return; }

    // The patch reports 99 from [loadbang]. NOBODY HEARS IT, and that is
    // structural: the receive name needs $0, $0 comes from the opened patch,
    // and [loadbang] fires inside libpd_openfile. So a host cannot be listening
    // in time, however it is ordered.
    eqi(latency.latencySamples(), 0,
        "the [loadbang] report is not received -- it cannot be, and ADR-0183 "
        "records why");

    eng.prepare(48000.0, 512);
    eng.requestLatencyReport();
    eqi(latency.latencySamples(), 64,
        "the query after prepare is answered -- the protocol end to end through "
        "real libpd, and the ONLY report a host ever gets rather than a "
        "correction to an earlier one (ADR-0095 d2, amended)");

    // A ramp, so a delayed copy is distinguishable from any other copy: every
    // sample is unique, which a sine or a constant would not give.
    const int total = 2048;
    std::vector<float> sig(static_cast<std::size_t>(total));
    for (int f = 0; f < total; ++f) sig[static_cast<std::size_t>(f)] = static_cast<float>(f) / total;

    // Deliberately not multiples of 64. This is the case the adapter exists
    // for, and the one `NodeIo` actually delivers once an event splits a block.
    const std::vector<int> awkward{100, 37, 411, 64, 7, 250};

    check(eng.sendFloat("gain", 1.f), "gain 1 reaches [r $0-gain]");
    const auto pass = runSegmented(eng, sig, 0.f, awkward);

    const auto c = eng.counters();
    check(c.ticks > 0, "Pd actually rendered blocks (ticks > 0)");
    eqi(c.starved, 0, "no segment was starved of output");

    // Checked against the REPORTED delay, not a constant -- if the two ever
    // disagree this is what says so.
    const int d = eng.adapterLatencySamples();
    int matched = 0, compared = 0;
    for (int f = d; f < total; ++f) {
        ++compared;
        if (std::fabs(pass[static_cast<std::size_t>(f)] - sig[static_cast<std::size_t>(f - d)]) < 1e-6f)
            ++matched;
    }
    check(compared > 1500 && matched == compared,
          "at gain 1 the audio comes back unchanged, delayed by exactly the " +
          std::to_string(d) + " samples the engine reports (" +
          std::to_string(matched) + "/" + std::to_string(compared) + " samples)");

    // And the negative. If the engine were copying its input rather than
    // rendering, this would pass audio too and the pair would disagree.
    check(eng.sendFloat("gain", 0.f), "gain 0 reaches the patch");
    const auto silent = runSegmented(eng, sig, 0.f, awkward);
    float peak = 0.f;
    for (int f = d + 128; f < total; ++f)
        peak = std::max(peak, std::fabs(silent[static_cast<std::size_t>(f)]));
    check(peak < 1e-6f,
          "at gain 0 the same patch returns silence -- so the pass-through "
          "above was Pd's doing and not a memcpy (peak " +
          std::to_string(peak) + ")");
}

void testADevicePatchIsAnAbstractionAndRendersNothing() {
    section("ADR-0183 -- the patches in pd/ are abstractions, and libpd cannot render one");

    // This is the finding, asserted so it cannot quietly stop being true.
    // adi-rmsc.pd is inlet~/outlet~ with no adc~/dac~. Opened as a top-level
    // canvas -- which is the only thing libpd_openfile does -- its inlets
    // connect to nothing. It opens, it renders, and it emits silence, with no
    // error anywhere to say why.
    LibPdEngine eng(devicePatchDir(), "adi-rmsc.pd", 3, 2);
    PdLatencyReceiver latency;
    std::string err;
    if (!eng.open(latency, err)) { check(false, "adi-rmsc.pd opens: " + err); return; }
    eng.prepare(48000.0, 512);

    const int total = 512;
    std::vector<float> sig(static_cast<std::size_t>(total), 0.5f);
    const auto out = runSegmented(eng, sig, 0.f, {128});

    check(eng.counters().ticks > 0, "Pd rendered blocks for it");
    float peak = 0.f;
    for (float v : out) peak = std::max(peak, std::fabs(v));
    check(peak < 1e-9f,
          "and every output sample is zero: an abstraction opened as a "
          "top-level patch has nothing connected to its inlet~. A device patch "
          "has to reach adc~/dac~ somehow, and that is ADR-0183's open question "
          "-- not something for this engine to paper over");
}

void testAClosedEngineStillPassesAudio() {
    section("ADR-0095 -- a patch that never opened removes no signal");

    LibPdEngine eng(patchDir(), "no-such-patch.pd", 2, 2);
    PdLatencyReceiver latency;
    std::string err;
    check(!eng.open(latency, err), "a missing patch fails to open");
    check(!err.empty(), "and says why: " + err);
    eqi(eng.adapterLatencySamples(), 0, "an engine that never opened claims no delay");

    const int n = 128;
    Buffers b(2, n);
    for (int f = 0; f < n; ++f) { b.in[0][static_cast<std::size_t>(f)] = 0.5f; b.in[1][static_cast<std::size_t>(f)] = 0.5f; }
    auto io = b.io(2, n, 0, n);
    eng.process(io);
    bool through = true;
    for (int f = 0; f < n; ++f)
        if (std::fabs(b.out[0][static_cast<std::size_t>(f)] - 0.5f) > 1e-9f) through = false;
    check(through,
          "and passes its input straight through -- a device that failed to "
          "load must not silence the track");
}

}  // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::printf("adi_pd_engine_tests -- libpd, actually running\n\n");
    testPdOpensARealPatch();
    testTheAdapterCostsOneBlockAndSaysSo();
    testAudioGoesThroughPdAndComesBack();
    testADevicePatchIsAnAbstractionAndRendersNothing();
    testAClosedEngineStillPassesAudio();
    std::printf("\n%s -- %d checks, %d failure(s)\n",
                g_failures ? "FAILED" : "PASS", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
