// SPDX-License-Identifier: AGPL-3.0-or-later
// Retained function controls and prepared drag targets, with a software peer only.
// No native host, OS window, desktop input, capture or audio device is involved.
#include "../src/juce/ui_shell.hpp"
#include "../src/juce/ui_look.hpp"
#include "../src/juce/ui_stack_picker.hpp"
#include "adi/ui/project_document.hpp"
#include "temp_directory.hpp"
#include "juce/builtin_host.hpp"
#include "juce/mix_function_host.hpp"
#include "juce/device_bridge.hpp"
#include "adi/gesture_presentation.hpp"
#include <SQLiteCpp/SQLiteCpp.h>
#include "ui_fake_peer.hpp"
#include "ui_picture_compare.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <functional>
#include <iostream>
#include <memory>
#include <numbers>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

using namespace adi;
using namespace adi::ui;
namespace adi::ui {
struct MixFocusTestAccess {
    static const auto &match(const MixFocus &focus) { return focus.match_; }
    static auto matchCaption(const MixFocus &focus) { return focus.matchCaption_; }
    static bool hasMatchPath(const MixFocus &focus) { return !focus.matchPath_.isEmpty(); }
    static bool aligned(const MixFocus &focus) { return focus.scopeRange_ && focus.sum_; }
    static bool waitingForAlignment(const MixFocus &focus) { return focus.alignment_ != MixFocus::Alignment::Ready; }
    static bool independent(const MixFocus &focus) {
        return !focus.scopeRange_ && !focus.sum_ && focus.scopeHasTrace_ &&
               std::all_of(focus.scopeSumPaths_.begin(), focus.scopeSumPaths_.end(),
                           [](const auto &path) { return path.isEmpty(); }) &&
               std::all_of(focus.members_.begin(), focus.members_.end(),
                           [](const auto &member) { return !member.scopePaths[0].isEmpty(); });
    }
    static bool spectraWithoutMask(const MixFocus &focus) {
        return focus.masked_.empty() && !focus.sum_ &&
               std::all_of(focus.members_.begin(), focus.members_.end(),
                           [](const auto &member) { return !member.path.isEmpty(); });
    }
    static bool scopeInterval(const MixFocus &focus, std::int64_t first, std::size_t frames, double rate) {
        return focus.scopeRange_ && focus.scopeRange_->first == first && focus.scopeRange_->frames == frames &&
               focus.scopeRange_->sampleRate == rate && focus.sum_;
    }
    static bool sumPulseAt(const MixFocus &focus, float position, float value) {
        return scopePulseAt(focus.plot_, focus.scopeSumPaths_[0], position, value);
    }
    static bool differencePulseAt(const MixFocus &focus, float position, float value) {
        return scopePulseAt(focus.plot_, focus.scopeDifferencePaths_[0], position, value);
    }
    static bool hasDifference(const MixFocus &focus) { return focus.differenceRenderedRevision_.has_value(); }
    static bool scopePulseAt(juce::Rectangle<float> plot, const juce::Path &path, float position, float value) {
        if (plot.isEmpty()) return false;
        juce::Path::Iterator vertices(path);
        while (vertices.next()) {
            if (vertices.elementType != juce::Path::Iterator::lineTo) continue;
            const auto relative = (vertices.x1 - plot.getX()) / plot.getWidth();
            const auto amplitude = 2.f * (plot.getCentreY() - vertices.y1) / plot.getHeight();
            if (std::abs(relative - position) <= .013f && std::abs(amplitude - value) <= .015f) return true;
        }
        return false;
    }
    static bool flatScopePaths(const MixFocus &focus) {
        const auto oneSegment = [](const juce::Path &path) {
            juce::Path::Iterator vertices(path);
            return vertices.next() && vertices.elementType == juce::Path::Iterator::startNewSubPath &&
                   vertices.next() && vertices.elementType == juce::Path::Iterator::lineTo && !vertices.next();
        };
        return std::ranges::all_of(focus.scopeSumPaths_, oneSegment) &&
               std::ranges::all_of(focus.members_, [&](const auto &member) {
                   return std::ranges::all_of(member.scopePaths, oneSegment);
               });
    }
    static adi::uitest::ComparisonPicture scopePicture(MixFocus &focus) {
        const auto plot = focus.plot_.toNearestInt();
        return adi::uitest::pictureForComparison(juce::Image::RGB, plot.getWidth(), plot.getHeight(),
                                                [&focus, plot](juce::Graphics &g) {
                                                    g.setOrigin(-plot.getX(), -plot.getY());
                                                    focus.paint(g);
                                                });
    }
    static bool sumStrokeAt(MixFocus &focus, float value) {
        const auto plot = focus.plot_;
        if (plot.isEmpty()) return false;
        juce::Image image(juce::Image::RGB, focus.getWidth(), focus.getHeight(), true, juce::SoftwareImageType{});
        {
            juce::Graphics g(image);
            focus.paint(g);
        }
        const auto y = juce::roundToInt(plot.getCentreY() - value * plot.getHeight() * .5f);
        const juce::Colour stroke = look::text;
        const auto matches = [stroke](juce::Colour pixel) {
            return std::abs(pixel.getRed() - stroke.getRed()) <= 12 &&
                   std::abs(pixel.getGreen() - stroke.getGreen()) <= 12 &&
                   std::abs(pixel.getBlue() - stroke.getBlue()) <= 12;
        };
        for (const float position : {.2f, .4f, .6f, .8f}) {
            const auto x = juce::roundToInt(plot.getX() + plot.getWidth() * position);
            if (!matches(image.getPixelAt(x, y - 1)) && !matches(image.getPixelAt(x, y)) &&
                !matches(image.getPixelAt(x, y + 1))) return false;
        }
        return true;
    }
    static bool compactScope(const MixFocus &focus) { return focus.sumLabel_.isEmpty(); }
    static bool sumOverRangeInk(MixFocus &focus) {
        const auto plot = focus.plot_.toNearestInt();
        const auto left = plot.getX(), top = focus.scroll_.getBottom();
        const auto right = focus.sumLabel_.isEmpty() ? plot.getRight() : focus.sumLabel_.getX();
        juce::Image image(juce::Image::RGB, focus.getWidth(), focus.getHeight(), true, juce::SoftwareImageType{});
        {
            juce::Graphics g(image);
            focus.paint(g);
        }
        const juce::Colour indicator = look::record;
        for (auto y = top; y < plot.getY(); ++y)
            for (auto x = left; x < right; ++x) {
                const auto pixel = image.getPixelAt(x, y);
                if (std::abs(pixel.getRed() - indicator.getRed()) <= 16 &&
                    std::abs(pixel.getGreen() - indicator.getGreen()) <= 16 &&
                    std::abs(pixel.getBlue() - indicator.getBlue()) <= 16) return true;
            }
        return false;
    }
};
} // namespace adi::ui
namespace {
int checks = 0, failures = 0;
void check(bool value, const char *what) {
    ++checks;
    if (!value) { ++failures; std::printf("FAIL %s\n", what); }
}
struct MenuItem {
    juce::String name;
    bool enabled = false, ticked = false;
    std::function<void()> action;
};
std::vector<MenuItem> menuItems(const juce::PopupMenu &menu) {
    std::vector<MenuItem> items;
    for (juce::PopupMenu::MenuItemIterator it(menu); it.next();) {
        const auto &item = it.getItem();
        items.push_back({item.text, item.isEnabled, item.isTicked, item.action});
    }
    return items;
}
DevicePanelData makePanel(std::int64_t id, bool plugin, const char *name) {
    DevicePanelData data;
    data.id = id; data.track = 2; data.chain = 22; data.instance = 71;
    data.available = true; data.name = name; data.uid = "org.adidaw.mix.limiter";
    data.plugin = plugin; data.latency = 476;
    data.declared.push_back({"gain", "Gain", true});
    data.resolved.entries.push_back({"gain", false});
    data.resolved.modifiableCount = 1;
    panel::Record record;
    record.id = "gain"; record.name = "Gain"; record.stored = record.playing = .25;
    record.text = "6.0 dB";
    data.records.push_back(record); data.defaults["gain"] = .25;
    return data;
}
struct Event {
    std::int64_t device;
    engine::ParamEventKind kind;
    std::uint64_t instance;
    double value;
    std::string parameter{};
};
// Exercises the actual GUI callback/coordinator seam without opening hardware.
struct SimulatedDevice final : juce::AudioIODevice {
    explicit SimulatedDevice(const juce::String &name = "Owned callback fixture") : AudioIODevice(name, "test") {}
    double rate = 48000.;
    int frames = 256;
    juce::StringArray getOutputChannelNames() override { return {"L", "R"}; }
    juce::StringArray getInputChannelNames() override { return {}; }
    juce::Array<double> getAvailableSampleRates() override { return {44100., 48000.}; }
    juce::Array<int> getAvailableBufferSizes() override { return {256, 512}; }
    int getDefaultBufferSize() override { return 256; }
    juce::String open(const juce::BigInteger &, const juce::BigInteger &, double, int) override { return {}; }
    void close() override {}
    bool isOpen() override { return true; }
    void start(juce::AudioIODeviceCallback *) override {}
    void stop() override {}
    bool isPlaying() override { return true; }
    juce::String getLastError() override { return {}; }
    int getCurrentBufferSizeSamples() override { return frames; }
    double getCurrentSampleRate() override { return rate; }
    int getCurrentBitDepth() override { return 32; }
    juce::BigInteger getActiveOutputChannels() const override { return juce::BigInteger(3); }
    juce::BigInteger getActiveInputChannels() const override { return {}; }
    int getOutputLatencyInSamples() override { return 0; }
    int getInputLatencyInSamples() override { return 0; }
};
void audioDeviceStatus(AppCommands &commands, const std::filesystem::path &directory) {
    std::string error;
    auto doc = ProjectDocument::open(directory / "device-notice.adi", true, {}, error);
    check(bool(doc), "SC-28: an open project for device status");
    if (!doc)
        return;
    AdiRootComponent root(doc->view(), doc->ops(), doc->mailbox(), commands, doc->views(), doc->sessionState(),
                          "device-status");
    int settingsOpened = 0;
    root.openAudioSettings = [&] { ++settingsOpened; };
    root.setSize(1440, 900);
    SimulatedDevice primary("Primary interface"), alternate("Alternate interface");
    device::DeviceBridge bridge(*doc);
    const auto publish = [&] {
        if (auto message = bridge.takeDeviceProblem(); message.isNotEmpty())
            root.status.setAudioDeviceProblem(std::move(message));
    };

    bridge.audioDeviceAboutToStart(&primary);
    bridge.audioDeviceStopped();
    primary.frames = 512;
    bridge.audioDeviceAboutToStart(&primary);
    publish();
    check(bridge.core().isOpen() && !root.status.audioSettings.isVisible(),
          "SC-28: an in-place restart does not warn");

    bridge.audioDeviceStopped();
    bridge.audioDeviceAboutToStart(&alternate);
    bridge.acknowledgeDeviceSetup();
    publish();
    check(!root.status.audioSettings.isVisible(), "SC-28: changing devices in Settings does not warn");

    bridge.audioDeviceStopped();
    bridge.audioDeviceAboutToStart(&primary);
    publish();
    check(root.status.audioDeviceProblem().contains("Alternate interface") && root.status.audioSettings.isVisible(),
          "SC-28: an unexpected stop is reported even when a different interface opens immediately");
    root.status.audioSettings.onClick();
    check(settingsOpened == 1, "SC-28: the Status Bar can open Audio Settings");

    std::thread driver([&] { bridge.audioDeviceError("Driver lost its connection"); });
    driver.join();
    publish();
    root.say("Kept the audio recorded before the interruption.");
    check(root.status.audioDeviceProblem().contains("Driver lost its connection") &&
              root.error() == "Kept the audio recorded before the interruption.",
          "SC-28: driver details and the take's outcome are both available in the Status Bar");
    check(bridge.takeDeviceProblem().isEmpty(), "SC-28: each driver fault is consumed once");

    root.status.setAudioDeviceProblem({});
    OpRequest arm;
    arm.opType = "track.setArm";
    arm.payload = {{"id", 4}, {"armed", true}};
    const bool recording = doc->ops().submit(arm).ok && doc->synchronise() && doc->startRecording(0, 1, false);
    check(recording, "SC-28: a take is recording when its device stops");
    constexpr int frames = 512;
    std::array<float, frames> input{}, left{}, right{};
    input.fill(0.25f);
    const float *inputs[]{input.data()};
    float *outputs[]{left.data(), right.data()};
    if (recording)
        bridge.audioDeviceIOCallbackWithContext(inputs, 1, outputs, 2, frames, {});
    bridge.audioDeviceError("Stream ended");
    bridge.audioDeviceStopped();
    publish();
    check(!bridge.core().isOpen() && root.status.audioDeviceProblem().contains("Primary interface") &&
              root.status.audioDeviceProblem().contains("Stream ended"),
          "SC-28: a stopped driver releases the document and reports the stop and the error");
    const bool settled = doc->settleRecording();
    const auto landed = doc->takeLanded();
    check(settled && landed && landed->ok && landed->clips.size() == 1,
          "SC-28: the stopped driver still leaves its captured audio to land");
}
struct KeyProbe final : device::DeviceInstance {
    device::DeviceIdentity identity_;
    double keyPeak = 0, choice = 0;
    device::ParamDescriptor descriptor;
    KeyProbe() {
        identity_.format = "internal";
        identity_.uid = "owned.key";
        descriptor.id = "choice";
        descriptor.name = "Choice";
        descriptor.shape = device::ParamShape::Menu;
        descriptor.stepCount = 13;
        for (int i = 0; i < 13; ++i)
            descriptor.choiceLabels.push_back(std::to_string(i));
    }
    std::int32_t paramCount() const noexcept override { return 1; }
    const device::ParamDescriptor *paramAt(std::int32_t i) const noexcept override {
        return i == 0 ? &descriptor : nullptr;
    }
    device::ParamValue getParam(const std::string &) const noexcept override {
        return device::ParamValue::fromNormalized(choice);
    }
    bool setParam(const std::string &, const device::ParamValue &v) override {
        choice = v.normalized;
        return true;
    }
    const device::DeviceIdentity &identity() const noexcept override { return identity_; }
    bool loaded() const noexcept override { return true; }
    std::int32_t sidechainInputChannels() const noexcept override { return 2; }
    void process(const engine::NodeIo &io) noexcept override {
        for (int c = 0; c < io.channels; ++c)
            for (int i = io.blockOffset; i < io.blockOffset + io.frames; ++i) {
                const auto key = io.sidechain && io.sidechain[c] ? io.sidechain[c][i] : 0.f;
                keyPeak = std::max(keyPeak, std::abs(double(key)));
                if (io.out && io.out[c])
                    io.out[c][i] = key;
            }
    }
};
struct CallbackTone final : engine::Node {
    std::array<float, 2> channels{.25f, .25f};
    void process(const engine::NodeIo &io) noexcept override {
        if (!io.out) return;
        for (int c = 0; c < std::min(io.channels, 2); ++c)
            if (io.out[c])
                std::fill_n(io.out[c] + io.blockOffset, io.frames, channels[static_cast<std::size_t>(c)]);
    }
};
void callbackKeys(AppCommands &commands, const std::filesystem::path &directory) {
    StoreError status{};
    const auto path = directory / "callback-keys.adi";
    auto store = Store::create(path, status);
    store->db().exec("INSERT INTO tracks(id,kind,name) "
                     "VALUES(1,'master','Main'),(2,'audio','Key'),(3,'audio','Programme');"
                     "INSERT INTO device_chains(id,track_id,name) VALUES(3,3,'Devices');"
                     "INSERT INTO plugin_refs(id,format,uid,name) "
                     "VALUES(77,'internal','owned.key','Owned key');"
                     "INSERT INTO devices(id,chain_id,ord,name,plugin_ref_id) "
                     "VALUES(30,3,0,'Target',77);");
    store.reset();
    std::string error;
    auto doc = ProjectDocument::open(
        path, false,
        [](const engine::DeviceRequest &, std::string &) { return std::make_unique<KeyProbe>(); }, error);
    check(bool(doc), "callback key document opens");
    if (!doc)
        return;
    CallbackTone tone;
    doc->session().setSourcesFor([&](std::int64_t track) {
        return track == 2 ? std::vector<engine::Node *>{&tone} : std::vector<engine::Node *>{};
    });
    SimulatedDevice hardware;
    device::DeviceBridge bridge(*doc);
    bridge.setRequestedBlockSize(256);
    bridge.audioDeviceAboutToStart(&hardware);
    doc->session().graph().setFadeFrames(0);
    doc->session().transport().play();
    gesture::EnginePresentation presentation(doc->view(), doc->session());
    auto adopt = [&] {
        auto source = appliedSource(doc->store());
        return presentation.adoptCommitted(
            doc->store(), {source, History(doc->store()).head(), source.sequence()}, nullptr);
    };
    check(adopt().state != gesture::PresentationState::Refused,
          "GUI coordinator adopts initial callback graph");
    std::array<float, 256> l{}, r{};
    std::array<float *, 2> out{l.data(), r.data()};
    auto render = [&] {
        auto *probe = static_cast<KeyProbe *>(doc->session().instanceFor(30));
        probe->keyPeak = 0;
        for (int b = 0; b < 8; ++b)
            bridge.audioDeviceIOCallbackWithContext(nullptr, 0, out.data(), 2, 256, {});
        return probe->keyPeak;
    };
    check(render() == 0, "unrouted callback key is silent");
    for (const auto *tap : {"pre_fx", "post_fx", "post_mixer"}) {
        doc->tick(10);
        const auto data = doc->parameterFeed().current()->front();
        check(doc->setDeviceKey(data, 2, tap), "callback route commits through document");
        check(adopt().state != gesture::PresentationState::Refused, "GUI coordinator adopts routed graph");
        check(render() == .25, "DeviceBridge callback retains key after "
                               "coordinator graph publication");
        check(doc->deviceAction("track.rename", {{"id", 2}, {"name", std::string("Key ") + tap}}),
              "non-topology edit commits");
        check(adopt().state != gesture::PresentationState::Refused && render() == .25,
              "scalar coordinator generation retains auxiliary samples");
    }
    // Every asynchronous choice must survive a stale frame before delivery,
    // and use the same real document/Undo authority as the retained surfaces.
    doc->setParameterDemand(doc.get(), {30});
    doc->tick(100);
    auto shown = doc->parameterFeed().current()->front();
    AdiRootComponent root(doc->view(), doc->ops(), doc->mailbox(), commands, doc->views(),
                          doc->sessionState(), "menu");
    auto record = shown.parameters().front();
    ParameterControl control(root.devices, 30, record, 0);
    control.parameterGesture = [&](const std::string &id, engine::ParamEventKind kind, double value) {
        return doc->parameterGestureExpected(30, id, kind, value, shown.runtime, shown.instance);
    };
    auto *probe = static_cast<KeyProbe *>(doc->session().instanceFor(30));
    std::int64_t menuClock = 100;
    const auto pump = [&] {
        doc->tick(menuClock += 20);
        render();
        doc->tick(menuClock += 20);
    };
    for (int i = 1; i <= 13; ++i) {
        const auto before = probe->choice;
        const auto head = History(doc->store()).head();
        const double next = double(i % 13) / 12.;
        control.menu.setSelectedId(1 + i % 13, juce::dontSendNotification);
        control.present(record,
                        0);      // an intervening paint still has the previous value
        control.menu.onChange(); // deliver JUCE's deferred callback
                                 // deterministically, with no OS message loop
        pump();
        check(probe->choice == next, "every asynchronous Menu choice applies despite stale presentation");
        const bool undone = doc->ops().undo().ok;
        const bool synchronized = doc->synchronise();
        pump();
        const auto undoHead = History(doc->store()).head();
        const bool undoOkay = undone && synchronized && undoHead == head && probe->choice == before;
        if (!undoOkay)
            std::printf("MENU undo %d ok=%d sync=%d head=%lld/%lld value=%.17g/%.17g "
                        "error=%s\n",
                        i, undone, synchronized, static_cast<long long>(undoHead.value_or(-1)),
                        static_cast<long long>(head.value_or(-1)), probe->choice, before,
                        doc->error().c_str());
        check(undoOkay, "one Undo reverses exactly one Menu choice");
        const bool redone = doc->ops().redo().ok && doc->synchronise();
        pump();
        check(redone && probe->choice == next, "one Redo restores Menu choice");
        shown = doc->parameterFeed().current()->front();
        record = shown.parameters().front();
        control.present(record, 0);
    }
    bridge.audioDeviceStopped();
}
void callbackAnalyzerRateChange(const std::filesystem::path &directory) {
    constexpr std::int64_t mainTrack = 1;
    constexpr std::int64_t programmeTrack = 2;
    constexpr int fftSize = 1024;
    StoreError status{};
    const auto path = directory / "analyzer-rate.adi";
    auto store = Store::create(path, status);
    check(bool(store), "sample rate fixture creates a project");
    if (!store) return;
    store->db().exec("INSERT INTO tracks(id,kind,name) VALUES(1,'master','Main'),(2,'audio','Programme');"
                     "INSERT INTO mixer_strip(track_id,volume_db) VALUES(1,0),(2,0);");
    store.reset();

    CallbackTone tone;
    tone.channels = {.375f, .125f};
    std::string error;
    auto doc = ProjectDocument::open(path, false, {}, error);
    check(bool(doc), "sample rate fixture opens through ProjectDocument");
    if (!doc) return;
    doc->session().setSourcesFor([&](std::int64_t track) {
        return track == programmeTrack ? std::vector<engine::Node *>{&tone} : std::vector<engine::Node *>{};
    });
    SimulatedDevice hardware;
    device::DeviceBridge bridge(*doc);
    doc->session().graph().setFadeFrames(0);
    bridge.setRequestedBlockSize(hardware.frames);
    bridge.audioDeviceAboutToStart(&hardware);
    doc->session().transport().play();

    const auto broker = doc->analysis();
    SharedAnalysis::Request request{mainTrack, engine::ScopePoint::PostFader, fftSize};
    request.outputs = SharedAnalysis::Scope;
    auto scope = broker->acquire(request);
    request.outputs = SharedAnalysis::Spectrogram;
    auto spectrogram = broker->acquire(request);
    auto meter = broker->acquireStereoMeter(mainTrack, {});
    const auto leasesSurvive = [&] {
        const auto stats = broker->stats();
        return doc->analysis() == broker && scope.available() && spectrogram.available() && meter.available() &&
               stats.consumers == 3 && stats.domains == 2 && stats.taps == 1;
    };
    check(bridge.lastOpen().opened && leasesSurvive(), "one project Main tap feeds both windows and the stereo meter");

    std::array<std::array<float, 256>, 2> pcm{};
    std::array<float *, 2> out{pcm[0].data(), pcm[1].data()};
    const auto render = [&](int callbacks) {
        for (int i = 0; i < callbacks; ++i)
            bridge.audioDeviceIOCallbackWithContext(nullptr, 0, out.data(), 2, hardware.frames, {});
        broker->pump(); // Direct bounded pass; no requestFrame or view refresh rebinds the tap.
    };
    const auto freshAnalysis = [&] {
        const auto frame = scope.read();
        const auto reading = meter.readStereoMeter();
        if (!frame || frame != spectrogram.read() || !reading ||
            frame->scopeLeft.size() != fftSize || frame->scopeRight.size() != fftSize ||
            frame->columns.empty() || !frame->columns.back().binsDbfs) return false;
        const auto allValue = [](const auto &samples, float value) {
            return std::all_of(samples.begin(), samples.end(), [value](float sample) { return sample == value; });
        };
        return bridge.core().oversizeRefusals() == 0 &&
               frame->request.track == mainTrack && frame->advancing &&
               frame->sampleRate == hardware.rate && reading->sampleRate == hardware.rate &&
               frame->generation == reading->generation &&
               allValue(frame->scopeLeft, tone.channels[0]) && allValue(frame->scopeRight, tone.channels[1]) &&
               allValue(std::span(pcm[0]).first(static_cast<std::size_t>(hardware.frames)), tone.channels[0]) &&
               allValue(std::span(pcm[1]).first(static_cast<std::size_t>(hardware.frames)), tone.channels[1]) &&
               reading->reading.maximumPeak == std::array<double, 2>{std::abs(tone.channels[0]), std::abs(tone.channels[1])};
    };
    render(4);
    check(freshAnalysis(), "48 kHz/256 device audio drives Main scope, history and peak publication");
    // Retain the snapshots a frozen reader would own throughout both changes.
    const auto frozen = scope.read();
    const auto frozenMeter = meter.readStereoMeter();
    if (!frozen || !frozenMeter || frozen->columns.empty() || !frozen->columns.front().binsDbfs) {
        bridge.audioDeviceStopped();
        return;
    }
    const auto frozenLeft = frozen->scopeLeft;
    const auto frozenRight = frozen->scopeRight;
    const auto frozenColumnCount = frozen->columns.size();
    const auto frozenBins = frozen->columns.front().binsDbfs;
    const auto frozenBinsData = *frozenBins;
    const auto frozenPeaks = frozenMeter->reading.maximumPeak;

    const auto verifyReplacement = [&](const auto &previous, const auto &previousMeter) {
        const auto position = scope.sourcePosition();
        check(bridge.lastOpen().opened && leasesSurvive() && previous &&
                  position.generation > previous->generation && position.written == 0 &&
                  spectrogram.sourcePosition().generation == position.generation &&
                  meter.sourcePosition().generation == position.generation &&
                  !scope.read() && !spectrogram.read() && !meter.readStereoMeter(),
              "device prepare invalidates old results and preserves the same three leases before any analyser frame");
        render(1);
        check(!scope.read() && !spectrogram.read(),
              "a partial new window cannot resurrect samples or history from the former tap");
        render(3);
        check(freshAnalysis(), "current-rate PCM and metering reach the retained Main leases without reopening the project");
        const auto next = scope.read();
        const auto nextMeter = meter.readStereoMeter();
        check(previous && previousMeter && next && nextMeter && next->generation > previous->generation &&
                  next->sourceEpoch > previous->sourceEpoch && nextMeter->epoch > previousMeter->epoch &&
                  next->generation == position.generation && !next->columns.empty() &&
                  std::all_of(next->columns.begin(), next->columns.end(), [&](const auto &column) {
                      return column.binsDbfs && std::none_of(previous->columns.begin(), previous->columns.end(),
                          [&](const auto &old) { return old.binsDbfs == column.binsDbfs; });
                  }),
              "new source epochs publish only spectrogram columns acquired from the replacement tap");
    };
    const auto streamTime = bridge.core().streamTime();
    hardware.rate = 44100.;
    tone.channels = {-.25f, .0625f};
    bridge.audioDeviceAboutToStart(&hardware);
    check(bridge.core().reconfigurations() == 1 && bridge.core().streamTime() == streamTime,
          "the open device changes only its sample rate, preserving block size and stream clock");
    verifyReplacement(frozen, frozenMeter);

    const auto beforeStop = scope.read();
    const auto meterBeforeStop = meter.readStereoMeter();
    bridge.audioDeviceStopped();
    tone.channels = {.03125f, -.125f};
    bridge.audioDeviceAboutToStart(&hardware);
    check(bridge.core().reconfigurations() == 0 && bridge.core().streamTime() == 0,
          "stopping and reopening at the same format exercises the DeviceBridge open path");
    verifyReplacement(beforeStop, meterBeforeStop);
    check(frozen->sampleRate == 48000. && frozen->scopeLeft == frozenLeft && frozen->scopeRight == frozenRight &&
              frozen->columns.size() == frozenColumnCount && frozen->columns.front().binsDbfs == frozenBins &&
              *frozenBins == frozenBinsData &&
              frozenMeter->sampleRate == 48000. && frozenMeter->reading.maximumPeak == frozenPeaks,
          "a retained frozen scope, spectrogram and meter snapshot stays immutable across both replacements");
    bridge.audioDeviceStopped();
}
void functions(AppCommands &commands, const std::filesystem::path &directory) {
    std::string error;
    auto document = ProjectDocument::open(directory / "function-tabs.adi", true, {}, error);
    check(document != nullptr, "component project opens without a native host");
    if (!document) return;
    AdiRootComponent root(document->view(), document->ops(), document->mailbox(), commands,
                          document->views(), document->sessionState(), "function-tabs");
    root.setSize(1200, 800);
    UiPeerRoot peer;
    peer.setSize(1200, 800); peer.setVisible(true); peer.addToDesktop(0);
    check(dynamic_cast<UiFakePeer *>(peer.getPeer()), "test input uses UiFakePeer");
    peer.addAndMakeVisible(root);
    auto publication = std::make_shared<ParameterPublication>();
    publication->push_back(makePanel(21, true, "External limiter"));
    publication->push_back(makePanel(20, false, "Limiter"));
    auto &strip = root.devices;
    strip.setVisible(true);
    strip.publication = [&] { return publication; };
    strip.targetCurrent = [&](std::int64_t device, const engine::DeviceRuntimeToken &, std::uint64_t stamp) {
        return std::any_of(publication->begin(), publication->end(), [&](const auto &data) {
            return data.id == device && data.instance == stamp && data.available;
        });
    };
    std::vector<Event> events;
    bool rejectValue = false;
    strip.guardedGesture = [&](std::int64_t device, const std::string &, engine::ParamEventKind kind,
                                double value, const engine::DeviceRuntimeToken &, std::uint64_t stamp) {
        events.push_back({device, kind, stamp, value});
        return kind != engine::ParamEventKind::Value || !rejectValue;
    };
    std::string op;
    Payload payload;
    strip.submit = [&](const std::string &name, Payload data) { op = name; payload = std::move(data); return true; };
    int additions = 0;
    mix::Function added = mix::Function::Eq;
    strip.functionAvailability = [](mix::Function kind) {
        return kind == mix::Function::Limiter || kind == mix::Function::Compressor
            ? mix::Availability{true, {}} : mix::Availability{false, "Core pending"};
    };
    strip.addFunction = [&](mix::Function kind, std::int64_t track) {
        ++additions; added = kind; return track == 2;
    };
    int moves = 0;
    DevicePanelData moved;
    std::optional<std::int64_t> movedBefore;
    strip.moveFunction = [&](const DevicePanelData &expected, std::int64_t track, std::optional<std::int64_t> before) {
        ++moves; moved = expected; movedBefore = before; return track == 2;
    };
    strip.frame(2);
    check(strip.addTab.isVisible() && strip.panels.size() == 2, "chain-end add tab appears beside actual authored devices");
    if (strip.panels.size() != 2) return;
    auto *ordinary = strip.panels[0].get();
    auto *native = strip.panels[1].get();
    check(!ordinary->nativeFunction() && native->nativeFunction() &&
              native->nativeFunction()->kind == mix::Function::Limiter,
          "external UID coincidence remains an ordinary plug-in; native identity is explicit");
    check(native->parameterPages.empty(), "a plain Limiter without group descriptors invents no Clip page");
    {
        DevicePanelView card(strip, publication->back(), true);
        card.setSize((look::workspaceSegmentWidth * 3), look::mixStackRow);
        card.present(publication->back());
        const auto *gain = card.controls.empty() ? nullptr : card.controls.front().get();
        check(gain && gain->isVisible() && gain->getBottom() <= card.getHeight() &&
                  !card.configureButton.isVisible(), "the compact Limiter retains its actual Gain control");
        card.setSize((look::workspaceSegmentWidth * 3) * 2, look::mixStackFocusedRow);
        check(gain && card.controls.front().get() == gain && gain->isVisible(),
              "the expanded card uses the same owned control as the compact card");
    }
    check(!ordinary->beginFunctionDrag(), "ordinary plug-ins cannot enter the native function drag path");
    {
        constexpr auto oldRefusal = "Earlier device action was refused.";
        constexpr auto newRefusal = "Fixture device cannot be loaded.";
        constexpr auto uid = "fixture.builtin";
        bool allow = false;
        root.loadBuiltin = [&](const std::string &device, std::int64_t track, std::optional<std::int64_t> before) {
            if (device != uid || track != 2 || before != ordinary->id) return false;
            if (!allow) root.say(newRefusal);
            return allow;
        };
        root.say(oldRefusal);
        check(!root.insertDevice(uid, 2, ordinary->id) && root.error() == newRefusal,
              "a refused built-in insertion retains its own status");
        allow = true;
        check(root.insertDevice(uid, 2, ordinary->id) && root.error().empty(),
              "a successful built-in insertion clears the earlier device status");
        root.loadBuiltin = {};

        strip.holdsCopy = [] { return true; };
        strip.paste = [&](std::int64_t track, std::optional<std::int64_t> before) {
            return track == 2 && before == ordinary->id;
        };
        root.say(oldRefusal);
        check(strip.clipboard(DeviceChainStrip::Clip::Paste, ordinary->id) && root.error().empty(),
              "a successful device paste clears the earlier status on the real chain-strip path");
        strip.holdsCopy = {};
        strip.paste = {};
    }
    int duplicates = 0;
    strip.duplicate = [&](std::int64_t, std::int64_t, std::optional<std::int64_t>) { ++duplicates; return true; };
    const auto commandItems = menuItems(native->commands());
    const auto duplicate = std::find_if(commandItems.begin(), commandItems.end(), [](const auto &item) { return item.name == "Duplicate"; });
    check(duplicate != commandItems.end() && !duplicate->enabled &&
              !strip.clipboard(DeviceChainStrip::Clip::Duplicate, native->id) && duplicates == 0,
          "native function duplication cannot evade the one-function-per-channel rule");
    check(native->title.getTooltip().contains("476 samples") && native->title.getTooltip().contains("Native Mix View"),
          "native panel says its real latency and movable function scope");
    auto items = menuItems(strip.functionMenu());
    check(items.size() == 5, "add menu has exactly the five director-confirmed functions");
    if (items.size() != 5) return;
    check(!items[0].enabled && items[0].name.contains("Core pending") && items[1].enabled && !items[1].ticked &&
              items[4].ticked && !items[4].enabled,
          "menu distinguishes absent unavailable cores, addable core and already-present native function");
    constexpr auto existingNotice = "Previous status";
    constexpr auto staleNotice = "This Add menu is out of date. Reopen it and try again.";
    auto staleAction = items[1].action;
    root.say(existingNotice);
    strip.cancelFunctionInteractions();
    check(root.error() == existingNotice, "cancelling the Device View add menu alone leaves the Status Bar alone");
    const auto cancelHead = History(document->store()).head();
    staleAction();
    check(additions == 0 && root.error() == staleNotice && History(document->store()).head() == cancelHead,
          "attempting the cancelled Device View choice explains the refusal with no insertion or undo step");
    items = menuItems(strip.functionMenu()); items[1].action();
    check(additions == 1 && added == mix::Function::Compressor, "current enabled menu action calls shared insertion once");
    staleAction = menuItems(strip.functionMenu())[1].action;
    strip.frame(3); strip.frame(2);
    root.say(existingNotice);
    staleAction();
    check(additions == 1 && root.error() == staleNotice && History(document->store()).head() == cancelHead,
          "changing channels refuses the old Device View choice and says to reopen the menu");
    {
        root.chooseMixViewTab(static_cast<int>(MixViewPanel::Tab::Stack));
        root.setMixView(true, false);
        root.frame();
        auto &stack = root.mixViewPanel.stack;
        root.arrangement.geometry.selection.track = 2;
        root.publishSelection();
        stack.frame(root.reader());
        const auto toolbar = menuItems(stack.functionMenu());
        check(toolbar.size() > 1 && toolbar[1].enabled && toolbar[1].action,
              "the toolbar Add menu offers its current channel's Compressor");
        const auto beforeCommit = root.current();
        check(document->deviceAction("track.rename", {{"id", 2}, {"name", "Renamed while Add was open"}}) &&
                  root.current() != beforeCommit,
              "a real authored track commit invalidates the open toolbar menu");
        const auto toolbarHead = History(document->store()).head();
        root.say(existingNotice);
        if (toolbar.size() > 1 && toolbar[1].action) toolbar[1].action();
        check(additions == 1 && root.error() == staleNotice && History(document->store()).head() == toolbarHead,
              "a stale toolbar choice only reports the refusal and leaves the latest commit and Undo intact");
        stack.frame(root.reader());
        PluginEntry reference;
        reference.format = "vst3";
        reference.uid = "fixture.vendor.unique";
        reference.name = "Fixture VST";
        reference.kind = PluginEntry::Kind::MidiEffect;
        PluginScan scan;
        scan.phase = PluginScan::Phase::Done;
        scan.listed = 1;
        scan.listing.plugins = {reference, reference};
        root.browser.setPluginScan(scan);
        int plugIns = 0;
        root.loadPlugin = [&](const auto& picked, std::int64_t track, std::optional<std::int64_t> before, bool guessed) {
            if (picked.uid == reference.uid && picked.subtype == "midi_effect" &&
                track == 2 && !before && !guessed)
                ++plugIns;
            return true;
        };
        auto menu = stack.rowAddMenu(stack.model().rowRef(2));
        const auto result = menuItems(menu);
        const auto named = [&](const auto& label) {
            return std::find_if(result.begin(), result.end(), [&](const auto &item) { return item.name == label; });
        };
        const auto plugin = named("Fixture VST (VST3)");
        const auto limiter = named("Limiter");
        check(!result.empty() && result.front().name == "Mix View tools" && named("Devices") < named("Plug-ins") &&
              named("Plug-ins") < plugin && plugin != result.end() &&
              limiter != result.end() && limiter->ticked && !limiter->enabled &&
              std::count_if(result.begin(), result.end(), [&](const auto& item) { return item.name == plugin->name; }) == 1,
              "the picker orders devices after tools, deduplicates plug-ins and ticks the existing function");
        StackAddPicker picker(menu);
        auto* search = dynamic_cast<juce::TextEditor*>(picker.getChildComponent(0));
        auto* list = dynamic_cast<juce::ListBox*>(picker.getChildComponent(1));
        check(search && list, "the picker exposes a search field and a named menu");
        if (search && list) {
            search->setText("fixture vst", false);
            search->onTextChange();
            check(list->getListBoxModel()->getNumRows() == 2 && list->getSelectedRow() == 1 &&
                      list->getListBoxModel()->getNameForRow(1) == "Fixture VST (VST3)",
                  "case-insensitive search focuses the matching device and exposes its accessible name");
            search->onReturnKey();
        }
        check(plugIns == 1, "Enter loads the matching plug-in with its catalogue kind on the captured row");
        root.say(existingNotice);
        if (search) search->onEscapeKey();
        stack.cancelInteractions();
        check(root.error() == existingNotice, "dismissing the row picker alone does not report a refusal");
        if (search) search->onReturnKey();
        check(plugIns == 1 && root.error() == staleNotice && History(document->store()).head() == toolbarHead,
              "attempting a stale filtered row choice says to reopen and creates no edit or Undo step");
        root.browser.setPluginScan({});
        root.loadPlugin = {};
        root.setMixView(false, false);
        strip.frame(2);
    }
    ordinary = strip.panels[0].get(); native = strip.panels[1].get();
    native->enabled.onClick();
    check(op == "device.remove" && payload.at("id").get<std::int64_t>() == 20,
          "native function deactivation removes it through the shared undoable device op");
    auto submit = strip.submit;
    strip.submit = [](const std::string &, Payload) { return false; };
    native->enabled.setToggleState(false, juce::dontSendNotification);
    native->enabled.onClick();
    check(native->enabled.getToggleState(), "refused native removal restores its authored active state");
    strip.submit = submit;
    check(strip.setEnabledExpected(publication->back(), false) && op == "device.remove",
          "shared compact/native activation authority removes instead of bypassing");
    publication = std::make_shared<ParameterPublication>(*publication);
    publication->back().enabled = false;
    strip.frame(2); native = strip.panels[1].get(); ordinary = strip.panels[0].get();
    native->enabled.onClick();
    check(op == "device.setEnabled" && payload.at("enabled").get<bool>() &&
              payload.at("id").get<std::int64_t>() == 20,
          "expanded activation recovers an already-disabled native function");
    publication = std::make_shared<ParameterPublication>(*publication);
    publication->back().enabled = true;
    strip.frame(2); native = strip.panels[1].get(); ordinary = strip.panels[0].get();
    ordinary->enabled.onClick();
    check(op == "device.setEnabled" && payload.at("id").get<std::int64_t>() == 21,
          "ordinary plug-in activator keeps ordinary bypass semantics");
    {
        const auto targetCurrent = strip.targetCurrent;
        const auto reenter = [&](DevicePanelData expected, bool validate, const char *command, const char *label) {
            const bool authored = expected.enabled;
            const bool requested = !authored;
            Payload arguments{{"id", expected.id}};
            if (std::string_view(command) == "device.setEnabled") arguments["enabled"] = requested;
            int validations = 0, submissions = 0;
            bool sameTarget = false, sameEdit = false;
            strip.targetCurrent = [&](std::int64_t id, const engine::DeviceRuntimeToken &, std::uint64_t stamp) {
                ++validations;
                sameTarget = id == expected.id && stamp == expected.instance;
                // Model a reentrant refresh while the borrowed record stays alive.
                expected.enabled = requested;
                return validate && sameTarget;
            };
            strip.submit = [&](const std::string &name, Payload p) {
                ++submissions;
                sameEdit = name == command && p == arguments;
                return true;
            };
            const bool result = strip.setEnabledExpected(expected, requested);
            strip.targetCurrent = targetCurrent;
            strip.submit = submit;
            check(validations == 1 && sameTarget && result == validate &&
                      (validate ? submissions == 1 && sameEdit : submissions == 0),
                  label);
        };
        reenter(publication->back(), true, "device.remove", "active native click retains removal through revalidation");
        auto bypassed = publication->back();
        bypassed.enabled = false;
        reenter(bypassed, true, "device.setEnabled", "bypassed native click retains recovery through revalidation");
        reenter(publication->front(), true, "device.setEnabled", "ordinary revalidated click keeps bypass semantics");
        reenter(publication->back(), false, "device.remove", "refused revalidation submits no activation edit");
    }
    check(native->beginFunctionDrag() && native->functionDragTarget() &&
              native->functionDragTarget()->id == 20 && native->functionDragTarget()->instance == 71,
          "a function drag retains its authored identity and current instance");
    check(strip.dropFunction(*native, 21) && moves == 1 && moved.id == 20 && moved.instance == 71 && movedBefore == 21 &&
              !native->functionDragTarget(),
          "drop forwards one captured move and clears its drag owner before the callback");
    {
        const auto currentTarget = strip.targetCurrent;
        const auto submitMove = strip.moveFunction;
        // The destination can replace its panels during validation. Keep the drag source alive independently.
        DevicePanelView dragSource(strip, publication->back(), true);
        check(dragSource.beginFunctionDrag(), "independent source starts a native drag on the displayed track");
        bool changedDestination = false;
        std::optional<std::int64_t> submittedTrack;
        strip.targetCurrent = [&](std::int64_t id, const engine::DeviceRuntimeToken &runtime, std::uint64_t stamp) {
            const bool valid = currentTarget(id, runtime, stamp);
            strip.frame(3);
            changedDestination = strip.panels.empty();
            return valid;
        };
        strip.moveFunction = [&](const DevicePanelData &, std::int64_t track, std::optional<std::int64_t>) {
            submittedTrack = track;
            return true;
        };
        const bool accepted = strip.dropFunction(dragSource, 21);
        strip.targetCurrent = currentTarget;
        strip.moveFunction = submitMove;
        check(changedDestination, "validating the drop updates the destination to another track");
        check(!accepted && !submittedTrack, "destination track change refuses the drop without submitting a move");
        strip.frame(2);
        if (strip.panels.size() != 2) { check(false, "original track restores its device panels"); return; }
        native = strip.panels[1].get();
    }
    check(native->beginFunctionDrag(), "a second drag captures the displayed instance");
    publication = std::make_shared<ParameterPublication>(*publication);
    publication->back().instance = 72;
    check(!strip.dropFunction(*native, 21) && moves == 1, "publication replacement rejects an old drag before mutation");
    strip.frame(2);
    native = strip.panels[1].get();
    check(native->presentedInstance() == 72 && !native->functionDragTarget(), "new runtime rebuilds the retained function panel");
    if (native->controls.size() != 1) { check(false, "native descriptor makes its real parameter control"); return; }
    auto *control = native->controls.front().get();
    events.clear();
    control->slider.onDragStart(); control->slider.setValue(.4, juce::sendNotificationSync); control->slider.onDragEnd();
    check(events.size() == 3 && events[0].kind == engine::ParamEventKind::Begin &&
              events[1].kind == engine::ParamEventKind::Value && events[2].kind == engine::ParamEventKind::End &&
              events[0].instance == 72 && events[2].value == .4,
          "one native drag uses the common exact-instance begin/value/end route");
    events.clear();
    control->slider.onDragStart(); control->slider.setValue(.5, juce::sendNotificationSync);
    strip.cancelFunctionInteractions(); control->slider.onDragEnd();
    check(events.size() == 3 && events.back().kind == engine::ParamEventKind::End && events.back().value == .5,
          "owner cancellation closes its gesture once; a late release cannot close another owner");
    events.clear();
    control->slider.onDragStart(); rejectValue = true;
    control->slider.setValue(.6, juce::sendNotificationSync); control->slider.onDragEnd();
    check(events.size() == 3 && events.back().kind == engine::ParamEventKind::End && events.back().value == .5,
          "rejected parameter move closes at its last accepted value");
    rejectValue = false;
    events.clear();
    control->slider.onDragStart();
    publication = std::make_shared<ParameterPublication>(*publication); publication->back().instance = 73;
    strip.cancelFunctionInteractions(); control->slider.onDragEnd();
    check(events.size() == 2 && events.back().kind == engine::ParamEventKind::End && events.back().instance == 72,
          "stale cancellation reaches the retained old owner rather than the replacement runtime");
    strip.frame(2); native = strip.panels[1].get(); control = native->controls.front().get();
    events.clear();
    control->slider.onDragStart(); peer.setVisible(false);
    check(events.size() == 2 && events.back().kind == engine::ParamEventKind::End,
          "ancestor hiding ends a native gesture without a frame or native input");
    bool foreignLive = true;
    const auto beforeForeign = additions;
    strip.setVisible(false);
    auto foreign = menuItems(strip.functionMenuFor(3, [&] { return foreignLive; }));
    foreign[1].action();
    check(additions == beforeForeign + 1 && menuItems(strip.functionMenu())[4].ticked,
          "explicit-target add menu works for another visible owner without retargeting the hidden strip");
    foreignLive = false; foreign[1].action();
    check(additions == beforeForeign + 1, "expired foreign add-menu owner cannot add later");
    peer.removeChildComponent(&root); peer.removeFromDesktop();
}
void processingIssuePresentation(AppCommands &commands, const std::filesystem::path &directory) {
    std::string error;
    auto document = ProjectDocument::open(directory / "processing-issue.adi", true, {}, error);
    check(document != nullptr, "processing issue component project opens without a native host");
    if (!document) return;
    AdiRootComponent root(document->view(), document->ops(), document->mailbox(), commands,
                          document->views(), document->sessionState(), "processing-issue");
    root.setSize(1200, 800);
    UiPeerRoot peer;
    peer.setSize(1200, 800); peer.setVisible(true); peer.addToDesktop(0);
    peer.addAndMakeVisible(root);
    auto publication = std::make_shared<ParameterPublication>();
    publication->push_back(makePanel(20, false, "A long authored device name retained through configuration failure"));
    auto &strip = root.devices;
    strip.setVisible(true);
    strip.publication = [&] { return publication; };
    strip.targetCurrent = [&](std::int64_t id, const engine::DeviceRuntimeToken &, std::uint64_t stamp) {
        return publication->front().id == id && publication->front().instance == stamp &&
               publication->front().available;
    };
    std::vector<engine::ParamEventKind> events;
    strip.guardedGesture = [&](std::int64_t, const std::string &, engine::ParamEventKind kind,
                                double, const engine::DeviceRuntimeToken &, std::uint64_t) {
        events.push_back(kind);
        return true;
    };
    strip.frame(2);
    check(strip.panels.size() == 1 && strip.panels.front()->controls.size() == 1,
          "processing issue fixture has one real editable control");
    if (strip.panels.size() != 1 || strip.panels.front()->controls.size() != 1) return;
    auto *panel = strip.panels.front().get();
    auto *control = panel->controls.front().get();
    const auto titleBounds = panel->title.getBounds();
    const auto panelWidth = panel->widthFor(160);
    const auto ordinaryTooltip = panel->title.getTooltip();
    check(panel->title.getButtonText() == juce::String(publication->front().name) &&
              !ordinaryTooltip.contains("Configuration failed"),
          "ordinary processing has no failure warning");
    control->slider.onDragStart();
    publication = std::make_shared<ParameterPublication>(*publication);
    publication->front().processingIssue = "The configuration worker could not start.";
    strip.frame(2);
    check(strip.panels.front().get() == panel && panel->controls.front().get() == control &&
              panel->presentedInstance() == 71 && control->isEnabled() && panel->enabled.isEnabled(),
          "nonfatal processing issue retains the exact panel, runtime, control and activator");
    check(panel->title.getButtonText().startsWith("Configuration failed") &&
              panel->title.getButtonText().contains(juce::String(publication->front().name)) &&
              panel->title.getTooltip().contains("The configuration worker could not start.") &&
              panel->title.getTooltip().contains("To retry, reopen this project using File > Open Project.") &&
              panel->title.getTooltip().contains("476 samples"),
          "header puts configuration failure first and retains the device name and detailed latency tooltip");
    check(panel->title.getBounds() == titleBounds && panel->widthFor(160) == panelWidth &&
              events == std::vector<engine::ParamEventKind>{engine::ParamEventKind::Begin},
          "status publication neither expands layout nor ends an active gesture");
    strip.frame(2);
    control->slider.setValue(.4, juce::sendNotificationSync); control->slider.onDragEnd();
    check(events == std::vector<engine::ParamEventKind>{engine::ParamEventKind::Begin,
                  engine::ParamEventKind::Value, engine::ParamEventKind::End},
          "repeated failed-status presentation still permits the ordinary exact-owner edit");
    publication = std::make_shared<ParameterPublication>(*publication);
    publication->front().processingIssue.clear();
    strip.frame(2);
    check(strip.panels.front().get() == panel && panel->controls.front().get() == control &&
              panel->title.getButtonText() == juce::String(publication->front().name) &&
              panel->title.getTooltip() == ordinaryTooltip && panel->widthFor(160) == panelWidth &&
              control->isEnabled() && events.size() == 3,
          "recovery removes status text without rebuilding controls or emitting another edit");
    peer.removeChildComponent(&root); peer.removeFromDesktop();
}
DevicePanelData makePagedPanel() {
    auto data = makePanel(30, false, "Clip / Limiter");
    data.declared.clear(); data.resolved.entries.clear(); data.records.clear(); data.defaults.clear();
    const auto add = [&](const char *id, const char *name, const char *group, const char *groupName, bool enable, double value) {
        data.declared.push_back({id, name, true}); data.resolved.entries.push_back({id, false});
        panel::Record r; r.id = id; r.name = name; r.playing = r.stored = value;
        if (enable) { r.shape = panel::Shape::Switch; r.stepCount = 2; }
        data.records.push_back(r); data.defaults[id] = value;
        data.parameterGroups[id] = {group, groupName, enable};
    };
    // Existing Limiter ids may come first in the backend; display order still follows audio.
    add("lgain", "Limiter Gain", "limiter", "Limiter", false, .25);
    add("lenable", "Limiter On", "limiter", "Limiter", true, 1.);
    add("cgain", "Clip Gain", "clip", "Clip", false, .25);
    add("cenable", "Clip On", "clip", "Clip", true, 1.);
    add("lexport", "Limiter Export Quality", "limiter", "Limiter", false, 0.);
    data.declared.back().automatable = false;
    data.records.back().shape = panel::Shape::Switch; data.records.back().stepCount = 2;
    add("readonly", "Read-only Status", "limiter", "Limiter", false, .25);
    data.declared.back().modifiable = false; data.declared.back().automatable = false;
    data.resolved.modifiableCount = data.records.size();
    return data;
}
void parameterPages(AppCommands &commands, const std::filesystem::path &directory) {
    std::string error;
    auto document = ProjectDocument::open(directory / "parameter-pages.adi", true, {}, error);
    check(document != nullptr, "page component project opens headlessly");
    if (!document) return;
    AdiRootComponent root(document->view(), document->ops(), document->mailbox(), commands,
                          document->views(), document->sessionState(), "parameter-pages");
    root.setSize(1200, 800);
    UiPeerRoot peer;
    peer.setSize(1200, 800); peer.setVisible(true); peer.addToDesktop(0); peer.addAndMakeVisible(root);
    auto publication = std::make_shared<ParameterPublication>(); publication->push_back(makePagedPanel());
    auto &strip = root.devices;
    strip.setVisible(true); strip.publication = [&] { return publication; };
    strip.targetCurrent = [&](std::int64_t id, const engine::DeviceRuntimeToken &, std::uint64_t instance) {
        return publication->front().id == id && publication->front().instance == instance && publication->front().available;
    };
    std::vector<Event> events;
    bool reject = false;
    strip.guardedGesture = [&](std::int64_t id, const std::string &parameter, engine::ParamEventKind kind,
                                double value, const engine::DeviceRuntimeToken &, std::uint64_t instance) {
        events.push_back({id, kind, instance, value, parameter});
        if (kind == engine::ParamEventKind::Value && reject) return false;
        if (kind == engine::ParamEventKind::Value) {
            publication = std::make_shared<ParameterPublication>(*publication);
            for (auto &r : publication->front().records) if (r.id == parameter) r.stored = r.playing = value;
        }
        return true;
    };
    int actions = 0;
    strip.submit = [&](const std::string &, Payload) { ++actions; return true; };
    strip.frame(2);
    auto *panel = strip.panels.front().get();
    const auto findControl = [&](const std::string &id) -> ParameterControl * {
        const auto at = std::find_if(panel->controls.begin(), panel->controls.end(), [&](const auto &c) { return c->parameter == id; });
        return at == panel->controls.end() ? nullptr : at->get();
    };
    auto *clip = findControl("cgain"), *limiter = findControl("lgain");
    check(panel->parameterPages.size() == 2 && panel->parameterPages[0].id == "clip" &&
              panel->parameterPages[1].id == "limiter" && panel->selectedParameterPage() == "limiter",
          "only provided pages appear in serial Clip then Limiter order, with Limiter selected by default");
    if (!clip || !limiter || panel->parameterPages.size() != 2) return;
    auto *configuration = findControl("lexport"), *readOnly = findControl("readonly");
    check(configuration && configuration->isEnabled() && readOnly && !readOnly->isEnabled() &&
              !publication->front().declared[4].automatable,
          "explicit configuration is editable without automation eligibility, while read-only fields stay disabled");
    events.clear();
    if (configuration) {
        configuration->toggle.setToggleState(true, juce::dontSendNotification);
        configuration->toggle.onClick();
    }
    check(events.size() == 3 && events[1].parameter == "lexport" && events[1].value == 1.,
          "configuration toggle uses the same exact-target Begin Value End callback");
    events.clear();
    if (readOnly) readOnly->slider.onDragStart();
    check(events.empty(), "disabled read-only parameter cannot start a retained edit callback");
    strip.frame(2);
    check(!clip->isVisible() && limiter->isVisible() && !findControl("cenable")->isVisible() &&
              panel->parameterPages[0].enabled->getToggleState() && panel->parameterPages[1].enabled->getToggleState(),
          "selected page filters controls while both real enable parameters remain on");
    events.clear();
    check(panel->chooseParameterPage("clip") && clip->isVisible() && !limiter->isVisible() && events.empty() && actions == 0,
          "page selection changes presentation without processing, parameter writes or device ops");
    panel->chooseParameterPage("limiter");
    check(panel->setParameterPageEnabled("clip", false), "Clip can switch off independently of selected page");
    strip.frame(2);
    check(events.size() == 3 && events[0].parameter == "cenable" && events[1].value == 0 &&
              events[2].kind == engine::ParamEventKind::End && !panel->parameterPages[0].enabled->getToggleState() &&
              panel->parameterPages[1].enabled->getToggleState() && panel->selectedParameterPage() == "limiter" && actions == 0,
          "page enable uses one common begin/value/end gesture and retains the other stage");
    events.clear(); panel->chooseParameterPage("clip");
    check(events.empty() && !panel->parameterPages[0].enabled->getToggleState() &&
              panel->parameterPages[1].enabled->getToggleState(), "selecting inactive Clip never enables it or changes the active Limiter");
    panel->chooseParameterPage("limiter");
    events.clear(); panel->setParameterPageEnabled("limiter", false); strip.frame(2);
    panel->setParameterPageEnabled("clip", true); strip.frame(2);
    check(panel->parameterPages[0].enabled->getToggleState() && !panel->parameterPages[1].enabled->getToggleState() &&
              events.size() == 6 && events[1].parameter == "lenable" && events[4].parameter == "cenable",
          "both stages can be disabled or enabled independently through real parameter ids");
    panel->chooseParameterPage("clip");
    events.clear(); clip->slider.onDragStart(); clip->slider.setValue(.6, juce::sendNotificationSync);
    panel->chooseParameterPage("limiter"); clip->slider.onDragEnd(); clip->slider.setValue(.7, juce::sendNotificationSync);
    check(events.size() == 3 && events.back().kind == engine::ParamEventKind::End && events.back().parameter == "cgain" &&
              events.back().value == .6, "page change ends the exact held gesture; hidden late callbacks cannot resume it");
    events.clear(); limiter->slider.onDragStart(); panel->chooseParameterPage("clip"); clip->slider.onDragStart();
    limiter->slider.onDragEnd(); clip->slider.setValue(.65, juce::sendNotificationSync); clip->slider.onDragEnd();
    check(events.size() == 5 && events[1].parameter == "lgain" && events[1].kind == engine::ParamEventKind::End &&
              events.back().parameter == "cgain" && events.back().kind == engine::ParamEventKind::End,
          "late release from a hidden page cannot end the new page's gesture");
    events.clear(); reject = true; panel->setParameterPageEnabled("limiter", true); reject = false;
    check(events.size() == 3 && events.back().kind == engine::ParamEventKind::End && events.back().value == 0,
          "rejected page-enable value closes at its last accepted state");
    auto stalePageEnable = panel->parameterPages[0].enabled->onClick;
    publication = std::make_shared<ParameterPublication>(*publication);
    publication->front().parameterGroups["cgain"].groupName = "Clip revised";
    strip.frame(2); events.clear(); stalePageEnable();
    check(events.empty() && panel->parameterPages[0].name == "Clip revised", "superseded page metadata invalidates retained header actions");
    events.clear(); clip->slider.onDragStart(); panel->setVisible(false); clip->slider.onDragEnd();
    check(events.size() == 2 && events.back().kind == engine::ParamEventKind::End,
          "hiding a page panel closes its exact held parameter owner");
    panel->setVisible(true);
    events.clear(); clip->slider.onDragStart(); strip.fold(panel->id); clip->slider.onDragEnd();
    check(events.size() == 2 && events.back().kind == engine::ParamEventKind::End,
          "folding the function closes a held page gesture without waiting for native input");
    strip.fold(panel->id);
    stalePageEnable = panel->parameterPages[0].enabled->onClick;
    publication = std::make_shared<ParameterPublication>(*publication); ++publication->front().instance;
    events.clear(); stalePageEnable();
    check(events.empty(), "page enable rejects a replaced runtime before mutation");
    peer.removeChildComponent(&root); peer.removeFromDesktop();
}
void stackEqSpectrum(AppCommands &commands, const std::filesystem::path &directory) {
    std::string error;
    auto document = ProjectDocument::open(directory / "stack-eq-spectrum.adi", true, device::withBuiltinDevices(), error);
    check(document && document->addMixFunction(mix::Function::Eq, 2), "Stack spectrum opens a native EQ fixture");
    if (!document) return;
    document->tick(0);
    const auto publication = document->parameterFeed().current();
    const auto eq = std::ranges::find(*publication, "org.adidaw.mix.eq", &DevicePanelData::uid);
    if (eq == publication->end()) return;
    const auto device = eq->id;
    const engine::ScopeSource expected{2, engine::ScopePoint::DeviceOutput, device};
    auto tap = std::make_shared<engine::ScopeTap>(48000, 1.);
    std::vector<engine::ScopeSource> opened, closed;
    auto broker = std::make_shared<SharedAnalysis>(
        [&](engine::ScopeSource source, double) -> std::shared_ptr<const engine::ScopeTap> {
            opened.push_back(source);
            return source == expected ? tap : nullptr;
        }, [&](engine::ScopeSource source) { closed.push_back(source); }, SharedAnalysis::Mode::Manual);
    AdiRootComponent root(document->view(), document->ops(), document->mailbox(), commands,
                          document->views(), document->sessionState(), "stack-eq-spectrum");
    root.devices.publication = [&] { return document->parameterFeed().current(); };
    root.devices.parameterDemand = [&](const void *owner, const auto &ids) { document->setParameterDemand(owner, ids); };
    root.sharedAnalysis = broker;
    root.state().mixView = true;
    root.state().mixViewTab = static_cast<int>(MixViewPanel::Tab::Stack);
    root.arrangement.geometry.selection.track = 2;
    UiPeerRoot peer;
    peer.setBounds(0, 0, 1200, 720);
    peer.setVisible(true); peer.addToDesktop(0);
    StackTab surface(root);
    surface.setBounds(peer.getLocalBounds()); peer.addAndMakeVisible(surface);
    surface.filterChannels(std::vector<std::int64_t>{2});
    surface.model().setAutoZoom(false, 0);
    surface.frameAt(root.reader(), 500);
    check(opened == std::vector{expected} && broker->stats().consumers == 1,
          "visible EQ graph demands exactly its stable device output");

    const auto findPanel = [&](const auto &walk, juce::Component &parent) -> DevicePanelView * {
        if (auto *item = dynamic_cast<DevicePanelView *>(&parent); item && item->id == device) return item;
        for (auto *child : parent.getChildren())
            if (auto *found = walk(walk, *child)) return found;
        return nullptr;
    };
    const auto paintPanel = [](DevicePanelView &panel) {
        return adi::uitest::pictureForComparison(juce::Image::RGB, panel.getWidth(), panel.getHeight(),
                                                 [&panel](juce::Graphics &g) { panel.paint(g); });
    };
    auto *panel = findPanel(findPanel, surface);
    check(panel != nullptr, "the spectrum uses the existing EQ card");
    if (!panel) return;
    juce::Component::SafePointer<DevicePanelView> card(panel);
    SharedAnalysis::Request request{expected.track, expected.point};
    request.device = expected.device;
    request.channel = SharedAnalysis::Channel::StereoPower;
    auto sibling = broker->acquire(request);
    check(sibling.available() && broker->stats().consumers == 2 && broker->stats().domains == 1 &&
              broker->stats().spectralPlans == 1 && opened.size() == 1,
          "a compatible sibling shares the same tap, FFT plan and worker domain");

    tap->write(static_cast<const float *>(nullptr), nullptr, 1024, 0, false);
    broker->pump(); surface.frameAt(root.reader(), 520);
    const auto silent = paintPanel(*card);
    std::array<float, 1024> left{}, right{};
    for (std::size_t i = 0; i < left.size(); ++i) {
        left[i] = .5f * static_cast<float>(std::sin(2. * std::numbers::pi * 16. * static_cast<double>(i) / static_cast<double>(left.size())));
        right[i] = -left[i];
    }
    tap->write(left.data(), right.data(), static_cast<std::int32_t>(left.size()), 0, false);
    broker->pump(); surface.frameAt(root.reader(), 540);
    const auto audible = paintPanel(*card);
    const auto result = sibling.read();
    int changedPixels = 0;
    const auto graph = card->eqPreviewBounds().toNearestInt();
    const bool withinGraph = adi::uitest::comparePicturePixels(silent, audible, [&](int x, int y) {
        ++changedPixels;
        return graph.contains(x, y);
    });
    if (!result || std::abs(result->binsDbfs[16] + 6.0206f) >= .002f || changedPixels == 0 || !withinGraph)
        std::cout << "Stack spectrum fixture: bin16=" << (result ? result->binsDbfs[16] : -999.f)
                  << ", peak=" << (result ? result->peak[0] : -1.f)
                  << ", changed=" << changedPixels << ", withinGraph=" << withinGraph << '\n';
    check(result && std::abs(result->binsDbfs[16] + 6.0206f) < .002f && changedPixels > 0 && withinGraph,
          "valid antiphase sound after silence changes only pixels clipped to the graph");
    const auto shown = broker->stats();
    const auto width = card->getWidth();
    surface.model().setBaseHeight(122, 550);
    surface.frameAt(root.reader(), 800);
    broker->pump();
    check(card && card->getWidth() != width && broker->stats().transforms == shown.transforms &&
              broker->stats().publications == shown.publications && broker->stats().consumers == shown.consumers,
          "resizing changes graph geometry without requesting a new transform or lease");

    surface.setVisible(false);
    check(broker->stats().consumers == 1 && closed.empty(),
          "hiding the Stack immediately releases its reader while the sibling remains live");
    sibling.reset();
    check(broker->stats().consumers == 0 && closed == std::vector{expected},
          "last hide unbinds the physical EQ tap exactly once");
    surface.setVisible(true);
    surface.frameAt(root.reader(), 850);
    check(broker->stats().consumers == 1, "showing the EQ attaches one new reader");
    surface.filterChannels(std::nullopt);
    surface.setSize(1200, 165);
    surface.frameAt(root.reader(), 900);
    surface.model().scrollTo(surface.model().maxScroll(), 905);
    surface.frameAt(root.reader(), 1200);
    check(broker->stats().consumers == 0 && closed.size() == opened.size(),
          "scrolling all EQ graph pixels outside the viewport unbinds its device tap");
    surface.filterChannels(std::vector<std::int64_t>{2});
    surface.setBounds(peer.getLocalBounds()); surface.frameAt(root.reader(), 1300);
    const auto live = broker->stats();
    surface.suspendParameters(true);
    broker->pump();
    check(live.consumers == 1 && broker->stats().consumers == 0 && broker->stats().workerPasses == live.workerPasses,
          "suspending the window removes its last EQ reader and schedules no idle work");
    peer.removeChildComponent(&surface); peer.removeFromDesktop();
}

void stackLimiterReduction(AppCommands &commands, const std::filesystem::path &directory) {
    namespace l = mix::limiter;
    std::string error;
    auto document = ProjectDocument::open(directory / "stack-limiter-reduction.adi", true,
        device::withBuiltinDevices(), error);
    check(document && document->addMixFunction(mix::Function::Limiter, 2),
          "Stack reduction opens a native limiter fixture");
    if (!document) return;
    document->tick(0);
    const auto publication = document->parameterFeed().current();
    const auto row = std::ranges::find(*publication, "org.adidaw.mix.limiter", &DevicePanelData::uid);
    check(row != publication->end(), "fixture exposes the same exact native runtime to the Stack");
    if (row == publication->end()) return;
    const auto runtime = row->runtime;
    const auto device = row->id;
    auto mailbox = std::make_shared<l::ReductionMailbox>();
    mailbox->setPrepared(true);
    int acquisitions = 0;
    AdiRootComponent root(document->view(), document->ops(), document->mailbox(), commands,
                          document->views(), document->sessionState(), "stack-limiter-reduction");
    root.devices.publication = [&] { return document->parameterFeed().current(); };
    root.devices.parameterDemand = [&](const void *owner, const auto &ids) { document->setParameterDemand(owner, ids); };
    root.state().mixView = true;
    root.state().mixViewTab = static_cast<int>(MixViewPanel::Tab::Stack);
    root.arrangement.geometry.selection.track = 2;
    UiPeerRoot peer;
    peer.setBounds(0, 0, 1200, 720);
    peer.setVisible(true); peer.addToDesktop(0);
    StackTab surface(root);
    surface.setBounds(peer.getLocalBounds()); peer.addAndMakeVisible(surface);
    surface.filterChannels(std::vector<std::int64_t>{2});
    surface.model().setAutoZoom(false, 0);
    surface.model().setBaseHeight(184, 0);
    surface.setLimiterObserver([&](const DevicePanelData &target) {
        if (target.id != device || !target.runtime.sameInstance(runtime)) return l::ReductionMailbox::Lease{};
        ++acquisitions;
        return l::ReductionMailbox::observe(mailbox);
    });
    surface.frameAt(root.reader(), 500);
    check(mailbox->observed() && acquisitions == 1, "expanded visible Limiter card acquires one reading lease");
    const auto locate = [&](const auto &walk, juce::Component &parent) -> DevicePanelView * {
        if (auto *card = dynamic_cast<DevicePanelView *>(&parent); card && card->id == device) return card;
        for (auto *child : parent.getChildren())
            if (auto *found = walk(walk, *child)) return found;
        return nullptr;
    };
    auto *found = locate(locate, surface);
    check(found != nullptr, "the reduction plot lives in the existing Stack device card");
    if (!found) return;
    juce::Component::SafePointer<DevicePanelView> card(found);
    const auto paint = [](DevicePanelView &panel) {
        juce::Image image(juce::Image::RGB, panel.getWidth(), panel.getHeight(), true, juce::SoftwareImageType());
        juce::Graphics g(image);
        panel.paint(g);
        return image;
    };
    const auto rightPeakPixels = [](const juce::Image &image) {
        const auto colour = static_cast<juce::Colour>(look::meterPeak);
        const int x = image.getWidth() - 2 * static_cast<int>(look::gap) - 2;
        int pixels = 0;
        for (int y = DevicePanelView::kTitleHeight; y < image.getHeight(); ++y)
            if (image.getPixelAt(x, y) == colour) ++pixels;
        return pixels;
    };
    const auto empty = paint(*card);
    l::ReductionFrame summary;
    summary.renderBlock = 1;
    summary.observedFrames = 128;
    summary.reduction = {.75f, true};
    mailbox->publishSegment(mailbox->observationEpoch(), summary);
    surface.frameAt(root.reader(), 520);
    const auto peak = paint(*card);
    check(rightPeakPixels(peak) > rightPeakPixels(empty) && acquisitions == 1,
          "the latest measured peak paints a right-hand column without reacquiring the source");
    summary.renderBlock = 2;
    summary.reduction = {};
    mailbox->publishSegment(mailbox->observationEpoch(), summary);
    surface.frameAt(root.reader(), 540);
    const auto missing = paint(*card);
    check(rightPeakPixels(missing) == rightPeakPixels(empty) && mailbox->observed(),
          "an unavailable block paints a genuine gap in place of the latest peak");

    surface.model().setBaseHeight(112, 600);
    surface.frameAt(root.reader(), 950);
    check(!mailbox->observed(), "a compact card releases its reduction lease");
    surface.model().setBaseHeight(184, 980);
    surface.frameAt(root.reader(), 1330);
    check(mailbox->observed() && acquisitions == 2, "re-expansion reacquires an empty observation");
    root.devices.fold(device);
    surface.frameAt(root.reader(), 1360);
    check(!mailbox->observed(), "folded Limiter has no reduction subscription");
    root.devices.fold(device);
    surface.frameAt(root.reader(), 1600);
    check(mailbox->observed() && acquisitions == 3, "unfolding reopens one subscription");
    surface.filterChannels(std::nullopt);
    surface.setSize(1200, 165);
    surface.frameAt(root.reader(), 1640);
    surface.model().scrollTo(surface.model().maxScroll(), 1660);
    surface.frameAt(root.reader(), 2090);
    check(!mailbox->observed(), "a retained Limiter card with its plot fully offscreen releases demand");

    surface.filterChannels(std::vector<std::int64_t>{2});
    surface.setBounds(peer.getLocalBounds());
    surface.frameAt(root.reader(), 2410);
    const auto shown = mailbox->observed();
    auto sibling = l::ReductionMailbox::observe(mailbox);
    surface.setVisible(false);
    check(shown && mailbox->observed(), "hiding a Stack reader preserves a compatible sibling");
    sibling.reset();
    check(!mailbox->observed(), "the last hidden view leaves no producer demand");
    peer.removeChildComponent(&surface); peer.removeFromDesktop();
}

void focusWorkspace(AppCommands &commands, const std::filesystem::path &directory) {
    std::string error;
    auto doc = ProjectDocument::open(directory / "focus-workspace.adi", true, device::withBuiltinDevices(), error);
    check(doc != nullptr, "Focus opens a project fixture");
    if (!doc) return;

    constexpr std::int64_t first = 71, second = 72, added = 73;
    auto rootOwner = std::make_unique<AdiRootComponent>(doc->view(), doc->ops(), doc->mailbox(), commands,
                                                        doc->views(), doc->sessionState(), "focus-workspace");
    auto &root = *rootOwner;
    root.afterEdit = [&] { doc->synchronise(); };
    bool created = true;
    for (auto id : {first, second, added})
        created = root.submitEdit("track.create", {{"id", id}, {"kind", "audio"},
                                                    {"name", "Focus " + std::to_string(id)}},
                                  "Create Focus track") && created;
    check(created, "Focus fixture creates three authored channels");
    if (!created) return;
    root.arrangement.geometry.selection.track = 2;
    root.publishSelection();

    const auto ownSource = [](engine::ScopeSource source) {
        return source.point == engine::ScopePoint::PostFader &&
               (source.track == first || source.track == second || source.track == added);
    };
    std::vector<std::int64_t> opened, closed;
    auto tap = std::make_shared<engine::ScopeTap>(48000, 1.);
    auto secondTap = std::make_shared<engine::ScopeTap>(48000, 1.);
    auto broker = std::make_shared<SharedAnalysis>(
        [&](engine::ScopeSource source, double) -> std::shared_ptr<const engine::ScopeTap> {
            if (ownSource(source)) opened.push_back(source.track);
            return source.track == second ? secondTap : tap;
        },
        [&](engine::ScopeSource source) { if (ownSource(source)) closed.push_back(source.track); },
        SharedAnalysis::Mode::Manual);
    root.sharedAnalysis = broker;

    UiPeerRoot peer;
    juce::Component docked;
    peer.setSize(1440, 800);
    peer.setVisible(true);
    peer.addToDesktop(0);
    peer.addAndMakeVisible(docked);
    docked.setBounds(peer.getLocalBounds());
    docked.addAndMakeVisible(root);
    root.setBounds(docked.getLocalBounds());
    const auto solo = [&](std::int64_t id) {
        const auto current = root.current();
        const auto *track = current ? current->findTrack(id) : nullptr;
        return track && track->soloed;
    };
    const auto toggleMix = [&] {
        const auto ok = root.command(AppCommands::ToggleMixView);
        root.frame();
        return ok;
    };
    auto &mix = root.mixViewPanel;
    auto &focus = mix.focusPage;
    check(root.submitEdit("track.setSolo", {{"id", first}, {"soloed", true}}, "Solo first") && solo(first) &&
              toggleMix() && mix.tab() == MixViewPanel::Tab::Stack && opened.empty(),
          "one solo opens Stack and attaches no Focus comparison source");
    mix.stack.model().setChannelScope(stack::ChannelScope::WithTools, 0);
    check(toggleMix() && !root.state().mixView, "return to Arrangement before changing the solo group");
    check(root.submitEdit("track.setSolo", {{"id", second}, {"soloed", true}}, "Solo second") && solo(second) &&
              toggleMix() && mix.tab() == MixViewPanel::Tab::Focus && focus.isShowing(),
          "two authored solos open the Focus workspace");

    const auto initial = std::vector{first, second};
    check(focus.channelIds() == initial && focus.mainTrack() == first && opened == initial && closed.empty(),
          "the initial ordered solo membership attaches both post-fader sources once");
    check(mix.channels.focusMemberLabel(first) == "A" && mix.channels.focusMemberLabel(second) == "B" &&
              mix.channels.focusMemberLabel(added).isEmpty(),
          "the browser labels the Focus membership without labelling other channels");
    check(mix.stack.model().shownIndex(first) && mix.stack.model().shownIndex(second) &&
              !mix.stack.model().shownIndex(added),
          "member chains bypass Stack tool scope without adding an unrelated row");

    SharedAnalysis::Request same;
    same.track = first;
    same.channel = SharedAnalysis::Channel::StereoPower;
    auto sibling = broker->acquire(same);
    check(sibling.available() && opened == initial,
          "a compatible comparison consumer shares the existing source");
    check(root.chooseMixViewTab(static_cast<int>(MixViewPanel::Tab::Stack)), "Stack can be chosen explicitly");
    root.frame();
    check(!focus.isShowing() && closed == std::vector{second} && solo(first) && solo(second),
          "leaving Focus releases its comparison leases and keeps all authored solos");
    check(mix.channels.focusMemberLabel(first).isEmpty(), "Stack clears Focus-only badges");
    sibling.reset();
    check(closed == std::vector{second, first},
          "the last compatible reader releases the remaining source");

    check(root.chooseMixViewTab(static_cast<int>(MixViewPanel::Tab::Focus)), "return to the retained Focus group");
    root.frame();
    const auto sourceCloses = closed.size();
    focus.scope.setToggleState(true, juce::dontSendNotification);
    root.frame();
    check(!root.playing() && closed.size() == sourceCloses + 2,
          "stopped Focus Scope releases both member taps while waiting for alignable audio");
    focus.scope.setToggleState(false, juce::dontSendNotification);
    root.frame();

    auto detail = broker->acquire(same);
    auto rawRequest = same;
    rawRequest.outputs = SharedAnalysis::Scope;
    auto waveform = broker->acquire(rawRequest);
    const auto beforeAncestorHide = closed.size();
    docked.setVisible(false);
    check(focus.isVisible() && !focus.isShowing() && closed.size() == beforeAncestorHide + 1 &&
              closed.back() == second && detail.available() && waveform.available(),
          "ancestor hide drops Focus spectrum demand immediately and preserves both sibling readers");
    const auto opensBeforeRestore = opened.size();
    docked.setVisible(true);
    root.frame();
    check(focus.isShowing() && opened.size() == opensBeforeRestore + 1 && opened.back() == second,
          "restoring the parent reattaches only the previously released source");

    engine::Transport driver;
    driver.play(true);
    driver.locate(4096);
    doc->mailbox().publish(driver);
    focus.scope.setToggleState(true, juce::dontSendNotification);
    root.frame();
    std::array<float, 1024> pcm{};
    pcm.fill(.25f);
    tap->write(pcm.data(), pcm.data(), static_cast<std::int32_t>(pcm.size()), 0, true, 1);
    secondTap->write(pcm.data(), pcm.data(), static_cast<std::int32_t>(pcm.size()), 0, true, 1);
    broker->pump();
    root.frame();
    const auto retained = waveform.read();
    check(root.playing() && retained && detail.read() == retained && !retained->scopeLeft.empty() &&
              MixFocusTestAccess::aligned(focus),
          "the detailed analyzer and waveform share a publication and Focus aligns the two live sources");
    check(MixFocusTestAccess::sumStrokeAt(focus, .5f),
          "the Focus Scope visibly overlays the signed sum of two in-phase 0.25 member traces");
    check(MixFocusTestAccess::flatScopePaths(focus),
          "flat Focus member and sum traces contribute just one stroke segment per lane");
    auto floating = std::make_unique<juce::Component>();
    peer.addChildComponent(floating.get());
    floating->setBounds(peer.getLocalBounds());
    const auto beforeReparent = closed.size();
    floating->addChildComponent(&root);
    check(!focus.isShowing() && closed.size() == beforeReparent + 1 && closed.back() == second &&
              waveform.read() == retained && detail.read() == retained,
          "reparenting Focus Scope into a hidden owner drops only its own demand immediately");

    floating->setVisible(true);
    root.frame();
    const auto beforeOwnerClose = closed.size();
    docked.setVisible(false);
    check(focus.isShowing() && closed.size() == beforeOwnerClose,
          "hiding the former docked parent has no effect after reparenting");
    docked.setVisible(true);
    floating.reset();
    check(root.getParentComponent() == nullptr && closed.size() == beforeOwnerClose + 1 &&
              closed.back() == second && waveform.read() == retained && detail.read() == retained,
          "closing a visible hosting component detaches Focus Scope without another frame");

    docked.addAndMakeVisible(root);
    root.frame();
    check(root.submitEdit("transport.setLoop", {{"on", true}, {"start", 0}, {"end", 3840}}, "Loop"),
          "Focus fixture enables the authored Arrangement Loop");
    // Only the second channel observes a repeated heard interval before this frame.
    // The loop does not advance the audio engine's transport control revision.
    pcm.fill(.125f);
    secondTap->write(pcm.data(), pcm.data(), static_cast<std::int32_t>(pcm.size()), 0, true, 1);
    broker->pump();
    root.frame();
    check(MixFocusTestAccess::independent(focus), "loop passes with repeated stamps retain individual traces without a combined sum");
    focus.scope.setToggleState(false, juce::dontSendNotification);
    root.frame();
    broker->pump();
    root.frame();
    check(MixFocusTestAccess::spectraWithoutMask(focus), "Together retains member spectra while withholding a cross-loop mask");
    focus.select(second);
    focus.learnMatch.onClick();
    check(!focus.learnMatch.isEnabled() && !MixFocusTestAccess::match(focus),
          "Match refuses ambiguous loop data even with two selected spectra");
    check(root.submitEdit("transport.setLoop", {{"on", false}, {"start", 0}, {"end", 3840}}, "Loop Off"),
          "Focus fixture disables the authored Arrangement Loop");
    focus.scope.setToggleState(true, juce::dontSendNotification);
    root.frame();
    check(MixFocusTestAccess::waitingForAlignment(focus) && !MixFocusTestAccess::aligned(focus),
          "turning the loop off while playing cannot combine retained loop windows");
    driver.play(false);
    doc->mailbox().publish(driver);
    root.frame();
    driver.play(true);
    doc->mailbox().publish(driver);
    root.frame();
    check(MixFocusTestAccess::waitingForAlignment(focus) && !MixFocusTestAccess::aligned(focus),
          "restarting playback still waits for complete windows beyond the new tap boundaries");
    const auto revision = driver.revision();
    tap->write(pcm.data(), pcm.data(), static_cast<std::int32_t>(pcm.size()), 2048, true, revision);
    secondTap->write(pcm.data(), pcm.data(), static_cast<std::int32_t>(pcm.size()), 2048, true, revision);
    broker->pump();
    root.frame();
    check(MixFocusTestAccess::aligned(focus) && !MixFocusTestAccess::waitingForAlignment(focus),
          "fresh aligned member data restores combined Focus after a stop and start");
    const auto beforeSum = broker->stats();
    tap->write(pcm.data(), pcm.data(), static_cast<std::int32_t>(pcm.size()), 3072, true, revision);
    pcm.fill(-.125f);
    secondTap->write(pcm.data(), pcm.data(), static_cast<std::int32_t>(pcm.size()), 3072, true, revision);
    broker->pump();
    root.frame();
    const auto afterSum = broker->stats();
    check(MixFocusTestAccess::sumStrokeAt(focus, 0.f) && afterSum.taps == beforeSum.taps &&
              afterSum.consumers == beforeSum.consumers,
          "an opposed pair collapses the visible sum to zero immediately without acquiring another analysis reader");
    check(MixFocusTestAccess::flatScopePaths(focus),
          "exact cancellation keeps one centerline segment and does not expand the member paths");
    const auto writeSum = [&](float leftA, float rightA, float leftB, float rightB) {
        std::array<float, 1024> left{}, right{};
        const auto stamp = driver.position();
        const auto frames = static_cast<std::int32_t>(left.size());
        driver.advance(frames);
        doc->mailbox().publish(driver);
        left.fill(leftA); right.fill(rightA);
        tap->write(left.data(), right.data(), frames, stamp, true, driver.revision());
        left.fill(leftB); right.fill(rightB);
        secondTap->write(left.data(), right.data(), frames, stamp, true, driver.revision());
        broker->pump();
        root.frame();
    };
    writeSum(.5f, -.5f, .5f, -.5f);
    check(MixFocusTestAccess::aligned(focus) && !MixFocusTestAccess::sumOverRangeInk(focus),
          "an exact +1 left and -1 right sum leaves the Scope title normal");
    writeSum(.8f, 0.f, .8f, 0.f);
    check(MixFocusTestAccess::aligned(focus) && MixFocusTestAccess::sumOverRangeInk(focus),
          "the Scope title visibly marks an above-unity positive sum on the left channel alone");
    writeSum(0.f, -.8f, 0.f, -.8f);
    check(MixFocusTestAccess::aligned(focus) && MixFocusTestAccess::sumOverRangeInk(focus),
          "the Scope title visibly marks an above-unity negative sum on the right channel alone");
    const auto wideFocus = focus.getBounds();
    focus.setBounds(wideFocus.withWidth(3 * look::mixFocusSourceWidth));
    focus.frame(SnapshotReader(root.current()));
    check(MixFocusTestAccess::compactScope(focus) && MixFocusTestAccess::sumOverRangeInk(focus),
          "the over-range title remains visible at a width without a separate numeric sum readout");
    focus.setBounds(wideFocus);
    writeSum(.8f, -.8f, -.8f, .8f);
    check(MixFocusTestAccess::aligned(focus) && !MixFocusTestAccess::sumOverRangeInk(focus) &&
              broker->stats().consumers == afterSum.consumers && broker->stats().taps == afterSum.taps,
          "the over-range title clears on cancellation without another analysis consumer or tap");

    const auto checkScopeDisplays = [&] {
        juce::ComboBox *layout = nullptr;
        for (auto *child : focus.getChildren()) {
            auto *combo = dynamic_cast<juce::ComboBox *>(child);
            if (combo && combo->getTitle() == "Focus Scope display") layout = combo;
        }
        check(layout && layout->isShowing(), "Focus Scope offers its own display selector");
        if (!layout || !layout->isShowing()) return;
        check(layout->getNumItems() == 5 && layout->getText() == "Overlay" &&
                  layout->getItemText(1) == "Layers" && layout->getItemText(2) == "Stack" &&
                  layout->getItemText(3) == "Sum" && layout->getItemText(4) == "Difference",
              "the scope selector keeps Overlay as the default and offers Layers, Stack, Sum and Difference");
        if (layout->getNumItems() != 5) return;

        writeSum(.25f, .25f, -.125f, -.125f);
        const auto before = broker->stats();
        const auto openedBefore = opened.size(), closedBefore = closed.size();
        const auto overlay = MixFocusTestAccess::scopePicture(focus);
        const auto plotCentre = static_cast<float>(overlay.raster.getHeight()) * .5f;
        const auto sharedRow = [plotCentre](float level) { return plotCentre * (1.f - level); };
        const auto traceAt = [](const juce::Image &image, float row) {
            constexpr std::array positions{.2f, .4f, .6f, .8f};
            const auto y = juce::roundToInt(row);
            return std::ranges::all_of(positions, [&](float position) {
                const auto x = juce::roundToInt(position * static_cast<float>(image.getWidth()));
                return image.getPixelAt(x, y - 1) != look::frame || image.getPixelAt(x, y) != look::frame ||
                       image.getPixelAt(x, y + 1) != look::frame;
            });
        };
        const auto select = [&](int index) {
            layout->setSelectedItemIndex(index, juce::sendNotificationSync);
            return MixFocusTestAccess::scopePicture(focus);
        };
        check(traceAt(overlay.raster, sharedRow(.25f)) && traceAt(overlay.raster, sharedRow(-.125f)) &&
                  traceAt(overlay.raster, sharedRow(.125f)),
              "the original Overlay contains both members and their signed sum");
        const auto layers = select(1);
        check(traceAt(layers.raster, sharedRow(.25f)) && traceAt(layers.raster, sharedRow(-.125f)) &&
                  !traceAt(layers.raster, sharedRow(.125f)),
              "Layers shows both members around one baseline without a sum trace");
        const auto stack = select(2);
        check(traceAt(stack.raster, sharedRow(.25f) * .5f) &&
                  traceAt(stack.raster, plotCentre + sharedRow(-.125f) * .5f) &&
                  !traceAt(stack.raster, sharedRow(.25f)) && !traceAt(stack.raster, sharedRow(-.125f)),
              "Stack moves each member to its own baseline");
        const auto sum = select(3);
        check(traceAt(sum.raster, sharedRow(.125f)) && !traceAt(sum.raster, sharedRow(.25f)) &&
                  !traceAt(sum.raster, sharedRow(-.125f)),
              "Sum displays the signed combination without the member traces");
        const auto restored = select(0);
        check(adi::uitest::samePicturePixels(overlay, restored),
              "returning to Overlay restores the existing plot exactly");
        root.frame();
        const auto after = broker->stats();
        check(after.consumers == before.consumers && after.taps == before.taps &&
                  after.publications == before.publications && after.cursorReads == before.cursorReads &&
                  opened.size() == openedBefore && closed.size() == closedBefore,
              "scope display choices do not acquire, release or reprocess audio sources");
    };
    checkScopeDisplays();
    focus.scope.setToggleState(false, juce::dontSendNotification);
    root.frame();
    const auto checkAlignMeasure = [&] {
        juce::TextButton *measure = nullptr;
        juce::Label *readout = nullptr;
        for (auto *child : focus.getChildren()) {
            if (child->getTitle() == "Measure Focus alignment") measure = dynamic_cast<juce::TextButton *>(child);
            if (child->getTitle() == "Focus alignment preview") readout = dynamic_cast<juce::Label *>(child);
        }
        check(measure && measure->isShowing() && readout, "Together exposes the Focus Align preview");
        if (!measure || !readout) return;
        focus.select(second);
        constexpr int lag = 137;
        std::array<float, 1024> main{}, recipient{};
        const auto sampleAt = [](std::int64_t stamp) {
            auto bits = static_cast<std::uint32_t>(stamp);
            bits = (bits ^ (bits >> 16u)) * 0x7feb352du;
            bits = (bits ^ (bits >> 15u)) * 0x846ca68bu;
            return static_cast<float>((bits ^ (bits >> 16u)) & 0xffffu) / 65536.f - .5f;
        };
        for (int block = 0; block < 6; ++block) {
            const auto stamp = driver.position();
            for (std::size_t i = 0; i < main.size(); ++i) {
                const auto at = stamp + static_cast<std::int64_t>(i);
                main[i] = sampleAt(at);
                recipient[i] = sampleAt(at - lag);
            }
            const auto frames = static_cast<std::int32_t>(main.size());
            driver.advance(frames);
            doc->mailbox().publish(driver);
            tap->write(main.data(), main.data(), frames, stamp, true, driver.revision());
            secondTap->write(recipient.data(), recipient.data(), frames, stamp, true, driver.revision());
            broker->pump();
            root.frame();
        }
        const auto before = broker->stats();
        const auto beforeMeasure = root.current();
        const auto undoBefore = root.undoTitle(false);
        check(measure->isEnabled(), "coincident current audio can Measure without a mailbox Locate");
        measure->onClick();
        check(readout->getText().contains("Measuring"), "preview waits for a worker result");
        broker->pump();
        root.frame();
        check(readout->isShowing() && readout->getText().contains("137 samples") &&
                  readout->getText().contains("2.854 ms") && readout->getText().contains("+1.00"),
              "preview reports the recipient delay and measured candidate correlation");
        const auto measured = broker->stats();
        check(beforeMeasure == root.current() && undoBefore == root.undoTitle(false) &&
                  measured.consumers == before.consumers && measured.taps == before.taps &&
                  measured.transforms == before.transforms,
              "Measure adds no source, FFT, authored timing or undo entry");
        broker->pump();
        root.frame();
        check(broker->stats().workerPasses == measured.workerPasses,
              "holding an Align preview requests no further worker work");
        check(doc->mailbox().post(TransportMailbox::Command::Locate, driver.position()), "queue an Align Locate");
        root.frame();
        check(!readout->isShowing() && !measure->isEnabled(), "a pending same-position Locate clears the preview");
        doc->mailbox().drain(driver);
        root.frame();
        check(!measure->isEnabled(), "an applied Locate still waits for new Align audio");
    };
    checkAlignMeasure();
    std::array<float, 1024> reference{}, target{};
    const auto matchedSignal = [&](float targetGain) {
        for (std::size_t i = 0; i < reference.size(); ++i) {
            reference[i] = .5f * static_cast<float>(std::sin(
                2. * std::numbers::pi * 16. * static_cast<double>(i) / static_cast<double>(reference.size())));
            target[i] = reference[i] * targetGain;
        }
    };
    const auto writeMatch = [&](float gain, bool both = true) {
        matchedSignal(gain);
        const auto stamp = driver.position();
        const auto frames = static_cast<std::int32_t>(reference.size());
        driver.advance(frames);
        doc->mailbox().publish(driver);
        tap->write(reference.data(), reference.data(), frames, stamp, true, driver.revision());
        if (both)
            secondTap->write(target.data(), target.data(), frames, stamp, true, driver.revision());
        broker->pump();
        root.frame();
    };
    writeMatch(.5f);
    const auto beforeLearn = root.current();
    const auto processCounts = broker->stats();
    focus.learnMatch.onClick();
    const auto held = MixFocusTestAccess::match(focus);
    check(held && held->main == first && held->target == second && held->deltaDb[16] &&
              std::abs(*held->deltaDb[16] - 6.0206f) < .02f && MixFocusTestAccess::hasMatchPath(focus) &&
              MixFocusTestAccess::matchCaption(focus).contains("Focus 72 toward Focus 71"),
          "explicit Learn shows measured target toward Main after coincident calibrated playback");
    const auto afterLearn = broker->stats();
    check(root.current() == beforeLearn && afterLearn.consumers == processCounts.consumers &&
              afterLearn.taps == processCounts.taps && afterLearn.transforms == processCounts.transforms,
          "Learn writes no project edit and adds no analysis source or transform");
    writeMatch(1.f);
    const auto stillHeld = MixFocusTestAccess::match(focus);
    check(held && stillHeld && stillHeld->deltaDb == held->deltaDb,
          "later spectra do not change the learned Match");
    focus.learnMatch.onClick();
    const auto relearned = MixFocusTestAccess::match(focus);
    check(relearned && relearned->deltaDb[16] == 0.f,
          "only another explicit Learn updates the held curve to a new aligned window");
    focus.clearMatch.onClick();
    check(!MixFocusTestAccess::match(focus) && !MixFocusTestAccess::hasMatchPath(focus),
          "Clear Match only removes the held preview");

    const auto positionBeforeLocate = driver.position();
    check(doc->mailbox().post(TransportMailbox::Command::Locate, positionBeforeLocate),
          "queue a same-position Locate while the old spectra are visible");
    focus.learnMatch.onClick();
    check(!MixFocusTestAccess::match(focus),
          "Learn rejects a queued Locate even before an intervening UI frame");
    if (MixFocusTestAccess::match(focus))
        focus.clearMatch.onClick();
    root.frame();
    check(!focus.learnMatch.isEnabled(), "pending Locate disables the Learn control with no new spectrum");

    doc->mailbox().drain(driver);
    root.frame();
    focus.learnMatch.onClick();
    check(!focus.learnMatch.isEnabled() && !MixFocusTestAccess::match(focus),
          "acknowledging a same-position Locate still rejects the retained old spectra");
    if (MixFocusTestAccess::match(focus))
        focus.clearMatch.onClick();

    const auto locateRevision = driver.revision();
    check(doc->mailbox().post(TransportMailbox::Command::Stop) &&
              doc->mailbox().post(TransportMailbox::Command::Play),
          "queue a playback restart after acknowledging the Locate");
    doc->mailbox().drain(driver);
    check(driver.revision() > locateRevision, "playback restarts at a later capture epoch");
    const auto freshStamp = driver.position();
    writeMatch(2.f, false);
    focus.learnMatch.onClick();
    check(!focus.learnMatch.isEnabled() && !MixFocusTestAccess::match(focus),
          "reaching the new epoch on only one channel is insufficient for Match");
    secondTap->write(target.data(), target.data(), static_cast<std::int32_t>(target.size()), freshStamp, true,
                     driver.revision());
    broker->pump();
    root.frame();
    focus.learnMatch.onClick();
    const auto afterLocate = MixFocusTestAccess::match(focus);
    check(focus.learnMatch.isEnabled() && afterLocate && afterLocate->deltaDb[16] &&
              std::abs(*afterLocate->deltaDb[16] + 6.0206f) < .02f,
          "both channels can Learn from identical audible windows in a later epoch than the Locate");

    focus.select(first);
    check(!MixFocusTestAccess::match(focus) && !focus.learnMatch.isEnabled(),
          "choosing Main as the target drops the old proposal");
    focus.select(second);
    focus.learnMatch.onClick();
    secondTap = std::make_shared<engine::ScopeTap>(48000., 1.);
    broker->refreshSources();
    root.frame();
    check(!MixFocusTestAccess::match(focus) && !focus.learnMatch.isEnabled(),
          "a replacement tap with the same track ID discards its previous Match before new audio arrives");

    driver.play(false);
    doc->mailbox().publish(driver);
    focus.scope.setToggleState(false, juce::dontSendNotification);
    root.frame();
    waveform.reset();
    detail.reset();
    focus.setMembers({first, added, second});
    root.frame();
    check(focus.channelIds() == std::vector{first, added, second} && mix.stack.model().shownIndex(added),
          "manual membership is retained without writing a solo");
    focus.toggleMemberSolo(first);
    root.frame();
    check(!solo(first) && solo(second) && !solo(added) &&
              focus.channelIds() == std::vector{added, second} && focus.mainTrack() == second &&
              !mix.stack.model().shownIndex(first),
          "explicit unsolo removes its row and promotes the first remaining solo over a manual member");
    check(mix.channels.focusMemberLabel(first).isEmpty() && mix.channels.focusMemberLabel(second) == "B" &&
              mix.channels.focusMemberLabel(added) == "A",
          "the browser updates labels to the remaining membership order");
    check(doc->ops().undo().ok && doc->synchronise() && solo(first) && solo(second),
          "Focus unsolo is one ordinary authored undo step");

    check(toggleMix() && !root.state().mixView && solo(first) && solo(second),
          "closing Focus returns to Arrangement without modifying solos");
    check(root.command(AppCommands::ShowChannelEq) && mix.tab() == MixViewPanel::Tab::Stack,
          "the explicit Channel EQ command still opens Stack with two solos");
    docked.removeChildComponent(&root);
    peer.removeFromDesktop();
    rootOwner.reset();
}

void focusOneBarScope(AppCommands &commands, const std::filesystem::path &directory) {
    constexpr int rate = 48000, chunk = 1024;
    constexpr std::int64_t bar = 96000, early = 12000, late = 84000, pulseLength = 600;
    constexpr std::int64_t first = 181, second = 182;
    std::string error;
    auto doc = ProjectDocument::open(directory / "focus-one-bar.adi", true, device::withBuiltinDevices(), error);
    check(doc != nullptr, "one-bar Focus opens its own project fixture");
    if (!doc) return;
    doc->prepare(rate, chunk);
    auto root = std::make_unique<AdiRootComponent>(doc->view(), doc->ops(), doc->mailbox(), commands,
                                                   doc->views(), doc->sessionState(), "focus-one-bar");
    root->afterEdit = [&] { doc->synchronise(); };
    const auto addTrack = [&](std::int64_t id) {
        return root->submitEdit("track.create", {{"id", id}, {"kind", "audio"}, {"name", "One-bar pulse"}},
                                "Create Scope channel");
    };
    const bool created = addTrack(first) && addTrack(second);
    check(created, "one-bar Scope selects two authored audio tracks");
    if (!created) return;

    std::array<int, 2> opens{};
    auto a = std::make_shared<engine::ScopeTap>(rate, .25);
    auto b = std::make_shared<engine::ScopeTap>(rate, .25);
    auto broker = std::make_shared<SharedAnalysis>(
        [&](engine::ScopeSource source, double) -> std::shared_ptr<const engine::ScopeTap> {
            if (source.point != engine::ScopePoint::PostFader || source.device != 0) return {};
            if (source.track == first) { ++opens[0]; return a; }
            if (source.track == second) { ++opens[1]; return b; }
            return {};
        }, [](engine::ScopeSource) {}, SharedAnalysis::Mode::Manual);
    root->sharedAnalysis = broker;

    // Only the actual Focus component is shown. The root supplies the project
    // and transport without creating Info, Together or dock demand.
    UiPeerRoot peer;
    MixFocus focus(*root);
    peer.setSize(940, 460); peer.setVisible(true); peer.addToDesktop(0);
    peer.addAndMakeVisible(focus); focus.setBounds(peer.getLocalBounds());
    focus.setMembers({first, second});
    focus.scope.setToggleState(true, juce::dontSendNotification);
    engine::Transport driver;
    driver.play(true);
    const auto frame = [&] {
        doc->mailbox().publish(driver);
        root->frame();
        focus.frame(SnapshotReader(root->current()));
    };
    frame();
    const SnapshotReader initial(root->current());
    const auto &meters = initial.meters();
    check(root->playing() && root->bpm() == 120. && doc->mailbox().sampleRate() == rate &&
              (meters.empty() || (meters[0].numerator == 4 && meters[0].denominator == 4)) &&
              a->capacity() == 12000 && b->capacity() == 12000 && broker->stats().taps == 2,
          "one-bar Scope runs at 48 kHz, 120 BPM and 4/4 on two quarter-second rings");

    const auto writeUntil = [&](std::int64_t target) {
        std::array<float, chunk> pcmA{}, pcmB{}, rightA{}, rightB{};
        while (driver.position() < target) {
            const auto from = driver.position();
            const auto count = static_cast<std::int32_t>(std::min<std::int64_t>(chunk, target - from));
            for (std::int32_t i = 0; i < count; ++i) {
                const auto sample = (from + i) % bar;
                pcmA[static_cast<std::size_t>(i)] = sample >= early && sample < early + pulseLength ? .75f : 0.f;
                pcmB[static_cast<std::size_t>(i)] = sample >= late && sample < late + pulseLength ? -.625f : 0.f;
                const auto index = static_cast<std::size_t>(i);
                const bool firstBar = from + i < bar;
                rightA[index] = firstBar ? pcmA[index] : -pcmA[index];
                rightB[index] = firstBar ? pcmB[index] : 0.f;
            }
            a->write(pcmA.data(), rightA.data(), count, from, true, driver.revision());
            b->write(pcmB.data(), rightB.data(), count, from, true, driver.revision());
            driver.advance(count);
            broker->pump(); frame();
        }
    };
    writeUntil(late);
    check(!MixFocusTestAccess::sumPulseAt(focus, .875f, -.625f),
          "no late pulse is drawn before that audio arrives");
    writeUntil(bar - 1);
    check(!MixFocusTestAccess::scopeInterval(focus, 0, static_cast<std::size_t>(bar), rate),
          "Scope has no complete bar before its final heard sample");
    writeUntil(bar);
    check(a->oldest() > early + pulseLength && b->oldest() > early + pulseLength,
          "the early pulse has already fallen out of both physical rings");
    check(MixFocusTestAccess::scopeInterval(focus, 0, static_cast<std::size_t>(bar), rate),
          "Focus presents the complete two-second musical bar from its original heard position");
    check(MixFocusTestAccess::sumPulseAt(focus, .125f, .75f) &&
              MixFocusTestAccess::sumPulseAt(focus, .875f, -.625f),
          "the visible summed path keeps its early and late pulses at their positions in the same bar");
    const auto active = broker->stats();
    check(opens == std::array{1, 1} && active.taps == 2 && active.transforms == 0 && active.spectralPlans == 0,
          "one-bar Scope keeps the same two physical taps without a spectrum transform or plan");
    check(active.consumers == 1,
          "a complete bar replaces the two short previews with one grouped reader");
    juce::ComboBox *channels = nullptr;
    for (auto *component : focus.getChildren())
        if (component->getTitle() == "Focus Scope channels") channels = dynamic_cast<juce::ComboBox *>(component);
    check(channels && channels->isVisible() && channels->getText() == "Stereo",
          "Focus Scope defaults to Stereo and exposes the channel selector");
    if (channels) {
        const auto select = [&](FocusScopeChannels view) {
            channels->setSelectedId(static_cast<int>(view), juce::sendNotificationSync);
            frame();
        };
        select(FocusScopeChannels::Mid);
        check(!MixFocusTestAccess::sumPulseAt(focus, .125f, .75f),
              "selecting Mid never presents the old stereo bins as Mid");
        writeUntil(2 * bar);
        check(MixFocusTestAccess::scopeInterval(focus, bar, static_cast<std::size_t>(bar), rate) &&
                  !MixFocusTestAccess::sumPulseAt(focus, .125f, .75f) &&
                  MixFocusTestAccess::sumPulseAt(focus, .875f, -.3125f),
              "Mid cancels opposed stereo and halves a single-sided pulse in the displayed bar");
        check(opens == std::array{1, 1} && broker->stats().taps == 2 && broker->stats().consumers == 1 &&
                  broker->stats().transforms == 0,
              "changing Focus Scope projection keeps the existing grouped reader and taps");
        select(FocusScopeChannels::Left);
        check(channels->getText() == "Left" && MixFocusTestAccess::sumPulseAt(focus, .125f, .75f) &&
                  MixFocusTestAccess::sumPulseAt(focus, .875f, -.625f),
              "Left immediately reuses the captured left lane");
        select(FocusScopeChannels::Right);
        check(channels->getText() == "Right" && MixFocusTestAccess::sumPulseAt(focus, .125f, -.75f) &&
                  !MixFocusTestAccess::sumPulseAt(focus, .875f, -.625f),
              "Right immediately shows only the signed right lane");
        select(FocusScopeChannels::Side);
        check(channels->getText() == "Side" && !MixFocusTestAccess::sumPulseAt(focus, .125f, -.75f),
              "switching to Side clears the old Right lane");
        writeUntil(3 * bar);
        check(MixFocusTestAccess::scopeInterval(focus, 2 * bar, static_cast<std::size_t>(bar), rate) &&
                  MixFocusTestAccess::sumPulseAt(focus, .125f, .75f) &&
                  MixFocusTestAccess::sumPulseAt(focus, .875f, -.3125f),
              "Side preserves opposed signals and halves a single-sided pulse");
        select(FocusScopeChannels::Stereo);
        check(MixFocusTestAccess::sumPulseAt(focus, .125f, .75f) &&
                  MixFocusTestAccess::sumPulseAt(focus, .875f, -.625f),
              "returning to Stereo reuses the same completed bar's stereo columns immediately");
    }
    juce::ComboBox *display = nullptr;
    for (auto *component : focus.getChildren())
        if (component->getTitle() == "Focus Scope display") display = dynamic_cast<juce::ComboBox *>(component);
    check(display != nullptr, "Focus Scope makes its display selector available");
    if (display && channels) {
        focus.select(second);
        display->setSelectedId(5, juce::sendNotificationSync);
        frame();
        check(display->getText() == "Difference" && !MixFocusTestAccess::hasDifference(focus),
              "choosing Difference never shows the previous combined projection");
        writeUntil(4 * bar);
        check(MixFocusTestAccess::hasDifference(focus) &&
                  MixFocusTestAccess::differencePulseAt(focus, .125f, .75f) &&
                  MixFocusTestAccess::differencePulseAt(focus, .875f, .625f),
              "Stereo Difference subtracts the selected channel from starred Main");
        channels->setSelectedId(static_cast<int>(FocusScopeChannels::Right), juce::sendNotificationSync);
        frame();
        check(MixFocusTestAccess::hasDifference(focus) &&
                  MixFocusTestAccess::differencePulseAt(focus, .125f, -.75f) &&
                  !MixFocusTestAccess::differencePulseAt(focus, .875f, .625f),
              "Right Difference reuses the existing Stereo pair projection immediately");
        channels->setSelectedId(static_cast<int>(FocusScopeChannels::Mid), juce::sendNotificationSync);
        frame();
        check(!MixFocusTestAccess::hasDifference(focus),
              "switching to Mid clears the previous Stereo Difference");
        writeUntil(5 * bar);
        check(MixFocusTestAccess::hasDifference(focus) &&
                  !MixFocusTestAccess::differencePulseAt(focus, .125f, .75f) &&
                  MixFocusTestAccess::differencePulseAt(focus, .875f, .3125f),
              "Mid Difference cancels opposed stereo and halves a single-sided difference");
        channels->setSelectedId(static_cast<int>(FocusScopeChannels::Side), juce::sendNotificationSync);
        frame();
        check(!MixFocusTestAccess::hasDifference(focus),
              "Side Difference never reuses the old Mid Difference");
        writeUntil(6 * bar);
        check(MixFocusTestAccess::hasDifference(focus) &&
                  MixFocusTestAccess::differencePulseAt(focus, .125f, .75f) &&
                  MixFocusTestAccess::differencePulseAt(focus, .875f, .3125f),
              "Side Difference projects the signed paired differences before extrema");
        check(opens == std::array{1, 1} && broker->stats().taps == 2 && broker->stats().consumers == 1 &&
                  broker->stats().transforms == 0,
              "Difference adds no tap, consumer or spectrum transform");

        check(doc->mailbox().post(TransportMailbox::Command::Locate, 0), "queue a Difference Locate");
        frame();
        check(!MixFocusTestAccess::hasDifference(focus), "a pending Locate immediately clears the old Difference");
        doc->mailbox().drain(driver);
        frame();
        check(!MixFocusTestAccess::hasDifference(focus),
              "a settled Locate keeps the old Difference hidden until fresh paired audio");
        focus.select(first);
        frame();
        check(!MixFocusTestAccess::hasDifference(focus),
              "selecting starred Main as the other channel leaves Difference without a pair");
    }
    const auto visibleBounds = focus.getBounds();
    focus.setTopLeftPosition(peer.getWidth(), 0);
    check(broker->stats().taps == 0 && broker->stats().consumers == 0,
          "moving the plot fully out of view releases Focus demand before its next frame");
    focus.setBounds(visibleBounds);
    focus.frame(initial);
    check(broker->stats().taps == 2 && !MixFocusTestAccess::aligned(focus),
          "a restored plot waits for new source data instead of reusing the old bar");
    peer.setVisible(false);
    check(broker->stats().taps == 0 && broker->stats().consumers == 0,
          "hiding the one-bar Scope releases its sources immediately");
    peer.removeChildComponent(&focus); peer.removeFromDesktop();
}
void cleanCompressor(AppCommands &commands, const std::filesystem::path &directory) {
    std::string error;
    auto doc =
        ProjectDocument::open(directory / "clean.adi", true, device::withBuiltinDevices(), error);
    check(doc && doc->addMixFunction(mix::Function::Compressor, 2),
          "real Clean device can be added for component presentation");
    if (!doc)
        return;
    doc->tick(0);
    const auto publication = doc->parameterFeed().current();
    const auto found = std::find_if(publication->begin(), publication->end(), [](const auto &p) {
        return p.uid == "org.adidaw.mix.compressor";
    });
    if (found == publication->end()) {
        check(false, "Clean publication exists");
        return;
    }
    doc->setParameterDemand(doc.get(), {found->id});
    doc->tick(1);
    AdiRootComponent root(doc->view(), doc->ops(), doc->mailbox(), commands, doc->views(),
                          doc->sessionState(), "clean");
    root.setSize(1200, 800);
    UiPeerRoot peer;
    peer.setSize(1200, 800);
    peer.setVisible(true);
    peer.addToDesktop(0);
    peer.addAndMakeVisible(root);
    check(dynamic_cast<UiFakePeer *>(peer.getPeer()), "Clean presentation uses the software peer");
    auto &strip = root.devices;
    strip.setVisible(true);
    strip.publication = [&] { return doc->parameterFeed().current(); };
    strip.functionAvailability = device::mixFunctionAvailability;
    strip.addFunction = [&](mix::Function kind, std::int64_t track) { return doc->addMixFunction(kind, track); };
    strip.targetCurrent = [&](std::int64_t id, const engine::DeviceRuntimeToken &runtime,
                              std::uint64_t stamp) {
        const auto p = doc->parameterFeed().current();
        return std::any_of(p->begin(), p->end(), [&](const auto &d) {
            return d.id == id && d.runtime.sameInstance(runtime) && d.instance == stamp &&
                   d.available;
        });
    };
    strip.guardedGesture = [&](std::int64_t id, const std::string &parameter,
                               engine::ParamEventKind kind, double value,
                               const engine::DeviceRuntimeToken &runtime, std::uint64_t stamp) {
        return doc->parameterGestureExpected(id, parameter, kind, value, runtime, stamp);
    };
    strip.frame(2);
    check(strip.panels.size() == 1, "Clean creates one native device tab");
    if (strip.panels.size() == 1) {
        auto *panel = strip.panels.front().get();
        check(panel->controls.size() == 8 && panel->title.getTooltip().contains("0 samples"),
              "Clean shows eight live controls and real zero latency");
        check(panel->parameterPages.size() == 1 && panel->parameterPages.front().name == "Clean",
              "Clean has no unavailable style pages");
        if (!panel->controls.empty()) {
            auto *control = panel->controls.front().get();
            control->slider.onDragStart();
            control->slider.setValue(.25, juce::sendNotificationSync);
            control->slider.onDragEnd();
            check(doc->session().instanceFor(found->id)->getParam("0").normalized == .25,
                  "visible Clean control reaches the actual core parameter");
            const bool undo = doc->ops().undo().ok && doc->synchronise();
            doc->tick(2);
            check(undo && std::abs(doc->session().instanceFor(found->id)->getParam("0").normalized -
                                   .7) < 1e-6,
                  "visible Clean drag has one common undo");
        }
    }
    root.arrangement.geometry.selection.track = 2;
    root.command(AppCommands::ToggleMixView);
    root.chooseMixViewTab(1);
    root.mark(DirtySet::All);
    root.frame();
    auto &stack = root.mixViewPanel.stack;
    stack.frameAt(root.reader(), 0);
    const auto item = stack.model().chainRef(2, found->id);
    if (!item.row.valid() || stack.rows().empty())
        std::printf("STACK diagnostic: panel=%d stack=%d rows=%zu scope=%d item=%d/%d area=%d,%d error=%s\n",
          root.mixViewPanel.isVisible(), stack.isVisible(), stack.rows().size(),
          static_cast<int>(stack.model().windowView().channelScope), item.row.valid(),static_cast<int>(item.place),
          stack.getWidth(),stack.getHeight(),root.error().c_str());
    check(stack.model().windowView().channelScope == stack::ChannelScope::AllChannels &&
              item.row.valid() && item.place == stack::ItemPlace::Chain && !stack.rows().empty(),
          "actual Clean device appears in the default All channels Stack row");
    const auto bounds = stack.getBounds();
    bool fits = true;
    for (int width : {320, 480, 816, 1200}) {
        stack.setSize(width, 650);
        fits = fits && !stack.addFunction.getBounds().intersects(stack.search.getBounds()) &&
            stack.getLocalBounds().contains(stack.addFunction.getBounds()) &&
            stack.getLocalBounds().contains(stack.search.getBounds());
    }
    stack.setBounds(bounds);
    check(fits, "Mix View add and search controls stay distinct and inside narrow/wide headers");
    strip.frame(3);
    auto add = menuItems(stack.functionMenu());
    check(stack.addFunction.isVisible() && stack.addFunction.isEnabled() && add.size() == 5 &&
              add[1].ticked && !add[1].enabled && add[0].enabled,
          "Mix View offers a selected-channel add control with present and unavailable capabilities explicit");
    if (add.size() == 5) {
        add[0].action(); doc->tick(3);
        const auto after = doc->parameterFeed().current();
        const auto eq = std::find_if(after->begin(), after->end(), [](const auto &d) { return d.uid == "org.adidaw.mix.eq" && d.track == 2; });
        check(eq != after->end() && !menuItems(strip.functionMenu())[0].ticked && root.arrangement.geometry.selection.track == 2,
              "Mix View adds to its selected channel without selecting or altering the device-strip channel");
        const bool undo = doc->ops().undo().ok && doc->synchronise(); doc->tick(4);
        const auto remaining = doc->parameterFeed().current();
        const bool eqGone = eq != after->end() && std::none_of(remaining->begin(), remaining->end(),
            [&](const auto &d) { return d.id == eq->id; });
        const bool cleanKept = std::any_of(remaining->begin(), remaining->end(),
            [&](const auto &d) { return d.id == found->id && d.uid == "org.adidaw.mix.compressor"; });
        check(undo && eqGone && cleanKept, "one Undo removes only the function added in Mix View");
        root.frame(); stack.frameAt(root.reader(), 1);
        auto expired = menuItems(stack.functionMenu());
        const auto count = doc->store().db().execAndGet("SELECT count(*) FROM devices").getInt();
        stack.cancelInteractions();
        if (expired.size() == 5) expired[0].action();
        check(doc->store().db().execAndGet("SELECT count(*) FROM devices").getInt() == count,
              "cancelled Mix View add menu cannot mutate a later publication");
        auto oldSelection = menuItems(stack.functionMenu());
        root.arrangement.geometry.selection.track = 3; stack.frameAt(root.reader(), 2);
        root.arrangement.geometry.selection.track = 2; stack.frameAt(root.reader(), 3);
        if (oldSelection.size() == 5) oldSelection[0].action();
        check(doc->store().db().execAndGet("SELECT count(*) FROM devices").getInt() == count,
              "selection away and back cannot revive an old Mix View add menu");
    }
    // Drive the actual folded-dot handler using generated component events,
    // not desktop input. Its coordinates come from the presented model geometry.
    strip.submit = [&](const std::string &op, Payload p) { return doc->deviceAction(op, std::move(p)); };
    if (!strip.folded(found->id)) strip.fold(found->id);
    stack.model().setAutoZoom(false, 10);
    const auto refresh = [&] {
        doc->tick(10); root.mark(DirtySet::All); root.frame();
        const auto at = static_cast<stack::TimeMs>(juce::Time::getMillisecondCounterHiRes());
        // A lens transition may leave the row intersecting the viewport with its top clipped.
        // Scroll back before synthesising a click at the visible activator position.
        stack.model().scrollTo(0, at);
        stack.frameAt(root.reader(), at);
    };
    const auto clickDot = [&](std::int64_t id) {
        std::optional<stack::ItemGeo> target;
        stack.model().forEachVisibleRow([&](const auto &row) {
            stack.model().forEachChainItem(row.shown, [&](const auto &item) {
                if (item.ref.device == id && item.pill) target = item;
            });
        });
        if (!target) return false;
        const auto at = stack.listArea().getPosition().toFloat() +
            juce::Point<float>{static_cast<float>(target->x + target->width / 2),
                               static_cast<float>(target->top + look::border) +
                                   look::radiusSmall + static_cast<float>(look::gap) * .5f};
        if (!stack.listArea().contains(at.roundToInt())) return false;
        const auto now = juce::Time::getCurrentTime();
        const juce::MouseEvent event{juce::Desktop::getInstance().getMainMouseSource(), at,
            juce::ModifierKeys::leftButtonModifier, 1.f, 0.f, 0.f, 0.f, 0.f,
            &stack, &stack, now, at, now, 1, false};
        stack.mouseDown(event); stack.mouseUp(event);
        return true;
    };
    const auto enabledInStore = [&](std::int64_t id) -> std::optional<bool> {
        SQLite::Statement query(doc->store().db(), "SELECT enabled FROM devices WHERE id=?");
        query.bind(1, id);
        if (!query.executeStep()) return {};
        return query.getColumn(0).getInt() != 0;
    };
    const bool disabled = doc->deviceAction("device.setEnabled", {{"id", found->id}, {"enabled", false}});
    refresh();
    check(disabled && clickDot(found->id) && enabledInStore(found->id) == true,
          "actual folded native dot recovers a bypassed function without removing it");
    refresh();
    const bool removed = clickDot(found->id);
    check(removed && !enabledInStore(found->id), "actual active native dot removes its authored device row");
    const bool restored = doc->ops().undo().ok && doc->synchronise(); refresh();
    const auto restoredParams = doc->parameterFeed().current();
    check(restored && enabledInStore(found->id) == true &&
              std::any_of(restoredParams->begin(), restoredParams->end(), [&](const auto &d) {
                  return d.id == found->id && d.uid == "org.adidaw.mix.compressor";
              }), "one Undo recovers the active function removed by the actual folded dot");
    peer.removeChildComponent(&root);
    peer.removeFromDesktop();
}
void keyMenus(AppCommands& commands,const std::filesystem::path& directory){
    std::string error;auto document=ProjectDocument::open(directory/"key-menu.adi",true,{},error);
    if(!document){check(false,"key menu component document opens");return;}
    AdiRootComponent root(document->view(),document->ops(),document->mailbox(),commands,document->views(),document->sessionState(),"key-menu");
    root.setSize(960,620);auto publication=std::make_shared<ParameterPublication>();
    auto native=makePanel(44,false,"EQ");native.sidechainChannels=2;publication->push_back(native);
    auto& strip=root.devices;strip.setVisible(true);strip.publication=[&]{return publication;};
    strip.targetCurrent=[&](std::int64_t id,const engine::DeviceRuntimeToken&,std::uint64_t stamp){return id==44 && publication->front().instance==stamp;};
    bool invalidStoredRoute=false;
    // One named vector, filled by push_back and returned by plain if/return. The
    // same lists written as braced temporaries on the two arms of a conditional
    // expression are correct C++, but GCC's -Wdangling-pointer reports the
    // initializer_list array of the untaken arm (an error under -Werror).
    const auto keyChoice=[](std::optional<std::int64_t> from,const char* choiceTap,const char* name,bool current){
        DeviceKeyChoice choice;choice.source=from;choice.tap=choiceTap;choice.name=name;choice.current=current;return choice;
    };
    strip.keyChoices=[&](const DevicePanelData& d){
        std::vector<DeviceKeyChoice> choices;
        if(d.sidechainChannels<=0) {
            if(invalidStoredRoute)choices.push_back(keyChoice(std::nullopt,"post_mixer","None",false));
            return choices;
        }
        choices.push_back(keyChoice(std::nullopt,"post_mixer","None",false));
        choices.push_back(keyChoice(2,"pre_fx","Kick",true));
        choices.push_back(keyChoice(2,"post_fx","Kick",false));
        choices.push_back(keyChoice(2,"post_mixer","Kick",false));
        return choices;
    };
    int actions=0;std::optional<std::int64_t> source;std::string tap;
    strip.setKey=[&](const DevicePanelData& expected,const DeviceKeyChoice& choice){++actions;source=choice.source;tap=choice.tap;return expected.id==44 && expected.instance==publication->front().instance;};
    strip.frame(2);
    const auto submenu=[](const juce::PopupMenu& menu,const char* name){
        for(juce::PopupMenu::MenuItemIterator it(menu);it.next();)if(it.getItem().text==name && it.getItem().subMenu)return *it.getItem().subMenu;
        return juce::PopupMenu{};
    };
    auto menu=strip.panels.front()->commands();auto keys=submenu(menu,"Sidechain Input");auto kick=submenu(keys,"Kick");
    auto items=menuItems(kick);
    check(items.size()==3 && items[0].ticked && !items[1].ticked && !items[2].ticked,"declared key menu shows three actual taps with the current choice ticked");
    if(items.size()==3 && items[2].action)items[2].action();
    check(actions==1 && source==2 && tap=="post_mixer","sidechain menu delegates exact source, tap and runtime to one document command");
    items=menuItems(submenu(strip.panels.front()->commands(),"Sidechain Input"));
    if(!items.empty() && items[0].action)items[0].action();
    check(actions==2 && !source,"None delegates a route removal through the same command");
    auto old=menuItems(submenu(submenu(strip.panels.front()->commands(),"Sidechain Input"),"Kick"));
    publication=std::make_shared<ParameterPublication>(*publication);
    publication->front().instance++;
    if(!old.empty() && old[0].action)old[0].action();
    check(actions==2,"retained stale sidechain menu cannot reroute a replacement runtime");
    publication=std::make_shared<ParameterPublication>(*publication);
    publication->front().sidechainChannels=0;strip.frame(2);
    check(menuItems(submenu(strip.panels.front()->commands(),"Sidechain Input")).empty(),"internal-only devices expose no invented external key control");
    invalidStoredRoute=true;
    auto recovery=menuItems(submenu(strip.panels.front()->commands(),"Sidechain Input"));
    if(recovery.size()==1 && recovery[0].action)recovery[0].action();
    check(recovery.size()==1 && recovery[0].name=="None" && actions==3 && !source,"an unavailable stored key exposes only its recovery removal, never fabricated source choices");
}
} // namespace
int main() {
    juce::ScopedJuceInitialiser_GUI init;
    test::TempDirectory temporary("ui_mix_functions", "component");
    settings::AppSettings settings(appdata::App::Daw, temporary.path() / "settings.json");
    AppCommands commands(settings);
    audioDeviceStatus(commands, temporary.path());
    callbackKeys(commands, temporary.path());
    callbackAnalyzerRateChange(temporary.path());
    functions(commands, temporary.path());
    processingIssuePresentation(commands, temporary.path());
    parameterPages(commands, temporary.path());
    stackEqSpectrum(commands, temporary.path());
    stackLimiterReduction(commands, temporary.path());
    focusWorkspace(commands, temporary.path());
    focusOneBarScope(commands, temporary.path());
    cleanCompressor(commands, temporary.path());
    keyMenus(commands, temporary.path());
    std::printf("%d checks, %d failures (software-peer components)\n", checks, failures);
    return failures ? 1 : 0;
}
