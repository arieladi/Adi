// SPDX-License-Identifier: GPL-3.0-or-later
// Owned SDK fixture only. No editor, desktop input or audio device.
#include "adi/blob.hpp"
#include "adi/ui/project_document.hpp"
#include "juce/vst3_host.hpp"
#include <pluginterfaces/vst/ivsteditcontroller.h>
#include "temp_directory.hpp"
#include <SQLiteCpp/SQLiteCpp.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <stdexcept>
#include <vector>
using namespace adi;
namespace {
int checks = 0, failures = 0;
void check(bool yes, const char *label) {
    ++checks;
    if (!yes) {
        ++failures;
        std::printf("FAIL: %s\n", label);
    }
}
std::filesystem::path native(const std::string &s) {
    return std::filesystem::path(std::u8string(s.begin(), s.end()));
}
// SC-312: the MIDI map read from a plug-in's declared "MIDI CC c|n" parameters (JUCE's wrapper declares
// its map that way and answers IMidiMapping from it), for a controller the host cannot reach.
void declaredMidiMap() {
    const auto param = [](std::string name, std::uint32_t id) {
        device::ParamDescriptor d;
        d.name = std::move(name);
        d.id = std::to_string(id);
        return d;
    };
    const std::vector<device::ParamDescriptor> params{
        param("Cutoff", 7), param("MIDI CC 0|1", 0x6d636d01), param("MIDI CC 0|64", 0x6d636d40),
        param("MIDI CC 0|7", 0x6d636d07), param("MIDI CC 0|128", 0x6d636d80), param("MIDI CC 0|129", 0x6d636d81),
        param("MIDI CC 0|74", 0x6d636d4a), param("MIDI CC 3|11", 0x6d636f00), param("MIDI CC 0|101", 0x6d636d65),
        param("MIDI CC x|1", 99), param("MIDI CC 16|1", 98), param("MIDI CC 0|130", 97)};
    engine::ExpressionCaps caps;
    caps.controllerParam[0][7] = 1234; // already known: kept
    check(device::mapDeclaredMidiControllers(params, caps), "a declared map is found");
    check(caps.controllerParam[0][1] == 0x6d636d01 && caps.controllerParam[0][64] == 0x6d636d40,
          "the mod wheel and the sustain pedal land on their declared parameters");
    check(caps.bendParam[0] == 0x6d636d81 && caps.pressureParam[0] == 0x6d636d80,
          "controller 129 is the pitch bend, 128 channel aftertouch");
    check(caps.timbreParam[0] == 0x6d636d4a && caps.controllerParam[3][11] == 0x6d636f00 && caps.rpnParam[0] == 0x6d636d65,
          "CC74 is the timbre too; other channels and the MPE configuration's CC101 are read");
    check(caps.controllerParam[0][7] == 1234, "an entry the caps already have is kept");
    check(caps.controllerParam[0][2] == engine::kNoParam && caps.bendParam[1] == engine::kNoParam,
          "what is not declared stays unmapped; malformed names and channel 16 or controller 130 are ignored");
    engine::ExpressionCaps none;
    check(!device::mapDeclaredMidiControllers({param("Cutoff", 7)}, none), "no declared map, nothing found");
}
// SC-310: a value ADI sets on a VST3 (its panel, Undo, Delete to default, a project's rows replayed at
// open) reaches the PROCESSOR, through IParameterChanges in the next process call, as VST3 asks
// (ADR-0073). ADI calls the processor itself, so JUCE's own hand-over in processBlock never runs: a
// value set only through JUCE reached the edit controller and stopped there. The fixture records Drive
// as its processor received it and through nothing else, as a plug-in that keeps its processor apart.
// SC-332: a plug-in that renames a parameter (a macro) and says so shows the new name, at the next pump.
void renamedParameterShows(device::Vst3Host& host, const juce::PluginDescription& desc, void (*rename)(int)) {
    std::string error;
    auto instance = host.instantiate(desc, 48000.0, 64, error);
    if (!instance)
        throw std::runtime_error(error);
    device::Vst3Device vst(std::move(instance), device::identityOf(desc));
    const auto nameOf = [&]() -> std::string {
        for (std::int32_t i = 0; i < vst.paramCount(); ++i)
            if (const auto* d = vst.paramAt(i); d != nullptr && d->id == "4000") return d->name;
        return "<none>";
    };
    check(nameOf() == "Drive", "the parameter reads its first title");
    rename(1);
    vst.pumpMainThread();
    check(nameOf() == "Macro 1", "renamed by the plug-in, it reads the new title (SC-332)");
    rename(0);
    vst.pumpMainThread();
    check(nameOf() == "Drive", "and back");
}

void valuesReachProcessor(device::Vst3Host& host, const juce::PluginDescription& desc, double (*processorDrive)(),
                          bool offline) {
    constexpr double sampleRate = 48000.0;
    constexpr std::int32_t frames = 64;
    std::string error;
    auto instance = host.instantiate(desc, sampleRate, frames, error);
    if (!instance)
        throw std::runtime_error(error);
    device::Vst3Device vst(std::move(instance), device::identityOf(desc));
    // A project's parameter rows are replayed before the device first plays.
    check(vst.setParam("4000", device::ParamValue::fromNormalized(0.35)), "Drive set before the first block (project open)");
    vst.setOffline(offline);
    vst.prepare(sampleRate, frames);
    check(vst.usingRawProcessor(), "the values ride the raw VST3 process call");
    std::array<float, frames> inL{}, inR{}, outL{}, outR{};
    const float *input[]{inL.data(), inR.data()};
    float *output[]{outL.data(), outR.data()};
    const auto block = [&] {
        engine::NodeIo io;
        io.channels = 2;
        io.frames = frames;
        io.sampleRate = sampleRate;
        io.in = input;
        io.out = output;
        vst.process(io);
    };
    const auto near = [](double a, double b) { return std::fabs(a - b) < 1e-6; };
    block();
    std::printf("processor Drive after the replayed value: %.4f\n", processorDrive());
    check(near(processorDrive(), 0.35),
          offline ? "offline: a value set before the first block reaches the processor in it"
                  : "a value set before the first block (a project opening) reaches the processor in it");
    check(vst.setParam("4000", device::ParamValue::fromNormalized(0.65)), "Drive set from the panel");
    block();
    check(near(processorDrive(), 0.65),
          offline ? "offline: a panel value reaches the processor in the next block"
                  : "a panel value (or Undo's) reaches the processor in the next block");
    check(near(vst.getParam("4000").normalized, 0.65), "and the controller shows it");
    check(vst.setParam("4000", device::ParamValue::fromNormalized(0.2)), "Drive back to its default (Delete)");
    block();
    check(near(processorDrive(), 0.2), "Delete to default reaches the processor too");
    // Several sets between two blocks: the processor takes the last.
    vst.setParam("4000", device::ParamValue::fromNormalized(0.1));
    vst.setParam("4000", device::ParamValue::fromNormalized(0.9));
    block();
    check(near(processorDrive(), 0.9), "of two values set between blocks, the processor ends on the last");
}
// SC-311: an edit made in the plug-in's own window reaches its sound, and an automated parameter moves
// where its value is shown (ADI's panel reads the edit controller, as does the plug-in's window). The
// fixture's Gesture Trigger makes its controller drag Drive to 0.7 through the host's IComponentHandler,
// beginEdit, forty performEdits, endEdit, exactly as an editor reports a drag.
void editorAndAutomationFollow(device::Vst3Host& host, const juce::PluginDescription& desc, double (*processorDrive)()) {
    constexpr double sampleRate = 48000.0;
    constexpr std::int32_t frames = 64;
    std::string error;
    auto instance = host.instantiate(desc, sampleRate, frames, error);
    if (!instance)
        throw std::runtime_error(error);
    device::Vst3Device vst(std::move(instance), device::identityOf(desc));
    vst.prepare(sampleRate, frames);
    std::array<float, frames> inL{}, inR{}, outL{}, outR{};
    const float *input[]{inL.data(), inR.data()};
    float *output[]{outL.data(), outR.data()};
    const auto block = [&] {
        engine::NodeIo io;
        io.channels = 2;
        io.frames = frames;
        io.sampleRate = sampleRate;
        io.in = input;
        io.out = output;
        vst.process(io);
    };
    const auto near = [](double a, double b) { return std::fabs(a - b) < 1e-6; };
    block();
    auto *component = static_cast<Steinberg::Vst::IComponent *>(vst.rawComponent());
    Steinberg::Vst::IEditController *controller = nullptr;
    if (component != nullptr)
        component->queryInterface(Steinberg::Vst::IEditController::iid, reinterpret_cast<void **>(&controller));
    check(controller != nullptr, "the fixture's edit controller is reachable");
    if (controller == nullptr)
        return;
    controller->setParamNormalized(4001 /* Gesture Trigger: the editor drags Drive to 0.7 */, 1.0);
    block();
    std::printf("processor Drive after the editor's drag: %.4f\n", processorDrive());
    check(near(processorDrive(), 0.7), "an edit made in the plug-in's own window reaches the processor in the next block");
    // A lane plays Drive: the value reaches the processor through the block, and the controller (what
    // ADI's panel and the plug-in's window show) follows on the message thread.
    engine::Event lane{};
    lane.type = engine::EventType::ParamValue;
    lane.paramId = 4000;
    lane.value = 0.45;
    lane.frame = 0;
    check(vst.pushEvent(lane), "an automation value for Drive");
    block();
    check(near(processorDrive(), 0.45), "the lane's value reaches the processor");
    vst.pumpMainThread();
    std::printf("controller Drive after the lane's value and a pump: %.4f\n", vst.getParam("4000").normalized);
    check(near(vst.getParam("4000").normalized, 0.45),
          "an automated parameter moves where it is shown: the controller follows the lane");
    controller->release();
}
// SC-315: a mono plug-in on ADI's stereo channel is heard on both sides, and hears both sides folded.
// The decision both hosts (VST3 and CLAP) take for each graph channel, and the fold.
void monoOnStereo() {
    check(device::sourceChannelFor(0, 1) == 0 && device::sourceChannelFor(1, 1) == 0,
          "a plug-in with one output is heard on the left and on the right");
    check(device::sourceChannelFor(0, 2) == 0 && device::sourceChannelFor(1, 2) == 1,
          "a stereo plug-in is heard channel for channel");
    check(device::sourceChannelFor(2, 2) == -1 && device::sourceChannelFor(0, 0) == -1,
          "a channel the plug-in has no output for is silent; so is a plug-in with none");
    std::array<float, 3> mono{0.5f, -1.f, 0.f};
    const std::array<float, 3> right{0.5f, 1.f, 0.25f};
    device::foldIntoMono(mono.data(), right.data(), 3);
    check(mono == std::array<float, 3>{0.5f, 0.f, 0.125f},
          "a stereo channel into one input: the two sides folded, a centred signal at its own level");
    std::array<double, 2> wide{0.25, 0.75};
    const std::array<double, 2> wideRight{0.25, -0.75};
    device::foldIntoMono(wide.data(), wideRight.data(), 2);
    check(wide == std::array<double, 2>{0.25, 0.0}, "and at double width");
}
void expressionBlock(device::Vst3Host& host, const juce::PluginDescription& desc,
                     std::uint64_t (*completedProcessCalls)()) {
    constexpr double sampleRate = 48000.0;
    constexpr std::int32_t frames = 4096;
    constexpr std::uint64_t voices = 16;
    constexpr std::int32_t updates = 44;
    constexpr std::int32_t updateFrames = 96;
    constexpr std::array dimensions{ExpressionDim::Pitch, ExpressionDim::Pressure, ExpressionDim::Timbre,
                                    ExpressionDim::Gain, ExpressionDim::Pan};
    std::string error;
    auto instance = host.instantiate(desc, sampleRate, frames, error);
    if (!instance)
        throw std::runtime_error(error);
    device::Vst3Device vst(std::move(instance), device::identityOf(desc));
    vst.prepare(sampleRate, frames);
    check(vst.usingRawProcessor(), "expression budget exercises the raw VST3 host");

    std::vector<engine::Event> events;
    events.reserve(voices * (static_cast<std::size_t>(updates) * dimensions.size() + 2));
    for (std::uint64_t voice = 1; voice <= voices; ++voice)
        events.push_back({0, engine::EventType::NoteOn, 0, static_cast<std::uint16_t>(47 + voice), voice, 0, 0.8});
    for (std::int32_t update = 0; update < updates; ++update) {
        // Chase at the start, then a 500 Hz stream whose first grid point is frame 1.
        const auto frame = update == 0 ? 0 : 1 + (update - 1) * updateFrames;
        for (std::uint64_t voice = 1; voice <= voices; ++voice) {
            for (const auto dimension : dimensions) {
                events.push_back({frame, engine::EventType::NoteExpression, 0,
                                  static_cast<std::uint16_t>(dimension), voice, engine::kNativeNoteExpressionUnits,
                                  static_cast<double>(update) / updates});
            }
        }
    }
    for (std::uint64_t voice = 1; voice <= voices; ++voice)
        events.push_back({frames - 1, engine::EventType::NoteOff, 0,
                          static_cast<std::uint16_t>(47 + voice), voice, 0, 0.0});

    std::array<float, frames> left{}, right{};
    float* output[]{left.data(), right.data()};
    engine::NodeIo io;
    io.out = output;
    io.channels = 2;
    io.frames = frames;
    io.sampleRate = sampleRate;
    io.events = {events.data(), static_cast<std::int32_t>(events.size())};
    const auto completedBefore = completedProcessCalls();
    vst.process(io);
    check(completedProcessCalls() == completedBefore + 1, "the fixture processes one unsplit expression block");
    check(vst.expressionRoute() == engine::ExpressionRoute::NoteExpression, "the fixture uses VST3 note expression");
    const auto dropped = vst.eventsDropped();
    const auto outOfRange = vst.eventsOutOfRange();
    if (dropped != 0 || outOfRange != 0)
        std::printf("VST3 block of %zu events: %lld dropped, %lld out of range\n",
                    events.size(), static_cast<long long>(dropped), static_cast<long long>(outOfRange));
    check(dropped == 0, "the host keeps the full five-dimension expression block");
    check(outOfRange == 0, "the host keeps every event inside the full block");
}
using ReadLegacyMidi = int (*)(int*, int);
void mpeWire(device::Vst3Host& host, const juce::PluginDescription& unmapped,
             const juce::PluginDescription& mapped, ReadLegacyMidi readLegacyMidi,
             double (*mappedMemberBend)()) {
    constexpr double rate = 48000.0;
    constexpr int frames = 32;
    constexpr int segmentStart = 7;
    const std::array phrase{
        engine::Event{segmentStart, engine::EventType::NoteOn, 0, 60, 1, 0, 0.8},
        engine::Event{segmentStart + 5, engine::EventType::NoteExpression, 0,
                      static_cast<std::uint16_t>(ExpressionDim::Pitch), 1, 0, 6.5},
    };
    const engine::Event cc{segmentStart, engine::EventType::Controller, 4, 0, 0, 11, 0.8};
    constexpr int wireRows = 7;
    struct Observation {
        std::array<int, wireRows * 6> wire{};
        int legacyCount = 0;
        std::uint32_t unsupported = 0;
        double mappedBend = 0.0;
    };

    const auto run = [&](const juce::PluginDescription& desc, engine::RouteChoice choice,
                         engine::ExpressionRoute route, engine::EventSpan events) {
        std::string error;
        auto instance = host.instantiate(desc, rate, frames, error);
        if (!instance)
            throw std::runtime_error(error);
        device::Vst3Device vst(std::move(instance), device::identityOf(desc));
        vst.prepare(rate, frames);
        check(vst.usingRawProcessor() && vst.setExpressionRoute(choice), "wire test uses the raw VST3 host");
        std::array<float, frames> left{}, right{};
        float* output[]{left.data(), right.data()};
        engine::NodeIo io;
        io.out = output;
        io.channels = 2;
        io.frames = 11;
        io.blockOffset = segmentStart;
        io.sampleRate = rate;
        io.events = events;
        vst.process(io);
        check(vst.expressionRoute() == route, "host keeps the requested expression route");
        Observation result;
        result.legacyCount = readLegacyMidi(result.wire.data(), wireRows);
        result.unsupported = vst.unsupportedControllers();
        result.mappedBend = mappedMemberBend();
        return result;
    };
    const engine::EventSpan notes{phrase.data(), static_cast<std::int32_t>(phrase.size())};
    const auto legacy = run(unmapped, engine::RouteChoice::MpeMidi,
                            engine::ExpressionRoute::MpeMidi, notes);
    // Bus, segment offset, MIDI channel, controller, LSB, MSB.
    constexpr std::array expected{
        0, 0, 0, 101, 0, 0,
        0, 0, 0, 100, 6, 0,
        0, 0, 0, 6, 15, 0,
        0, 0, 1, static_cast<int>(engine::kCtrlPitchBend), 0, 64,
        0, 0, 1, 74, 64, 0,
        0, 0, 1, static_cast<int>(engine::kCtrlAfterTouch), 0, 0,
        0, 5, 1, static_cast<int>(engine::kCtrlPitchBend), 85, 72,
    };
    check(legacy.legacyCount == wireRows && legacy.wire == expected,
          "unmapped MpeMidi reaches the plug-in with exact MCM and member bend bytes and offsets");
    check(legacy.unsupported == 0, "delivered MpeMidi controls are not counted as unsupported");

    const auto parameters = run(mapped, engine::RouteChoice::MpeMidi,
                                engine::ExpressionRoute::MpeMidi, notes);
    constexpr double unroundedBend = (8192.0 + (6.5 / 48.0) * 8191.0) / 16383.0;
    check(parameters.legacyCount == 0 && parameters.unsupported == 0 &&
              std::abs(parameters.mappedBend - unroundedBend) < 1e-14,
          "mapped MpeMidi keeps the unrounded bend parameter and sends no legacy duplicates");

    const auto plain = run(unmapped, engine::RouteChoice::Plain,
                           engine::ExpressionRoute::Plain, {&cc, 1});
    check(plain.unsupported == 1 && plain.legacyCount == 0,
          "Plain still counts an unmapped ordinary CC without delivering it");
    const auto expression = run(unmapped, engine::RouteChoice::NoteExpression,
                                engine::ExpressionRoute::NoteExpression, {&cc, 1});
    check(expression.unsupported == 1 && expression.legacyCount == 0,
          "NoteExpression still counts an unmapped ordinary CC without delivering it");
    const auto ordinaryMpe = run(unmapped, engine::RouteChoice::MpeMidi,
                                 engine::ExpressionRoute::MpeMidi, {&cc, 1});
    check(ordinaryMpe.unsupported == 1 && ordinaryMpe.legacyCount == 3,
          "MpeMidi sends its configuration and counts the unmapped ordinary CC without delivering it");
}
using ReadNoteWire = int (*)(int*, int);
void mpeSwitchReleases(device::Vst3Host& host, const juce::PluginDescription& desc,
                       ReadNoteWire readNoteWire) {
    constexpr double rate = 48000.0;
    constexpr int frames = 16;
    constexpr int heldNotes = 128;
    constexpr int fields = 3;
    std::string error;
    auto instance = host.instantiate(desc, rate, frames, error);
    if (!instance)
        throw std::runtime_error(error);
    device::Vst3Device vst(std::move(instance), device::identityOf(desc));
    vst.prepare(rate, frames);
    check(vst.usingRawProcessor() && vst.setExpressionRoute(engine::RouteChoice::MpeMidi),
          "release test selects MpeMidi on the raw VST3 adapter");

    std::array<float, frames> left{}, right{};
    float* output[]{left.data(), right.data()};
    engine::NodeIo io;
    io.out = output;
    io.channels = 2;
    io.frames = frames;
    io.sampleRate = rate;
    std::array<engine::Event, frames> ons{};
    std::array<int, heldNotes> channels{};
    std::array<int, heldNotes * fields> wire{};
    bool started = true;
    for (int first = 0; first < heldNotes; first += frames) {
        for (int frame = 0; frame < frames; ++frame) {
            ons[static_cast<std::size_t>(frame)] = {frame, engine::EventType::NoteOn, 0, 60,
                static_cast<std::uint64_t>(first + frame + 1), 0, 0.8};
        }
        io.events = {ons.data(), frames};
        vst.process(io);
        const int count = readNoteWire(wire.data(), heldNotes);
        started = started && count == frames;
        for (int i = 0; i < frames && i < count; ++i) {
            const auto row = static_cast<std::size_t>(i * fields);
            const int id = wire[row];
            const int channel = wire[row + 1];
            started = started && id == first + i + 1 && channel >= 1 && channel <= 15 && wire[row + 2] == 1;
            channels[static_cast<std::size_t>(first + i)] = channel;
        }
    }
    check(started && vst.expressionRoute() == engine::ExpressionRoute::MpeMidi,
          "all 128 NoteOns reach the plug-in on MPE member channels across 16-frame blocks");

    check(vst.setExpressionRoute(engine::RouteChoice::Plain), "request the explicit switch from MpeMidi to Plain");
    io.events = {};
    vst.process(io);
    const int count = readNoteWire(wire.data(), heldNotes);
    bool exact = count == heldNotes;
    std::array<bool, heldNotes> released{};
    for (int i = 0; i < heldNotes && i < count; ++i) {
        const auto row = static_cast<std::size_t>(i * fields);
        const int id = wire[row];
        if (id < 1 || id > heldNotes) {
            exact = false;
            continue;
        }
        const auto note = static_cast<std::size_t>(id - 1);
        exact = exact && !released[note] && wire[row + 1] == channels[note] && wire[row + 2] == 0;
        released[note] = true;
    }
    check(exact && vst.expressionRoute() == engine::ExpressionRoute::Plain,
          "the first Plain block delivers exactly 128 distinct NoteOffs on their original MPE channels");

    bool drained = true;
    for (int block = 0; block < 3; ++block) {
        vst.process(io);
        drained = readNoteWire(nullptr, 0) == 0 && drained;
    }
    check(drained, "later Plain blocks carry no deferred or duplicate NoteOffs");
    check(vst.eventsDropped() == 0 && vst.eventsOutOfRange() == 0 && vst.paramsDropped() == 0,
          "the adapter drops no events or mapped parameters during the route switch");
}
struct KeyProbe {
    double (*first)();
    double (*last)();
    int (*channels)();
    int (*inPlace)();

    bool silent() const { return channels() == 0 && first() == 0. && last() == 0.; }
    bool stereoKey(double left, double right) const {
        return channels() == 2 && first() == left && last() == right;
    }
};

class StereoConstant final : public engine::Node {
  public:
    StereoConstant(float left, float right) : levels_{left, right} {}
    void process(const engine::NodeIo &io) noexcept override {
        for (std::int32_t ch = 0; ch < io.channels; ++ch)
            std::fill_n(io.out[ch] + io.blockOffset, io.frames, levels_[static_cast<std::size_t>(ch)]);
    }
  private:
    std::array<float, 2> levels_;
};

class KeyHalfGain final : public device::DeviceInstance {
  public:
    static constexpr auto uid = "test.adidaw.key-half-gain";
    const device::DeviceIdentity &identity() const noexcept override { return identity_; }
    bool loaded() const noexcept override { return true; }
    void process(const engine::NodeIo &io) noexcept override {
        for (std::int32_t channel = 0; channel < io.channels; ++channel) {
            auto *output = io.out[channel] + io.blockOffset;
            const auto *input = io.in && io.in[channel] ? io.in[channel] + io.blockOffset : nullptr;
            if (input)
                std::transform(input, input + io.frames, output, [](float sample) { return .5f * sample; });
            else
                std::fill_n(output, io.frames, 0.f);
        }
    }
  private:
    const device::DeviceIdentity identity_{"internal", uid, "Key Half Gain", "", ""};
};

using RefuseStateStreams = void (*)(unsigned);
constexpr unsigned componentStream = 1, controllerStream = 2;

void stateCaptureContract(device::Vst3Host &host, const juce::PluginDescription &description,
                          RefuseStateStreams refuseStreams) {
    const auto make = [&] {
        std::string error;
        auto adapter = host.makeDevice(description, 48000., 32, error);
        if (!adapter) throw std::runtime_error(error);
        return adapter;
    };
    const auto capture = [refuseStreams](device::DeviceInstance &adapter, unsigned refused) {
        refuseStreams(refused);
        auto bytes = adapter.saveState("chunk");
        refuseStreams(0);
        return bytes;
    };
    const auto hasStreams = [](const std::vector<std::uint8_t> &bytes, unsigned expected) {
        const auto xml = juce::AudioProcessor::getXmlFromBinary(bytes.data(), static_cast<int>(bytes.size()));
        return xml && xml->hasTagName("VST3PluginState") &&
               (xml->getChildByName("IComponent") != nullptr) == ((expected & componentStream) != 0) &&
               (xml->getChildByName("IEditController") != nullptr) == ((expected & controllerStream) != 0);
    };
    const auto roundTrips = [&](const std::vector<std::uint8_t> &bytes, unsigned refused) {
        auto reopened = make();
        return !bytes.empty() && reopened->loadState("chunk", bytes) && capture(*reopened, refused) == bytes;
    };
    auto adapter = make();
    const auto baseline = capture(*adapter, 0);
    check(hasStreams(baseline, componentStream | controllerStream),
          "SC-430: the native fixture hands over both VST3 streams");
    check(capture(*adapter, componentStream | controllerStream).empty(),
          "SC-430: the JUCE wrapper alone does not make a refused VST3 state readable");
    check(capture(*adapter, componentStream).empty(),
          "SC-430: losing a previously captured component stream fails capture");
    check(capture(*adapter, controllerStream).empty(),
          "SC-430: losing a previously captured controller stream fails capture");
    check(!baseline.empty() && capture(*adapter, 0) == baseline,
          "SC-430: a healthy retry returns both intact VST3 streams");

    auto stateless = make();
    check(hasStreams(capture(*stateless, componentStream | controllerStream), 0),
          "SC-430: a VST3 that has never provided either stream saves an empty wrapper");
    auto component = make();
    const auto componentOnly = capture(*component, controllerStream);
    check(hasStreams(componentOnly, componentStream),
          "SC-430: a VST3 that has never provided controller state still saves its component");
    check(roundTrips(componentOnly, controllerStream),
          "SC-430: reopening a saved component-only VST3 still permits its optional controller stream");
    auto controller = make();
    const auto controllerOnly = capture(*controller, componentStream);
    check(hasStreams(controllerOnly, controllerStream),
          "SC-430: a VST3 that has never provided component state still saves its controller");
    check(roundTrips(controllerOnly, componentStream),
          "SC-430: reopening a saved controller-only VST3 still permits its optional component stream");
}

void documentContract(device::Vst3Host &host, const juce::PluginDescription &description,
                      const KeyProbe &keyProbe, RefuseStateStreams refuseStreams) {
    constexpr std::int64_t keyTrack = 2, programmeTrack = 3, recipientId = 30;
    constexpr std::int32_t frames = 256;
    const auto identity = device::identityOf(description);
    test::TempDirectory temp("vst3_key", "document_dormant_aux");
    const auto file = temp.path() / "dormant-aux.adi";
    {
        StoreError status{};
        auto store = Store::create(file, status);
        if (!store) throw std::runtime_error(toString(status));
        store->db().exec("INSERT INTO tracks(id,kind,name) VALUES "
                         "(1,'master','Main'),(2,'audio','Key'),(3,'audio','Programme');"
                         "INSERT INTO device_chains(id,track_id,name) VALUES (2,2,'Key FX'),(3,3,'Devices');");
        SQLite::Statement scale(store->db(),
            "INSERT INTO plugin_refs(id,format,uid,name) VALUES (76,'internal',?,'Key Half Gain')");
        scale.bind(1, KeyHalfGain::uid);
        scale.exec();
        SQLite::Statement ref(store->db(),
            "INSERT INTO plugin_refs(id,format,uid,name) VALUES (77,'vst3',?,?)");
        ref.bind(1, identity.uid);
        ref.bind(2, identity.name);
        ref.exec();
        store->db().exec("INSERT INTO devices(id,chain_id,ord,plugin_ref_id,name) "
                         "VALUES (20,2,0,76,'Key Half Gain'),(30,3,0,77,'Dormant auxiliary');");
    }

    // The real host adapter receives the discovered owned module via the
    // document's usual loader, without scanning other plug-ins.
    const engine::DeviceLoader loader = [&](const engine::DeviceRequest &request, std::string &error)
        -> std::unique_ptr<device::DeviceInstance> {
        if (request.ref && request.ref->format == "internal" && request.ref->uid == KeyHalfGain::uid)
            return std::make_unique<KeyHalfGain>();
        if (!request.ref || request.ref->format != identity.format || request.ref->uid != identity.uid) {
            error = "Unexpected device in the dormant auxiliary fixture";
            return {};
        }
        return host.makeDevice(description, request.sampleRate, request.maxFrames, error);
    };
    StereoConstant key(.5f, .75f), programme(.125f, .25f);
    const auto prepare = [&](ui::ProjectDocument &document) {
        auto &session = document.session();
        session.setSourcesFor([&](std::int64_t track) {
            if (track == keyTrack) return std::vector<engine::Node *>{&key};
            if (track == programmeTrack) return std::vector<engine::Node *>{&programme};
            return std::vector<engine::Node *>{};
        });
        session.graph().setFadeFrames(0);
        document.prepare(48000., frames);
        return document.mailbox().post(ui::TransportMailbox::Command::Play);
    };
    using Audio = std::array<std::array<float, frames>, 2>;
    const auto render = [](ui::ProjectDocument &document) {
        Audio output{};
        std::array<float *, 2> planes{output[0].data(), output[1].data()};
        engine::AudioIo io;
        io.out = planes.data();
        io.numOut = 2;
        io.frames = frames;
        document.process(io);
        return output;
    };
    const auto routed = [&](ui::ProjectDocument &document, const char *tap) {
        const auto &routes = document.session().model().routing;
        return routes.size() == 1 && routes.front().srcId == keyTrack &&
               routes.front().dstId == recipientId && routes.front().keyTap == tap;
    };
    const auto presented = [](ui::ProjectDocument &document) {
        const auto publication = document.parameterFeed().current();
        const auto found = std::find_if(publication->begin(), publication->end(),
            [](const ui::DevicePanelData &panel) { return panel.id == recipientId; });
        if (found == publication->end()) throw std::runtime_error("The fixture device panel was not published");
        return *found;
    };

    std::string error;
    auto document = ui::ProjectDocument::open(file, false, loader, error);
    if (!document) throw std::runtime_error(error);
    check(prepare(*document), "the document audio callback accepts a queued Play");
    document->tick(1);
    auto *device = dynamic_cast<device::Vst3Device *>(document->session().instanceFor(recipientId));
    if (!device) throw std::runtime_error("The document did not load the owned VST3 adapter");
    check(device->usingRawProcessor() && device->sidechainInputChannels() == 2,
          "the real graph has a declared but initially dormant stereo auxiliary bus");
    const auto baseline = render(*document);
    check(keyProbe.silent() && keyProbe.inPlace() == 1 &&
              std::all_of(baseline[0].begin(), baseline[0].end(), [](float sample) { return sample > 0.f; }) &&
              std::all_of(baseline[1].begin(), baseline[1].end(), [](float sample) { return sample > 0.f; }),
          "programme plays through the actual SDK callback with the dormant key closed");

    check(document->setDeviceKey(presented(*document), keyTrack, "post_fx") && routed(*document, "post_fx"),
          "assigning the key through ProjectDocument commits the intended post-FX route");
    const auto assigned = render(*document);
    check(keyProbe.stereoKey(.25, .375) && assigned == baseline,
          "the first callback after assigning a dormant key receives the post-FX samples and unchanged programme");
    const auto configuredActivations = device->activations();
    const auto *timelineReader = document->session().clips();
    document->tick(2);
    check(device->activations() == configuredActivations && document->session().clips() == timelineReader,
          "the receipt drain does not activate or rebuild the just-configured key bus again");

    check(document->setDeviceKey(presented(*document), keyTrack, "pre_fx") && routed(*document, "pre_fx"),
          "a retap changes the stored source tap without changing bus width");
    const auto retapped = render(*document);
    check(keyProbe.stereoKey(.5, .75) && retapped == baseline && device->activations() == configuredActivations &&
              document->session().clips() == timelineReader,
          "a retap with the same auxiliary shape preserves the SDK, timeline reader and programme");

    check(document->setDeviceKey(presented(*document), {}, "pre_fx") && document->session().model().routing.empty(),
          "removing the route from the document clears just its key");
    (void)render(*document);
    check(keyProbe.silent(), "the first callback after removal closes the default-inactive auxiliary again");
    check(document->ops().undo().ok && document->synchronise() && routed(*document, "pre_fx"),
          "one Undo restores the complete pre-FX key route");
    (void)render(*document);
    check(keyProbe.stereoKey(.5, .75), "Undo reopens the dormant bus and restores the pre-FX key samples");
    check(document->ops().redo().ok && document->synchronise() && document->session().model().routing.empty(),
          "one Redo removes the route again");
    (void)render(*document);
    check(keyProbe.silent(), "Redo closes the auxiliary bus again");

    check(document->ops().undo().ok && document->synchronise() && routed(*document, "pre_fx"),
          "leave the authored route for a clean reopen");
    const auto chunkHash = [](ui::ProjectDocument &project) {
        const auto &states = project.session().model().pluginState;
        const auto chunk = std::find_if(states.begin(), states.end(), [](const auto &state) {
            return state.deviceId == recipientId && state.role == "chunk";
        });
        return chunk == states.end() ? std::string{} : chunk->hash;
    };
    check(document->takeSnapshot("Healthy VST3", 1000, 0).ok, "SC-430: save the native VST3 baseline");
    const auto savedHash = chunkHash(*document);
    document->release();
    document.reset();
    auto reopened = ui::ProjectDocument::open(file, false, loader, error);
    if (!reopened) throw std::runtime_error(error);
    check(routed(*reopened, "pre_fx") && prepare(*reopened), "the reopened project reads the stored route");
    (void)render(*reopened);
    check(keyProbe.stereoKey(.5, .75), "first reopened audio callback hears the stored pre-FX key without a new edit");
    refuseStreams(controllerStream);
    const auto staleSnapshot = reopened->takeSnapshot("VST3 controller refused", 1001, 0);
    refuseStreams(0);
    check(staleSnapshot.ok && !savedHash.empty() && chunkHash(*reopened) == savedHash &&
              reopened->pluginStateWarning().find("Dormant auxiliary") != std::string::npos,
          "SC-430: Take Snapshot names the reopened VST3 and retains its last saved chunk after a refusal");
    reopened->release();
}

void contract() {
    declaredMidiMap();
    monoOnStereo();
    test::TempDirectory temp("vst3_key", "owned");
    scan::Config config;
    config.executable = native(ADI_SCAN_HELPER);
    config.registry = temp.path() / "registry.sqlite";
    device::Vst3Host host;
    juce::KnownPluginList list;
    const auto scan = host.scanFile(juce::String::fromUTF8(ADI_TEST_VST3), list, config);
    check(scan.complete && list.getNumTypes() == 3,
          "dedicated scanner discovers only owned fixture variants");
    // Observe the exact module the format loaded inside its bundle, not the
    // separate build output (which would have independent process globals).
    const juce::File bundle(juce::String::fromUTF8(ADI_TEST_VST3));
    juce::File binary;
    for (const auto &file :
         bundle.getChildFile("Contents").findChildFiles(juce::File::findFiles, true))
        if (file.getFileNameWithoutExtension() == bundle.getFileNameWithoutExtension())
            binary = file;
    juce::DynamicLibrary module(binary.getFullPathName());
    auto first = reinterpret_cast<double (*)()>(module.getFunction("adiFixtureKeyFirst"));
    auto last = reinterpret_cast<double (*)()>(module.getFunction("adiFixtureKeyLast"));
    auto channels = reinterpret_cast<int (*)()>(module.getFunction("adiFixtureKeyChannels"));
    auto inPlace=reinterpret_cast<int(*)()>(module.getFunction("adiFixtureMainInPlace"));
    const auto completedProcessCalls = reinterpret_cast<std::uint64_t (*)()>(
        module.getFunction("adiFixtureCompletedProcessCalls"));
    const auto readLegacyMidi = reinterpret_cast<ReadLegacyMidi>(module.getFunction("adiFixtureReadLegacyMidi"));
    const auto mappedMemberBend = reinterpret_cast<double (*)()>(module.getFunction("adiFixtureMappedMemberBend"));
    const auto readNoteWire = reinterpret_cast<ReadNoteWire>(module.getFunction("adiFixtureReadNoteWire"));
    const auto refuseStreams = reinterpret_cast<RefuseStateStreams>(module.getFunction("adiFixtureRefuseStateStreams"));
    auto auxiliaryBuffersValid = reinterpret_cast<int (*)()>(module.getFunction("adiFixtureAuxiliaryBuffersValid"));
    auto auxiliarySinkSilent = reinterpret_cast<int (*)()>(module.getFunction("adiFixtureAuxiliarySinkSilent"));
    if (!first || !last || !channels || !inPlace || !auxiliaryBuffersValid || !auxiliarySinkSilent ||
        !completedProcessCalls || !readLegacyMidi || !mappedMemberBend || !readNoteWire || !refuseStreams)
        throw std::runtime_error("owned fixture observations unavailable");
    const auto types = list.getTypes();
    const auto expression = std::find_if(types.begin(), types.end(),
        [](const auto& desc) { return desc.name == "ADI Test NoteExpr"; });
    const auto mapped = std::find_if(types.begin(), types.end(),
        [](const auto& desc) { return desc.name == "ADI Test MPE"; });
    if (expression == types.end() || mapped == types.end())
        throw std::runtime_error("owned VST3 expression fixture not discovered");
    stateCaptureContract(host, *expression, refuseStreams);
    mpeWire(host, *expression, *mapped, readLegacyMidi, mappedMemberBend);
    mpeSwitchReleases(host, *mapped, readNoteWire);
    expressionBlock(host, *expression, completedProcessCalls);
    const auto processorDrive = reinterpret_cast<double (*)()>(module.getFunction("adiFixtureProcessorDrive"));
    check(processorDrive != nullptr, "the fixture reports Drive as its processor received it");
    if (processorDrive != nullptr)
        for (const bool offline : {false, true})
            valuesReachProcessor(host, *expression, processorDrive, offline);
    if (processorDrive != nullptr)
        editorAndAutomationFollow(host, *expression, processorDrive);
    const auto rename = reinterpret_cast<void (*)(int)>(module.getFunction("adiFixtureRenameDrive"));
    check(rename != nullptr, "the fixture can rename its Drive parameter");
    if (rename != nullptr)
        renamedParameterShows(host, *expression, rename);
    for (const auto &desc : types)
        for (bool wide : {false, true}) {
            std::string error;
            auto instance = host.instantiate(desc, 48000, 32, error);
            if (!instance)
                throw std::runtime_error(error);
            device::Vst3Device d(std::move(instance), device::identityOf(desc));
            const bool mono = desc.name.containsIgnoreCase("Plain");
            const bool additionalBuses = desc.name.containsIgnoreCase("NoteExpr");
            const bool defaultActive = mono;
            const int keyWidth = mono ? 1 : 2;
            check(d.sidechainInputChannels() == keyWidth,
                  "declared auxiliary width is cached before prepare");
            d.willBeHanded(wide ? engine::Precision::Float64 : engine::Precision::Float32);
            d.prepare(48000, 32);
            const auto initialShape = d.preparationShapeEpoch();
            const auto initialActivations = d.activations();
            check(d.configureSidechain(true), "owner can request the declared key input");
            check((d.preparationShapeEpoch() == initialShape) == defaultActive,
                  defaultActive ? "connecting a default-active key preserves the preparation stamp"
                                : "connecting a dormant key changes the preparation stamp");
            d.prepare(48000, 32);
            check(d.activations() == initialActivations + (defaultActive ? 0 : 1),
                  defaultActive ? "connecting a default-active key preserves activation"
                                : "connecting a dormant key prepares the changed layout once");
            check(d.usingRawProcessor(), "test exercises production raw VST3 bus tables");
            std::array<float, 32> in{}, inR{}, keyL{}, keyR{}, outL{}, outR{};
            std::array<double, 32> in64{}, inR64{}, keyL64{}, keyR64{}, outL64{}, outR64{};
            for (std::size_t i = 0; i < 32; ++i) {
                in[i] = (static_cast<float>(i) - 16.f) / 32.f;
                inR[i] = .75f - static_cast<float>(i) / 64.f;
                // Distinct channel patterns and double-only low bits catch
                // channel swaps, offsets and an unintended float round-trip.
                in64[i] = static_cast<double>(in[i]) + static_cast<double>(i + 1) * 1e-11;
                inR64[i] = static_cast<double>(inR[i]) - static_cast<double>(i + 1) * 3e-11;
                keyL[i] = static_cast<float>(i) / 32;
                keyL64[i] = static_cast<double>(i) / 32;
                keyR[i] = .5f;
                keyR64[i] = .5;
                outL[i] = outR[i] = -1;
                outL64[i] = outR64[i] = -1;
            }
            const float *input[]{in.data(), inR.data()};
            const float *key[]{keyL.data(), keyR.data()};
            float *output[]{outL.data(), outR.data()};
            const double *input64[]{in64.data(), inR64.data()};
            const double *key64[]{keyL64.data(), keyR64.data()};
            double *output64[]{outL64.data(), outR64.data()};
            engine::NodeIo io;
            io.channels = 2;
            io.frames = 11;
            io.blockOffset = 7;
            io.sampleRate = 48000;
            if (wide) {
                io.in64 = input64;
                io.out64 = output64;
                io.sidechain64 = key64;
            } else {
                io.in = input;
                io.out = output;
                io.sidechain = key;
            }
            io.sidechainSilent = false;
            d.process(io);
            check(channels() == keyWidth && first() == 7. / 32 &&
                      last() == (mono ? 17. / 32 : .5),
                  "exact mono/stereo key segment reaches the aux bus at native/narrowed precision");
            check(inPlace()==1,"aux delivery retains the main in-place SDK fast path");
            if (additionalBuses) {
                check(auxiliaryBuffersValid() == 1, "four inputs and three outputs cover silent, inactive and sink buses");
            }
            const auto programmeExact = [&] {
                for (std::size_t i = 0; i < 32; ++i) {
                    const bool inside = i >= 7 && i < 18;
                    if (wide) {
                        const double left =
                            inside ? (d.processingDouble()
                                          ? in64[i]
                                          : static_cast<double>(static_cast<float>(in64[i])))
                                   : -1.;
                        const double right =
                            inside ? (d.processingDouble()
                                          ? inR64[i]
                                          : static_cast<double>(static_cast<float>(inR64[i])))
                                   : -1.;
                        if (outL64[i] != left || outR64[i] != right)
                            return false;
                    } else if (outL[i] != (inside ? in[i] : -1.f) ||
                               outR[i] != (inside ? inR[i] : -1.f))
                        return false;
                }
                return true;
            };
            check(programmeExact(), "all programme samples/channels retain exact supported "
                                    "precision and segment bounds");
            const auto activations = d.activations();
            d.prepare(48000, 32);
            check(d.activations() == activations, "unchanged rebuild never restarts the plugin");
            io.sidechain = nullptr;
            io.sidechain64 = nullptr;
            io.sidechainSilent = true;
            d.process(io);
            check(programmeExact(),
                  "clearing the key preserves the entire programme array exactly");
            check(first() == 0 && last() == 0,
                  "disconnected key is silent, not stale data or programme");
            if (additionalBuses) {
                check(auxiliaryBuffersValid() == 1, "additional buffers stay distinct with a disconnected key");
                check(auxiliarySinkSilent() == 1, "auxiliary sink starts clear on the next segment");
            }
            const auto connectedShape = d.preparationShapeEpoch();
            check(d.configureSidechain(false), "owner can disable a formerly requested key");
            check((d.preparationShapeEpoch() == connectedShape) == defaultActive,
                  defaultActive ? "disconnecting a default-active key preserves the preparation stamp"
                                : "disconnecting a requested dormant key changes the preparation stamp");
            d.prepare(48000, 32);
            check(d.activations() == activations + (defaultActive ? 0 : 1),
                  defaultActive ? "disconnecting a default-active key preserves activation"
                                : "disconnecting a requested dormant key prepares the changed layout once");
            d.process(io);
            check(inPlace()==1 && programmeExact(),"removing the key retains in-place processing and exact full programme arrays");
            if (additionalBuses) {
                check(auxiliaryBuffersValid() == 1, "dormant key leaves the later active auxiliary slot addressable");
            }
            check(channels() == (defaultActive ? keyWidth : 0) && first() == 0 && last() == 0,
                  defaultActive ? "default-active aux carries silence after route removal"
                                : "default-inactive aux is deactivated after route removal");
        }
    const auto descriptions = list.getTypes();
    const auto stereo = std::find_if(descriptions.begin(), descriptions.end(), [](const auto &description) {
        return description.name == "ADI Test NoteExpr";
    });
    if (stereo == descriptions.end()) throw std::runtime_error("Owned stereo auxiliary fixture unavailable");
    documentContract(host, *stereo, {first, last, channels, inPlace}, refuseStreams);
}
} // namespace
int main() {
    juce::ScopedJuceInitialiser_GUI init;
    try {
        contract();
    } catch (const std::exception &e) {
        ++failures;
        std::printf("EXCEPTION: %s\n", e.what());
    }
    std::printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
