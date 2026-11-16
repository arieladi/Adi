// SPDX-License-Identifier: GPL-3.0-or-later
// Owned SDK fixture only. No editor, desktop input or audio device.
#include "juce/vst3_host.hpp"
#include "temp_directory.hpp"
#include <array>
#include <cstdio>
#include <stdexcept>
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
    if (!first || !last || !channels)
        throw std::runtime_error("owned fixture observations unavailable");
    for (const auto &desc : list.getTypes())
        for (bool wide : {false, true}) {
            std::string error;
            auto instance = host.instantiate(desc, 48000, 32, error);
            if (!instance)
                throw std::runtime_error(error);
            device::Vst3Device d(std::move(instance), device::identityOf(desc));
            const bool mono = desc.name.containsIgnoreCase("Plain");
            check(d.sidechainInputChannels() == (mono ? 1 : 2),
                  "declared dormant auxiliary width is cached before prepare");
            check(d.configureSidechain(true), "owner can request the declared key input");
            d.willBeHanded(wide ? engine::Precision::Float64 : engine::Precision::Float32);
            d.prepare(48000, 32);
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
            check(channels() == (mono ? 1 : 2) && first() == 7. / 32 &&
                      last() == (mono ? 17. / 32 : .5),
                  "exact mono/stereo key segment reaches the aux bus at native/narrowed precision");
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
            check(d.configureSidechain(false), "owner can disable a formerly requested key");
            d.prepare(48000, 32);
            d.process(io);
            check(channels() == 0, "default-inactive aux is deactivated after route removal");
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
