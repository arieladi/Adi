// SPDX-License-Identifier: GPL-3.0-or-later
// Owned SDK fixture only. No editor, desktop input or audio device.
#include "adi/blob.hpp"
#include "juce/vst3_host.hpp"
#include "temp_directory.hpp"
#include <algorithm>
#include <array>
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
    if (!first || !last || !channels || !inPlace || !completedProcessCalls)
        throw std::runtime_error("owned fixture observations unavailable");
    const auto types = list.getTypes();
    const auto expression = std::find_if(types.begin(), types.end(),
        [](const auto& desc) { return desc.name == "ADI Test NoteExpr"; });
    if (expression == types.end())
        throw std::runtime_error("owned VST3 expression fixture not discovered");
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
