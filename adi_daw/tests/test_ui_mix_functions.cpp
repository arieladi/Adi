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
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <map>
#include <memory>
#include <new>
#include <numbers>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace {
thread_local bool auditingStackDemandIds = false;
thread_local unsigned stackDemandIdAllocations = 0;
}
void *operator new(std::size_t bytes) {
    if (auditingStackDemandIds && bytes == sizeof(std::int64_t)) ++stackDemandIdAllocations;
    if (auto *memory = std::malloc(bytes ? bytes : 1)) return memory;
    throw std::bad_alloc();
}
void *operator new[](std::size_t bytes) { return operator new(bytes); }
void *operator new(std::size_t bytes, const std::nothrow_t &) noexcept {
    try { return operator new(bytes); }
    catch (...) { return nullptr; }
}
void *operator new[](std::size_t bytes, const std::nothrow_t &tag) noexcept { return operator new(bytes, tag); }
void operator delete(void *memory) noexcept { std::free(memory); }
void operator delete[](void *memory) noexcept { std::free(memory); }
void operator delete(void *memory, std::size_t) noexcept { std::free(memory); }
void operator delete[](void *memory, std::size_t) noexcept { std::free(memory); }
void operator delete(void *memory, const std::nothrow_t &) noexcept { std::free(memory); }
void operator delete[](void *memory, const std::nothrow_t &) noexcept { std::free(memory); }

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
    static bool foldedLayerAt(const MixFocus &focus, std::int64_t track, float position, float value) {
        const auto layer = std::ranges::find(focus.members_, track, &MixFocus::Member::id);
        return layer != focus.members_.end() && layer->scopePaths[1].isEmpty() &&
               scopePulseAt(focus.plot_, layer->scopePaths[0], position, value);
    }
    static bool hasScopeTrace(const MixFocus &focus) { return focus.scopeHasTrace_; }
    static bool hasDifference(const MixFocus &focus) { return focus.differenceRenderedRevision_.has_value(); }
    static auto scopeWaveformRange(const MixFocus &focus) { return focus.scopeRange_; }
    static auto scopeGrid(const MixFocus &focus) {
        std::vector<std::pair<int, bool>> lines;
        if (focus.scopeGridStamp_ && focus.scopeTraceRevision())
            for (const auto &line : focus.scopeGrid_) lines.emplace_back(line.x, line.bar);
        return lines;
    }
    static auto scopePlot(const MixFocus &focus) { return focus.plot_.toNearestInt(); }
    static auto scopeVisibleRange(const MixFocus &focus) { return focus.scopeVisibleRange(); }
    static float transformedScopeX(const MixFocus &focus, float captureFraction) {
        const auto &plot = focus.plot_;
        const auto source = juce::Point<float>{plot.getX() + captureFraction * plot.getWidth(), plot.getY()};
        return (source.transformedBy(focus.scopeTimeTransform()).x - plot.getX()) / plot.getWidth();
    }
    static auto scopeReading(const MixFocus &focus) { return focus.sum_; }
    static bool hasScopeSummarySpace(const MixFocus &focus) { return !focus.sumLabel_.isEmpty(); }
    static auto scopePhase(const MixFocus &focus) { return focus.scopePhase_; }
    static juce::MouseEvent moveScopePointer(MixFocus &focus, float x, float y) {
        const auto &plot = focus.plot_;
        const juce::Point<float> at{plot.getX() + x * plot.getWidth(), plot.getY() + y * plot.getHeight()};
        const auto now = juce::Time::getCurrentTime();
        const juce::MouseEvent event{juce::Desktop::getInstance().getMainMouseSource(), at, juce::ModifierKeys{},
            0.f, 0.f, 0.f, 0.f, 0.f, &focus, &focus, now, at, now, 0, false};
        static_cast<juce::Component &>(focus).mouseMove(event);
        return event;
    }
    static void wheelScope(MixFocus &focus, float x, float y, float delta,
                           int modifiers = juce::ModifierKeys::altModifier) {
        const auto &plot = focus.plot_;
        const juce::Point<float> at{plot.getX() + x * plot.getWidth(), plot.getY() + y * plot.getHeight()};
        const auto now = juce::Time::getCurrentTime();
        const juce::MouseEvent event{juce::Desktop::getInstance().getMainMouseSource(), at, juce::ModifierKeys{modifiers},
            0.f, 0.f, 0.f, 0.f, 0.f, &focus, &focus, now, at, now, 0, false};
        static_cast<juce::Component &>(focus).mouseWheelMove(event,
            juce::MouseWheelDetails{0.f, delta, false, false, false});
    }
    static juce::String scopeCursorText(const MixFocus &focus) {
        for (auto *child : focus.getChildren())
            if (const auto *label = dynamic_cast<const juce::Label *>(child);
                label && label->isVisible() && label->getTitle() == "Scope cursor")
                return label->getText();
        return {};
    }
    static void scopeRegionPointer(MixFocus &focus, void (juce::Component::*handler)(const juce::MouseEvent &),
                                   juce::Point<float> start, juce::Point<float> end) {
        const auto &plot = focus.plot_;
        const auto pointInPlot = [&](juce::Point<float> relative) {
            return juce::Point<float>{plot.getX() + relative.x * plot.getWidth(),
                                      plot.getY() + relative.y * plot.getHeight()};
        };
        const auto now = juce::Time::getCurrentTime();
        const auto pressed = handler != &juce::Component::mouseUp;
        const juce::MouseEvent event{juce::Desktop::getInstance().getMainMouseSource(), pointInPlot(end),
            juce::ModifierKeys{pressed ? juce::ModifierKeys::leftButtonModifier : 0},
            1.f, 0.f, 0.f, 0.f, 0.f, &focus, &focus, now, pointInPlot(start), now, 1, start != end};
        (static_cast<juce::Component &>(focus).*handler)(event);
    }
    static void dragScopeRegion(MixFocus &focus, juce::Point<float> start, juce::Point<float> end) {
        scopeRegionPointer(focus, &juce::Component::mouseDown, start, start);
        scopeRegionPointer(focus, &juce::Component::mouseDrag, start, end);
        scopeRegionPointer(focus, &juce::Component::mouseUp, start, end);
    }
    static juce::String scopeRegionText(const MixFocus &focus) {
        for (auto *child : focus.getChildren())
            if (const auto *label = dynamic_cast<const juce::Label *>(child);
                label && label->isVisible() && label->getTitle() == "Scope region")
                return label->getText().removeCharacters(",");
        return {};
    }
    static std::optional<std::int64_t> scopeMidpoint(const MixFocus &focus) {
        if (!focus.scopeRange_) return {};
        return focus.scopeRange_->first + static_cast<std::int64_t>(focus.scopeRange_->frames / 2);
    }
    static bool requestsResidualSpectrum(const MixFocus &focus) {
        return focus.differenceRequest_ && focus.differenceRequest_->spectrum;
    }
    static auto layerAdjustment(MixFocus &focus) {
        const auto *layer = focus.selectedScopeLayer();
        return layer ? layer->scopeAdjustment : FocusScopeLayerAdjustment{};
    }
    static juce::PopupMenu layerMenu(MixFocus &focus) {
        const auto *layer = focus.selectedScopeLayer();
        return layer ? focus.scopeLayerMenu(*layer) : juce::PopupMenu{};
    }
    static void chooseLayerItem(MixFocus &focus, int id) {
        focus.applyScopeLayerAction(focus.active_, focus.membershipPose_, static_cast<MixFocus::ScopeLayerAction>(id));
    }
    static auto cachedLayerAdjustment(const MixFocus &focus, std::size_t index) {
        return focus.scopeAdjustments_.at(index);
    }
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
        {
            using Scale = SharedSpectrumView::SpectrumScale;
            auto &mix = root.mixViewPanel;
            auto &info = mix.channels;
            const auto originalBroker = root.sharedAnalysis;
            const auto tap = std::make_shared<engine::ScopeTap>(48000., SharedAnalysis::kTapSeconds);
            auto broker = std::make_shared<SharedAnalysis>(
                [tap](std::int64_t, engine::ScopePoint) { return tap; },
                [](std::int64_t, engine::ScopePoint) {}, SharedAnalysis::Mode::Manual);
            root.sharedAnalysis = broker;
            info.infoMode.setSelectedId(info.infoMode.getItemId(1), juce::sendNotificationSync);
            root.frame();
            std::array<float, 1024> audio;
            audio.fill(.25f);
            tap->write(audio.data(), audio.data(), static_cast<std::int32_t>(audio.size()), 0, true);
            broker->pump();
            root.frame();
            const auto shown = info.preview.snapshot();
            const auto before = broker->stats();
            mix.analysis.detail.setSpectrumScale(Scale::Extended);
            root.frame();
            const auto after = broker->stats();
            check(info.preview.isShowing() && info.preview.mode() == SharedSpectrumView::Mode::Spectrum &&
                      info.preview.spectrumScale() == Scale::Extended,
                  "Mix View Info spectrum uses the current Analysers layout");
            check(shown && info.preview.snapshot() == shown && after.taps == before.taps &&
                      after.domains == before.domains && after.spectralPlans == before.spectralPlans &&
                      after.transforms == before.transforms,
                  "changing the Info layout keeps the existing spectrum publication and analysis work");
            info.infoMode.setSelectedId(info.infoMode.getItemId(0), juce::sendNotificationSync);
            mix.analysis.detail.setSpectrumScale(Scale::Wide);
            root.sharedAnalysis = originalBroker;
            root.frame();
        }
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
    bool auditFrame = false;
    unsigned publicationReads = 0, demandUpdates = 0;
    std::size_t demandedCount = 0;
    root.devices.publication = [&] {
        if (auditFrame && ++publicationReads == 2) auditingStackDemandIds = false;
        auto picture = document->parameterFeed().current();
        if (auditFrame && publicationReads == 1) auditingStackDemandIds = true;
        return picture;
    };
    root.devices.parameterDemand = [&](const void *owner, const auto &ids) {
        ++demandUpdates;
        demandedCount = ids.size();
        document->setParameterDemand(owner, ids);
    };
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
    surface.frameAt(root.reader(), 505);
    const auto steadyUpdates = demandUpdates;
    bool demandScanReached = true;
    stackDemandIdAllocations = 0;
    auditFrame = true;
    for (const auto at : {506, 507, 508, 509}) {
        // Bracket the demand scan before the second publication read, so card text and
        // rendering allocations do not contribute to the per-device allocation count.
        publicationReads = 0;
        surface.frameAt(root.reader(), at);
        auditingStackDemandIds = false;
        demandScanReached = demandScanReached && publicationReads >= 2;
    }
    auditFrame = false;
    const bool sameCard = card && locate(locate, surface) == card.getComponent();
    if (!demandScanReached || demandUpdates != steadyUpdates || demandedCount != 1 || !sameCard)
        std::printf("STACK idle fixture: demand scan reached=%d, last reads=%u, demand=%u/%u, IDs=%zu, same card=%d\n",
                    demandScanReached, publicationReads, demandUpdates, steadyUpdates, demandedCount, sameCard);
    if (stackDemandIdAllocations != 0)
        std::printf("STACK idle device-ID allocations: %u\n", stackDemandIdAllocations);
    check(demandScanReached && demandUpdates == steadyUpdates && demandedCount == 1 && sameCard,
          "unchanged Stack frames keep the same view and native parameter demand");
    check(stackDemandIdAllocations == 0, "the steady Stack demand scan allocates no temporary device IDs");
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
    const auto beforeFold = demandUpdates;
    root.devices.fold(device);
    surface.frameAt(root.reader(), 1360);
    check(!mailbox->observed(), "folded Limiter has no reduction subscription");
    check(demandUpdates == beforeFold + 1 && demandedCount == 0, "folding releases parameter demand once");
    root.devices.fold(device);
    surface.frameAt(root.reader(), 1600);
    check(mailbox->observed() && acquisitions == 3, "unfolding reopens one subscription");
    check(demandUpdates == beforeFold + 2 && demandedCount == 1, "unfolding restores parameter demand once");
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

void foldedChannelBrowserSearch(AppCommands &commands, const std::filesystem::path &directory) {
    const auto path = directory / "folded-channel-browser-search.adi";
    StoreError status{};
    auto store = Store::create(path, status);
    check(store != nullptr, "a hundred-channel browser fixture creates its project");
    if (!store) return;
    store->db().exec(
        "INSERT INTO tracks(id,kind,name,index_in_parent) VALUES(1,'master','Main',0);"
        "WITH RECURSIVE channel_numbers(n) AS (SELECT 1 UNION ALL SELECT n+1 FROM channel_numbers WHERE n<100)"
        " INSERT INTO tracks(id,kind,name,index_in_parent) "
        "SELECT 100+n,'audio',CASE n "
        "WHEN 3 THEN 'Caf\xc3\xa9 003' "
        "WHEN 54 THEN 'Stra\xc3\x9f" "e 054' "
        "WHEN 99 THEN '\xc3\x86ther 099' "
        "ELSE printf('Channel %03d',n) END,n FROM channel_numbers;"
        "INSERT INTO device_chains(id,track_id,name) VALUES(77,151,'Devices'),(78,152,'Devices');"
        "INSERT INTO plugin_refs(id,format,uid,name) VALUES(77,'internal','owned.key','Search fixture');"
        "INSERT INTO devices(id,chain_id,ord,name,plugin_ref_id)"
        "VALUES(77,77,0,'P\xc3\xa9" "ch\xc3\xa9 Lead',77),(78,78,0,'Spare tool',77);");
    store.reset();

    std::string error;
    auto document = ProjectDocument::open(path, false,
        [](const engine::DeviceRequest &, std::string &) { return std::make_unique<KeyProbe>(); }, error);
    check(document != nullptr, "the ordinary named tools load through the headless document");
    if (!document) return;
    AdiRootComponent root(document->view(), document->ops(), document->mailbox(), commands,
                          document->views(), document->sessionState(), "folded-channel-browser-search");
    std::size_t publicationReads = 0;
    root.devices.publication = [&] {
        ++publicationReads;
        return document->parameterFeed().current();
    };
    UiPeerRoot peer;
    peer.setBounds(0, 0, 1400, 900);
    root.setBounds(peer.getLocalBounds());
    peer.addAndMakeVisible(root);
    peer.setVisible(true);
    peer.addToDesktop(0);
    const bool softwarePeer = peer.getPeer() && !peer.getPeer()->getNativeHandle();
    check(softwarePeer && root.setMixView(true, false),
          "the hundred-channel search is exercised on the normal Mix View software peer");
    root.frame();
    auto &browser = root.mixViewPanel.channels;
    check(browser.isShowing() && browser.trackAt(100) != 0 && browser.trackAt(101) == 0,
          "the browser contains all one hundred tracks and Main");
    root.arrangement.geometry.selection.track = 101;
    root.publishSelection();
    root.frame();

    const auto search = [&](const char *query) {
        browser.search.setText(query, false);
        browser.search.onTextChange();
    };
    const auto only = [&](std::int64_t track) { return browser.trackAt(0) == track && browser.trackAt(1) == 0; };
    const auto head = History(document->store()).head();
    search("  cAfE 003  ");
    check(only(103), "accent-insensitive search finds the first named channel");
    search("STRASSE 054");
    check(only(154), "search folds the middle channel's sharp s");
    search("aether 099");
    check(only(199), "search folds the end channel's ligature");
    search("peche lead");
    check(only(151), "a folded device name finds its differently named channel");
    const auto reads = publicationReads;
    for (int i = 0; i < 4; ++i) browser.frame(root.reader());
    check(publicationReads == reads && document->sessionState().selection().track == 101,
          "the retained query does not scan devices again or change selection on unchanged browser frames");
    if (only(151)) browser.list.selectRow(0);
    root.frame();
    check(document->sessionState().selection().track == 151 && root.mixViewPanel.stack.focused() == 151 &&
              History(document->store()).head() == head,
          "choosing the matching channel focuses its actual Stack row without authoring any search edit");

    search("cafe 003");
    const auto renamedTrack = document->deviceAction("track.rename", {{"id", 103}, {"name", "Renamed channel"}});
    root.frame();
    const bool removedTrackMatch = browser.trackAt(0) == 0;
    const auto newTrack = document->deviceAction("track.rename", {{"id", 197}, {"name", "Caf\xc3\xa9 003"}});
    root.frame();
    check(renamedTrack && removedTrackMatch && newTrack && only(197),
          "a retained query uses the new folded track names after same-size snapshot updates");

    search("peche lead");
    const bool renamedTool = document->deviceAction("device.rename", {{"id", 77}, {"name", "Old tool"}});
    root.frame();
    const bool removedToolMatch = browser.trackAt(0) == 0;
    const bool newTool = document->deviceAction("device.rename", {{"id", 78}, {"name", "P\xc3\xa9" "ch\xc3\xa9 Lead"}});
    root.frame();
    const bool movedToolMatch = only(152);
    const bool removedTool = document->deviceAction("device.remove", {{"id", 78}});
    root.frame();
    check(renamedTool && removedToolMatch && newTool && movedToolMatch && removedTool &&
              browser.trackAt(0) == 0 && document->sessionState().selection().track == 151,
          "renamed and removed devices update the folded tool matches without moving the chosen channel");
    peer.removeChildComponent(&root);
    peer.removeFromDesktop();
}

void pinnedSpectrumOverlay(AppCommands &commands, const std::filesystem::path &directory,
                           int rate, std::int32_t overviewFft) {
    constexpr std::int64_t main = 1, first = 11, wide = 5000000001, missing = 9000000001;
    constexpr std::int32_t normalFft = 1024;
    const auto path = directory / ("pinned-spectrum-" + std::to_string(rate) + ".adi");
    StoreError status{};
    auto store = Store::create(path, status);
    check(store != nullptr, "pinned Spectrum fixture creates a project");
    if (!store) return;
    store->db().exec(
        "UPDATE project SET sample_rate=44100;"
        "INSERT INTO tracks(id,kind,name,index_in_parent) VALUES(1,'master','Main',0),(5000000001,'audio','Wide ID',40);"
        "WITH RECURSIVE pin_numbers(n) AS (SELECT 11 UNION ALL SELECT n+1 FROM pin_numbers WHERE n<30)"
        " INSERT INTO tracks(id,kind,name,index_in_parent) "
        "SELECT n,'audio',printf('Pinned %d',n),n FROM pin_numbers;");
    store.reset();

    std::string error;
    auto doc = ProjectDocument::open(path, false, {}, error);
    check(doc != nullptr, "pinned Spectrum opens its headless project");
    if (!doc) return;
    const std::vector<std::int64_t> pins{main, first, first, wide, missing};
    check(doc->views().saveStackPins({}, pins, error), "establish persisted project pins with duplicate and missing IDs");
    const auto head = History(doc->store()).head();

    using Source = engine::ScopeSource;
    using Tap = engine::ScopeTap;
    std::map<Source, std::shared_ptr<Tap>> taps;
    std::size_t attachments = 0;
    auto tapRate = static_cast<double>(rate);
    auto broker = std::make_shared<SharedAnalysis>(
        [&](Source source, double) -> std::shared_ptr<const Tap> {
            auto &tap = taps[source];
            if (!tap || tap->sampleRate() != tapRate) {
                tap = std::make_shared<Tap>(tapRate, 1.);
                ++attachments;
            }
            return tap;
        },
        [&](Source source) { taps.erase(source); }, SharedAnalysis::Mode::Manual);
    const auto key = [](std::int64_t track) { return Source{track, engine::ScopePoint::PostFader, 0}; };
    const auto attached = [&](std::int64_t track) { return taps.contains(key(track)); };
    const auto tapFor = [&](std::int64_t track) -> std::shared_ptr<Tap> {
        const auto found = taps.find(key(track));
        return found == taps.end() ? nullptr : found->second;
    };

    AdiRootComponent root(doc->view(), doc->ops(), doc->mailbox(), commands,
                          doc->views(), doc->sessionState(), "pinned-spectrum");
    root.sharedAnalysis = broker;
    UiPeerRoot peer;
    peer.setBounds(0, 0, 1400, 900);
    root.setBounds(peer.getLocalBounds());
    peer.addAndMakeVisible(root);
    peer.setVisible(true);
    peer.addToDesktop(0);
    const bool softwarePeer = peer.getPeer() && !peer.getPeer()->getNativeHandle();
    const auto detachPeer = [&] { peer.removeChildComponent(&root); peer.removeFromDesktop(); };
    check(softwarePeer && root.setMixView(true, false) &&
              root.chooseMixViewTab(static_cast<int>(MixViewPanel::Tab::Analysis)),
          "the test opens the real Mix View Analysers on a software peer");
    root.frame();
    auto &pages = root.mixViewPanel.analysis;
    using Page = MixAnalysers::Page;
    const auto sendTone = [](Tap &tap, std::int32_t frames, int bin, std::int64_t stamp) {
        std::vector<float> samples(static_cast<std::size_t>(frames));
        for (std::size_t i = 0; i < samples.size(); ++i)
            samples[i] = static_cast<float>(.5 * std::sin(2. * std::numbers::pi * static_cast<double>(bin) *
                static_cast<double>(i) / static_cast<double>(frames)));
        tap.write(samples.data(), samples.data(), frames, stamp, true, 4);
    };
    const auto select = [](juce::ComboBox &menu, const juce::String &name) {
        for (int item = 0; item < menu.getNumItems(); ++item) {
            if (menu.getItemText(item) != name) continue;
            menu.setSelectedId(menu.getItemId(item), juce::sendNotificationSync);
            return true;
        }
        return false;
    };
    pages.setPage(Page::Spectrum);
    pages.followSelection(false);
    root.frame();
    broker->pump();
    const auto findToggle = [](const auto &walk, juce::Component &parent) -> juce::ToggleButton * {
        if (auto *toggle = dynamic_cast<juce::ToggleButton *>(&parent); toggle && toggle->getTitle() == "Pinned tracks")
            return toggle;
        for (auto *child : parent.getChildren())
            if (auto *found = walk(walk, *child)) return found;
        return nullptr;
    };
    auto *control = findToggle(findToggle, pages);
    juce::ComboBox *blockSize = nullptr, *overlap = nullptr, *smoothing = nullptr;
    for (auto *child : pages.detail.getChildren()) {
        if (child->getTitle() == "Spectrum block size") blockSize = dynamic_cast<juce::ComboBox *>(child);
        if (child->getTitle() == "Spectrum overlap") overlap = dynamic_cast<juce::ComboBox *>(child);
        if (child->getTitle() == "Spectrum smoothing") smoothing = dynamic_cast<juce::ComboBox *>(child);
    }
    check(blockSize && blockSize->isShowing() && blockSize->getNumItems() == 6 && blockSize->getSelectedId() == 1,
          "the dedicated Spectrum offers six block-size choices and defaults to Auto");
    check(overlap && overlap->isShowing() && overlap->getNumItems() == 4 && overlap->getSelectedId() == 3,
          "the dedicated Spectrum offers four overlap choices and defaults to 75 percent");
    check(smoothing && smoothing->isShowing() && smoothing->getNumItems() == 16 && smoothing->getSelectedId() == 1,
          "the dedicated Spectrum offers the full smoothing range and defaults to Off");
    check(control && control->isShowing() && !control->getToggleState() &&
              attached(main) && !attached(first) && !attached(wide) && !attached(missing),
          "Spectrum offers Pinned tracks default off with no extra demand");
    auto primaryTap = tapFor(main);
    if (!control || !primaryTap) { detachPeer(); return; }
    sendTone(*primaryTap, normalFft, 16, 0);
    broker->pump();
    root.frame();
    const auto normal = pages.detail.view.snapshot();
    check(normal && normal->sampleRate == static_cast<double>(rate) &&
              normal->request.fftSize == normalFft && normal->hopSize == normalFft / 4,
          "ordinary Spectrum starts at its original 1024-point profile on the matched format");
    auto ordinaryReader = broker->acquire({main, pages.detail.point(), normalFft, pages.detail.view.channel()});
    const auto renderSpectrum = [&] {
        auto &view = pages.detail.view;
        juce::Image image(juce::Image::RGB, view.getWidth(), view.getHeight(), true, juce::SoftwareImageType{});
        juce::Graphics graphics(image);
        view.paint(graphics);
        return image;
    };
    const auto samePixels = [](const juce::Image &a, const juce::Image &b, juce::Rectangle<int> area) {
        area = area.getIntersection(a.getBounds()).getIntersection(b.getBounds());
        for (int y = area.getY(); y < area.getBottom(); ++y)
            for (int x = area.getX(); x < area.getRight(); ++x)
                if (a.getPixelAt(x, y) != b.getPixelAt(x, y)) return false;
        return true;
    };
    if (smoothing && normal) {
        auto &view = pages.detail.view;
        view.freeze(true);
        const auto raw = renderSpectrum();
        const auto before = broker->stats();
        for (const auto item : {2, smoothing->getNumItems()}) {
            smoothing->setSelectedId(item, juce::sendNotificationSync);
            check(!samePixels(raw, renderSpectrum(), raw.getBounds()), "changing Spectrum smoothing reprojects a held plot");
        }
        smoothing->setSelectedId(1, juce::sendNotificationSync);
        const auto after = broker->stats();
        check(samePixels(raw, renderSpectrum(), raw.getBounds()) && view.snapshot() == normal &&
                  before.transforms == after.transforms && before.taps == after.taps &&
                  before.consumers == after.consumers && before.spectralPlans == after.spectralPlans,
              "smoothing is reversible on the same held bins and requests no audio work");
        view.freeze(false);
        sendTone(*primaryTap, normalFft, 16, primaryTap->written());
        broker->pump();
        root.frame();
    }
    const auto baseline = broker->stats();
    const auto attachmentsBeforeToggle = attachments;
    const auto choose = [&](bool enabled) {
        control->setToggleState(enabled, juce::dontSendNotification);
        if (control->onClick) control->onClick();
        root.frame();
    };
    const auto noExtraPins = [&] {
        return !attached(first) && !attached(wide) && !attached(missing) &&
               broker->stats().consumers == baseline.consumers;
    };

    choose(true);
    const auto single = broker->stats();
    auto firstTap = tapFor(first), wideTap = tapFor(wide);
    // The ordinary reader still uses its 1024-point plan. Overview needs
    // three more plans, but both primary readers share a physical tap.
    check(firstTap && wideTap && !attached(missing) && single.consumers == baseline.consumers + 2 &&
              single.taps == baseline.taps + 2 && single.spectralPlans == baseline.spectralPlans + 3 &&
              History(doc->store()).head() == head,
          "only live non-primary pins lease two realtime spectra without authoring an edit");
    check(tapFor(main) == primaryTap && attachments == attachmentsBeforeToggle + 2,
          "enabling pinned Spectrum retains the original primary tap and attaches only the pins");
    broker->pump();
    const auto settled = broker->stats();
    const auto attachedBefore = attachments;
    for (int frame = 0; frame < 4; ++frame) {
        root.frame();
        broker->pump();
    }
    const auto idle = broker->stats();
    check(attachments == attachedBefore && idle.workerPasses == settled.workerPasses &&
              idle.transforms == settled.transforms && idle.publications == settled.publications,
          "unchanged Spectrum frames neither reacquire pinned taps nor add worker work");
    if (firstTap && wideTap) {
        adi::ui::SharedAnalysis::Request request{main, pages.detail.point(), overviewFft,
                                                pages.detail.view.channel()};
        request.hopSize = overviewFft / 4;
        auto primary = broker->acquire(request);
        request.track = first;
        auto low = broker->acquire(request);
        request.track = wide;
        auto high = broker->acquire(request);
        const auto shared = broker->stats();
        check(primary && low && high && shared.taps == idle.taps &&
                  shared.spectralPlans == idle.spectralPlans && shared.domains == idle.domains,
              "Overview primary and pins reuse explicit 75 percent-overlap observers without extra plans");

        // Supply an extra primary hop so earlier demand cannot shift the comparison boundary.
        const auto start = primaryTap->written();
        sendTone(*primaryTap, overviewFft + overviewFft / 4, 20, start);
        sendTone(*firstTap, overviewFft, 4, start);
        sendTone(*wideTap, overviewFft, 32, start);
        for (int pass = 0; pass < 12; ++pass) broker->pump();
        root.frame();

        const auto picture = pages.detail.view.snapshot();
        check(picture && picture == primary.read() && picture->sampleRate == static_cast<double>(rate) &&
                  picture->request.fftSize == overviewFft && picture->hopSize == overviewFft / 4 &&
                  pages.detail.view.spectrumBins().size() == static_cast<std::size_t>(overviewFft / 2 + 1),
              "visible Overview adopts its rate-sized realtime profile and shared immutable primary");
        const auto ordinary = ordinaryReader.read();
        check(ordinary && ordinary->request.track == main && ordinary->request.fftSize == normalFft &&
                  ordinary->hopSize == normalFft / 4 && ordinary->generation == primaryTap->generation(),
              "an ordinary observer retains its 1024-point plan and shares Overview's primary tap");
        const auto a = low.read(), b = high.read();
        const auto peakBin = [](const auto &bins) { return std::ranges::max_element(bins) - bins.begin(); };
        check(a && b && a->request.track == first && b->request.track == wide &&
                  a->request.fftSize == overviewFft && b->request.fftSize == overviewFft &&
                  a->hopSize == overviewFft / 4 && b->hopSize == overviewFft / 4 &&
                  peakBin(a->binsDbfs) == 4 && peakBin(b->binsDbfs) == 32,
              "matching pinned observers keep their real source identities and distinct tone bins");

        if (rate == 48000 && blockSize) {
            constexpr int fixedFft = 2048;
            constexpr std::int64_t fixedStamp = 20000, thawStamp = fixedStamp + fixedFft;
            auto &view = pages.detail.view;
            const auto attachmentCount = attachments;
            const auto selectBlockSize = [&](int id) {
                blockSize->setSelectedId(id, juce::sendNotificationSync);
                root.frame();
            };
            const auto writeBlock = [&](int size, std::int64_t stamp) {
                sendTone(*primaryTap, size, 16, stamp);
                sendTone(*firstTap, size, 4, stamp);
                sendTone(*wideTap, size, 32, stamp);
                for (int pass = 0; pass < 12; ++pass) broker->pump();
                root.frame();
            };
            selectBlockSize(3);
            const auto selected = broker->stats();
            auto fixedRequest = request;
            fixedRequest.fftSize = fixedFft;
            fixedRequest.hopSize = fixedFft / 4;
            const auto observeFixed = [&](std::int64_t track) {
                fixedRequest.track = track;
                return broker->acquire(fixedRequest);
            };
            auto fixedMain = observeFixed(main), fixedLow = observeFixed(first), fixedHigh = observeFixed(wide);
            check(broker->stats().spectralPlans == selected.spectralPlans && broker->stats().domains == selected.domains,
                  "the selected block size is already requested by the primary and both pins");
            writeBlock(fixedFft, fixedStamp);
            const auto fixedPicture = view.snapshot(), fixedA = fixedLow.read(), fixedB = fixedHigh.read();
            const auto coherent = [&](const auto &item, std::int64_t track, int bin) {
                return item && item->request.track == track && item->request.fftSize == fixedFft &&
                    item->hopSize == fixedFft / 4 && item->firstStamp == fixedStamp &&
                    item->binsDbfs.size() == static_cast<std::size_t>(fixedFft / 2 + 1) && peakBin(item->binsDbfs) == bin;
            };
            check(fixedPicture == fixedMain.read() && coherent(fixedPicture, main, 16) &&
                      coherent(fixedA, first, 4) && coherent(fixedB, wide, 32) &&
                      tapFor(main) == primaryTap && tapFor(first) == firstTap && tapFor(wide) == wideTap &&
                      attachments == attachmentCount,
                  "2048 uses a fresh coherent primary and both pins on their existing physical taps");

            view.freeze(true);
            const auto held = broker->stats();
            if (smoothing && fixedA && fixedB) {
                const auto raw = renderSpectrum();
                smoothing->setSelectedId(smoothing->getNumItems(), juce::sendNotificationSync);
                const auto smoothed = renderSpectrum();
                // At each raw pin's peak the other traces are far below its level.
                for (const auto &pin : {fixedA, fixedB}) {
                    const auto bin = static_cast<std::size_t>(peakBin(pin->binsDbfs));
                    const auto hz = static_cast<double>(bin) * pin->sampleRate / fixedFft;
                    const auto x = juce::roundToInt(5. + (raw.getWidth() - 28.) * std::log(hz / 20.) / std::log(1000.));
                    const auto y = juce::roundToInt(21. - (raw.getHeight() - 55.) * pin->binsDbfs[bin] / 96.);
                    check(!samePixels(raw, smoothed, {x - 4, y - 4, 9, 9}),
                          "the held pin's own peak reprojects with the shared smoothing choice");
                }
                smoothing->setSelectedId(1, juce::sendNotificationSync);
                check(samePixels(raw, renderSpectrum(), raw.getBounds()) &&
                          broker->stats().transforms == held.transforms,
                      "turning smoothing off restores both held pin paths without a transform");
            }
            selectBlockSize(1);
            check(view.frozen() && view.snapshot() == fixedPicture && broker->stats().consumers == held.consumers &&
                      broker->stats().spectralPlans == held.spectralPlans && broker->stats().transforms == held.transforms,
                  "selecting Auto while frozen preserves the displayed 2048-point picture and demand");
            view.freeze(false);
            root.frame();
            check(!view.snapshot(), "thaw waits for a fresh window using the newly selected block size");
            writeBlock(overviewFft, thawStamp);
            const auto automatic = view.snapshot();
            check(automatic && automatic == primary.read() && automatic->request.fftSize == 4096 &&
                      automatic->hopSize == 1024 && automatic->firstStamp >= thawStamp && blockSize->getSelectedId() == 1 &&
                      attachments == attachmentCount,
                  "thaw restores the 48 kHz Auto profile on the existing taps");

            if (overlap) {
                constexpr auto halfStamp = thawStamp + 4096, restoredStamp = halfStamp + 4096;
                const auto selectOverlap = [&](int id) {
                    overlap->setSelectedId(id, juce::sendNotificationSync);
                    root.frame();
                };
                selectOverlap(2);
                check(!view.snapshot(), "changing overlap waits for fresh audio at the new hop");
                const auto halfDemand = broker->stats();
                auto halfRequest = request;
                halfRequest.hopSize = overviewFft / 2;
                const auto observeHalf = [&](std::int64_t track) {
                    halfRequest.track = track;
                    return broker->acquire(halfRequest);
                };
                auto halfMain = observeHalf(main), halfLow = observeHalf(first), halfHigh = observeHalf(wide);
                check(broker->stats().spectralPlans == halfDemand.spectralPlans && broker->stats().domains == halfDemand.domains,
                      "primary and both pins already request the 50 percent overlap");
                writeBlock(overviewFft, halfStamp);
                const std::array windows{halfMain.read(), halfLow.read(), halfHigh.read()};
                check(view.snapshot() == windows[0] && std::ranges::all_of(windows, [&](const auto &item) {
                          return item && item->request.fftSize == overviewFft && item->hopSize == overviewFft / 2 &&
                              item->firstStamp == halfStamp;
                      }) && tapFor(main) == primaryTap && tapFor(first) == firstTap && tapFor(wide) == wideTap &&
                          attachments == attachmentCount,
                      "50 percent overlap gives the primary and both pins fresh matched 4096/2048 windows on the same taps");
                selectOverlap(3);
                writeBlock(overviewFft, restoredStamp);
                const auto restored = view.snapshot();
                check(restored && restored == primary.read() && restored->hopSize == overviewFft / 4 &&
                          restored->firstStamp == restoredStamp && attachments == attachmentCount,
                      "restoring 75 percent selects a fresh quarter-hop window without replacing taps");
            }
        }

        if (rate == 48000) {
            using Mode = SharedSpectrumView::Mode;
            const auto beforeAverage = broker->stats();
            const auto attachmentCount = attachments;
            const bool offered = select(pages.spectrumTraceBox, "Realtime Average");
            root.frame();
            check(offered && pages.page() == Page::Spectrum && pages.detail.mode() == Mode::SpectrumAverage &&
                      control->isEnabled() && control->getToggleState(),
                  "Realtime Average stays on Spectrum and keeps the pinned tracks enabled");
            if (offered) {
                constexpr std::int64_t newStamp = 100000;
                constexpr std::array<int, 3> bin{16, 4, 32};
                const auto writeOverview = [&](std::int64_t stamp) {
                    sendTone(*primaryTap, overviewFft, bin[0], stamp);
                    sendTone(*firstTap, overviewFft, bin[1], stamp);
                    sendTone(*wideTap, overviewFft, bin[2], stamp);
                    for (int pass = 0; pass < 12; ++pass) broker->pump();
                    root.frame();
                };
                writeOverview(newStamp);
                const std::array raw{primary.read(), low.read(), high.read()};
                check(std::ranges::all_of(raw, [](const auto &item) {
                    return item && (item->outputs & SharedAnalysis::Average1s) && item->sampleRate == 48000. &&
                           item->request.fftSize == 4096 && item->hopSize == 1024;
                }) && tapFor(main) == primaryTap && tapFor(first) == firstTap && tapFor(wide) == wideTap,
                      "the actual primary and pin consumers admit Average1s on their 4096/1024 sources");

                auto readAverage = [&](std::int64_t track) {
                    auto averageRequest = request;
                    averageRequest.track = track;
                    averageRequest.outputs = SharedAnalysis::Average1s;
                    return broker->acquire(averageRequest);
                };
                auto averageMain = readAverage(main), averageFirst = readAverage(first), averageWide = readAverage(wide);
                const std::array averaged{averageMain.readAverage(), averageFirst.readAverage(), averageWide.readAverage()};
                const auto firstHopDb = 20. * std::log10(1. - std::pow(.1, 1024. / 48000.));
                const std::array<std::int64_t, 3> expectedTracks{main, first, wide};
                bool samePublishedWindows = true;
                for (std::size_t side = 0; side < averaged.size(); ++side) {
                    const auto k = static_cast<std::size_t>(bin[side]);
                    const auto &item = averaged[side];
                    samePublishedWindows = samePublishedWindows && item && raw[side] && item->window == raw[side] && item->binsDbfs &&
                        item->window->request.track == expectedTracks[side] && item->processedWindows == 1 &&
                        k < item->binsDbfs->size() && k < raw[side]->binsDbfs.size() &&
                        std::abs((*item->binsDbfs)[k] - raw[side]->binsDbfs[k] - firstHopDb) < .01;
                }
                check(samePublishedWindows && pages.detail.view.snapshot() == raw[0] && averaged[0] &&
                          averaged[0]->binsDbfs && pages.detail.view.spectrumBins().data() == averaged[0]->binsDbfs->data(),
                      "Realtime Average shares the actual immutable windows and presents the existing magnitude law");

                auto &view = pages.detail.view;
                juce::Image image(juce::Image::RGB, view.getWidth(), view.getHeight(), true, juce::SoftwareImageType{});
                juce::Graphics g(image);
                view.paint(g);
                const auto peakInk = [&](std::size_t side) {
                    if (!averaged[side] || !averaged[side]->binsDbfs) return false;
                    const auto k = static_cast<std::size_t>(bin[side]);
                    if (k >= averaged[side]->binsDbfs->size()) return false;
                    const auto hz = static_cast<double>(bin[side]) * 48000. / 4096.;
                    const auto x = juce::roundToInt(5. + (image.getWidth() - 28.) * std::log(hz / 20.) / std::log(1000.));
                    const auto y = juce::roundToInt(21. - (image.getHeight() - 55.) * (*averaged[side]->binsDbfs)[k] / 96.);
                    for (int dy = -2; dy <= 2; ++dy)
                        for (int dx = -2; dx <= 2; ++dx)
                            if (image.getPixelAt(x + dx, y + dy) != look::display) return true;
                    return false;
                };
                check(view.spectrumScale() == SharedSpectrumView::SpectrumScale::Wide &&
                          peakInk(0) && peakInk(1) && peakInk(2),
                      "the rendered Realtime Average shows both pinned peaks at their averaged heights");

                const auto warm = broker->stats();
                for (int frame = 0; frame < 4; ++frame) { root.frame(); broker->pump(); }
                const auto still = broker->stats();
                check(warm.taps == beforeAverage.taps && warm.domains == beforeAverage.domains &&
                          warm.spectralPlans == beforeAverage.spectralPlans && attachments == attachmentCount &&
                          still.transforms == warm.transforms && still.publications == warm.publications &&
                          still.workerPasses == warm.workerPasses && still.cursorReads == warm.cursorReads,
                      "Realtime Average adds no FFT plans, taps or unchanged-frame worker work");
                averageMain.reset(); averageFirst.reset(); averageWide.reset();

                const bool restoredMode = select(pages.spectrumTraceBox, "Realtime");
                root.frame();
                writeOverview(newStamp + overviewFft);
                const auto now = broker->stats();
                const auto restored = primary.read(), pinA = low.read(), pinB = high.read();
                check(restoredMode && pages.detail.mode() == Mode::Spectrum && restored && pinA && pinB &&
                          pages.detail.view.snapshot() == restored &&
                          pages.detail.view.spectrumBins().data() == restored->binsDbfs.data() &&
                          !(restored->outputs & SharedAnalysis::Average1s) &&
                          !(pinA->outputs & SharedAnalysis::Average1s) && !(pinB->outputs & SharedAnalysis::Average1s) &&
                          now.spectralPlans == beforeAverage.spectralPlans && now.taps == beforeAverage.taps,
                      "returning to Realtime restores raw primary and pin traces on the same plans");
            }

            const bool offeredSigma = select(pages.spectrumTraceBox, "Realtime Sigma");
            root.frame();
            check(offeredSigma && pages.page() == Page::Spectrum && control->isEnabled() &&
                      control->getToggleState() && tapFor(main) == primaryTap && tapFor(first) == firstTap,
                  "Realtime Sigma is a Spectrum trace that keeps its existing pinned sources");
            if (offeredSigma) {
                constexpr std::int64_t sigmaStamp = 300000;
                constexpr std::size_t mainBin = 8, pinBin = 4;
                broker->pump();
                const auto beforeSigma = broker->stats();
                sendTone(*primaryTap, overviewFft, static_cast<int>(mainBin), sigmaStamp);
                sendTone(*firstTap, overviewFft, static_cast<int>(pinBin), sigmaStamp);
                for (int pass = 0; pass < 12; ++pass) broker->pump();
                root.frame();
                const auto rawMain = primary.read(), rawPin = low.read();
                const auto produced = broker->stats();
                check(rawMain && rawPin && rawMain->firstStamp == sigmaStamp && rawPin->firstStamp == sigmaStamp &&
                          rawMain->request.fftSize == overviewFft && rawPin->request.fftSize == overviewFft &&
                          rawMain->hopSize == 1024 && rawPin->hopSize == 1024 &&
                          (rawMain->outputs & SharedAnalysis::RealtimeSigma) &&
                          (rawPin->outputs & SharedAnalysis::RealtimeSigma),
                      "the visible primary and fresh pin already request sigma on their 4096/1024 plans");

                auto sigmaRequest = request;
                sigmaRequest.outputs = SharedAnalysis::RealtimeSigma;
                sigmaRequest.track = main;
                auto sigmaMainLease = broker->acquire(sigmaRequest);
                sigmaRequest.track = first;
                auto sigmaPinLease = broker->acquire(sigmaRequest);
                const auto mainSigma = sigmaMainLease.readRealtimeSigma(), pinSigma = sigmaPinLease.readRealtimeSigma();
                const auto step = 1. - std::pow(.1, 1024. / (48000. * .5));
                const auto firstHopDb = 20. * std::log10((1. - step) * std::sqrt(step));
                const auto matches = [&](const auto &sigma, const auto &raw, std::size_t bin) {
                    return sigma && raw && sigma->window == raw &&
                        bin < sigma->binsDbfs.size() && bin < raw->binsDbfs.size() &&
                        std::abs(sigma->binsDbfs[bin] - raw->binsDbfs[bin] - firstHopDb) < .01;
                };
                auto &view = pages.detail.view;
                check(matches(mainSigma, rawMain, mainBin) && matches(pinSigma, rawPin, pinBin) &&
                          view.snapshot() == rawMain && view.spectrumBins().data() == mainSigma->binsDbfs.data(),
                      "Sigma projects the first-hop standard deviation from the actual shared immutable windows");

                if (pinSigma && pinBin < pinSigma->binsDbfs.size()) {
                    juce::Image image(juce::Image::RGB, view.getWidth(), view.getHeight(), true, juce::SoftwareImageType{});
                    juce::Graphics g(image);
                    view.paint(g);
                    const auto hz = static_cast<double>(pinBin) * 48000. / 4096.;
                    const auto x = juce::roundToInt(5. + (image.getWidth() - 28.) * std::log(hz / 20.) / std::log(1000.));
                    const auto y = juce::roundToInt(21. - (image.getHeight() - 55.) * pinSigma->binsDbfs[pinBin] / 96.);
                    bool visiblePinPeak = false;
                    for (int dy = -2; dy <= 2; ++dy)
                        for (int dx = -2; dx <= 2; ++dx)
                            visiblePinPeak = visiblePinPeak || (image.getBounds().contains(x + dx, y + dy) &&
                                image.getPixelAt(x + dx, y + dy) != look::display);
                    check(view.spectrumScale() == SharedSpectrumView::SpectrumScale::Wide && visiblePinPeak,
                          "the pinned trace is painted at the sigma level rather than its raw peak");
                }
                const auto observed = broker->stats();
                check(produced.taps == beforeSigma.taps && produced.domains == beforeSigma.domains &&
                          produced.spectralPlans == beforeSigma.spectralPlans &&
                          observed.spectralPlans == produced.spectralPlans &&
                          produced.transforms > beforeSigma.transforms &&
                          produced.transforms - beforeSigma.transforms ==
                              produced.analysisWindows - beforeSigma.analysisWindows,
                      "Sigma adds no plan and each real window has just its existing analyser transform");
                sigmaMainLease.reset(); sigmaPinLease.reset();
                select(pages.spectrumTraceBox, "Realtime");
                root.frame();
            }
        }
    }
    const bool renamed = doc->deviceAction("track.rename", {{"id", wide}, {"name", "Renamed wide ID"}});
    root.frame();
    check(renamed && firstTap && wideTap && tapFor(first) == firstTap && tapFor(wide) == wideTap,
          "renaming a pinned 64-bit channel preserves both live tap identities");

    std::vector<std::int64_t> many{main};
    for (std::int64_t track = first; track <= 30; ++track) many.push_back(track);
    many.push_back(wide);
    many.push_back(missing);
    check(doc->views().saveStackPins(pins, many, error), "update project pins while Spectrum is visible");
    root.frame();
    const auto capped = broker->stats();
    check(capped.consumers == baseline.consumers + 15 && capped.taps == baseline.taps + 15 &&
              !attached(missing), "project pins are bounded to sixteen total curves including the visible primary");
    check(doc->views().saveStackPins(many, pins, error), "restore the small pin set for page transitions");
    root.frame();
    const auto attachmentsBeforeRestore = attachments;
    choose(false);
    check(noExtraPins() && tapFor(main) == primaryTap && attachments == attachmentsBeforeRestore &&
              broker->stats().spectralPlans == baseline.spectralPlans,
          "turning Pinned tracks off drops pin leases and retains the primary tap");
    ordinaryReader.reset();
    {
        adi::ui::SharedAnalysis::Request request{main, pages.detail.point(), normalFft,
                                                pages.detail.view.channel()};
        const auto before = broker->stats();
        auto ordinary = broker->acquire(request);
        sendTone(*primaryTap, normalFft, 16, primaryTap->written());
        broker->pump();
        root.frame();
        const auto restored = pages.detail.view.snapshot();
        check(ordinary && restored && restored == ordinary.read() && restored->request.fftSize == normalFft &&
                  restored->hopSize == normalFft / 4 && broker->stats().spectralPlans == before.spectralPlans,
              "turning Pinned tracks off restores and shares the ordinary 1024-point plan");
    }
    choose(true);
    pages.setPage(Page::All);
    {
        // All may keep a physical tap for a visible table row's meters.
        // Neither pinned spectrum plan should survive the page switch.
        const auto before = broker->stats().spectralPlans;
        SharedAnalysis::Request request{first, pages.detail.point(), overviewFft, pages.detail.view.channel()};
        request.hopSize = overviewFft / 4;
        auto probeFirst = broker->acquire(request);
        request.track = wide;
        auto probeWide = broker->acquire(request);
        check(probeFirst && probeWide && broker->stats().spectralPlans == before + 2,
              "switching to All immediately drops both pinned spectrum plans");
    }
    pages.setPage(Page::Spectrum);
    root.frame();
    choose(true);
    pages.setPage(Page::Compare);
    check(!attached(first) && !attached(wide), "switching to Compare releases all pinned Spectrum demand immediately");
    check(blockSize && !blockSize->isShowing() && overlap && !overlap->isShowing() &&
              smoothing && !smoothing->isShowing(),
          "Compare hides the dedicated Spectrum block-size, overlap and smoothing controls");

    root.frame();
    const bool pairSelected = select(pages.compareA, "Pinned 11") && select(pages.compareB, "Renamed wide ID");
    root.frame();
    const auto a = tapFor(first), b = tapFor(wide);
    check(pairSelected && a && b, "Compare still opens its own two explicitly selected identities");
    if (a && b) {
        std::array<float, 1024> audio;
        audio.fill(.5f);
        a->write(audio.data(), audio.data(), 1024, 0, true, 4);
        audio.fill(.25f);
        b->write(audio.data(), audio.data(), 1024, 0, true, 4);
        broker->pump();
        root.frame();
        const auto pair = pages.comparisonView();
        check(pair.status == AnalysisComparison::Status::Independent && pair.b.metadata &&
                  pair.b.metadata->endpoint.request.track == wide,
              "Compare publishes B from its original authored track");
        const auto heldValue = pair.b.binsDbfs.empty() ? 0.f : pair.b.binsDbfs.front();
        if (pages.holdB.onClick) pages.holdB.onClick();
        check(pages.comparisonView().status == AnalysisComparison::Status::Held && !attached(wide),
              "Hold B keeps its picture and releases its own live demand");
        pages.setPage(Page::Spectrum);
        root.frame();
        check(pages.comparisonView().referenceHeld && attached(wide),
              "Spectrum may reopen pinned B without clearing Compare's local hold");
        if (rate == 48000 && select(pages.spectrumTraceBox, "Realtime Average")) {
            root.frame();
            check(pages.comparisonView().referenceHeld && attached(wide),
                  "Realtime Average may open a pinned live tap without replacing Compare's held reference");
            select(pages.spectrumTraceBox, "Realtime");
        }
        if (rate == 48000 && select(pages.spectrumTraceBox, "Realtime Sigma")) {
            root.frame();
            check(pages.comparisonView().referenceHeld && attached(wide),
                  "Realtime Sigma keeps Compare's separate held reference while its own pin is live");
            select(pages.spectrumTraceBox, "Realtime");
        }
        pages.setPage(Page::Compare);
        root.frame();
        const auto held = pages.comparisonView();
        check(held.referenceHeld && !held.b.binsDbfs.empty() && held.b.binsDbfs.front() == heldValue && !attached(wide),
              "returning to Compare keeps the original held bins and no live B tap");
    }
    if (rate == 48000) {
        pages.setPage(Page::Spectrum);
        root.frame();
        const auto primaryBefore = tapFor(main), firstBefore = tapFor(first), wideBefore = tapFor(wide);
        const auto count = attachments;
        const auto previous = broker->stats();
        const bool averaged = select(pages.spectrumTraceBox, "Realtime Average");
        root.frame();
        const auto active = broker->stats();
        check(averaged && primaryBefore && firstBefore && wideBefore && tapFor(main) == primaryBefore &&
                  tapFor(first) == firstBefore && tapFor(wide) == wideBefore && attachments == count &&
                  active.consumers == previous.consumers && active.spectralPlans == previous.spectralPlans,
              "the view's own leases retain all three physical taps when changing to Realtime Average");
        pages.setPage(Page::Average);
        root.frame();
        const auto averageTap = tapFor(main);
        if (averageTap) sendTone(*averageTap, normalFft, 8, averageTap->written());
        broker->pump();
        root.frame();
        const auto averagePicture = pages.detail.view.snapshot();
        check(!control->isVisible() && !attached(first) && !attached(wide) && averagePicture &&
                  averagePicture->request.fftSize == normalFft && (averagePicture->outputs & SharedAnalysis::Average1s),
              "the separate Average page releases the pins and uses its original analysis size");
        pages.setPage(Page::Spectrum);
        select(pages.spectrumTraceBox, "Realtime");

        choose(false);
        const bool cumulative = select(pages.spectrumTraceBox, "Cumulative Average");
        root.frame();
        auto &view = pages.detail.view;
        const auto selectedTap = tapFor(main);
        check(cumulative && selectedTap && pages.page() == Page::Spectrum &&
                  control->isShowing() && control->isEnabled() && !attached(first) && !attached(wide) &&
                  pages.detail.resetButton.isShowing(),
              "Cumulative Average is a single-source Spectrum trace with its own Reset");
        if (cumulative && selectedTap) {
            constexpr auto hop = normalFft / 4;
            constexpr std::int64_t start = 200000;
            constexpr std::size_t toneBin = 8;
            const auto writeTone = [&](Tap &tap, int count, float magnitude, std::int64_t stamp) {
                std::vector<float> samples(static_cast<std::size_t>(count));
                for (std::size_t i = 0; i < samples.size(); ++i)
                    samples[i] = magnitude * static_cast<float>(std::sin(
                        2. * std::numbers::pi * static_cast<double>(toneBin) * static_cast<double>(i) / normalFft));
                tap.write(samples.data(), samples.data(), count, stamp, true, 4);
                for (int pass = 0; pass < 12; ++pass) broker->pump();
                root.frame();
            };
            writeTone(*selectedTap, normalFft, .5f, start);
            const auto firstView = view.snapshot();
            const auto firstBins = view.spectrumBins();
            const auto initialDb = firstBins.size() > toneBin ? firstBins[toneBin] : 0.f;
            check(firstView && firstView->request.track == main &&
                      firstView->request.fftSize == normalFft && firstView->hopSize == hop &&
                      (firstView->outputs & SharedAnalysis::CumulativeAverage) &&
                      std::abs(initialDb - 20. * std::log10(.5)) < .02,
                  "the ordinary cumulative trace projects a real calibrated tone from its 1024-point source");

            writeTone(*selectedTap, hop, 0.f, start + normalFft);
            const auto diluted = view.spectrumBins();
            check(diluted.size() > toneBin && diluted[toneBin] < initialDb - .05f &&
                      diluted[toneBin] > initialDb - 3.f,
                  "a fresh quieter window reduces the displayed cumulative magnitude");
            const auto beforeReset = broker->stats();
            if (pages.detail.resetButton.onClick) pages.detail.resetButton.onClick();
            root.frame();
            broker->pump();
            root.frame();
            check(!view.snapshot() && view.spectrumBins().empty() && tapFor(main) == selectedTap &&
                      broker->stats().transforms == beforeReset.transforms,
                  "Reset clears only the current display interval without opening another tap or running an FFT");

            constexpr auto resetStamp = start + normalFft + hop;
            writeTone(*selectedTap, hop, .125f, resetStamp);
            check(!view.snapshot(), "Cumulative Average waits for a whole post-Reset window");
            writeTone(*selectedTap, normalFft - hop, .125f, resetStamp + hop);
            const auto restarted = view.snapshot();
            const auto restartedBins = view.spectrumBins();
            check(restarted && restarted->firstStamp == resetStamp && restartedBins.size() > toneBin &&
                      std::abs(restartedBins[toneBin] - 20. * std::log10(.125)) < .02,
                  "the first complete post-Reset window sets the new cumulative level without older audio");

            pages.setPage(Page::Average);
            root.frame();
            const auto separateTap = tapFor(main);
            if (separateTap) writeTone(*separateTap, normalFft, .125f, resetStamp + 3 * normalFft);
            const auto exponential = view.snapshot();
            const auto expBins = view.spectrumBins();
            const auto step = 1. - std::pow(.1, hop / 48000.);
            check(separateTap && exponential && view.mode() == SharedSpectrumView::Mode::SpectrumAverage &&
                      (exponential->outputs & SharedAnalysis::Average1s) &&
                      !(exponential->outputs & SharedAnalysis::CumulativeAverage) &&
                      expBins.size() > toneBin && std::abs(expBins[toneBin] - 20. * std::log10(.125 * step)) < .02,
                  "the separate one-second page keeps its exponential average after the cumulative trace");

            pages.setPage(Page::Spectrum);
            // Isolate the source from the visible Main meter and the mirrored Info pane.
            constexpr std::int64_t isolatedTrack = 12;
            const bool isolatedSource = select(pages.sourceBox, "Pinned 12");
            const bool infoWasVisible = commands.infoView();
            const bool infoClosed = !infoWasVisible || commands.setInfoView(false, error);
            root.frame();
            broker->pump();
            const auto hiddenTap = tapFor(isolatedTrack);
            pages.setVisible(false);
            const bool released = !view.subscribed() && !view.snapshot() && !attached(isolatedTrack);
            const auto hidden = broker->stats();
            if (hiddenTap) writeTone(*hiddenTap, normalFft, .5f, resetStamp + 5 * normalFft);
            const auto still = broker->stats();
            check(isolatedSource && infoClosed && hiddenTap && released && !view.subscribed() && !view.snapshot(),
                  "hiding Cumulative Average drops its subscription and discards its picture");
            check(still.spectralPlans == hidden.spectralPlans && still.transforms == hidden.transforms &&
                      still.workerPasses == hidden.workerPasses && still.publications == hidden.publications,
                  "fresh input does not drive work for a hidden Cumulative Average");
            if (infoWasVisible) commands.setInfoView(true, error);
            pages.setVisible(true);
            pages.followSelection(false);
            choose(true);
            const auto pinnedPrimaryTap = tapFor(main), pinnedFirstTap = tapFor(first), pinnedWideTap = tapFor(wide);
            SharedAnalysis::Request pinRequest{first, pages.detail.point(), overviewFft, view.channel()};
            pinRequest.hopSize = overviewFft / 4;
            auto pinA = broker->acquire(pinRequest);
            pinRequest.track = wide;
            auto pinB = broker->acquire(pinRequest);
            constexpr std::int64_t pinStamp = resetStamp + 7 * normalFft;
            if (pinnedPrimaryTap && pinnedFirstTap && pinnedWideTap) {
                sendTone(*pinnedPrimaryTap, overviewFft, 8, pinStamp);
                sendTone(*pinnedFirstTap, overviewFft, 4, pinStamp);
                sendTone(*pinnedWideTap, overviewFft, 16, pinStamp);
                for (int pass = 0; pass < 12; ++pass) broker->pump();
                root.frame();
            }
            const auto pinnedMain = view.snapshot(), pinnedA = pinA.read(), pinnedB = pinB.read();
            check(pinnedMain && pinnedA && pinnedB && pinnedMain->request.fftSize == overviewFft &&
                      (pinnedMain->outputs & SharedAnalysis::CumulativeAverage) &&
                      (pinnedA->outputs & SharedAnalysis::CumulativeAverage) &&
                      (pinnedB->outputs & SharedAnalysis::CumulativeAverage),
                  "Cumulative Average subscribes the primary and both pins on their shared Overview plans");
            choose(false);
            select(pages.spectrumTraceBox, "Realtime");
            root.frame();
        }
    }
    if (rate == 96000) {
        pages.setPage(Page::Spectrum);
        root.frame();
        const auto oldPrimary = tapFor(main), oldFirst = tapFor(first), oldWide = tapFor(wide);
        check(oldPrimary && oldFirst && oldWide && oldPrimary->sampleRate() == 96000.,
              "the same Spectrum panel has its normal primary and pins attached before a format rebind");
        if (oldPrimary && oldFirst && oldWide) {
            sendTone(*oldPrimary, overviewFft, 16, 0);
            sendTone(*oldFirst, overviewFft, 4, 0);
            sendTone(*oldWide, overviewFft, 32, 0);
            for (int pass = 0; pass < 12; ++pass) broker->pump();
            root.frame();
        }
        const auto oldPicture = pages.detail.view.snapshot();
        check(oldPicture && oldPicture->sampleRate == 96000.,
              "the Spectrum panel has a published 96 kHz picture before the format rebind");

        // The device prepares another rate while the stored project stays at 44.1 kHz.
        constexpr double reboundRate = 48000.;
        constexpr std::int32_t reboundFft = 4096;
        tapRate = reboundRate;
        broker->refreshSources(true);
        root.frame();
        const auto newPrimary = tapFor(main), newFirst = tapFor(first), newWide = tapFor(wide);
        check(newPrimary && newFirst && newWide && newPrimary != oldPrimary && newFirst != oldFirst &&
                  newWide != oldWide && newPrimary->sampleRate() == reboundRate,
              "the prepared rate replaces the primary and both pinned physical taps");
        const auto pending = pages.detail.view.snapshot();
        check(!pending || (pending->sampleRate == reboundRate && pending->request.fftSize == reboundFft),
              "changing the prepared rate clears the previous Spectrum picture before new samples");
        if (newPrimary && newFirst && newWide) {
            SharedAnalysis::Request request{main, pages.detail.point(), reboundFft, pages.detail.view.channel()};
            request.hopSize = reboundFft / 4;
            const auto before = broker->stats();
            auto primary = broker->acquire(request);
            request.track = first;
            auto left = broker->acquire(request);
            request.track = wide;
            auto right = broker->acquire(request);
            const auto sharing = broker->stats();
            check(primary && left && right && sharing.taps == before.taps &&
                      sharing.domains == before.domains && sharing.spectralPlans == before.spectralPlans,
                  "a prepared rate change reuses exactly one new profile per visible Spectrum source");
            sendTone(*newPrimary, reboundFft, 16, 0);
            sendTone(*newFirst, reboundFft, 4, 0);
            sendTone(*newWide, reboundFft, 32, 0);
            for (int pass = 0; pass < 12; ++pass) broker->pump();
            root.frame();
            const auto picture = pages.detail.view.snapshot(), pinA = left.read(), pinB = right.read();
            check(picture && pinA && pinB && picture == primary.read() && picture->sampleRate == reboundRate &&
                      pinA->sampleRate == reboundRate && pinB->sampleRate == reboundRate &&
                      picture->request.fftSize == reboundFft && picture->hopSize == reboundFft / 4 &&
                      pinA->request.track == first && pinB->request.track == wide,
                  "the existing panel publishes the new 48 kHz primary and pin profiles after a 96 kHz rebind");
        }
    }
    if (rate == 48000) {
        const auto findLabel = [](const auto &walk, juce::Component &parent, const juce::String &name) -> juce::Label * {
            if (auto *label = dynamic_cast<juce::Label *>(&parent); label && label->getTitle() == name) return label;
            for (auto *child : parent.getChildren())
                if (auto *label = walk(walk, *child, name)) return label;
            return nullptr;
        };
        const auto statistic = [&](const juce::String &name) {
            const auto *label = findLabel(findLabel, pages, name);
            return label && label->isShowing() && !label->getBounds().isEmpty() ? label->getText() : juce::String{};
        };
        const auto balance = [&] { return statistic("RMS Balance"); };
        const auto correlation = [&] { return statistic("Stereo Correlation"); };
        const auto integratedRms = [&] { return statistic("Integrated RMS"); };
        const auto maxCrest = [&] { return statistic("Max Crest"); };
        const auto samplePeak = [&] { return statistic("Sample Peak"); };
        const auto sampleClips = [&] { return statistic("Sample Clips"); };
        std::uint64_t stereoRevision = 4;
        const auto sendLevels = [&](float left, float right, bool newEpoch = false) {
            const auto tap = tapFor(main);
            if (!tap) return false;
            const std::vector<float> a(static_cast<std::size_t>(overviewFft), left);
            const std::vector<float> b(static_cast<std::size_t>(overviewFft), right);
            if (newEpoch) ++stereoRevision;
            tap->write(a.data(), b.data(), overviewFft, tap->written(), true, stereoRevision);
            for (int pass = 0; pass < 12; ++pass) broker->pump();
            root.frame();
            return true;
        };

        pages.setPage(Page::Spectrum);
        choose(false);
        check(balance().isEmpty() && correlation().isEmpty() && integratedRms().isEmpty() && maxCrest().isEmpty() &&
                  samplePeak().isEmpty() && sampleClips().isEmpty() && broker->stats().spectrumLevelHistories == 0,
              "ordinary Spectrum has no pinned readouts or level history");
        choose(true);
        const auto plan = broker->stats();
        bool wroteInverted = true;
        for (int block = 0; block < 4; ++block) wroteInverted = sendLevels(.5f, -.25f) && wroteInverted;
        const auto picture = pages.detail.view.snapshot();
        const auto observedBalance = balance(), observedCorrelation = correlation();
        check(wroteInverted && picture && picture->rms[0] == .5f && picture->rms[1] == .25f &&
                  picture->correlation == -1. && observedBalance.contains("L 6.0") &&
                  observedCorrelation.contains("-1.00"),
              "pinned readouts use the primary window's RMS direction and signed anti-correlation");
        const auto observedRms = integratedRms(), observedCrest = maxCrest();
        check(observedRms.contains("L -6.0") && observedRms.contains("R -12.0") &&
                  observedCrest.contains("L 0.0") && observedCrest.contains("R 0.0"),
              "the expanded pinned second row shows stereo integrated dBFS and maximum crest");
        const auto observedPeak = samplePeak(), observedClips = sampleClips();
        check(observedPeak == "Sample peak  L -6.0  R -12.0 dBFS" &&
                  observedClips == "Sample clips  L 0  R 0",
              "expanded Spectrum reports both held sample peaks and clip counts");
        const auto published = broker->stats();
        check(published.consumers == plan.consumers && published.taps == plan.taps &&
                  published.domains == plan.domains && published.spectralPlans == plan.spectralPlans &&
                  published.stereoBalanceHistories == 1 && published.spectrumLevelHistories == 1,
              "pinned readouts do not add a consumer or plan to the existing primary and pin spectra");
        for (int frame = 0; frame < 4; ++frame) { root.frame(); broker->pump(); }
        const auto statisticsIdle = broker->stats();
        check(statisticsIdle.workerPasses == published.workerPasses && statisticsIdle.transforms == published.transforms &&
                  statisticsIdle.publications == published.publications,
              "unchanged pinned readouts do not do worker work");

        const auto fullBounds = pages.getBounds();
        const auto fullBottom = pages.detail.getBottom();
        const auto shortHeight = 2 * look::border + look::fieldHeight + look::gap +
                                 look::mixInfoHeight + 2 * look::mixChannelRowHeight;
        pages.setBounds(fullBounds.withHeight(shortHeight));
        root.frame();
        const auto shortLayout = broker->stats();
        check(balance() == observedBalance && correlation() == observedCorrelation &&
                  integratedRms().isEmpty() && maxCrest().isEmpty() && samplePeak().isEmpty() &&
                  sampleClips().isEmpty() && shortLayout.stereoBalanceHistories == 1 &&
                  shortLayout.spectrumLevelHistories == 0 && shortLayout.taps == statisticsIdle.taps &&
                  shortLayout.consumers == statisticsIdle.consumers && shortLayout.transforms == statisticsIdle.transforms &&
                  pages.detail.view.snapshot() == picture,
              "a shorter Spectrum keeps balance and correlation and drops both level rows and their history");
        pages.setBounds(fullBounds);
        root.frame();
        check(samplePeak().contains("--") && sampleClips().contains("--") &&
                  pages.detail.view.snapshot() == picture && broker->stats().spectrumLevelHistories == 1,
              "the taller layout starts a fresh shared level interval while retaining the Spectrum picture");
        const auto firstRowWidth = 2 * look::mixChannelMinWidth + look::gap + 2 * look::border;
        pages.setBounds(fullBounds.withWidth(firstRowWidth));
        root.frame();
        const auto oneRow = broker->stats();
        check(balance() == observedBalance && correlation() == observedCorrelation &&
                  integratedRms().isEmpty() && maxCrest().isEmpty() && oneRow.stereoBalanceHistories == 1 &&
                  oneRow.spectrumLevelHistories == 0 && oneRow.taps == statisticsIdle.taps &&
                  oneRow.consumers == statisticsIdle.consumers && oneRow.transforms == statisticsIdle.transforms &&
                  oneRow.spectralPlans == statisticsIdle.spectralPlans && pages.detail.view.snapshot() == picture &&
                  pages.detail.getBottom() == fullBottom + 2 * look::mixChannelRowHeight &&
                  samplePeak().isEmpty() && sampleClips().isEmpty(),
              "the intermediate width keeps balance and correlation and releases the shared level readings");
        pages.setBounds(fullBounds.withWidth(look::mixChannelMinWidth));
        root.frame();
        const auto compact = broker->stats();
        check(balance().isEmpty() && correlation().isEmpty() && pages.detail.getBottom() > fullBottom &&
                  pages.detail.view.snapshot() == picture && compact.domains == statisticsIdle.domains &&
                  compact.transforms == statisticsIdle.transforms && compact.stereoBalanceHistories == 0 &&
                  compact.spectrumLevelHistories == 0 && integratedRms().isEmpty() && maxCrest().isEmpty(),
              "compact pinned Spectrum removes the readout row without losing the primary picture");
        pages.setBounds(fullBounds);
        root.frame();
        check(!observedBalance.isEmpty() && balance() == observedBalance && correlation() == observedCorrelation &&
                  integratedRms().contains("--") && maxCrest().contains("--") &&
                  samplePeak().contains("--") && sampleClips().contains("--") &&
                  pages.detail.view.snapshot() == picture && broker->stats().spectrumLevelHistories == 1,
              "expanding the panel preserves its picture while admitting a fresh level interval");
        bool wroteReopened = true;
        for (int block = 0; block < 4; ++block) wroteReopened = sendLevels(.5f, -.25f) && wroteReopened;
        const auto reopenedPicture = pages.detail.view.snapshot();
        check(wroteReopened && reopenedPicture && reopenedPicture != picture &&
                  integratedRms() == observedRms && maxCrest() == observedCrest &&
                  !observedPeak.isEmpty() && samplePeak() == observedPeak && sampleClips() == observedClips,
              "new audio restores the level values after the row reopens");

        const auto freeze = [&](bool value) {
            auto &button = pages.detail.freezeButton;
            button.setToggleState(value, juce::dontSendNotification);
            if (button.onClick) button.onClick();
            root.frame();
        };
        freeze(true);
        check(!pages.detail.view.subscribed() && !attached(first) && !attached(wide) &&
                  pages.detail.view.snapshot() == reopenedPicture && !observedBalance.isEmpty() &&
                  balance() == observedBalance && correlation() == observedCorrelation &&
                  !observedRms.isEmpty() && integratedRms() == observedRms && maxCrest() == observedCrest &&
                  !observedPeak.isEmpty() && samplePeak() == observedPeak && sampleClips() == observedClips &&
                  broker->stats().spectrumLevelHistories == 0,
              "Freeze preserves the same statistics and releases their existing Spectrum demand");
        freeze(false);
        check(!balance().contains("L 6.0") && !correlation().contains("-1.00") &&
                  !integratedRms().contains("-6.0") && !maxCrest().contains("0.0") &&
                  samplePeak().contains("--") && sampleClips().contains("--"),
              "resuming clears frozen numbers until a fresh Spectrum window arrives");
        const bool wroteAligned = sendLevels(.25f, .5f);
        check(wroteAligned && balance().contains("R 6.0") && correlation().contains("+1.00"),
              "fresh primary samples update the stereo statistics after Freeze");
        const bool wroteOneSide = sendLevels(.5f, 0.f, true);
        check(wroteOneSide && balance().contains("L only") && correlation().contains("--"),
              "a fresh one-sided epoch has a balance direction without stereo correlation");
        const bool wroteQuiet = sendLevels(0.f, 0.f, true);
        check(wroteQuiet && balance().contains("--") && correlation().contains("--"),
              "a fresh silent epoch has neither a balance direction nor stereo correlation");

        if (const auto tap = tapFor(main)) {
            std::vector<float> left(static_cast<std::size_t>(overviewFft), .25f), right(left);
            left[0] = right[0] = 1.f;
            left[16] = right[16] = -1.f;
            left[26] = 1.25f;
            left[30] = -1.5f;
            right[26] = -2.f;
            tap->write(left.data(), right.data(), overviewFft, tap->written(), true, stereoRevision);
            for (int pass = 0; pass < 12; ++pass) broker->pump();
            root.frame();
            const auto clippedPeak = samplePeak(), clippedCount = sampleClips();
            check(clippedPeak == "Sample peak  L +3.5  R +6.0 dBFS" &&
                      clippedCount == "Sample clips  L 2  R 1",
                  "sample clipping counts each strict overshoot once, including negative samples, with unity excluded");
            const bool wroteQuieter = sendLevels(.125f, .25f);
            const auto quietPicture = pages.detail.view.snapshot();
            check(wroteQuieter && quietPicture && quietPicture->peak[0] == .125f && quietPicture->peak[1] == .25f &&
                      !clippedPeak.isEmpty() && samplePeak() == clippedPeak && sampleClips() == clippedCount,
                  "sample peaks and clip counts remain held after louder samples leave the Spectrum window");
            {
                auto rawSibling = broker->acquire({main, pages.detail.point(), overviewFft, pages.detail.view.channel()});
                auto ordinary = broker->acquire({main, pages.detail.point(), normalFft, pages.detail.view.channel()});
                for (int pass = 0; pass < 12; ++pass) broker->pump();
                root.frame();
                const auto siblingBefore = rawSibling.read();
                const auto ordinaryBefore = ordinary.read();
                const auto beforeReset = broker->stats();
                const auto resetAttachments = attachments;
                const auto resetHead = History(doc->store()).head();

                // These overs arrived before Reset, but the shared worker has not read them yet.
                tap->write(left.data(), right.data(), overviewFft, tap->written(), true, stereoRevision);
                auto &reset = pages.detail.resetButton;
                check(rawSibling && ordinary && siblingBefore == quietPicture && ordinaryBefore && reset.onClick &&
                          beforeReset.spectrumLevelHistories == 1,
                      "the Reset oracle has the held levels and two active raw observers");
                if (reset.onClick) reset.onClick();
                root.frame();

                const auto afterReset = broker->stats();
                check(!pages.detail.view.snapshot() && integratedRms().contains("--") && maxCrest().contains("--") &&
                          samplePeak().contains("--") && sampleClips().contains("--"),
                      "Reset clears the visible Spectrum interval before the next worker publication");
                check(tapFor(main) == tap && attachments == resetAttachments && History(doc->store()).head() == resetHead &&
                          afterReset.consumers == beforeReset.consumers && afterReset.taps == beforeReset.taps &&
                          afterReset.domains == beforeReset.domains && afterReset.spectralPlans == beforeReset.spectralPlans &&
                          afterReset.transforms == beforeReset.transforms && afterReset.spectrumLevelHistories == 1 &&
                          rawSibling.read() == siblingBefore && ordinary.read() == ordinaryBefore,
                      "Reset leaves the authored project, physical tap, FFT demand and other raw pictures intact");

                for (int pass = 0; pass < 12; ++pass) broker->pump();
                root.frame();
                const auto bufferedSibling = rawSibling.read();
                check(bufferedSibling && bufferedSibling != siblingBefore &&
                          bufferedSibling->peak[0] == 1.5f && bufferedSibling->peak[1] == 2.f &&
                          ordinary.read() && ordinary.read() != ordinaryBefore &&
                          samplePeak().contains("--") && sampleClips().contains("--"),
                      "raw observers receive the queued overs while the reset level interval waits for new audio");

                const bool wroteFresh = sendLevels(.125f, .25f);
                const auto fresh = pages.detail.view.snapshot();
                const auto afterFresh = broker->stats();
                check(wroteFresh && fresh && fresh == rawSibling.read() && fresh != bufferedSibling &&
                          samplePeak() == "Sample peak  L -18.1  R -12.0 dBFS" && sampleClips() == "Sample clips  L 0  R 0" &&
                          History(doc->store()).head() == resetHead && tapFor(main) == tap && attachments == resetAttachments &&
                          afterFresh.domains == beforeReset.domains && afterFresh.spectralPlans == beforeReset.spectralPlans,
                      "fresh quiet samples reset held levels and retain the original shared Spectrum plan");
            }
            choose(false);
            check(samplePeak().isEmpty() && sampleClips().isEmpty() && broker->stats().spectrumLevelHistories == 0,
                  "turning off pinned Spectrum drops its held sample statistics");
            choose(true);
            check(samplePeak().contains("--") && sampleClips().contains("--"),
                  "reopening pinned Spectrum waits for data from a fresh sample interval");
            check(sendLevels(.125f, .25f) && samplePeak() == "Sample peak  L -18.1  R -12.0 dBFS" &&
                      sampleClips() == "Sample clips  L 0  R 0",
                  "the reopened level interval does not keep peaks or clips from the old one");
        } else {
            check(false, "the sample statistics oracle has its primary source");
        }

        const bool balanceSourceSelected = select(pages.sourceBox, "Pinned 12");
        const bool infoWasVisible = commands.infoView();
        const bool infoClosed = !infoWasVisible || commands.setInfoView(false, error);
        root.frame();
        for (int pass = 0; pass < 12; ++pass) broker->pump();
        root.frame();
        const auto balanceTap = tapFor(12);
        check(balanceSourceSelected && infoClosed && balanceTap, "the two-second oracle has an isolated Spectrum source");
        if (const auto &tap = balanceTap) {
            constexpr int window = 4096, hop = 1024, hops = 90;
            constexpr float bed = .0625f;
            constexpr std::int64_t start = 200000;
            const auto epoch = ++stereoRevision;
            const auto beforeHistory = broker->stats();
            std::array<float, window> left, right;
            left.fill(bed); right.fill(bed);
            std::fill_n(left.begin(), 256, 1.f);
            std::fill_n(right.begin() + 256, 768, .75f);
            tap->write(left.data(), right.data(), window, start, true, epoch);
            broker->pump();

            left.fill(bed);
            for (int current = 0; current < hops; ++current) {
                // The newest window is inverted while most of the two-second history is in phase.
                right.fill(current >= hops - window / hop ? -bed : bed);
                tap->write(left.data(), right.data(), hop, start + window + current * hop, true, epoch);
                if (current % 3 == 2) broker->pump();
            }
            root.frame();
            const auto trailing = pages.detail.view.snapshot();
            const auto afterHistory = broker->stats();
            // At 96,256 frames only the first 256 frames have aged out.
            // The power sums are 375 and 804, giving R 3.3122478 dB.
            check(trailing && trailing->transportRevision == epoch && trailing->firstStamp == start + hops * hop &&
                      trailing->rms[0] == bed && trailing->rms[1] == bed && trailing->correlation == -1. &&
                      balance().contains("R 3.3") && correlation().contains("-1.00"),
                  "pinned balance retains the exact two-second tail while correlation uses only the current window");
            check(afterHistory.taps == beforeHistory.taps && afterHistory.domains == beforeHistory.domains &&
                      afterHistory.spectralPlans == beforeHistory.spectralPlans &&
                      afterHistory.stereoBalanceHistories == 1 &&
                      afterHistory.analysisWindows == beforeHistory.analysisWindows + hops + 1 &&
                      afterHistory.transforms == beforeHistory.transforms + hops,
                  "two-second balance adds no tap or transform including the Mid spectrum's inverted zero window");

            right.fill(bed);
            const auto heardEnd = start + window + hops * hop;
            tap->write(left.data(), right.data(), hop, heardEnd, true, epoch);
            broker->pump();
            root.frame();
            check(balance().contains("Centre") && correlation().contains("-0.50"),
                  "the next hop ages out the remaining imbalance independently of the short correlation");

            left.fill(.125f); right.fill(0.f);
            const auto gapStart = heardEnd + hop + 17;
            tap->write(left.data(), right.data(), window, gapStart, true, epoch);
            broker->pump();
            root.frame();
            const auto resumed = pages.detail.view.snapshot();
            check(trailing && resumed && resumed->transportRevision == epoch &&
                      resumed->sourceEpoch > trailing->sourceEpoch && resumed->firstStamp == gapStart &&
                      balance().contains("L only") && correlation().contains("--"),
                  "a same-revision advancing gap starts a fresh balance history at the new heard position");
        }
        if (infoWasVisible) commands.setInfoView(true, error);
        pages.followSelection(false);
        root.frame();

        const auto findTruePeak = [](const auto &walk, juce::Component &node) -> juce::ToggleButton * {
            if (auto *button = dynamic_cast<juce::ToggleButton *>(&node);
                button && button->getTitle() == "Spectrum True Peak") return button;
            for (auto *child : node.getChildren())
                if (auto *button = walk(walk, *child)) return button;
            return nullptr;
        };
        auto *truePeakSwitch = findTruePeak(findTruePeak, pages);
        check(truePeakSwitch && truePeakSwitch->isShowing() && truePeakSwitch->getButtonText() == "TP" &&
                  !truePeakSwitch->getToggleState() && broker->stats().spectrumTruePeakHistories == 0,
              "expanded Spectrum offers an accessible TP switch that starts off without analysis demand");
        if (truePeakSwitch) {
            const auto truePeak = [&] { return statistic("True Peak"); };
            const auto changeTruePeak = [&](bool enabled) {
                truePeakSwitch->setToggleState(enabled, juce::dontSendNotification);
                if (truePeakSwitch->onClick) truePeakSwitch->onClick();
                root.frame();
            };
            const auto *peakLabel = findLabel(findLabel, pages, "Sample Peak");
            auto *statsPanel = truePeakSwitch->getParentComponent();
            check(peakLabel && statsPanel &&
                      statsPanel->getComponentAt(truePeakSwitch->getBounds().getCentre()) == truePeakSwitch &&
                      !statsPanel->getComponentAt(peakLabel->getBounds().getCentre()),
                  "the statistics switch receives clicks and the readout stays mouse-transparent");
            const auto sendInterSample = [&] {
                const auto tap = tapFor(main);
                if (!tap) return false;
                constexpr std::array quadrature{.8f, .8f, -.8f, -.8f};
                std::vector<float> signal(static_cast<std::size_t>(overviewFft));
                for (std::size_t i = 0; i < signal.size(); ++i) signal[i] = quadrature[i % quadrature.size()];
                tap->write(signal.data(), signal.data(), overviewFft, tap->written(), true, stereoRevision);
                for (int pass = 0; pass < 12; ++pass) broker->pump();
                root.frame();
                return true;
            };
            ++stereoRevision;
            const bool wroteWarm = sendInterSample();
            const auto peakBaseline = broker->stats();
            const bool wroteOff = sendInterSample();
            const auto off = broker->stats();
            const auto samplePicture = pages.detail.view.snapshot();
            const auto uncounted = sampleClips();
            check(wroteWarm && wroteOff && samplePicture &&
                      samplePeak() == "Sample peak  L -1.9  R -1.9 dBFS" && uncounted == "Sample clips  L 0  R 0",
                  "the quarter-rate fixture stays below full scale at its sample points");
            changeTruePeak(true);
            const auto on = broker->stats();
            check(truePeak().contains("--") && truePeak().endsWith("dBTP") && samplePeak().isEmpty() &&
                      sampleClips() == uncounted && pages.detail.view.snapshot() == samplePicture &&
                      on.spectrumTruePeakHistories == 1 && on.taps == off.taps && on.domains == off.domains &&
                      on.consumers == off.consumers && on.spectralPlans == off.spectralPlans && on.transforms == off.transforms,
                  "enabling True Peak preserves the Raw picture, sample clips and existing demand while waiting");
            const bool wroteOn = sendInterSample();
            const auto analysed = broker->stats();
            check(wroteOn && truePeak().contains("L +") && truePeak().contains("R +") && truePeak().endsWith("dBTP") &&
                      sampleClips() == uncounted && off.transforms > peakBaseline.transforms &&
                      analysed.transforms - on.transforms == off.transforms - peakBaseline.transforms,
                  "True Peak detects an intersample overshoot with the same FFT count and zero clipped samples");
            changeTruePeak(false);
            check(truePeak().isEmpty() && samplePeak() == "Sample peak  L -1.9  R -1.9 dBFS" &&
                      sampleClips() == uncounted && broker->stats().spectrumTruePeakHistories == 0,
                  "switching off releases True Peak and immediately restores the held Sample Peak");
            changeTruePeak(true);
            check(truePeak().contains("--") && sampleClips() == uncounted,
                  "re-enabling cannot show a True Peak value from an earlier interval");
            const bool wroteFresh = sendInterSample();
            const auto heldText = truePeak();
            const auto frozenPicture = pages.detail.view.snapshot();
            freeze(true);
            check(wroteFresh && heldText.contains("L +") && truePeak() == heldText && truePeakSwitch->getToggleState() &&
                      !pages.detail.view.subscribed() && broker->stats().spectrumTruePeakHistories == 0 &&
                      pages.detail.view.snapshot() == frozenPicture,
                  "Freeze retains True Peak values and switch state while releasing its analysis");
            freeze(false);
            check(truePeakSwitch->getToggleState() && truePeak().contains("--") &&
                      broker->stats().spectrumTruePeakHistories == 1,
                  "resuming keeps True Peak enabled and waits for fresh audio");
            const bool wroteResumed = sendInterSample();
            const auto beforeCompact = pages.detail.view.snapshot();
            pages.setBounds(fullBounds.withWidth(firstRowWidth));
            root.frame();
            check(wroteResumed && beforeCompact && !truePeakSwitch->isShowing() && truePeakSwitch->getToggleState() &&
                      truePeak().isEmpty() && pages.detail.view.snapshot() == beforeCompact &&
                      broker->stats().spectrumTruePeakHistories == 0,
                  "the compact layout releases True Peak without changing its choice or Raw picture");
            pages.setBounds(fullBounds);
            root.frame();
            check(truePeakSwitch->isShowing() && truePeakSwitch->getToggleState() && truePeak().contains("--") &&
                      broker->stats().spectrumTruePeakHistories == 1,
                  "reopening the full layout retains the True Peak choice and starts with no old measurement");
        }
        pages.setVisible(false);
        const auto hidden = broker->stats();
        for (int frame = 0; frame < 4; ++frame) { root.frame(); broker->pump(); }
        const auto hiddenIdle = broker->stats();
        check(balance().isEmpty() && correlation().isEmpty() && !pages.detail.view.subscribed() &&
                  !attached(first) && !attached(wide) && hiddenIdle.workerPasses == hidden.workerPasses &&
                  hiddenIdle.transforms == hidden.transforms && hiddenIdle.publications == hidden.publications &&
                  hiddenIdle.stereoBalanceHistories == 0 && hiddenIdle.spectrumLevelHistories == 0 &&
                  integratedRms().isEmpty() && maxCrest().isEmpty() && samplePeak().isEmpty() &&
                  sampleClips().isEmpty() && hiddenIdle.spectrumTruePeakHistories == 0 &&
                  (!truePeakSwitch || (!truePeakSwitch->isShowing() && truePeakSwitch->getToggleState())),
              "a hidden Spectrum exposes no readouts and adds no worker activity");
        if (truePeakSwitch) {
            pages.setVisible(true);
            root.frame();
            check(truePeakSwitch->isShowing() && truePeakSwitch->getToggleState() &&
                      statistic("True Peak").contains("--") && broker->stats().spectrumTruePeakHistories == 1,
                  "reshowing the Spectrum restores the session's True Peak choice with fresh-only data");
        }
    }
    detachPeer();
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
    MixFocusTestAccess::moveScopePointer(focus, .5f, .25f);
    check(MixFocusTestAccess::scopeCursorText(focus).isEmpty(),
          "unaligned individual layers do not invent a common time or level cursor");
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
        check(MixFocusTestAccess::scopePhase(focus) == 1., "Overlay measures the summed stereo pair");
        layout->setSelectedItemIndex(1, juce::sendNotificationSync);
        root.frame();
        check(MixFocusTestAccess::scopePhase(focus) == -1., "Layers measures the first two members' Mid signals");
        layout->setSelectedItemIndex(2, juce::sendNotificationSync);
        root.frame();
        check(MixFocusTestAccess::scopePhase(focus) == -1., "Stack keeps the two-member phase operands");
        layout->setSelectedItemIndex(3, juce::sendNotificationSync);
        root.frame();
        check(MixFocusTestAccess::scopePhase(focus) == 1., "Sum uses the signed combination's left and right");
        layout->setSelectedItemIndex(4, juce::sendNotificationSync);
        root.frame();
        check(!MixFocusTestAccess::scopePhase(focus), "Difference has no phase bar demand");
        juce::ToggleButton *residual = nullptr;
        for (auto *child : focus.getChildren())
            if (child->getTitle() == "Difference spectrum") residual = dynamic_cast<juce::ToggleButton *>(child);
        check(residual && residual->isShowing() && !MixFocusTestAccess::requestsResidualSpectrum(focus),
              "Difference offers its spectrum without requesting it by default");
        if (residual) {
            focus.select(second);
            residual->setToggleState(true, juce::dontSendNotification);
            residual->onClick();
            check(MixFocusTestAccess::requestsResidualSpectrum(focus), "Spectrum explicitly requests the aligned delta");
            residual->setToggleState(false, juce::dontSendNotification);
            residual->onClick();
            check(!MixFocusTestAccess::requestsResidualSpectrum(focus), "returning to the waveform drops spectrum demand");
        }
        layout->setSelectedItemIndex(0, juce::sendNotificationSync);
        root.frame();
        const auto after = broker->stats();
        check(after.consumers == before.consumers && after.taps == before.taps &&
                  after.publications == before.publications && after.cursorReads == before.cursorReads &&
                  opened.size() == openedBefore && closed.size() == closedBefore,
              "scope display choices do not acquire, release or reprocess audio sources");
    };
    checkScopeDisplays();
    const auto checkLayerAdjustments = [&] {
        writeSum(.25f, .25f, .25f, .25f);
        focus.select(second);
        const auto beforeLayer = root.current();
        const auto beforeUndo = root.undoTitle(false);
        const auto beforeMeasure = broker->stats();
        const auto choose = [&](juce::String label) {
            const auto menu = MixFocusTestAccess::layerMenu(focus);
            for (juce::PopupMenu::MenuItemIterator it(menu); it.next();) {
                const auto &item = it.getItem();
                if (item.text == label && item.isEnabled) {
                    MixFocusTestAccess::chooseLayerItem(focus, item.itemID);
                    return true;
                }
            }
            return false;
        };
        const auto reading = MixFocusTestAccess::scopeReading(focus);
        check(reading && reading->peak[0] == .5 && reading->peak[1] == .5,
              "identical recorded layers sum to their combined level before the preview edit");
        check(choose("Invert polarity"), "the selected layer offers a polarity preview");
        const auto cancelled = MixFocusTestAccess::scopeReading(focus);
        check(cancelled && cancelled->peak[0] == 0. && cancelled->peak[1] == 0.,
              "inversion cancels equal raw layers in both channels before display binning");
        check(choose("Later by 10 samples") && choose("Later by 1 sample") &&
                  MixFocusTestAccess::layerAdjustment(focus) == FocusScopeLayerAdjustment{11, true},
              "coarse and fine adjustments compose as eleven samples for this member");
        const auto shifted = MixFocusTestAccess::scopeReading(focus);
        check(shifted && shifted->peak[0] == .25 && shifted->rms[0] > 0. && shifted->rms[0] < .1,
              "the delayed layer leaves only a short leading edge in the signed sum");
        check(choose("Show milliseconds") && choose("Earlier by 0.021 ms") &&
                  MixFocusTestAccess::layerAdjustment(focus) == FocusScopeLayerAdjustment{10, true},
              "switching readout units retains the exact offset and one-sample step at 48 kHz");
        focus.select(first);
        check(MixFocusTestAccess::layerAdjustment(focus) == FocusScopeLayerAdjustment{},
              "other members retain independent preview settings");
        focus.select(second);
        check(choose("Reset layer") && choose("Show samples") &&
                  MixFocusTestAccess::layerAdjustment(focus) == FocusScopeLayerAdjustment{},
              "reset restores this layer without changing the other controls");
        const auto reset = MixFocusTestAccess::scopeReading(focus);
        const auto afterMeasure = broker->stats();
        check(reset && reset->peak[0] == .5 && beforeLayer == root.current() && root.undoTitle(false) == beforeUndo &&
                  afterMeasure.transforms == beforeMeasure.transforms && afterMeasure.taps == beforeMeasure.taps &&
                  afterMeasure.consumers == beforeMeasure.consumers,
              "layer actions restore the original sum and write no project, undo, tap or transform");
    };
    checkLayerAdjustments();
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
    juce::ComboBox *musicalWindow = nullptr;
    for (auto *component : focus.getChildren())
        if (component->getTitle() == "Scope window")
            musicalWindow = dynamic_cast<juce::ComboBox *>(component);
    check(musicalWindow && musicalWindow->isVisible() && musicalWindow->getSelectedId() == 2 &&
              musicalWindow->getText() == "One Bar" && musicalWindow->getItemId(0) == 3 &&
              musicalWindow->getItemText(0) == "1/16 Note" && musicalWindow->getItemId(1) == 1,
          "Focus Scope exposes a musical window selector without changing the default bar");
    juce::ToggleButton *freeze = nullptr;
    for (auto *component : focus.getChildren())
        if (component->getTitle() == "Freeze Focus Scope")
            freeze = dynamic_cast<juce::ToggleButton *>(component);
    check(freeze && freeze->isVisible() && freeze->getButtonText() == "Freeze" && !freeze->isEnabled(),
          "Focus Scope shows Freeze but cannot hold a picture before it has received aligned audio");
    juce::Label *differenceRms = nullptr;
    for (auto *component : focus.getChildren())
        if (component->getTitle() == "Scope Difference RMS")
            differenceRms = dynamic_cast<juce::Label *>(component);
    check(differenceRms && !differenceRms->isVisible(),
          "Scope exposes an accessible Difference RMS only for a valid Difference waveform");
    const auto rmsText = [&]() -> juce::String {
        return differenceRms && differenceRms->isVisible() ? differenceRms->getText() : juce::String{};
    };
    const auto hasRms = [&](const char *level) { return rmsText().contains(level) && rmsText().contains("dBFS"); };
    juce::ToggleButton *truePeakSwitch = nullptr;
    juce::Label *capturedTruePeak = nullptr;
    for (auto *component : focus.getChildren()) {
        if (component->getTitle() == "Focus Scope true peak")
            truePeakSwitch = dynamic_cast<juce::ToggleButton *>(component);
        if (component->getTitle() == "Scope captured true peak")
            capturedTruePeak = dynamic_cast<juce::Label *>(component);
    }
    check(truePeakSwitch && capturedTruePeak && !truePeakSwitch->isVisible() && !capturedTruePeak->isVisible(),
          "the Focus captured true-peak controls are accessible and hidden in the initial Overlay");
    const auto &meters = initial.meters();
    check(root->playing() && root->bpm() == 120. && doc->mailbox().sampleRate() == rate &&
              (meters.empty() || (meters[0].numerator == 4 && meters[0].denominator == 4)) &&
              a->capacity() == 12000 && b->capacity() == 12000 && broker->stats().taps == 2,
          "one-bar Scope runs at 48 kHz, 120 BPM and 4/4 on two quarter-second rings");
    MixFocusTestAccess::moveScopePointer(focus, .5f, .25f);
    check(MixFocusTestAccess::scopeCursorText(focus).isEmpty(),
          "Scope cannot supply a cursor before it has received source audio");
    MixFocusTestAccess::dragScopeRegion(focus, {.5f, .25f}, {.625f, .25f});
    check(MixFocusTestAccess::scopeRegionText(focus).isEmpty(),
          "Scope cannot measure a region before it receives source audio");
    MixFocusTestAccess::moveScopePointer(focus, .5f, .25f);

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
    const auto head = History(doc->store()).head();
    check(MixFocusTestAccess::scopeCursorText(focus).contains("48000"),
          "the cursor acquires the completed bar when the pointer has stayed still");
    const auto pointer = MixFocusTestAccess::moveScopePointer(focus, .5f, .25f);
    const auto cursor = MixFocusTestAccess::scopeCursorText(focus).removeCharacters(",");
    check(cursor.contains("1.3") && cursor.contains("48000") && cursor.contains("1000") &&
              cursor.contains("ms") && cursor.contains("-6.0") && cursor.contains("dBFS"),
          "the midpoint of the heard bar shows beat 3, absolute sample 48000, 1000 ms and the half-scale ordinate");
    const auto windowBeforeZoom = MixFocusTestAccess::scopeWaveformRange(focus);
    const auto statsBeforeZoom = broker->stats();
    const auto readingBeforeZoom = MixFocusTestAccess::scopeReading(focus);
    MixFocusTestAccess::wheelScope(focus, .5f, .25f, 1.f, 0);
    MixFocusTestAccess::wheelScope(focus, .5f, -.1f, 1.f);
    check(MixFocusTestAccess::scopeCursorText(focus).removeCharacters(",") == cursor,
          "plain wheel and Alt-wheel outside the waveform do not change its vertical scale");
    MixFocusTestAccess::wheelScope(focus, .5f, .25f, 1.f);
    const auto zoomed = MixFocusTestAccess::scopeCursorText(focus).removeCharacters(",");
    const auto zoomedWindow = MixFocusTestAccess::scopeWaveformRange(focus);
    const auto zoomedReading = MixFocusTestAccess::scopeReading(focus);
    check(!cursor.isEmpty() && zoomed == cursor.replace("-6.0 dBFS", "-12.0 dBFS") &&
              MixFocusTestAccess::sumPulseAt(focus, .125f, 1.f) &&
              zoomedWindow && windowBeforeZoom && zoomedWindow->first == windowBeforeZoom->first &&
              zoomedWindow->frames == windowBeforeZoom->frames && zoomedWindow->sampleRate == windowBeforeZoom->sampleRate &&
              zoomedReading && readingBeforeZoom && zoomedReading->peak == readingBeforeZoom->peak &&
              zoomedReading->rms == readingBeforeZoom->rms,
          "Alt-wheel zooms in by 6 dB and refreshes a stationary cursor without changing samples, time or meters");
    MixFocusTestAccess::wheelScope(focus, .5f, .25f, -1.f);
    const auto statsAfterZoom = broker->stats();
    check(MixFocusTestAccess::scopeCursorText(focus).removeCharacters(",") == cursor &&
              MixFocusTestAccess::sumPulseAt(focus, .125f, .75f) &&
              statsAfterZoom.taps == statsBeforeZoom.taps && statsAfterZoom.consumers == statsBeforeZoom.consumers &&
              statsAfterZoom.transforms == statsBeforeZoom.transforms && statsAfterZoom.cursorReads == statsBeforeZoom.cursorReads &&
              statsAfterZoom.frameRequests == statsBeforeZoom.frameRequests &&
              statsAfterZoom.workerPasses == statsBeforeZoom.workerPasses && History(doc->store()).head() == head,
          "reversing the wheel restores the original waveform and cursor with no analysis work or authored edit");
    constexpr auto panModifiers = juce::ModifierKeys::altModifier | juce::ModifierKeys::shiftModifier;
    const auto gridBeforePan = MixFocusTestAccess::scopeGrid(focus);
    MixFocusTestAccess::wheelScope(focus, .5f, .25f, 1.f, panModifiers);
    const auto panned = MixFocusTestAccess::scopeCursorText(focus).removeCharacters(",");
    check(panned == cursor.replace("-6.0 dBFS", "-2.5 dBFS") &&
              MixFocusTestAccess::sumPulseAt(focus, .125f, .50f) &&
              MixFocusTestAccess::scopeGrid(focus) == gridBeforePan &&
              MixFocusTestAccess::scopeWaveformRange(focus) == windowBeforeZoom,
          "Alt+Shift-wheel pans the waveform and cursor vertically around the same heard sample and grid");
    MixFocusTestAccess::wheelScope(focus, .5f, .25f, -1.f, panModifiers);
    const auto statsAfterPan = broker->stats();
    check(MixFocusTestAccess::scopeCursorText(focus).removeCharacters(",") == cursor &&
              MixFocusTestAccess::sumPulseAt(focus, .125f, .75f) &&
              statsAfterPan.taps == statsAfterZoom.taps && statsAfterPan.transforms == statsAfterZoom.transforms &&
              statsAfterPan.frameRequests == statsAfterZoom.frameRequests &&
              statsAfterPan.workerPasses == statsAfterZoom.workerPasses && History(doc->store()).head() == head,
          "reversing the pan restores the trace exactly without extra analysis or project edits");
    const auto timeZoomModifier = juce::ModifierKeys::commandModifier;
    const auto timePanModifier = juce::ModifierKeys::shiftModifier;
    MixFocusTestAccess::wheelScope(focus, .5f, .25f, 1.f, timeZoomModifier);
    MixFocusTestAccess::moveScopePointer(focus, .25f, .25f);
    const auto visibleTime = MixFocusTestAccess::scopeVisibleRange(focus);
    const auto timeGrid = MixFocusTestAccess::scopeGrid(focus);
    const auto waveformPlot = MixFocusTestAccess::scopePlot(focus);
    check(visibleTime == FocusScopeRange{24000, 48000, rate} && timeGrid.size() == 2 &&
              timeGrid.front().first == waveformPlot.getX() &&
              std::abs(MixFocusTestAccess::transformedScopeX(focus, .375f) - .25f) < .001f &&
              MixFocusTestAccess::scopeCursorText(focus).removeCharacters(",").contains("36000 samples"),
          "time zoom shows the centred half-open sample range on the grid, waveform and cursor");
    MixFocusTestAccess::dragScopeRegion(focus, {.5f, .25f}, {.75f, .25f});
    const auto zoomRegion = MixFocusTestAccess::scopeRegionText(focus).removeCharacters(",");
    check(zoomRegion.contains("48000 to 60000 samples") && zoomRegion.contains("250.0 ms"),
          "a region drawn in the smaller viewport still measures the heard samples and duration");
    MixFocusTestAccess::wheelScope(focus, .5f, .25f, -1.f, timePanModifier);
    check(MixFocusTestAccess::scopeVisibleRange(focus) == FocusScopeRange{36000, 48000, rate} &&
              MixFocusTestAccess::scopeRegionText(focus).removeCharacters(",") == zoomRegion,
          "panning moves the sample viewport but preserves the stored region and its measurements");
    MixFocusTestAccess::wheelScope(focus, .5f, .25f, 1.f, timePanModifier);
    MixFocusTestAccess::dragScopeRegion(focus, {.5f, .25f}, {.5f, .25f});
    MixFocusTestAccess::wheelScope(focus, .5f, .25f, -1.f, timeZoomModifier);
    const auto statsAfterTime = broker->stats();
    check(windowBeforeZoom && MixFocusTestAccess::scopeVisibleRange(focus) == *windowBeforeZoom &&
              MixFocusTestAccess::scopeWaveformRange(focus) == windowBeforeZoom &&
              MixFocusTestAccess::scopeCursorText(focus).removeCharacters(",") == cursor &&
              statsAfterTime.taps == statsAfterPan.taps && statsAfterTime.transforms == statsAfterPan.transforms &&
              statsAfterTime.frameRequests == statsAfterPan.frameRequests &&
              statsAfterTime.workerPasses == statsAfterPan.workerPasses && History(doc->store()).head() == head,
          "reversing the time view restores the original range and cursor without requesting audio or editing the project");
    MixFocusTestAccess::moveScopePointer(focus, .5f, .75f);
    check(MixFocusTestAccess::scopeCursorText(focus).contains("-6.0 dBFS"),
          "the positive and negative half-scale ordinates show the same dBFS magnitude");
    static_cast<juce::Component &>(focus).mouseExit(pointer);
    check(MixFocusTestAccess::scopeCursorText(focus).isEmpty(), "leaving the Scope clears its pointer readout");

    const juce::Point<float> regionStart{.5f, .25f}, regionEnd{.625f, .25f};
    MixFocusTestAccess::scopeRegionPointer(focus, &juce::Component::mouseDown, regionStart, regionStart);
    MixFocusTestAccess::scopeRegionPointer(focus, &juce::Component::mouseDrag, regionStart, regionEnd);
    const auto forward = MixFocusTestAccess::scopeRegionText(focus);
    check(forward.contains("48000") && forward.contains("60000") && forward.contains("1000.0") &&
              forward.contains("1250.0") && forward.contains("250.0 ms") && forward.contains("4.0 Hz"),
          "dragging from 50 to 62.5 percent measures exactly 12000 samples of the heard 48 kHz bar");
    MixFocusTestAccess::scopeRegionPointer(focus, &juce::Component::mouseUp, regionStart, regionEnd);
    MixFocusTestAccess::moveScopePointer(focus, .75f, .25f);
    check(!forward.isEmpty() && MixFocusTestAccess::scopeRegionText(focus) == forward,
          "releasing the pointer commits the heard region and ordinary hover leaves it in place");
    MixFocusTestAccess::wheelScope(focus, .75f, .25f, 1.f);
    check(!forward.isEmpty() && MixFocusTestAccess::scopeRegionText(focus) == forward &&
              MixFocusTestAccess::scopeCursorText(focus).isEmpty(),
          "vertical zoom retains the committed heard region and its priority over hover");
    MixFocusTestAccess::wheelScope(focus, .75f, .25f, -1.f);
    MixFocusTestAccess::dragScopeRegion(focus, regionEnd, regionStart);
    check(!forward.isEmpty() && MixFocusTestAccess::scopeRegionText(focus) == forward,
          "reversing the drag gives the same sorted endpoints, period and pitch");

    MixFocusTestAccess::scopeRegionPointer(focus, &juce::Component::mouseDown, regionStart, regionStart);
    MixFocusTestAccess::scopeRegionPointer(focus, &juce::Component::mouseUp, regionStart, regionEnd);
    check(!forward.isEmpty() && MixFocusTestAccess::scopeRegionText(focus) == forward,
          "the last coordinate of a fast press and release is included without an intervening drag event");

    // 1/128 of a 96000-sample bar is exactly 750 samples. A 64 Hz
    // reciprocal is nearest C1 under the DAW's middle-C convention.
    MixFocusTestAccess::dragScopeRegion(focus, regionStart, {.5078125f, .25f});
    const auto pitch = MixFocusTestAccess::scopeRegionText(focus);
    check(pitch.contains("48000") && pitch.contains("48750") && pitch.contains("64.0 Hz") &&
              pitch.contains("C1"), "the reciprocal period uses the same note convention as Spectrum");
    MixFocusTestAccess::dragScopeRegion(focus, regionStart, regionStart);
    check(MixFocusTestAccess::scopeRegionText(focus).isEmpty(),
          "a zero-length click clears the previous region without inventing a frequency");
    const auto hovered = broker->stats();
    check(hovered.consumers == active.consumers && hovered.taps == active.taps &&
              hovered.spectralPlans == active.spectralPlans && hovered.transforms == active.transforms &&
              hovered.cursorReads == active.cursorReads && hovered.frameRequests == active.frameRequests &&
              hovered.workerPasses == active.workerPasses && History(doc->store()).head() == head,
          "Scope cursor and region movement perform no analysis and author no project edit");

    MixFocusTestAccess::scopeRegionPointer(focus, &juce::Component::mouseDown, regionStart, regionStart);
    MixFocusTestAccess::scopeRegionPointer(focus, &juce::Component::mouseDrag, regionStart, regionEnd);
    const auto previousBounds = focus.getBounds();
    focus.setBounds(previousBounds.withWidth(previousBounds.getWidth() - 8));
    MixFocusTestAccess::scopeRegionPointer(focus, &juce::Component::mouseUp, regionStart, regionEnd);
    check(MixFocusTestAccess::scopeRegionText(focus).isEmpty(),
          "a resize during the held drag cannot combine the old and new plot coordinates");
    focus.setBounds(previousBounds);
    frame();

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
        MixFocusTestAccess::dragScopeRegion(focus, regionStart, regionEnd);
        const bool wasMeasurable = !MixFocusTestAccess::scopeRegionText(focus).isEmpty();
        select(FocusScopeChannels::Mid);
        check(wasMeasurable && MixFocusTestAccess::scopeRegionText(focus).isEmpty(),
              "changing channel projection clears the region tied to the previous projection");
        check(!MixFocusTestAccess::sumPulseAt(focus, .125f, .75f),
              "selecting Mid never presents the old stereo bins as Mid");
        writeUntil(2 * bar);
        check(MixFocusTestAccess::scopeInterval(focus, bar, static_cast<std::size_t>(bar), rate) &&
                  !MixFocusTestAccess::sumPulseAt(focus, .125f, .75f) &&
                  MixFocusTestAccess::sumPulseAt(focus, .875f, -.3125f),
              "Mid cancels opposed stereo and halves a single-sided pulse in the displayed bar");
        MixFocusTestAccess::moveScopePointer(focus, .5f, .25f);
        const auto later = MixFocusTestAccess::scopeCursorText(focus).removeCharacters(",");
        check(later.contains("2.3") && later.contains("144000") && later.contains("3000") && later.contains("ms"),
              "the same screen coordinate on the next captured bar uses that bar's heard origin");
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
        if (truePeakSwitch && capturedTruePeak) {
            display->setSelectedId(4, juce::sendNotificationSync);
            frame();
            check(truePeakSwitch->isVisible() && truePeakSwitch->isEnabled() && !truePeakSwitch->getToggleState() &&
                      !capturedTruePeak->isVisible() && MixFocusTestAccess::hasScopeSummarySpace(focus),
                  "Stereo Sum makes captured true peak optional and leaves the unrequested readout hidden");
        }
        display->setSelectedId(3, juce::sendNotificationSync);
        check(!truePeakSwitch || !truePeakSwitch->isVisible(), "Stack leaves the Sum true-peak switch hidden");
        frame();
        MixFocusTestAccess::moveScopePointer(focus, .5f, .625f);
        const auto stacked = MixFocusTestAccess::scopeCursorText(focus);
        check(stacked.contains("-6.0 dBFS"), "the second Stack lane uses its own signed full-scale axis");
        MixFocusTestAccess::wheelScope(focus, .5f, .625f, 1.f);
        check(!stacked.isEmpty() && MixFocusTestAccess::scopeCursorText(focus) ==
              stacked.replace("-6.0 dBFS", "-12.0 dBFS"),
              "the second Stack lane uses the same vertical zoom without shifting its heard sample");
        MixFocusTestAccess::wheelScope(focus, .5f, .625f, -1.f);
        const juce::Point<float> stackStart{.5f, .625f}, stackEnd{.625f, .625f};
        MixFocusTestAccess::dragScopeRegion(focus, stackStart, stackEnd);
        check(MixFocusTestAccess::scopeRegionText(focus).contains("240000") &&
                  MixFocusTestAccess::scopeRegionText(focus).contains("252000"),
              "a region in the second Stack lane uses that lane's heard interval");
        MixFocusTestAccess::dragScopeRegion(focus, stackStart, regionEnd);
        check(MixFocusTestAccess::scopeRegionText(focus).isEmpty(),
              "a drag crossing Stack lanes cannot commit a region from mixed ordinates");
        focus.select(second);
        display->setSelectedId(5, juce::sendNotificationSync);
        frame();
        check(display->getText() == "Difference" && !MixFocusTestAccess::hasDifference(focus),
              "choosing Difference never shows the previous combined projection");
        check(rmsText().isEmpty(), "entering Difference waits for a matching waveform before showing RMS");
        writeUntil(4 * bar);
        check(MixFocusTestAccess::hasDifference(focus) &&
                  MixFocusTestAccess::differencePulseAt(focus, .125f, .75f) &&
                  MixFocusTestAccess::differencePulseAt(focus, .875f, .625f),
              "Stereo Difference subtracts the selected channel from starred Main");
        // Each pulse covers 600 of 96,000 samples. Left has amplitudes .75
        // and .625, while Right has only the first .75 pulse.
        check(hasRms("-22.2") && hasRms("-24.5"),
              "a completed Stereo bar reports both independent RMS values from every source sample");
        const auto stereoWork = broker->stats();
        channels->setSelectedId(static_cast<int>(FocusScopeChannels::Left), juce::sendNotificationSync);
        frame();
        check(hasRms("-22.2") && !rmsText().contains("-24.5"),
              "Left Difference immediately uses the existing left-lane RMS alone");
        channels->setSelectedId(static_cast<int>(FocusScopeChannels::Right), juce::sendNotificationSync);
        frame();
        check(MixFocusTestAccess::hasDifference(focus) &&
                  MixFocusTestAccess::differencePulseAt(focus, .125f, -.75f) &&
                  !MixFocusTestAccess::differencePulseAt(focus, .875f, .625f),
              "Right Difference reuses the existing Stereo pair projection immediately");
        check(hasRms("-24.5") && !rmsText().contains("-22.2"),
              "Right Difference immediately uses the existing right-lane RMS alone");
        const auto laneWork = broker->stats();
        check(opens == std::array{1, 1} && laneWork.taps == stereoWork.taps &&
                  laneWork.consumers == stereoWork.consumers && laneWork.transforms == stereoWork.transforms &&
                  laneWork.cursorReads == stereoWork.cursorReads && laneWork.publications == stereoWork.publications,
              "changing the visible stereo lane creates no capture, publication or transform work");
        channels->setSelectedId(static_cast<int>(FocusScopeChannels::Mid), juce::sendNotificationSync);
        frame();
        check(!MixFocusTestAccess::hasDifference(focus),
              "switching to Mid clears the previous Stereo Difference");
        check(rmsText().isEmpty(), "a new folded projection hides the former lane RMS until it is measured");
        writeUntil(5 * bar);
        check(MixFocusTestAccess::hasDifference(focus) &&
                  !MixFocusTestAccess::differencePulseAt(focus, .125f, .75f) &&
                  MixFocusTestAccess::differencePulseAt(focus, .875f, .3125f),
              "Mid Difference cancels opposed stereo and halves a single-sided difference");
        check(hasRms("-32.1"), "Mid combines the paired samples before squaring");
        channels->setSelectedId(static_cast<int>(FocusScopeChannels::Side), juce::sendNotificationSync);
        frame();
        check(!MixFocusTestAccess::hasDifference(focus),
              "Side Difference never reuses the old Mid Difference");
        writeUntil(6 * bar);
        check(MixFocusTestAccess::hasDifference(focus) &&
                  MixFocusTestAccess::differencePulseAt(focus, .125f, .75f) &&
                  MixFocusTestAccess::differencePulseAt(focus, .875f, .3125f),
              "Side Difference projects the signed paired differences before extrema");
        check(hasRms("-23.8"), "Side reflects the anti-phase pulse and the half-strength single-sided pulse");
        check(opens == std::array{1, 1} && broker->stats().taps == 2 && broker->stats().consumers == 1 &&
                  broker->stats().transforms == 0,
              "Difference adds no tap, consumer or spectrum transform");

        MixFocusTestAccess::moveScopePointer(focus, .5f, .25f);
        const auto differenceCursor = MixFocusTestAccess::scopeCursorText(focus).removeCharacters(",");
        check(differenceCursor.contains("528000") && differenceCursor.contains("11000") &&
                  differenceCursor.contains("-6.0"),
              "the aligned Difference waveform labels the exact heard interval and its own amplitude axis");
        juce::ToggleButton *differenceSpectrum = nullptr;
        for (auto *component : focus.getChildren())
            if (component->getTitle() == "Difference spectrum")
                differenceSpectrum = dynamic_cast<juce::ToggleButton *>(component);
        check(differenceSpectrum != nullptr, "Difference exposes a separately selectable spectrum");
        if (differenceSpectrum) {
            MixFocusTestAccess::dragScopeRegion(focus, regionStart, regionEnd);
            check(!MixFocusTestAccess::scopeRegionText(focus).isEmpty(),
                  "an aligned Difference waveform can be measured before switching to its spectrum");
            differenceSpectrum->setToggleState(true, juce::dontSendNotification);
            if (differenceSpectrum->onClick) differenceSpectrum->onClick();
            check(MixFocusTestAccess::scopeCursorText(focus).isEmpty(),
                  "choosing the Difference spectrum immediately hides the waveform cursor");
            check(MixFocusTestAccess::scopeRegionText(focus).isEmpty(),
                  "choosing the Difference spectrum clears the waveform region");
            check(rmsText().isEmpty(), "the Difference spectrum immediately removes the waveform RMS");
            frame();
            // The demand is picked up by a newly requested full bar.
            writeUntil(8 * bar);
            MixFocusTestAccess::moveScopePointer(focus, .5f, .25f);
            check(MixFocusTestAccess::requestsResidualSpectrum(focus) && MixFocusTestAccess::hasDifference(focus) &&
                      MixFocusTestAccess::scopeCursorText(focus).isEmpty(),
                  "a completed Difference spectrum never exposes a waveform time or dBFS ordinate cursor");
            MixFocusTestAccess::dragScopeRegion(focus, regionStart, regionEnd);
            check(MixFocusTestAccess::scopeRegionText(focus).isEmpty(),
                  "a completed Difference spectrum never starts a time-domain region");
            differenceSpectrum->setToggleState(false, juce::dontSendNotification);
            if (differenceSpectrum->onClick) differenceSpectrum->onClick();
            frame();
            writeUntil(10 * bar);
        }

        MixFocusTestAccess::moveScopePointer(focus, .5f, .25f);
        check(!MixFocusTestAccess::scopeCursorText(focus).isEmpty(), "the Difference waveform cursor is visible before Locate");
        MixFocusTestAccess::scopeRegionPointer(focus, &juce::Component::mouseDown, regionStart, regionStart);
        MixFocusTestAccess::scopeRegionPointer(focus, &juce::Component::mouseDrag, regionStart, regionEnd);
        check(!MixFocusTestAccess::scopeRegionText(focus).isEmpty(),
              "the Difference region is visible during its drag before Locate");
        const auto seekAt = driver.position();
        check(doc->mailbox().post(TransportMailbox::Command::Locate, seekAt),
              "queue a same-position Difference Locate with a live scope cursor");
        MixFocusTestAccess::scopeRegionPointer(focus, &juce::Component::mouseDrag, regionStart, regionEnd);
        check(MixFocusTestAccess::scopeRegionText(focus).isEmpty(),
              "a pending same-position Locate invalidates the held region before the next frame");
        MixFocusTestAccess::moveScopePointer(focus, .5f, .25f);
        check(MixFocusTestAccess::scopeCursorText(focus).isEmpty(),
              "a pending same-position Locate suppresses the older cursor even before a frame");
        frame();
        doc->mailbox().drain(driver);
        frame();
        MixFocusTestAccess::moveScopePointer(focus, .5f, .25f);
        check(MixFocusTestAccess::scopeCursorText(focus).isEmpty(),
              "acknowledging the same-position Locate cannot re-label an old waveform");
        writeUntil(seekAt + 2 * bar);
        MixFocusTestAccess::moveScopePointer(focus, .5f, .25f);
        const auto resumed = MixFocusTestAccess::scopeMidpoint(focus);
        check(resumed && *resumed >= seekAt && MixFocusTestAccess::scopeCursorText(focus).contains(
                  juce::String(static_cast<juce::int64>(*resumed)) + " samples"),
              "a newly captured interval after the same-position seek restores the cursor");
        const auto complete = MixFocusTestAccess::scopeWaveformRange(focus);
        check(complete && complete->first == seekAt + bar && complete->frames == static_cast<std::size_t>(bar) &&
                  complete->sampleRate == rate && opens == std::array{1, 1} && broker->stats().taps == 2,
              "a same-position seek resumes complete heard bars on the original two taps");
        MixFocusTestAccess::scopeRegionPointer(focus, &juce::Component::mouseUp, regionStart, regionEnd);
        check(MixFocusTestAccess::scopeRegionText(focus).isEmpty(),
              "a release after the new bar cannot commit the pre-Locate region");
        MixFocusTestAccess::dragScopeRegion(focus, regionStart, regionEnd);
        check(complete && MixFocusTestAccess::scopeRegionText(focus).contains(
                  juce::String(static_cast<juce::int64>(complete->first + bar / 2))),
              "a fresh drag measures the newly captured interval after the same-position seek");

        check(doc->mailbox().post(TransportMailbox::Command::Locate, 0), "queue a Difference Locate");
        frame();
        check(!MixFocusTestAccess::hasDifference(focus), "a pending Locate immediately clears the old Difference");
        check(rmsText().isEmpty(), "an unfrozen queued Locate removes the old RMS with its waveform");
        doc->mailbox().drain(driver);
        frame();
        check(!MixFocusTestAccess::hasDifference(focus),
              "a settled Locate keeps the old Difference hidden until fresh paired audio");
        focus.select(first);
        frame();
        check(!MixFocusTestAccess::hasDifference(focus),
              "selecting starred Main as the other channel leaves Difference without a pair");
    }
    if (freeze && display && channels) {
        display->setSelectedId(1, juce::sendNotificationSync);
        focus.select(second);
        frame();
        writeUntil(2 * bar);
        const auto shown = MixFocusTestAccess::scopeWaveformRange(focus);
        const auto peak = MixFocusTestAccess::scopeReading(focus);
        const auto phase = MixFocusTestAccess::scopePhase(focus);
        check(shown && shown->frames == static_cast<std::size_t>(bar) && peak && freeze->isEnabled(),
              "a complete aligned Focus Scope bar enables Freeze");
        if (shown && shown->frames == static_cast<std::size_t>(bar) && peak && freeze->isEnabled()) {
            const auto setFrozen = [&](bool value) {
                freeze->setToggleState(value, juce::dontSendNotification);
                if (freeze->onClick) freeze->onClick();
            };
            MixFocusTestAccess::dragScopeRegion(focus, regionStart, regionEnd);
            const auto selectedRegion = MixFocusTestAccess::scopeRegionText(focus);
            const auto beforeFreeze = broker->stats();
            setFrozen(true);
            const auto held = broker->stats();
            check(!selectedRegion.isEmpty() && freeze->getToggleState() && beforeFreeze.taps == 2 &&
                      held.taps == 0 && held.domains == 0 && held.consumers == 0,
                  "Freeze releases all of this Scope's tap and worker leases immediately");
            const auto stillHeld = [&] {
                const auto pictured = MixFocusTestAccess::scopeWaveformRange(focus);
                const auto sum = MixFocusTestAccess::scopeReading(focus);
                return freeze->getToggleState() && pictured && pictured->first == shown->first &&
                       pictured->frames == shown->frames && pictured->sampleRate == shown->sampleRate &&
                       sum && sum->peak == peak->peak && sum->rms == peak->rms &&
                       MixFocusTestAccess::scopePhase(focus) == phase &&
                       MixFocusTestAccess::scopeRegionText(focus) == selectedRegion;
            };

            driver.play(false);
            frame();
            MixFocusTestAccess::moveScopePointer(focus, .75f, .25f);
            check(stillHeld(), "Stop and pointer motion keep the frozen heard interval and selected region");
            MixFocusTestAccess::wheelScope(focus, .75f, .25f, 1.f);
            check(stillHeld(), "zooming a frozen waveform keeps the held interval, meter values and selected region");
            MixFocusTestAccess::wheelScope(focus, .75f, .25f, -1.f);
            const auto heldGrid = MixFocusTestAccess::scopeGrid(focus);
            MixFocusTestAccess::wheelScope(focus, .75f, .25f, -1.f, panModifiers);
            check(stillHeld() && MixFocusTestAccess::scopeGrid(focus) == heldGrid,
                  "panning a frozen waveform keeps the selected region, grid and measured signal");
            MixFocusTestAccess::wheelScope(focus, .75f, .25f, 1.f, panModifiers);
            MixFocusTestAccess::wheelScope(focus, .5f, .25f, 1.f, timeZoomModifier);
            MixFocusTestAccess::wheelScope(focus, .5f, .25f, -1.f, timePanModifier);
            check(stillHeld() && MixFocusTestAccess::scopeVisibleRange(focus).frames == shown->frames / 2,
                  "time zoom and pan work on the held interval while preserving the frozen region and measurements");
            MixFocusTestAccess::wheelScope(focus, .5f, .25f, -1.f, timeZoomModifier);
            check(doc->mailbox().post(TransportMailbox::Command::Locate, bar / 2),
                  "queue a Locate while the Scope holds a different heard interval");
            frame();
            doc->mailbox().drain(driver);
            frame();
            check(stillHeld(), "a pending and acknowledged Locate leave the explicit frozen interval intact");
            driver.play(true);
            frame();
            writeUntil(bar / 2 + 4 * chunk);
            const auto frozenBounds = focus.getBounds();
            focus.setBounds(frozenBounds.withWidth(frozenBounds.getWidth() - 8));
            check(stillHeld() && MixFocusTestAccess::hasScopeTrace(focus),
                  "resizing a frozen Scope projects the retained waveform without replacing its measurement");
            focus.setBounds(frozenBounds);
            const auto still = broker->stats();
            check(stillHeld() && still.taps == 0 && still.domains == 0 && still.consumers == 0 &&
                      still.frameRequests == held.frameRequests && still.workerPasses == held.workerPasses &&
                      still.transforms == held.transforms && still.cursorReads == held.cursorReads &&
                      still.publications == held.publications && History(doc->store()).head() == head,
                  "frozen Scope reads no new samples, schedules no work and authors no edit");

            SharedAnalysis::Request request{first};
            request.channel = SharedAnalysis::Channel::StereoPower;
            request.outputs = SharedAnalysis::Scope;
            auto sibling = broker->acquire(request);
            writeUntil(driver.position() + 4 * chunk);
            const auto siblingPicture = sibling.read();
            check(siblingPicture && stillHeld() && broker->stats().taps == 1 && broker->stats().consumers == 1,
                  "only the sibling Scope reads the first source while Focus holds its picture");
            const auto thawedAt = driver.position();
            setFrozen(false);
            frame();
            broker->pump();
            frame();
            const auto stale = MixFocusTestAccess::scopeWaveformRange(focus);
            check(!freeze->getToggleState() && !stale && !MixFocusTestAccess::hasScopeTrace(focus) &&
                      MixFocusTestAccess::scopeRegionText(focus).isEmpty() &&
                      sibling.read() == siblingPicture && broker->stats().taps == 2,
                  "thaw releases the held picture and rejects the sibling's pre-thaw window");
            writeUntil(3 * bar);
            const auto fresh = MixFocusTestAccess::scopeWaveformRange(focus);
            check(fresh && fresh->first >= thawedAt && freeze->isEnabled(),
                  "a fresh post-thaw capture restores the Scope's own picture");

            if (fresh && freeze->isEnabled()) {
                MixFocusTestAccess::dragScopeRegion(focus, regionStart, regionEnd);
                const bool hadRegion = !MixFocusTestAccess::scopeRegionText(focus).isEmpty();
                const auto owner = std::move(root->sharedAnalysis);
                setFrozen(true);
                check(!freeze->getToggleState() && MixFocusTestAccess::hasScopeTrace(focus),
                      "Freeze refuses the previous analysis owner before the next component frame");
                root->sharedAnalysis = owner;
                setFrozen(true);
                channels->setSelectedId(static_cast<int>(FocusScopeChannels::Mid), juce::sendNotificationSync);
                frame();
                check(hadRegion && !freeze->getToggleState() && MixFocusTestAccess::scopeRegionText(focus).isEmpty(),
                      "changing the channel projection invalidates the held Scope and its region");
            }
            sibling.reset();
        }
    }
    if (differenceRms && freeze && display && channels) {
        focus.select(second);
        channels->setSelectedId(static_cast<int>(FocusScopeChannels::Stereo), juce::sendNotificationSync);
        display->setSelectedId(5, juce::sendNotificationSync);
        frame();
        writeUntil(5 * bar);
        const auto reading = rmsText();
        check(hasRms("-22.2") && hasRms("-24.5") && freeze->isEnabled(),
              "an aligned Difference waveform can hold its own RMS");
        if (!reading.isEmpty() && freeze->isEnabled() && freeze->onClick) {
            freeze->setToggleState(true, juce::dontSendNotification); freeze->onClick();
            const auto held = broker->stats();
            driver.play(false);
            frame();
            check(doc->mailbox().post(TransportMailbox::Command::Locate, driver.position()),
                  "queue a same-position Locate while Difference RMS is frozen");
            frame();
            doc->mailbox().drain(driver);
            frame();
            const auto bounds = focus.getBounds();
            focus.setBounds(bounds.withWidth(bounds.getWidth() - 8));
            const auto still = broker->stats();
            check(rmsText() == reading && freeze->getToggleState() && held.taps == 0 && still.taps == 0 &&
                      still.publications == held.publications && still.workerPasses == held.workerPasses &&
                      still.transforms == held.transforms && still.cursorReads == held.cursorReads,
                  "a frozen RMS survives Stop, Locate and resize without new analysis work");
            focus.setBounds(bounds);
            freeze->setToggleState(false, juce::dontSendNotification); freeze->onClick();
            driver.play(true);
            frame();
            check(rmsText().isEmpty(), "thaw clears frozen RMS until fresh paired audio arrives");
        }
    }
    if (truePeakSwitch && capturedTruePeak && display && channels) {
        channels->setSelectedId(static_cast<int>(FocusScopeChannels::Stereo), juce::sendNotificationSync);
        display->setSelectedId(4, juce::sendNotificationSync);
        frame();
        truePeakSwitch->setToggleState(true, juce::dontSendNotification);
        truePeakSwitch->onClick();
        check(!capturedTruePeak->isVisible(), "an unmeasured Sum picture publishes no true peak");
        const auto before = broker->stats();
        writeUntil(driver.position() + 3 * bar);
        check(capturedTruePeak->isVisible() && capturedTruePeak->getText().contains("dBFS") &&
                  capturedTruePeak->getText().contains("dBTP") && broker->stats().taps == 2 &&
                  broker->stats().transforms == before.transforms,
              "Stereo Sum reports the sample and captured true peak without a new tap or spectrum transform");
        check(doc->mailbox().post(TransportMailbox::Command::Locate, driver.position()),
              "queue a same-position Locate for the measured Sum");
        frame();
        check(!capturedTruePeak->isVisible(), "queued Locate hides the unheld true peak before audio acknowledgement");
        doc->mailbox().drain(driver);
        frame();
        check(!capturedTruePeak->isVisible(), "settled Locate waits for a newly captured true peak");
        display->setSelectedId(1, juce::sendNotificationSync);
        check(!truePeakSwitch->isVisible() && !capturedTruePeak->isVisible(),
              "leaving Sum immediately hides the captured true peak and its control");
    }
    if (musicalWindow && display && channels) {
        channels->setSelectedId(static_cast<int>(FocusScopeChannels::Stereo), juce::sendNotificationSync);
        display->setSelectedId(1, juce::sendNotificationSync);
        focus.select(second);
        driver.play(true);
        frame();
        const auto atBar = (driver.position() / bar + 2) * bar;
        writeUntil(atBar);
        check(MixFocusTestAccess::scopeInterval(focus, atBar - bar, static_cast<std::size_t>(bar), rate),
              "the default selector still shows the entire completed bar");
        MixFocusTestAccess::dragScopeRegion(focus, regionStart, regionEnd);
        const auto sourceOpens = opens;
        const auto before = broker->stats();
        const auto windowHistory = History(doc->store()).head();
        constexpr auto beat = bar / 4;
        musicalWindow->setSelectedId(1, juce::sendNotificationSync);
        frame();
        check(musicalWindow->getText() == "One Beat" && MixFocusTestAccess::scopeRegionText(focus).isEmpty() &&
                  !MixFocusTestAccess::scopeInterval(focus, atBar - bar, static_cast<std::size_t>(bar), rate),
              "changing to One Beat rejects the former bar and its selected region");
        writeUntil(atBar + beat - 1);
        check(!MixFocusTestAccess::scopeInterval(focus, atBar, static_cast<std::size_t>(beat), rate),
              "the one-beat view cannot publish before its final heard sample");
        writeUntil(atBar + beat);
        check(MixFocusTestAccess::scopeInterval(focus, atBar, static_cast<std::size_t>(beat), rate),
              "at 120 BPM and 48 kHz exactly 24000 samples form a completed 4/4 beat");
        writeUntil(atBar + bar);
        check(MixFocusTestAccess::scopeInterval(focus, atBar + 3 * beat, static_cast<std::size_t>(beat), rate),
              "each successive beat keeps its own grid-aligned heard extent");
        musicalWindow->setSelectedId(2, juce::sendNotificationSync);
        frame();
        writeUntil(atBar + 2 * bar);
        const auto after = broker->stats();
        check(musicalWindow->getText() == "One Bar" &&
                  MixFocusTestAccess::scopeInterval(focus, atBar + bar, static_cast<std::size_t>(bar), rate) &&
                  opens == sourceOpens && after.taps == before.taps && after.consumers == before.consumers &&
                  after.transforms == before.transforms && History(doc->store()).head() == windowHistory,
              "returning to bar captures the full next bar with the original taps and no project edit or FFT");
        const auto samePosition = driver.position();
        check(doc->mailbox().post(TransportMailbox::Command::Locate, samePosition),
              "queue a same-position Locate with a complete Overlay bar visible");
        frame();
        check(!MixFocusTestAccess::hasScopeTrace(focus) && !MixFocusTestAccess::scopeWaveformRange(focus) &&
                  MixFocusTestAccess::scopeGrid(focus).empty(),
              "the pending Locate hides the old Overlay waveform and heard grid");
        musicalWindow->setSelectedId(1, juce::sendNotificationSync);
        broker->pump();
        frame();
        check(!MixFocusTestAccess::hasScopeTrace(focus) && !MixFocusTestAccess::scopeWaveformRange(focus),
              "changing the window while Locate is pending cannot publish an old audio window");
        doc->mailbox().drain(driver);
        frame();
        check(!MixFocusTestAccess::hasScopeTrace(focus) && !MixFocusTestAccess::scopeWaveformRange(focus),
              "acknowledging Locate waits for audio in the new transport revision");
        writeUntil(samePosition + beat - 1);
        check(!MixFocusTestAccess::hasScopeTrace(focus), "the new beat cannot appear before its final heard sample");
        writeUntil(samePosition + beat);
        check(MixFocusTestAccess::scopeInterval(focus, samePosition, static_cast<std::size_t>(beat), rate) &&
                  opens == sourceOpens && broker->stats().transforms == after.transforms,
              "the selected beat arrives from the acknowledged revision on the original two taps");
        const auto noteStart = driver.position();
        musicalWindow->setSelectedId(3, juce::sendNotificationSync);
        frame();
        constexpr auto sixteenth = beat / 4;
        writeUntil(noteStart + sixteenth - 1);
        check(musicalWindow->getText() == "1/16 Note" && !MixFocusTestAccess::hasScopeTrace(focus),
              "the sixteenth selector clears the old beat and waits for the complete note");
        writeUntil(noteStart + sixteenth);
        check(MixFocusTestAccess::scopeInterval(focus, noteStart, static_cast<std::size_t>(sixteenth), rate) &&
                  opens == sourceOpens && broker->stats().transforms == after.transforms,
              "the sixteenth captures exactly 6000 heard samples at 120 BPM on the same two taps");
        musicalWindow->setSelectedId(2, juce::sendNotificationSync);
    }
    if (musicalWindow && display && channels) {
        check(doc->mailbox().post(TransportMailbox::Command::Locate, 0), "queue Locate for the fractional Scope grid");
        doc->mailbox().drain(driver);
        frame();
        check(root->setTempoAtPlay("123"), "set the Scope project's tempo to 123 BPM");
        frame();
        constexpr std::int64_t fractionalBar = 93659;
        writeUntil(fractionalBar);
        const auto heard = MixFocusTestAccess::scopeWaveformRange(focus);
        check(heard && heard->first == 0 && heard->frames == fractionalBar,
              "a bar at 123 BPM owns exactly 93659 heard samples");
        if (heard && heard->first == 0 && heard->frames == fractionalBar) {
            const auto readsBeat = [&](std::int64_t sample, const juce::String &beat) {
                const auto x = static_cast<float>((static_cast<double>(sample) + .5) / fractionalBar);
                MixFocusTestAccess::moveScopePointer(focus, x, .25f);
                const auto reading = MixFocusTestAccess::scopeCursorText(focus).removeCharacters(",");
                return reading.startsWith(beat + "   " + juce::String(static_cast<juce::int64>(sample)) + " samples");
            };
            check(readsBeat(46828, "1.2"), "the sample before a rounded beat edge stays in the preceding beat");
            check(readsBeat(46829, "1.3"), "the rounded-down heard edge and its beat label agree");
            check(readsBeat(46830, "1.3") && readsBeat(46828, "1.2"),
                  "the cursor follows the same heard edge in both directions");
            const auto plot = MixFocusTestAccess::scopePlot(focus);
            const auto xAt = [&](std::int64_t sample) {
                return plot.getX() + juce::roundToInt(static_cast<double>(sample) / fractionalBar * plot.getWidth());
            };
            const std::vector<std::pair<int, bool>> expected{{xAt(0), true}, {xAt(23415), false},
                                                            {xAt(46829), false}, {xAt(70244), false}};
            check(MixFocusTestAccess::scopeGrid(focus) == expected,
                  "the visible grid projects the captured 123 BPM sample positions exactly");
            if (freeze && freeze->isEnabled()) {
                freeze->setToggleState(true, juce::dontSendNotification);
                freeze->onClick();
                const auto statsBefore = broker->stats();
                check(doc->mailbox().post(TransportMailbox::Command::Locate, 0), "queue Locate while holding the grid");
                doc->mailbox().drain(driver);
                frame();
                check(root->setTempoAtPlay("120"), "change project tempo while the 123 BPM grid is held");
                frame();
                const auto statsAfter = broker->stats();
                check(MixFocusTestAccess::scopeGrid(focus) == expected && readsBeat(46829, "1.3") &&
                          statsAfter.taps == 0 && statsAfter.transforms == statsBefore.transforms &&
                          statsAfter.frameRequests == statsBefore.frameRequests,
                      "the held grid keeps its captured timing across a project edit with no analyser work");
                freeze->setToggleState(false, juce::dontSendNotification);
                freeze->onClick();
                frame();
                check(MixFocusTestAccess::scopeGrid(focus).empty(), "thaw hides the grid until new audio arrives");
            }
        }
    }
    if (musicalWindow && display && channels) {
        // Prime alignment after the preceding thaw before seeking to the start.
        writeUntil(driver.position() + bar / 4);
        const auto selectAtStart = [&](FocusScopeWindow window) {
            check(doc->mailbox().post(TransportMailbox::Command::Locate, 0), "queue Locate to start a multi-bar window");
            musicalWindow->setSelectedId(static_cast<int>(window), juce::sendNotificationSync);
            frame();
            doc->mailbox().drain(driver);
            frame();
        };
        selectAtStart(FocusScopeWindow::TwoBars);
        if (root->bpm() != 120.)
            check(root->setTempoAtPlay("120"), "set 120 BPM for the multi-bar capture");
        frame();
        const auto sourcesBefore = opens;
        const auto statsBefore = broker->stats();
        writeUntil(2 * bar - 1);
        check(!MixFocusTestAccess::hasScopeTrace(focus) && broker->stats().consumers == 1,
              "Two Bars waits for every heard sample on the one grouped reader");
        writeUntil(2 * bar);
        check(musicalWindow->getText() == "Two Bars" &&
                  MixFocusTestAccess::scopeInterval(focus, 0, static_cast<std::size_t>(2 * bar), rate),
              "Two Bars projects the whole 192000-sample capture");
        selectAtStart(FocusScopeWindow::FourBars);
        writeUntil(4 * bar - 1);
        check(!MixFocusTestAccess::hasScopeTrace(focus) && broker->stats().consumers == 1,
              "Four Bars waits for the last sample in its revision without running unseen short previews");
        writeUntil(4 * bar);
        const auto statsAfter = broker->stats();
        check(musicalWindow->getText() == "Four Bars" &&
                  MixFocusTestAccess::scopeInterval(focus, 0, static_cast<std::size_t>(4 * bar), rate) &&
                  MixFocusTestAccess::sumPulseAt(focus, .125f / 4.f, .75f) &&
                  MixFocusTestAccess::sumPulseAt(focus, (3.f + .875f) / 4.f, -.625f) &&
                  opens == sourcesBefore && statsAfter.taps == 2 && statsAfter.transforms == statsBefore.transforms,
              "Four Bars keeps the first and last pulses through 384000 samples using the same two taps");
    }
    if (musicalWindow && display && channels) {
        constexpr std::int64_t firstTimed = 12345, timedFrames = 4080;
        const auto oldOpens = opens;
        const auto oldTransforms = broker->stats().transforms;
        check(doc->mailbox().post(TransportMailbox::Command::Locate, firstTimed), "queue a non-grid start for Free Run");
        musicalWindow->setSelectedId(static_cast<int>(FocusScopeWindow::Milliseconds85), juce::sendNotificationSync);
        frame();
        check(!MixFocusTestAccess::hasScopeTrace(focus), "Free Run waits for the acknowledged non-grid start");
        doc->mailbox().drain(driver);
        frame();
        writeUntil(firstTimed + timedFrames - 1);
        check(!MixFocusTestAccess::hasScopeTrace(focus) && broker->stats().consumers == 1,
              "the first fixed-time window waits for its final sample with one grouped reader");
        writeUntil(firstTimed + timedFrames);
        check(musicalWindow->getText() == "85 ms" &&
                  MixFocusTestAccess::scopeInterval(focus, firstTimed, static_cast<std::size_t>(timedFrames), rate),
              "the fixed-time window starts at sample 12345 and captures all 4080 frames");
        check(root->setTempoAtPlay("123") && root->setMeterAtPlay("3/4"),
              "edit the musical grid while the next fixed-time window is pending");
        frame();
        writeUntil(firstTimed + 2 * timedFrames);
        check(MixFocusTestAccess::scopeInterval(focus, firstTimed + timedFrames, static_cast<std::size_t>(timedFrames), rate) &&
                  opens == oldOpens && broker->stats().transforms == oldTransforms,
              "free-running captures stay contiguous across tempo and meter edits on the same source taps");
        const auto selectTimed = [&](FocusScopeWindow choice, const juce::String &label, std::int64_t frames) {
            const auto first = driver.position();
            musicalWindow->setSelectedId(static_cast<int>(choice), juce::sendNotificationSync);
            frame();
            writeUntil(first + frames);
            check(musicalWindow->getText() == label &&
                      MixFocusTestAccess::scopeInterval(focus, first, static_cast<std::size_t>(frames), rate) && opens == oldOpens,
                  "changing the millisecond preset starts its full interval on the same taps");
            return first;
        };
        selectTimed(FocusScopeWindow::Milliseconds170, "170 ms", 8160);
        const auto wideStart = selectTimed(FocusScopeWindow::Milliseconds340, "340 ms", 16320);
        const auto stoppedReading = MixFocusTestAccess::scopeReading(focus);
        check(doc->mailbox().post(TransportMailbox::Command::Stop), "queue the normal Stop command for the held picture");
        frame();
        doc->mailbox().drain(driver);
        frame();
        const auto stoppedStats = broker->stats();
        const auto heldReading = MixFocusTestAccess::scopeReading(focus);
        check(MixFocusTestAccess::scopeInterval(focus, wideStart, 16320, rate) && stoppedReading && heldReading &&
                  stoppedReading->peak == heldReading->peak && stoppedReading->rms == heldReading->rms &&
                  stoppedStats.taps == 0 && stoppedStats.consumers == 0,
              "Stop holds the completed waveform and readings with no source taps or consumers");
        MixFocusTestAccess::moveScopePointer(focus, .5f, .25f);
        frame();
        check(!MixFocusTestAccess::scopeCursorText(focus).isEmpty() &&
                  broker->stats().frameRequests == stoppedStats.frameRequests &&
                  broker->stats().transforms == stoppedStats.transforms,
              "a stopped waveform can be inspected without waking the analysis worker");
        check(doc->mailbox().post(TransportMailbox::Command::Locate, firstTimed), "queue Locate while Scope is stopped");
        frame();
        check(!MixFocusTestAccess::hasScopeTrace(focus) && !MixFocusTestAccess::scopeWaveformRange(focus),
              "a queued Locate retracts the picture held by ordinary Stop");
        doc->mailbox().drain(driver);
        driver.play(true);
        frame();
        writeUntil(firstTimed + bar / 2);
        const auto resumed = MixFocusTestAccess::scopeWaveformRange(focus);
        check(resumed && resumed->frames == 16320 && resumed->first >= firstTimed &&
                  broker->stats().taps == 2,
              "Play reacquires both taps and publishes a fresh window after the seek");
        if (freeze && freeze->isEnabled()) {
            freeze->setToggleState(true, juce::dontSendNotification);
            freeze->onClick();
            const auto held = broker->stats();
            check(doc->mailbox().post(TransportMailbox::Command::Locate, 0), "queue a seek while Free Run is frozen");
            doc->mailbox().drain(driver);
            frame();
            check(resumed && MixFocusTestAccess::scopeInterval(focus, resumed->first, 16320, rate) &&
                      held.taps == 0 && broker->stats().frameRequests == held.frameRequests,
                  "Freeze holds the completed time window across Locate without new broker work");
            freeze->setToggleState(false, juce::dontSendNotification);
            freeze->onClick();
        }
    }
    const auto visibleBounds = focus.getBounds();
    focus.setTopLeftPosition(peer.getWidth(), 0);
    check(broker->stats().taps == 0 && broker->stats().consumers == 0,
          "moving the plot fully out of view releases Focus demand before its next frame");
    focus.setBounds(visibleBounds);
    focus.frame(initial);
    check(broker->stats().taps == 2 && !MixFocusTestAccess::aligned(focus),
          "a restored plot waits for new source data instead of reusing the old bar");
    if (musicalWindow && display) {
        display->setSelectedId(5, juce::sendNotificationSync);
        focus.setMembers({first});
        frame();
        check(musicalWindow->isEnabled() && display->getSelectedId() == 1 && !display->isItemEnabled(5),
              "one remaining Focus channel can choose a window and falls back from Difference to Overlay");
        display->setSelectedId(1, juce::sendNotificationSync);
        writeUntil(driver.position() + 8 * chunk);
        const auto soloStart = driver.position();
        const auto transforms = broker->stats().transforms;
        musicalWindow->setSelectedId(static_cast<int>(FocusScopeWindow::Milliseconds85), juce::sendNotificationSync);
        frame();
        writeUntil(soloStart + 4079);
        check(!MixFocusTestAccess::hasScopeTrace(focus), "a solo 85 ms window waits for its last sample");
        writeUntil(soloStart + 4080);
        check(MixFocusTestAccess::scopeInterval(focus, soloStart, 4080, rate) &&
                  MixFocusTestAccess::scopePhase(focus) == 1. && broker->stats().taps == 1 &&
                  broker->stats().consumers == 1 && broker->stats().transforms == transforms,
              "one channel shows the full fixed window and its L/R phase with just one tap and no transforms");
        driver.play(false);
        frame();
        check(MixFocusTestAccess::scopeInterval(focus, soloStart, 4080, rate) && broker->stats().taps == 0,
              "stopping holds the one-source capture and detaches its tap");
        focus.setMembers({first, second});
        check(display->isItemEnabled(5), "Difference is offered again when the second Focus channel returns");
    }
    if (musicalWindow && display && channels) {
        const auto layerChannelItem = [&](const juce::String &label) {
            const auto layer = MixFocusTestAccess::layerMenu(focus);
            for (juce::PopupMenu::MenuItemIterator item(layer); item.next();) {
                const auto &parent = item.getItem();
                if (parent.text != "Channels" || !parent.subMenu) continue;
                for (juce::PopupMenu::MenuItemIterator child(*parent.subMenu); child.next();) {
                    const auto &option = child.getItem();
                    if (option.text == label && option.isEnabled) return std::pair{option.itemID, option.isTicked};
                }
            }
            return std::pair{0, false};
        };
        const auto chooseChannel = [&](const juce::String &label) {
            const auto option = layerChannelItem(label);
            if (option.first != 0) MixFocusTestAccess::chooseLayerItem(focus, option.first);
            return option.first != 0 && layerChannelItem(label).second;
        };
        const auto priorHead = History(doc->store()).head();
        focus.select(first);
        check(layerChannelItem("Follow window").second && chooseChannel("Mid") && !layerChannelItem("Side").second,
              "the first layer can choose Mid from its checked Channels submenu");
        focus.select(second);
        check(layerChannelItem("Follow window").second && chooseChannel("Side") && !layerChannelItem("Mid").second,
              "the second layer still follows the window until it independently chooses Side");
        focus.select(first);
        check(layerChannelItem("Mid").second && !layerChannelItem("Follow window").second,
              "returning to the first layer shows its previous checked choice");

        driver.play(true);
        frame();
        writeUntil(driver.position() + 8 * chunk);
        const auto before = broker->stats();
        const auto openBefore = opens;
        const auto start = driver.position();
        musicalWindow->setSelectedId(static_cast<int>(FocusScopeWindow::Milliseconds170), juce::sendNotificationSync);
        frame();
        constexpr auto frames = 8160;
        std::array<float, chunk> leftA, leftB, right;
        leftA.fill(.75f); leftB.fill(-.75f); right.fill(.25f);
        while (driver.position() < start + frames) {
            const auto from = driver.position();
            const auto count = static_cast<std::int32_t>(std::min<std::int64_t>(chunk, start + frames - from));
            a->write(leftA.data(), right.data(), count, from, true, driver.revision());
            b->write(leftB.data(), right.data(), count, from, true, driver.revision());
            driver.advance(count);
            broker->pump();
            frame();
        }
        const auto reading = MixFocusTestAccess::scopeReading(focus);
        check(channels->getText() == "Stereo" && MixFocusTestAccess::scopeInterval(focus, start, frames, rate) &&
                  MixFocusTestAccess::foldedLayerAt(focus, first, .999f, .5f) &&
                  MixFocusTestAccess::foldedLayerAt(focus, second, .999f, -.5f) &&
                  reading && reading->peak == std::array{0., .5},
              "the grouped waveform draws independent Mid and Side traces and reads the raw stereo sum");
        const auto after = broker->stats();
        check(opens == openBefore && after.taps == 2 && after.consumers == 1 &&
                  after.transforms == before.transforms && History(doc->store()).head() == priorHead,
              "layer channel choices keep one grouped reader without replacing sources, transforming or editing");
        check(freeze && freeze->isEnabled(), "both projected layers can be held in the same Freeze");
        if (freeze && freeze->isEnabled()) {
            freeze->setToggleState(true, juce::dontSendNotification);
            freeze->onClick();
            focus.select(second);
            const auto menu = MixFocusTestAccess::layerMenu(focus);
            int later = 0;
            for (juce::PopupMenu::MenuItemIterator item(menu); item.next();)
                if (item.getItem().text == "Later by 1 sample" && item.getItem().isEnabled) later = item.getItem().itemID;
            const bool wasFrozen = freeze->getToggleState();
            if (later) MixFocusTestAccess::chooseLayerItem(focus, later);
            constexpr FocusScopeLayerAdjustment expected{1, false};
            check(wasFrozen && later && !freeze->getToggleState() &&
                      MixFocusTestAccess::layerAdjustment(focus) == expected &&
                      MixFocusTestAccess::cachedLayerAdjustment(focus, 1) == expected,
                  "editing a frozen layer thaws once and carries the updated offset into the next capture");
        }
    }
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
    foldedChannelBrowserSearch(commands, temporary.path());
    pinnedSpectrumOverlay(commands, temporary.path(), 48000, 4096);
    pinnedSpectrumOverlay(commands, temporary.path(), 96000, 8192);
    focusWorkspace(commands, temporary.path());
    focusOneBarScope(commands, temporary.path());
    cleanCompressor(commands, temporary.path());
    keyMenus(commands, temporary.path());
    std::printf("%d checks, %d failures (software-peer components)\n", checks, failures);
    return failures ? 1 : 0;
}
