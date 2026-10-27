// SPDX-License-Identifier: GPL-3.0-or-later
// Adapter parity, against each actual compiled Pd canvas. These are ADI-core
// renders, not measurements of Ableton Live and not claims of Live parity.
#include "adi/blob.hpp"
#include "adi/engine/graph.hpp"
#include "adi/pd_builtins/colorcab_sample.hpp"
#include "adi/pd_builtins/native_devices.hpp"
#include "juce/pd_engine.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cstdlib>
#include <new>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>

namespace {
thread_local bool countAllocations = false;
thread_local std::size_t allocations = 0;
}
void* operator new(std::size_t size) {
    if (countAllocations) ++allocations;
    if (auto* allocation = std::malloc(size ? size : 1)) return allocation;
    throw std::bad_alloc();
}
void* operator new[](std::size_t size) { return operator new(size); }
void operator delete(void* allocation) noexcept { std::free(allocation); }
void operator delete[](void* allocation) noexcept { std::free(allocation); }
void operator delete(void* allocation, std::size_t) noexcept { std::free(allocation); }
void operator delete[](void* allocation, std::size_t) noexcept { std::free(allocation); }
namespace {
using adi::device::LibPdEngine;
int checks = 0, failures = 0;
void check(bool ok, const char* device, const char* description) {
    ++checks;
    if (!ok) { ++failures; std::printf("FAIL %s: %s\n", device, description); }
}
bool sameSample(float left, float right) noexcept {
    // dac~ adds its signal into a cleared +0 output bus; -0 + +0 becomes
    // +0. The direct core has no summing bus. Only that IEEE zero sign may
    // differ: every nonzero result must retain exactly the same float bits.
    return (left == 0 && right == 0) ||
           std::bit_cast<std::uint32_t>(left) == std::bit_cast<std::uint32_t>(right);
}
std::string read(const std::string& path) {
    std::ifstream stream(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
}
struct Change { int id; float value; };
struct Fixture {
    const char* file;
    int inputs;
    std::array<Change, 4> changes;
};
constexpr std::array<Fixture, 17> fixtures{{
    {"AutoFilter.pd", 4, {{{3, 2500}, {11, 38}, {33, 1}, {34, 65}}}},
    {"Chord Comb.pd", 2, {{{1, 3}, {3, 0.2f}, {4, 0.4f}, {5, 0.7f}}}},
    {"Chorus-Ensemble.pd", 2, {{{3, 160}, {10, 2.3f}, {11, 70}, {16, 75}}}},
    {"Color Cab.pd", 2, {{{2, 0.7f}, {0, 0}, {0, 0}, {0, 0}}}},
    {"Delay.pd", 2, {{{3, 1}, {5, 1}, {10, 55}, {25, 80}}}},
    {"EQEight.pd", 2, {{{3, 1100}, {4, 6}, {83, 0}, {87, 1}}}},
    {"Echo.pd", 2, {{{4, 1}, {6, 0.0625f}, {15, 48}, {0, 0}}}},
    {"MultibandDynamics.pd", 4, {{{17, -32}, {23, 0.6f}, {35, 1}, {37, 70}}}},
    {"Overdrive.pd", 2, {{{1, 700}, {3, 75}, {4, 31}, {6, 60}}}},
    {"Phaser-Flanger.pd", 2, {{{7, 1}, {9, 0.25f}, {19, 74}, {20, 35}}}},
    {"Redux.pd", 2, {{{1, 7800}, {2, 33}, {3, 7}, {9, 70}}}},
    {"Resonators.pd", 2, {{{5, 0.4f}, {8, 43}, {13, 5}, {30, 70}}}},
    {"Reverb.pd", 2, {{{5, 1}, {20, 4}, {23, 0.3f}, {31, 72}}}},
    {"Saturator.pd", 2, {{{2, 9}, {8, 1300}, {11, 70}, {18, 1}}}},
    {"Shifter.pd", 2, {{{1, 2}, {2, 7}, {31, 1}, {33, 12}}}},
    {"Utility.pd", 2, {{{4, 140}, {11, 6}, {12, -20}, {14, 1}}}},
    {"Vocoder.pd", 4, {{{1, 1}, {12, 2}, {20, 2}, {24, 70}}}}
}};
std::unique_ptr<adi::device::PdSampleBuffer> sample(double rate = 48000, double level = 1) {
    auto result = std::make_unique<adi::device::PdColorCabSample>();
    result->sampleRate = rate;
    result->channels = 1;
    result->frames = 1;
    result->interleaved = {1.0f};
    result->design.sampleRate = rate;
    result->design.kernel.size = 3;
    result->design.kernel.taps[0] = 0.7 * level;
    result->design.kernel.taps[1] = -0.2 * level;
    result->design.kernel.taps[2] = 0.1 * level;
    return result;
}
void parity(const Fixture& fixture) {
    const auto text = read(std::string(ADI_DEVICE_PATCH_DIR) + "/" + fixture.file);
    const auto declarations = adi::device::parsePdDeclarations(text);
    LibPdEngine native(ADI_DEVICE_PATCH_DIR, fixture.file, fixture.inputs, 2);
    LibPdEngine pd(ADI_DEVICE_PATCH_DIR, fixture.file, fixture.inputs, 2,
                   LibPdEngine::Execution::PureData, LibPdEngine::BlockMode::WholeBlocks);
    native.addSearchPath(ADI_PD_PATCH_DIR);
    pd.addSearchPath(ADI_PD_PATCH_DIR);
    adi::device::PdLatencyReceiver nativeLatency, pdLatency;
    std::string error;
    check(native.open(nativeLatency, error), fixture.file, "native opens");
    check(pd.open(pdLatency, error), fixture.file, "Pd opens");
    check(native.isNative() && !pd.isNative(), fixture.file, "independent execution paths");
    native.prepare(48000, 64);
    pd.prepare(48000, 64);
    native.bindParameters(declarations);
    pd.bindParameters(declarations);
    native.bindSamples(declarations);
    pd.bindSamples(declarations);
    if (!declarations.samples.empty()) {
        check(native.publishSample(1, sample()) && pd.publishSample(1, sample()),
              fixture.file, "prepared sample publishes to both engines");
    }
    // Offset sentinels and four output planes cover the adapter's complete
    // channel contract, including null planes and the mapped sidechain bus.
    std::array<std::array<float, 80>, 4> input{}, sidechain{}, actual{}, expected{};
    std::array<const float*, 4> in{}, sc{};
    std::array<float*, 4> outNative{}, outPd{};
    constexpr int offset = 7;
    double maxError = 0, wrongDelayError = 0, bypassError = 0;
    bool guards = true, extraSilent = true, finite = true, bitExact = true;
    allocations = 0;
    float previous = 0;
    adi::engine::TransportInfo transport;
    transport.playing = true;
    transport.bpm = 137.25;
    for (int block = 0; block < 320; ++block) {
        if (block == 32) {
            for (const auto& change : fixture.changes) {
                if (change.id == 0) continue;
                countAllocations = true;
                const bool nativeSent = native.sendParameter(change.id, change.value);
                countAllocations = false;
                check(nativeSent && pd.sendParameter(change.id, change.value),
                      fixture.file, "nondefault parameter reaches both engines");
            }
        }
        if (block == 190) transport.bpm = 93.75;
        for (std::size_t channel = 0; channel < input.size(); ++channel) {
            in[channel] = input[channel].data();
            sc[channel] = sidechain[channel].data();
            outNative[channel] = actual[channel].data();
            outPd[channel] = expected[channel].data();
            actual[channel].fill(123.0f);
            expected[channel].fill(123.0f);
            for (std::size_t frame = 0; frame < input[channel].size(); ++frame) {
                const auto position = static_cast<double>(block * 64) + static_cast<double>(frame);
                const double phase = 0.047 * position + static_cast<double>(channel);
                input[channel][frame] = static_cast<float>(0.2 * std::sin(phase) +
                                                           0.09 * std::cos(phase * 2.71));
                sidechain[channel][frame] = static_cast<float>(0.4 * std::cos(phase * 0.37));
            }
        }
        adi::engine::NodeIo io;
        io.in = in.data();
        io.sidechain = sc.data();
        io.channels = block >= 64 && block < 96 ? 1 : block >= 96 && block < 128 ? 4 : 2;
        io.blockOffset = offset;
        io.frames = 64;
        io.transport = &transport;
        if (block >= 128 && block < 160) io.in = nullptr;
        if (block >= 160 && block < 176) in[1] = nullptr;
        if (block >= 176 && block < 184) outNative[1] = outPd[1] = nullptr;
        if (block >= 184 && block < 190) io.sidechain = nullptr;
        adi::engine::Event event;
        if (std::string_view(fixture.file) == "Shifter.pd") {
            event.frame = offset;
            event.noteId = 42;
            if (block == 40 || block == 230) {
                event.type = block == 40 ? adi::engine::EventType::NoteOn
                                        : adi::engine::EventType::NoteOff;
                event.dim = 60;
                event.value = block == 40 ? 0.71 : 0;
                io.events = {&event, 1};
            } else if (block == 100) {
                event.type = adi::engine::EventType::NoteExpression;
                event.dim = static_cast<std::uint16_t>(adi::ExpressionDim::Pitch);
                event.value = 2.75;
                io.events = {&event, 1};
            }
        }
        io.out = outNative.data();
        countAllocations = true;
        native.process(io);
        countAllocations = false;
        io.out = outPd.data();
        pd.process(io);
        for (int channel = 0; channel < io.channels; ++channel) {
            const auto c = static_cast<std::size_t>(channel);
            if (!outNative[c]) continue;
            for (std::size_t frame = 0; frame < actual[c].size(); ++frame) {
                if (frame < offset || frame >= offset + 64) {
                    guards &= actual[c][frame] == 123.0f && expected[c][frame] == 123.0f;
                    continue;
                }
                finite &= std::isfinite(actual[c][frame]) && std::isfinite(expected[c][frame]);
                bitExact &= sameSample(actual[c][frame], expected[c][frame]);
                maxError = std::max(maxError, std::abs(static_cast<double>(actual[c][frame]) -
                                                       expected[c][frame]));
                if (channel >= 2) extraSilent &= actual[c][frame] == 0;
                if (channel == 0 && block >= 32) {
                    wrongDelayError = std::max(wrongDelayError,
                        std::abs(static_cast<double>(expected[c][frame]) - previous));
                    bypassError = std::max(bypassError,
                        std::abs(static_cast<double>(expected[c][frame]) - input[c][frame]));
                    previous = expected[c][frame];
                }
            }
        }
    }
    native.requestLatencyReport();
    pd.requestLatencyReport();
    check(nativeLatency.latencySamples() == pdLatency.latencySamples(), fixture.file,
          "native reports only the core latency");
    // Same core, input floats, message precision and 64-sample process calls:
    // only dac~'s zero-sign normalization may differ. This catches a default
    // rounded differently, even by one ulp, with no numeric tolerance.
    check(finite && bitExact && maxError == 0, fixture.file,
          "native and real-Pd renders match exactly apart from zero signs");
    check(allocations == 0, fixture.file, "native parameters and processing allocate nothing");
    check(guards && extraSilent, fixture.file, "segment guards and extra channels respected");
    check(wrongDelayError > 1e-5, fixture.file, "one-sample delay plant fails parity");
    check(bypassError > 1e-5, fixture.file, "bypass plant fails parity");
    std::printf("PARITY %s max-error %.9g\n", fixture.file, maxError);
}
void colorCabReplacements(double rate) {
    const auto text = read(std::string(ADI_DEVICE_PATCH_DIR) + "/Color Cab.pd");
    const auto declarations = adi::device::parsePdDeclarations(text);
    LibPdEngine wide(ADI_DEVICE_PATCH_DIR, "Color Cab.pd", 2, 2);
    LibPdEngine split(ADI_DEVICE_PATCH_DIR, "Color Cab.pd", 2, 2);
    LibPdEngine pd(ADI_DEVICE_PATCH_DIR, "Color Cab.pd", 2, 2,
                   LibPdEngine::Execution::PureData, LibPdEngine::BlockMode::WholeBlocks);
    std::array<LibPdEngine*, 3> engines{&wide, &split, &pd};
    std::array<adi::device::PdLatencyReceiver, 3> latency;
    std::string error;
    for (std::size_t index = 0; index < engines.size(); ++index) {
        engines[index]->addSearchPath(ADI_PD_PATCH_DIR);
        check(engines[index]->open(latency[index], error), "Color Cab retry", "opens");
        engines[index]->prepare(rate, 512);
        engines[index]->bindSamples(declarations);
    }
    std::array<float, 512> input{};
    std::array<const float*, 2> in{input.data(), input.data()};
    std::array<std::array<std::array<float, 512>, 2>, 3> output{};
    bool exact = true;
    for (int block = 0; block < 8; ++block) {
        if (block < 3) {
            constexpr std::array<double, 3> levels{1, 0.2, 1.8};
            for (auto* device : engines)
                check(device->publishSample(1, sample(rate, levels[static_cast<std::size_t>(block)])),
                      "Color Cab retry", "replacement published during preceding fade");
        }
        for (std::size_t frame = 0; frame < input.size(); ++frame)
            input[frame] = static_cast<float>(0.3 * std::sin(
                0.07 * (static_cast<double>(block * 512) + static_cast<double>(frame))));
        for (std::size_t index = 0; index < engines.size(); ++index) {
            std::array<float*, 2> out{output[index][0].data(), output[index][1].data()};
            adi::engine::NodeIo io;
            io.in = in.data(); io.out = out.data(); io.channels = 2;
            constexpr std::array<int, 4> pieces{7, 93, 1, 411};
            if (index == 1) {
                for (const int count : pieces) {
                    io.frames = count;
                    engines[index]->process(io);
                    io.blockOffset += count;
                }
            } else {
                const int count = index == 0 ? 512 : 64;
                io.frames = count;
                for (int offset = 0; offset < 512; offset += count) {
                    io.blockOffset = offset;
                    engines[index]->process(io);
                }
            }
        }
        for (std::size_t frame = 0; frame < input.size(); ++frame)
            for (std::size_t channel = 0; channel < 2; ++channel)
                for (std::size_t index = 0; index < 2; ++index)
                    exact &= std::bit_cast<std::uint32_t>(output[index][channel][frame]) ==
                             std::bit_cast<std::uint32_t>(output[2][channel][frame]);
    }
    check(exact, "Color Cab retry", "pending kernels retain Pd cadence across 512 and awkward segments");
    std::printf("PARITY Color Cab replacement %.0f Hz: %s\n", rate, exact ? "exact" : "failed");
}

void reprepareTail(const char* file) {
    const auto text = read(std::string(ADI_DEVICE_PATCH_DIR) + "/" + file);
    const auto declarations = adi::device::parsePdDeclarations(text);
    LibPdEngine native(ADI_DEVICE_PATCH_DIR, file, 2, 2);
    LibPdEngine pd(ADI_DEVICE_PATCH_DIR, file, 2, 2,
                   LibPdEngine::Execution::PureData, LibPdEngine::BlockMode::WholeBlocks);
    std::array<LibPdEngine*, 2> engines{&native, &pd};
    std::array<adi::device::PdLatencyReceiver, 2> latency;
    std::string error;
    for (std::size_t index = 0; index < engines.size(); ++index) {
        engines[index]->addSearchPath(ADI_PD_PATCH_DIR);
        check(engines[index]->open(latency[index], error), file, "tail test opens");
        engines[index]->prepare(48000.75, 64); // libpd's effective rate is 48000
        engines[index]->bindParameters(declarations);
        const int mix = std::string_view(file) == "Delay.pd" ? 25 : 31;
        engines[index]->sendParameter(mix, 100);
    }
    std::array<float, 64> input{};
    std::array<const float*, 2> in{input.data(), input.data()};
    std::array<std::array<std::array<float, 64>, 2>, 2> output{};
    bool exact = true;
    double tail = 0;
    for (int block = 0; block < 400; ++block) {
        if (block == 128) {
            native.prepare(48000, 511);
            pd.prepare(48000, 511);
        }
        for (std::size_t frame = 0; frame < input.size(); ++frame)
            input[frame] = block < 128 ? static_cast<float>(0.2 * std::sin(
                0.061 * (static_cast<double>(block * 64) + static_cast<double>(frame)))) : 0;
        for (std::size_t index = 0; index < engines.size(); ++index) {
            std::array<float*, 2> out{output[index][0].data(), output[index][1].data()};
            adi::engine::NodeIo io;
            io.in = in.data(); io.out = out.data(); io.channels = 2; io.frames = 64;
            engines[index]->process(io);
        }
        for (std::size_t frame = 0; frame < input.size(); ++frame) {
            for (std::size_t channel = 0; channel < 2; ++channel)
                exact &= std::bit_cast<std::uint32_t>(output[0][channel][frame]) ==
                         std::bit_cast<std::uint32_t>(output[1][channel][frame]);
            if (block >= 128) tail = std::max(tail, std::abs(static_cast<double>(output[1][0][frame])));
        }
    }
    check(exact, file, "same-rate prepare changes capacity without resetting the core");
    check(tail > 1e-5, file, "tail-reset plant would fail");
}

void recognition() {
    check(sameSample(0.0f, -0.0f) && !sameSample(1.0f, std::nextafter(1.0f, 2.0f)),
          "comparison", "zero signs match but a one-ulp plant fails");
    auto text = read(std::string(ADI_DEVICE_PATCH_DIR) + "/Utility.pd");
    auto native = adi::device::makeNativePdDevice(text, 2, 2);
    check(native != nullptr, "recognition", "canonical canvas recognized");
    check(!adi::device::makeNativePdDevice(text, 1, 2), "recognition", "wrong inputs rejected");
    check(!adi::device::makeNativePdDevice(text, 2, 1), "recognition", "wrong outputs rejected");
    std::string crlf;
    for (std::size_t i = 0; i < text.size(); ++i) {
        const char byte = text[i];
        if (byte == '\r' && i + 1 < text.size() && text[i + 1] == '\n') continue;
        if (byte == '\n') crlf.push_back('\r');
        crlf.push_back(byte);
    }
    check(adi::device::makeNativePdDevice(crlf, 2, 2) != nullptr, "recognition", "CRLF accepted");
    text += "#X text 1 1 edited;\n";
    check(!adi::device::makeNativePdDevice(text, 2, 2), "recognition", "edited patch rejected");
    const auto abstraction = read(std::string(ADI_PD_PATCH_DIR) + "/adi.param.pd");
    check(adi::device::nativePdAbstractionMatches("adi.param.pd", abstraction),
          "recognition", "canonical abstraction recognized");
    check(!adi::device::nativePdAbstractionMatches("adi.param.pd", abstraction + " "),
          "recognition", "edited abstraction rejected");
}
} // namespace
int main() {
    recognition();
    for (const auto& fixture : fixtures) parity(fixture);
    colorCabReplacements(48000);
    colorCabReplacements(44100);
    reprepareTail("Delay.pd");
    reprepareTail("Reverb.pd");
    std::printf("%s: native device parity, %d checks, %d failures\n",
                failures ? "FAIL" : "PASS", checks, failures);
    return failures ? 1 : 0;
}
