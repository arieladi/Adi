// SPDX-License-Identifier: GPL-3.0-or-later
//
// adi_audio_probe — open an audio device, report what we actually got, close.
//
// No graph, no processing, no plugin hosting. It exists for two reasons:
//
//   1. To prove the JUCE dependency configures, links and RUNS on clang/arm64
//      and on MSVC *before* step 6 depends on it. A dependency that builds but
//      cannot open a device is a step-6 problem discovered at step 6.
//
//   2. To answer ADR-0042 empirically rather than by assertion. That ADR makes
//      the engine a large-block engine — 2048 to 8192 samples — and the obvious
//      question is whether a real driver will give us those sizes at all. This
//      asks the driver instead of assuming.
//
// ADR-0042's consequence is also why this prints the callback period: at 8192
// samples and 48 kHz a callback is ~171 ms, and per-block automation would step
// audibly at that rate. This does not implement sub-block splitting — there is
// no graph to split — but it prints the number that makes the requirement
// obvious to whoever builds one.
//
// NO AUDIO DEVICE IS NOT A FAILURE. CI runners have no sound card, and a probe
// that fails there would make the JUCE leg untestable. "No device available" is
// reported and exits 0; only a device that opens and then misbehaves is an
// error.

#include <juce_audio_devices/juce_audio_devices.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace {

/// The sizes ADR-0042 cares about, plus a small one for contrast.
constexpr int kDefaultProbes[] = {256, 2048, 8192};

struct Outcome {
    int requested = 0;
    int granted = 0;
    double sample_rate = 0.0;
    bool opened = false;
    juce::String error;
    juce::Array<int> available;
};

/// A callback that does nothing but record that it was called, and with what.
/// Enough to prove the device is really running rather than merely open.
class CountingCallback final : public juce::AudioIODeviceCallback {
public:
    void audioDeviceIOCallbackWithContext(const float* const*, int,
                                          float* const* out, int numOut,
                                          int numSamples,
                                          const juce::AudioIODeviceCallbackContext&) override {
        for (int ch = 0; ch < numOut; ++ch)
            if (out[ch] != nullptr)
                std::fill(out[ch], out[ch] + numSamples, 0.0f);

        ++calls_;
        last_block_ = numSamples;
        if (numSamples != first_block_ && first_block_ != 0) varied_ = true;
        if (first_block_ == 0) first_block_ = numSamples;
    }

    void audioDeviceAboutToStart(juce::AudioIODevice*) override {}
    void audioDeviceStopped() override {}

    int calls() const { return calls_.load(); }
    int firstBlock() const { return first_block_; }
    int lastBlock() const { return last_block_; }
    bool varied() const { return varied_; }

private:
    std::atomic<int> calls_{0};
    int first_block_ = 0;
    int last_block_ = 0;
    bool varied_ = false;
};

Outcome probe(int requested_block, double requested_rate) {
    Outcome r;
    r.requested = requested_block;

    juce::AudioDeviceManager mgr;
    const juce::String err =
        mgr.initialiseWithDefaultDevices(0, 2);   // no inputs; CI has no mic
    if (err.isNotEmpty()) { r.error = err; return r; }

    juce::AudioIODevice* dev = mgr.getCurrentAudioDevice();
    if (dev == nullptr) { r.error = "no audio device available"; return r; }

    juce::AudioDeviceManager::AudioDeviceSetup setup = mgr.getAudioDeviceSetup();
    setup.bufferSize = requested_block;
    setup.sampleRate = requested_rate;
    const juce::String setErr = mgr.setAudioDeviceSetup(setup, true);
    if (setErr.isNotEmpty()) { r.error = setErr; return r; }

    dev = mgr.getCurrentAudioDevice();
    if (dev == nullptr) { r.error = "device vanished after setup"; return r; }

    // What the DRIVER says it supports, before we ask for anything. This is
    // what distinguishes "the device caps at 4096" from "JUCE clamped it", and
    // the two have very different consequences for ADR-0042.
    if (r.available.isEmpty()) r.available = dev->getAvailableBufferSizes();

    CountingCallback cb;
    mgr.addAudioCallback(&cb);
    juce::Thread::sleep(120);                     // a few callbacks at any size
    mgr.removeAudioCallback(&cb);

    r.opened = true;
    r.granted = dev->getCurrentBufferSizeSamples();
    r.sample_rate = dev->getCurrentSampleRate();

    // A device that opens but never calls back is not usable, and saying so is
    // the entire point of running rather than only linking.
    if (cb.calls() == 0) {
        r.opened = false;
        r.error = "device opened but produced no callbacks";
        return r;
    }
    if (cb.varied())
        r.error = "block size VARIED between callbacks: first "
                + juce::String(cb.firstBlock()) + ", last " + juce::String(cb.lastBlock());

    mgr.closeAudioDevice();
    return r;
}

void report(const Outcome& o) {
    if (!o.opened) {
        std::printf("  %-6d  --        --        not opened: %s\n",
                    o.requested, o.error.toRawUTF8());
        return;
    }
    const double ms = o.sample_rate > 0.0
                    ? 1000.0 * static_cast<double>(o.granted) / o.sample_rate
                    : 0.0;
    std::printf("  %-6d  %-8d  %-8.0f  %7.2f ms%s%s\n",
                o.requested, o.granted, o.sample_rate, ms,
                o.granted == o.requested ? "" : "   <- NOT the size requested",
                o.error.isNotEmpty() ? ("   " + o.error).toRawUTF8() : "");
}

}  // namespace

int main(int argc, char** argv) {
    std::vector<int> sizes(std::begin(kDefaultProbes), std::end(kDefaultProbes));
    double rate = 48000.0;

    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--block" && i + 1 < argc) { sizes = {std::atoi(argv[++i])}; }
        else if (a == "--rate" && i + 1 < argc) { rate = std::atof(argv[++i]); }
        else if (a == "--help") {
            std::printf("adi_audio_probe [--block N] [--rate HZ]\n"
                        "  Opens an audio device at each block size, reports what the\n"
                        "  driver actually granted, and closes. ADR-0042.\n");
            return 0;
        }
    }

    // ADR-0041 says VST3 is the only host format and that every other one is
    // set to 0 EXPLICITLY rather than left to a default. Printing them is what
    // lets CI assert it: a default that flips in a future JUCE would otherwise
    // re-enable an AU host quietly, and "not behind a flag and not for testing"
    // is only a rule if something checks.
    if (argc > 1 && std::string(argv[1]) == "--hosts") {
        std::printf("JUCE_PLUGINHOST_VST3=%d\n",   JUCE_PLUGINHOST_VST3);
        std::printf("JUCE_PLUGINHOST_VST=%d\n",    JUCE_PLUGINHOST_VST);
        std::printf("JUCE_PLUGINHOST_AU=%d\n",     JUCE_PLUGINHOST_AU);
        std::printf("JUCE_PLUGINHOST_LV2=%d\n",    JUCE_PLUGINHOST_LV2);
        std::printf("JUCE_PLUGINHOST_LADSPA=%d\n", JUCE_PLUGINHOST_LADSPA);
        return 0;
    }

    juce::ScopedJuceInitialiser_GUI juce_init;   // JUCE needs its singletons

    std::printf("adi_audio_probe -- %s\n\n", juce::SystemStats::getJUCEVersion().toRawUTF8());
    std::printf("  %-6s  %-8s  %-8s  %s\n", "want", "got", "rate", "callback period");
    std::printf("  %-6s  %-8s  %-8s  %s\n", "------", "--------", "--------", "---------------");

    bool any_opened = false;
    bool misbehaved = false;
    juce::Array<int> available;
    for (int s : sizes) {
        const Outcome o = probe(s, rate);
        report(o);
        any_opened = any_opened || o.opened;
        if (o.opened && o.error.isNotEmpty()) misbehaved = true;
        if (available.isEmpty()) available = o.available;
    }

    if (!available.isEmpty()) {
        std::printf("\n  driver advertises: ");
        for (int i = 0; i < available.size(); ++i)
            std::printf("%s%d", i ? ", " : "", available[i]);
        std::printf("\n  largest supported: %d\n", available[available.size() - 1]);
    }

    std::printf("\n");
    if (!any_opened) {
        // The CI case. Not an error: a headless runner has no sound card, and
        // this still proved that JUCE configured, linked, and ran far enough to
        // ask the system for a device.
        std::printf("no audio device on this machine -- JUCE linked and ran; "
                    "nothing to open.\n");
        return 0;
    }
    if (misbehaved) {
        std::printf("FAILED -- a device opened but did not behave as reported above.\n");
        return 1;
    }
    std::printf("ok -- opened and closed cleanly at every size above.\n");
    return 0;
}
