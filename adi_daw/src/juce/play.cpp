// SPDX-License-Identifier: GPL-3.0-or-later
//
// adi_play -- a project through a real audio device. ADR-0122's proof by ear.
//
//   adi_play project.adi                      load, open the default device, run 4 s
//   adi_play project.adi --tone --seconds 8   a 220 Hz tone into the first track's chain
//   adi_play project.adi --block 512 --resize 2048
//                                             change the block size half way through:
//                                             the graph is rebuilt on the same plugin
//                                             instances (ADR-0122 d6); nothing reloads
//   adi_play project.adi --dry                load and report; open nothing
//   adi_play --list [--search DIR]            what the loader can find
//
// What this proves that the headless suite cannot: a driver callback reaches
// `Session::process` at the size the driver granted; `prepare` and `release`
// arrive from the device's own start and stop; the 20 ms timer answers plugin
// restart requests and frees retired graphs; and a real plugin, found by its
// `plugin_refs` row, sits in the chain. "No audio device" is not a failure
// here, for the same reason as in `adi_audio_probe`: CI has none.
//
// It does not play clips. There is no clip reader yet (step 7's first engine
// piece, and a `sourcesFor` node when it arrives) -- so a project is silent
// unless `--tone` pushes something into a track, or a device makes sound of
// its own.

#include "adi/engine/session.hpp"
#include "adi/store.hpp"
#include "adi/store_rows.hpp"
#include "juce/device_bridge.hpp"
#include "juce/juce_device_loader.hpp"

#include <juce_audio_devices/juce_audio_devices.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace {

constexpr double kPi = 3.14159265358979323846;

/// A sine at -18 dBFS: loud enough to hear a chain, quiet enough to leave on.
class SineNode final : public adi::engine::Node {
public:
    SineNode(double hz, float gain) : hz_(hz), gain_(gain) {}
    void process(const adi::engine::NodeIo& io) noexcept override {
        const double sr = io.sampleRate > 0.0 ? io.sampleRate : 48000.0;
        const double step = 2.0 * kPi * hz_ / sr;
        double ph = phase_;
        for (std::int32_t i = 0; i < io.frames; ++i) {
            const float v = gain_ * static_cast<float>(std::sin(ph));
            for (std::int32_t c = 0; c < io.channels; ++c)
                io.out[c][io.blockOffset + i] = v;
            ph += step;
            if (ph > 2.0 * kPi) ph -= 2.0 * kPi;
        }
        phase_ = ph;
    }
    [[nodiscard]] const char* name() const noexcept override { return "tone"; }
private:
    double hz_;
    float gain_;
    double phase_ = 0.0;
};

/// Wraps the session so the report can say what LEFT the master: a peak, in
/// dBFS, per phase. "The graph ran" and "audio came out" are different claims.
class MeteredSession final : public adi::engine::BlockProcessor {
public:
    explicit MeteredSession(adi::engine::Session& s) : s_(&s) {}
    void prepare(double sampleRate, std::int32_t maxFrames) override { s_->prepare(sampleRate, maxFrames); }
    void release() override { s_->release(); }
    void process(const adi::engine::AudioIo& io) noexcept override {
        s_->process(io);
        float pk = 0.0f;
        for (std::int32_t c = 0; c < io.numOut; ++c) {
            const float* o = io.out != nullptr ? io.out[c] : nullptr;
            if (o == nullptr) continue;
            for (std::int32_t i = 0; i < io.frames; ++i) pk = std::max(pk, std::fabs(o[i]));
        }
        float cur = peak_.load(std::memory_order_relaxed);
        while (pk > cur && !peak_.compare_exchange_weak(cur, pk, std::memory_order_relaxed)) {}
    }
    /// Read and reset -- one phase's peak.
    float takePeak() noexcept { return peak_.exchange(0.0f, std::memory_order_relaxed); }
private:
    adi::engine::Session* s_;
    std::atomic<float> peak_{0.0f};
};

std::string dbfs(float peak) {
    if (peak <= 0.0f) return "silence";
    char buf[32];
    std::snprintf(buf, sizeof buf, "%.1f dBFS", 20.0 * std::log10(static_cast<double>(peak)));
    return buf;
}

struct Options {
    std::string file;
    int block = 512;
    double rate = 0.0;      // 0: the project's
    int seconds = 4;
    int resize = 0;
    bool tone = false;
    std::int64_t toneTrack = 0;
    bool dry = false;
    bool list = false;
    bool fixture = false;
    std::string deviceType;   // "Windows Audio (Exclusive Mode)", "DirectSound", "CoreAudio"...
    std::string deviceName;
    std::vector<std::string> searchDirs;
};

int usage() {
    std::printf(
        "adi_play <project.adi> [--block N] [--rate HZ] [--seconds S] [--resize M]\n"
        "                       [--tone [TRACK]] [--type DEVICE-TYPE] [--device NAME]\n"
        "                       [--search DIR]... [--fixture] [--dry]\n"
        "adi_play --list [--search DIR]... [--fixture]\n"
        "  Opens a project, resolves its devices, plays it through the default audio\n"
        "  device at the granted block size. --resize changes the block size half way\n"
        "  through the run without reloading a plugin (ADR-0122). --tone pushes a 220 Hz\n"
        "  sine into TRACK's chain (default: the first non-master track). --type picks a\n"
        "  device type: ASIO (Windows, ADR-0137), \"Windows Audio (Exclusive Mode)\" or\n"
        "  \"DirectSound\" exercise a real block-size change; WASAPI shared grants its own period.\n");
    return 2;
}

bool parse(int argc, char** argv, Options& o) {
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto next = [&](int& idx) -> const char* { return idx + 1 < argc ? argv[++idx] : nullptr; };
        if (a == "--block")        { const char* v = next(i); if (!v) return false; o.block = std::atoi(v); }
        else if (a == "--rate")    { const char* v = next(i); if (!v) return false; o.rate = std::atof(v); }
        else if (a == "--seconds") { const char* v = next(i); if (!v) return false; o.seconds = std::atoi(v); }
        else if (a == "--resize")  { const char* v = next(i); if (!v) return false; o.resize = std::atoi(v); }
        else if (a == "--search")  { const char* v = next(i); if (!v) return false; o.searchDirs.emplace_back(v); }
        else if (a == "--type")    { const char* v = next(i); if (!v) return false; o.deviceType = v; }
        else if (a == "--device")  { const char* v = next(i); if (!v) return false; o.deviceName = v; }
        else if (a == "--tone") {
            o.tone = true;
            if (i + 1 < argc && argv[i + 1][0] != '-') o.toneTrack = std::atoll(argv[++i]);
        }
        else if (a == "--dry")     o.dry = true;
        else if (a == "--list")    o.list = true;
        else if (a == "--fixture") o.fixture = true;
        else if (a == "--help" || a == "-h") return false;
        else if (!a.empty() && a[0] == '-') { std::printf("unknown option %s\n", a.c_str()); return false; }
        else o.file = a;
    }
    if (o.block <= 0 || o.seconds <= 0) return false;
    return o.list || !o.file.empty();
}

std::string n(std::int64_t v) { return std::to_string(v); }

std::int64_t firstNonMaster(const adi::rows::Model& m) {
    for (const adi::rows::Track& t : m.tracks) {
        if (t.kind == "audio" || t.kind == "midi" || t.kind == "instrument" ||
            t.kind == "group" || t.kind == "return")
            return t.id;
    }
    return 0;
}

void report(const adi::engine::Session& s, const adi::device::JuceDeviceLoader& loader,
            const adi::rows::Model& m) {
    std::printf("  project  \"%s\"  %s tracks, %s device rows, %s chains, project rate %s\n",
                m.project.name.c_str(), n(static_cast<std::int64_t>(m.tracks.size())).c_str(),
                n(static_cast<std::int64_t>(m.devices.size())).c_str(),
                n(static_cast<std::int64_t>(m.deviceChains.size())).c_str(),
                n(m.project.sampleRate).c_str());
    const auto& st = s.stats();
    std::printf("  devices  %s loaded, %s placeholders, %s skipped  (loader: %s vst3, %s clap, "
                "%s unhosted, %s not found, %s failed, %s scans)\n",
                n(st.loaded).c_str(), n(st.placeholders).c_str(), n(st.skipped).c_str(),
                n(loader.stats().vst3).c_str(), n(loader.stats().clap).c_str(),
                n(loader.stats().unhosted).c_str(), n(loader.stats().notFound).c_str(),
                n(loader.stats().failed).c_str(), n(loader.stats().scans).c_str());
    for (std::size_t i = 0; i < s.entryCount(); ++i) {
        const adi::engine::Session::Entry& e = s.entryAt(i);
        std::printf("    devices#%s  track %s  %-24s %s%s\n", n(e.deviceId).c_str(),
                    n(e.trackId).c_str(), e.name.c_str(),
                    e.placeholder ? "PLACEHOLDER" : "loaded",
                    e.error.empty() ? "" : ("  -- " + e.error).c_str());
    }
    if (!s.problems().empty()) {
        std::printf("  problems (%s):\n", n(static_cast<std::int64_t>(s.problems().size())).c_str());
        for (const std::string& p : s.problems()) std::printf("    - %s\n", p.c_str());
    }
}

}  // namespace

int main(int argc, char** argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    Options o;
    if (!parse(argc, argv, o)) return usage();

    juce::ScopedJuceInitialiser_GUI juceInit;
    std::printf("adi_play -- %s\n\n", juce::SystemStats::getJUCEVersion().toRawUTF8());

    adi::device::JuceDeviceLoader loader;
    for (const std::string& d : o.searchDirs) {
        const juce::String s(d);
        loader.addSearchPath(juce::File::isAbsolutePath(s)
                                 ? juce::File(s)
                                 : juce::File::getCurrentWorkingDirectory().getChildFile(s));
    }
#if defined(ADI_TEST_VST3)
    if (o.fixture) loader.addSearchPath(juce::File(ADI_TEST_VST3).getParentDirectory());
#else
    if (o.fixture) std::printf("  (no fixture VST3 in this build; --fixture ignored)\n");
#endif

    if (o.list) {
        loader.scan();
        const std::vector<std::string> lines = loader.knownPlugins();
        for (const std::string& l : lines) std::printf("  %s\n", l.c_str());
        std::printf("\n%s plugins\n", n(static_cast<std::int64_t>(lines.size())).c_str());
        return 0;
    }

    adi::StoreError se = adi::StoreError::Ok;
    auto store = adi::Store::open(o.file, se, /*readOnly=*/true);
    if (!store) {
        std::printf("FAILED -- cannot open %s (store error %d)\n", o.file.c_str(), static_cast<int>(se));
        return 1;
    }
    const adi::rows::Model peek = adi::rows::readModel(*store);
    const double rate = o.rate > 0.0 ? o.rate : static_cast<double>(peek.project.sampleRate);

    adi::engine::Session session;
    session.devices().watchClapGlue(loader.clap().glue(), "clap");
    SineNode tone(220.0, 0.125f);
    std::int64_t toneTrack = 0;
    if (o.tone) {
        toneTrack = o.toneTrack > 0 ? o.toneTrack : firstNonMaster(peek);
        session.setSourcesFor([&](std::int64_t id) {
            return id == toneTrack ? std::vector<adi::engine::Node*>{&tone}
                                   : std::vector<adi::engine::Node*>{};
        });
        std::printf("  tone     220 Hz at -18 dBFS into tracks#%s\n", n(toneTrack).c_str());
    }

    adi::engine::SessionSpec spec;
    spec.sampleRate = rate;
    spec.maxFrames = o.block;
    const bool loaded = session.load(*store, loader.fn(), spec);
    report(session, loader, peek);
    if (!loaded) {
        std::printf("\nFAILED -- %s\n", session.error().c_str());
        return 1;
    }
    if (o.dry) {
        std::printf("\nok -- loaded and reported; --dry opens no device.\n");
        return 0;
    }

    juce::AudioDeviceManager mgr;
    if (!o.deviceType.empty()) {
        bool known = false;
        std::string types;
        for (auto* t : mgr.getAvailableDeviceTypes()) {
            types += (types.empty() ? "" : ", ") + t->getTypeName().toStdString();
            if (t->getTypeName() == juce::String(o.deviceType)) known = true;
        }
        if (!known) {
            std::printf("\nFAILED -- no device type '%s'; this build offers: %s\n",
                        o.deviceType.c_str(), types.c_str());
            return 1;
        }
        mgr.setCurrentAudioDeviceType(juce::String(o.deviceType), true);
    }
    const juce::String initErr = mgr.initialiseWithDefaultDevices(0, 2);   // no inputs; CI has no mic
    if (initErr.isNotEmpty() || mgr.getCurrentAudioDevice() == nullptr) {
        std::printf("\nno audio device on this machine -- loaded and reported; nothing to open.%s%s\n",
                    initErr.isNotEmpty() ? " " : "", initErr.toRawUTF8());
        return 0;
    }
    juce::AudioDeviceManager::AudioDeviceSetup setup = mgr.getAudioDeviceSetup();
    if (!o.deviceName.empty()) setup.outputDeviceName = juce::String(o.deviceName);
    setup.bufferSize = o.block;
    setup.sampleRate = rate;
    const juce::String setErr = mgr.setAudioDeviceSetup(setup, true);
    if (setErr.isNotEmpty()) {
        std::printf("\nFAILED -- device setup: %s\n", setErr.toRawUTF8());
        return 1;
    }
    juce::AudioIODevice* dev = mgr.getCurrentAudioDevice();
    if (dev == nullptr) {
        std::printf("\nFAILED -- the device vanished after setup\n");
        return 1;
    }
    std::printf("\n  device   %s (%s)\n  granted  %d frames at %.0f Hz  (asked %d at %.0f)\n",
                dev->getName().toRawUTF8(), dev->getTypeName().toRawUTF8(),
                dev->getCurrentBufferSizeSamples(), dev->getCurrentSampleRate(), o.block, rate);

    MeteredSession metered(session);
    adi::device::DeviceBridge bridge(metered);
    bridge.setRequestedBlockSize(o.block);
    adi::device::DeviceHostTimer timer(session.devices());
    timer.start(20);
    mgr.addAudioCallback(&bridge);

    juce::MessageManager* mm = juce::MessageManager::getInstance();
    const int totalMs = o.seconds * 1000;
    bool resizeExercised = false;
    bool resizeMissed = false;
    float peakFirst = 0.0f;
    if (o.resize > 0) {
        mm->runDispatchLoopUntil(totalMs / 2);
        peakFirst = metered.takePeak();
        const std::int64_t callbacksBefore = bridge.core().callbacks();
        const std::int64_t publishedBefore = session.graph().stats().published;
        const std::int64_t changesBefore = session.stats().formatChanges;
        const int grantedBefore = dev->getCurrentBufferSizeSamples();
        setup.bufferSize = o.resize;
        const juce::String reErr = mgr.setAudioDeviceSetup(setup, true);
        dev = mgr.getCurrentAudioDevice();
        const int grantedAfter = dev != nullptr ? dev->getCurrentBufferSizeSamples() : 0;
        std::printf("  resized  %d -> %d frames (asked %d)%s%s   after %s callbacks; graphs published %s -> %s\n",
                    grantedBefore, grantedAfter, o.resize,
                    reErr.isNotEmpty() ? "  ERROR " : "", reErr.toRawUTF8(),
                    n(callbacksBefore).c_str(), n(publishedBefore).c_str(),
                    n(session.graph().stats().published).c_str());
        if (grantedAfter != grantedBefore) {
            resizeExercised = true;
            resizeMissed = session.stats().formatChanges == changesBefore;
        } else {
            std::printf("  note     the driver kept %d frames, so the block-size change was NOT exercised "
                        "(WASAPI shared mode grants its own period; try --type \"Windows Audio "
                        "(Exclusive Mode)\" or DirectSound)\n", grantedBefore);
        }
        mm->runDispatchLoopUntil(totalMs - totalMs / 2);
    } else {
        mm->runDispatchLoopUntil(totalMs);
    }
    const float peakLast = metered.takePeak();

    mgr.removeAudioCallback(&bridge);
    timer.stop();
    mgr.closeAudioDevice();

    const adi::device::DeviceCore& core = bridge.core();
    const adi::engine::Session::Stats& ss = session.stats();
    const adi::engine::GraphHost::Stats& gs = session.graph().stats();
    std::printf("\n  callbacks %s since the device last started; %s blocks through the graph in all "
                "(reconfigurations %d, oversize refusals %s)\n",
                n(core.callbacks()).c_str(), n(gs.blocks).c_str(), core.reconfigurations(),
                n(core.oversizeRefusals()).c_str());
    std::printf("  session   prepares %s, format changes %s, rebuilds %s; graphs published %s, "
                "swaps %s, reclaimed %s\n",
                n(ss.prepares).c_str(), n(ss.formatChanges).c_str(), n(ss.rebuilds).c_str(),
                n(gs.published).c_str(), n(gs.swaps).c_str(), n(gs.reclaimed).c_str());
    std::printf("  timer     %s ticks, %s retaps, %s plugin-driven rebuilds (%s failed)\n",
                n(timer.ticks()).c_str(), n(timer.retaps()).c_str(),
                n(session.devices().stats().rebuilds).c_str(),
                n(session.devices().stats().rebuildsFailed).c_str());
    if (o.resize > 0)
        std::printf("  master    peak %s before the resize, %s after\n", dbfs(peakFirst).c_str(), dbfs(peakLast).c_str());
    else
        std::printf("  master    peak %s\n", dbfs(peakLast).c_str());
    if (o.tone && std::max(peakFirst, peakLast) < 0.001f)
        std::printf("  note      the tone did not reach the master -- is tracks#%s's chain an instrument? "
                    "(an instrument replaces its input; use --tone <track> on an effect chain)\n",
                    n(toneTrack).c_str());
    const juce::String mismatch = bridge.mismatchReport();
    if (mismatch.isNotEmpty()) std::printf("  note      %s\n", mismatch.toRawUTF8());

    if (core.callbacks() == 0) {
        std::printf("\nFAILED -- the device opened but produced no callbacks.\n");
        return 1;
    }
    if (bridge.lastError().isNotEmpty()) {
        std::printf("\nFAILED -- device error: %s\n", bridge.lastError().toRawUTF8());
        return 1;
    }
    if (resizeMissed) {
        std::printf("\nFAILED -- the driver changed its block size and the session saw no format change.\n");
        return 1;
    }
    if (o.resize > 0 && resizeExercised)
        std::printf("  resize    exercised: a new graph at the new size on the same instances (ADR-0122 d6)\n");
    std::printf("\nok -- %s blocks through the session, %s graph(s), nothing reloaded.\n",
                n(gs.blocks).c_str(), n(gs.published).c_str());
    return 0;
}
