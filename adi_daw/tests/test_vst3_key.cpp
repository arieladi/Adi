// SPDX-License-Identifier: GPL-3.0-or-later
// Owned SDK fixture only. No editor, desktop input or audio device.
#include "adi/blob.hpp"
#include "juce/vst3_host.hpp"
#include "temp_directory.hpp"
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
void contract() {
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
    if (!first || !last || !channels || !inPlace || !completedProcessCalls ||
        !readLegacyMidi || !mappedMemberBend || !readNoteWire)
        throw std::runtime_error("owned fixture observations unavailable");
    const auto types = list.getTypes();
    const auto expression = std::find_if(types.begin(), types.end(),
        [](const auto& desc) { return desc.name == "ADI Test NoteExpr"; });
    const auto mapped = std::find_if(types.begin(), types.end(),
        [](const auto& desc) { return desc.name == "ADI Test MPE"; });
    if (expression == types.end() || mapped == types.end())
        throw std::runtime_error("owned VST3 expression fixture not discovered");
    mpeWire(host, *expression, *mapped, readLegacyMidi, mappedMemberBend);
    mpeSwitchReleases(host, *mapped, readNoteWire);
    expressionBlock(host, *expression, completedProcessCalls);
    for (const auto &desc : types)
        for (bool wide : {false, true}) {
            std::string error;
            auto instance = host.instantiate(desc, 48000, 32, error);
            if (!instance)
                throw std::runtime_error(error);
            device::Vst3Device d(std::move(instance), device::identityOf(desc));
            const bool mono = desc.name.containsIgnoreCase("Plain");
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
            check(channels() == (defaultActive ? keyWidth : 0) && first() == 0 && last() == 0,
                  defaultActive ? "default-active aux carries silence after route removal"
                                : "default-inactive aux is deactivated after route removal");
        }
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
