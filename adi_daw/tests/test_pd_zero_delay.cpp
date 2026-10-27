// SPDX-License-Identifier: GPL-3.0-or-later
// ADR-0203 d4: native devices add no adapter delay; whole Pd ticks do not
// require a priming block. These tests use the native DSP as the oracle, not
// a second copy of the adapter, and deliberately exercise nonzero offsets.
#include "adi/dsp/auto_filter.hpp"
#include "adi/dsp/eq_eight.hpp"
#include "juce/pd_engine.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <new>
#include <string>
#include <system_error>
#include <vector>
#if defined(_MSC_VER) && defined(_DEBUG)
#include <crtdbg.h>
#endif

namespace {
thread_local bool countAllocations = false;
thread_local std::size_t allocations = 0;
}

void* operator new(std::size_t n) {
    if (countAllocations) ++allocations;
    if (void* p = std::malloc(n == 0 ? 1 : n)) return p;
    throw std::bad_alloc();
}
void* operator new[](std::size_t n) { return operator new(n); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }

#if defined(_MSC_VER) && defined(_DEBUG)
int allocationHook(int kind, void*, std::size_t, int, long, const unsigned char*, int) {
    if (countAllocations && (kind == _HOOK_ALLOC || kind == _HOOK_REALLOC)) ++allocations;
    return 1;
}
#endif

namespace {
using adi::device::LibPdEngine;
using adi::device::PdDeclarations;
using adi::device::PdLatencyReceiver;
using Execution = LibPdEngine::Execution;
using BlockMode = LibPdEngine::BlockMode;
int checks = 0;
int failures = 0;
constexpr float sentinel = -919.f;

void check(bool ok, const char* message) {
    ++checks;
    if (!ok) {
        ++failures;
        std::printf("FAIL %s\n", message);
    }
}

std::string readFile(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    check(input.good(), "test input file is readable");
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

void writeFile(const std::filesystem::path& path, const std::string& text) {
    std::ofstream output(path, std::ios::binary);
    output << text;
    output.close();
    check(output.good(), "test fixture written");
}

struct TempDir {
    std::filesystem::path path;
    TempDir() {
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        path = std::filesystem::temp_directory_path() /
               ("adi-pd-zero-delay-" + std::to_string(stamp));
        check(std::filesystem::create_directory(path), "temporary fixture directory created");
    }
    ~TempDir() {
        std::error_code error;
        std::filesystem::remove_all(path, error);
    }
};

PdDeclarations declarations(const char* filename) {
    const auto parsed = adi::device::parsePdDeclarations(
        readFile(std::filesystem::path(ADI_DEVICE_PATCH_DIR) / filename));
    check(parsed.problems.empty(), "canonical declarations parse without problems");
    return parsed;
}

bool open(LibPdEngine& engine, PdLatencyReceiver& latency) {
    engine.addSearchPath(ADI_PD_PATCH_DIR);
    std::string error;
    const bool ok = engine.open(latency, error);
    check(ok, "engine opens");
    if (!ok) std::printf("OPEN ERROR %s\n", error.c_str());
    return ok;
}

template <class Core>
void setDefaults(Core& core, const PdDeclarations& decls) {
    // [adi.param]'s loadbang sends t_float, even when the core stores double.
    for (const auto& param : decls.params)
        core.set(static_cast<typename Core::Param>(param.id - 1),
                 static_cast<float>(param.def));
}

bool same(float a, float b) {
    return std::bit_cast<std::uint32_t>(a) == std::bit_cast<std::uint32_t>(b);
}

struct Audio {
    std::array<std::vector<float>, 2> input, output;
    std::array<const float*, 2> in{};
    std::array<float*, 2> out{};
    explicit Audio(std::size_t n) {
        for (std::size_t c = 0; c < input.size(); ++c) {
            input[c].resize(n);
            output[c].assign(n, sentinel);
            for (std::size_t i = 0; i < n; ++i) {
                const double phase = static_cast<double>(i) * (c == 0 ? .137 : .083);
                input[c][i] = static_cast<float>(.3 * std::sin(phase) +
                                               .1 * std::cos(phase * 2.31));
            }
            in[c] = input[c].data();
            out[c] = output[c].data();
        }
    }
    adi::engine::NodeIo io(int offset, int frames, bool alias = false) {
        for (std::size_t c = 0; c < out.size(); ++c)
            out[c] = alias ? input[c].data() : output[c].data();
        adi::engine::NodeIo result;
        result.in = in.data();
        result.out = out.data();
        result.channels = 2;
        result.blockOffset = offset;
        result.frames = frames;
        result.sampleRate = 48000;
        return result;
    }
};

void testNativeNull(bool alias) {
    const auto decls = declarations("EQEight.pd");
    LibPdEngine engine(ADI_DEVICE_PATCH_DIR, "EQEight.pd", 2, 2);
    PdLatencyReceiver latency;
    if (!open(engine, latency)) return;
    check(engine.isNative(), "canonical EQEight selects its native core");
    engine.prepare(48000, 512);
    engine.bindParameters(decls);
    engine.requestLatencyReport();
    check(engine.adapterLatencySamples() == 0, "native EQEight adds no adapter samples");

    adi::dsp::EqEight reference;
    setDefaults(reference, decls);
    reference.prepare(48000);
    check(engine.sendParameter(3, 1731.f), "native band frequency routes");
    check(engine.sendParameter(4, 7.25f), "native band gain routes");
    reference.set(static_cast<adi::dsp::EqEight::Param>(2), 1731.f);
    reference.set(static_cast<adi::dsp::EqEight::Param>(3), 7.25f);

    constexpr int start = 19;
    constexpr int frames = 3072;
    Audio audio(static_cast<std::size_t>(start + frames + 17));
    const auto original = audio.input;
    std::array<std::vector<float>, 2> expected;
    for (auto& channel : expected) channel.resize(static_cast<std::size_t>(frames));
    // A single uninterrupted reference call also catches state that depends
    // incorrectly on adapter segment boundaries.
    reference.process(original[0].data() + start, original[1].data() + start,
                      expected[0].data(), expected[1].data(), frames);
    constexpr std::array<int, 9> lengths{1, 7, 63, 64, 65, 100, 257, 411, 37};
    std::size_t segment = 0;
    int done = 0;
    allocations = 0;
    countAllocations = true;
    while (done < frames) {
        const int count = std::min(lengths[segment % lengths.size()], frames - done);
        engine.process(audio.io(start + done, count, alias));
        done += count;
        ++segment;
    }
    countAllocations = false;
    check(allocations == 0, "native awkward segments allocate nothing");
    bool equal = true;
    bool guarded = true;
    for (std::size_t c = 0; c < expected.size(); ++c) {
        const auto& actual = alias ? audio.input[c] : audio.output[c];
        for (int i = 0; i < frames; ++i)
            equal &= same(actual[static_cast<std::size_t>(start + i)],
                          expected[c][static_cast<std::size_t>(i)]);
        for (std::size_t i = 0; i < actual.size(); ++i) {
            if (i >= static_cast<std::size_t>(start) &&
                i < static_cast<std::size_t>(start + frames)) continue;
            guarded &= same(actual[i], alias ? original[c][i] : sentinel);
        }
    }
    check(equal, alias ? "in-place native output nulls bit-exactly against EqEight"
                       : "native output nulls bit-exactly against EqEight at arbitrary offsets");
    check(guarded, "native rendering leaves samples outside each segment untouched");
    check(engine.counters().ticks == 0, "native rendering never runs the Pd DSP graph");
}

void testLatencyAndInstances() {
    const auto decls = declarations("EQEight.pd");
    auto backend = std::make_unique<LibPdEngine>(ADI_DEVICE_PATCH_DIR, "EQEight.pd", 2, 2);
    backend->addSearchPath(ADI_PD_PATCH_DIR);
    LibPdEngine* raw = backend.get();
    adi::device::PdDevice device(std::move(backend),
                                {"internal", "EQEight", "EQ Eight", "ADI", "1"});
    check(device.loaded(), "native EQEight loads through PdDevice");
    device.prepare(48000, 64);
    raw->bindParameters(decls);
    check(device.latencySamples() == 0, "PdDevice initially reports core latency only");
    const auto epoch = device.latencyEpoch();
    check(raw->sendParameter(87, 1.f), "native high-quality switch routes");
    check(device.latencySamples() == 16, "PdDevice reports 16, not 80, in high quality");
    check(device.latencyEpoch() > epoch, "intrinsic latency change reaches the coalescer epoch");
    const auto changed = device.latencyEpoch();
    check(raw->sendParameter(87, 1.f), "repeated high-quality setting accepted");
    check(device.latencyEpoch() == changed, "unchanged intrinsic latency does not bump epoch");
    check(!raw->sendParameter(0, 1.f) && !raw->sendParameter(88, 1.f),
          "native parameters retain the declaration's one-based identity bounds");

    LibPdEngine other(ADI_DEVICE_PATCH_DIR, "EQEight.pd", 2, 2);
    PdLatencyReceiver otherLatency;
    if (!open(other, otherLatency)) return;
    other.prepare(48000, 64);
    other.bindParameters(decls);
    other.requestLatencyReport();
    check(otherLatency.latencySamples() == 0, "second instance does not inherit first latency");
    check(other.sendParameter(4, -11.f), "second instance accepts independent gain");
    check(raw->sendParameter(4, 9.f), "first instance accepts independent gain");

    adi::dsp::EqEight reference;
    setDefaults(reference, decls);
    reference.prepare(48000);
    reference.set(static_cast<adi::dsp::EqEight::Param>(3), -11.f);
    Audio first(512), second(512);
    std::array<std::array<float, 512>, 2> expected{};
    device.process(first.io(0, 512));
    other.process(second.io(0, 512));
    reference.process(second.in[0], second.in[1], expected[0].data(), expected[1].data(), 512);
    bool equal = true;
    for (std::size_t c = 0; c < expected.size(); ++c)
        for (std::size_t i = 0; i < expected[c].size(); ++i)
            equal &= same(second.output[c][i], expected[c][i]);
    check(equal, "interleaved native instances keep independent DSP and parameter state");
}

void testRealtimeControlsAndTempo() {
    const auto decls = declarations("AutoFilter.pd");
    LibPdEngine engine(ADI_DEVICE_PATCH_DIR, "AutoFilter.pd", 4, 2);
    PdLatencyReceiver latency;
    if (!open(engine, latency)) return;
    check(engine.isNative(), "AutoFilter's native backend is available");
    engine.prepare(48000, 128);
    engine.bindParameters(decls);
    adi::dsp::AutoFilter reference;
    setDefaults(reference, decls);
    reference.prepare(48000);
    Audio audio(128);
    std::array<std::array<float, 128>, 2> expected{};
    adi::engine::TransportInfo transport;
    transport.bpm = 137;
    transport.playing = true;
    auto io = audio.io(0, 128);
    io.sidechain = audio.in.data();
    io.transport = &transport;
    bool accepted = true;
    bool equal = true;
    allocations = 0;
    countAllocations = true;
    for (int block = 0; block < 16; ++block) {
        const float cutoff = 500.f + static_cast<float>(block) * 41.f;
        accepted &= engine.sendParameter(3, cutoff);
        accepted &= engine.sendParameter(11, 35.f);
        accepted &= engine.sendParameter(13, 1.f);
        reference.set(adi::dsp::AutoFilter::Frequency, cutoff);
        reference.set(adi::dsp::AutoFilter::LfoAmount, 35.f);
        reference.set(adi::dsp::AutoFilter::LfoMode, 1.f);
        reference.tempo(transport.bpm);
        engine.process(io);
        reference.process(audio.in[0], audio.in[1], expected[0].data(), expected[1].data(),
                          128, audio.in[0], audio.in[1]);
        for (std::size_t c = 0; c < expected.size(); ++c)
            for (std::size_t i = 0; i < expected[c].size(); ++i)
                equal &= same(audio.output[c][i], expected[c][i]);
        transport.bpm += 1;
    }
    countAllocations = false;
    check(accepted, "realtime native parameter changes all route");
    check(allocations == 0, "native parameter, transport, sidechain and audio paths allocate nothing");
    check(equal, "native tempo and sidechain match direct AutoFilter processing");
}

void testNativeLifecycle() {
    const auto decls = declarations("EQEight.pd");
    LibPdEngine engine(ADI_DEVICE_PATCH_DIR, "EQEight.pd", 2, 2);
    PdLatencyReceiver latency;
    if (!open(engine, latency)) return;
    engine.prepare(48000, 64);
    engine.bindParameters(decls);
    check(engine.sendParameter(87, 1.f), "lifecycle fixture acquires intrinsic delay");
    check(engine.sendParameter(4, 12.f), "lifecycle fixture acquires nondefault DSP state");
    Audio playing(128);
    engine.process(playing.io(0, 128));
    check(latency.latencySamples() == 16, "lifecycle fixture reports its live delay");

    engine.close();
    check(!engine.isNative() && engine.adapterLatencySamples() == 0,
          "close destroys native processing and clears adapter delay");
    check(!engine.sendParameter(87, 0.f), "closed native engine retains no parameter binding");

    std::string error;
    if (!engine.open(latency, error)) {
        check(false, "same native engine reopens");
        return;
    }
    check(engine.isNative(), "same native engine reselects its canonical core");
    check(!engine.sendParameter(87, 1.f), "reopened native engine requires fresh parameter bindings");
    Audio unprepared(96);
    allocations = 0;
    countAllocations = true;
    engine.process(unprepared.io(11, 64));
    countAllocations = false;
    bool copied = true;
    bool guarded = true;
    for (std::size_t c = 0; c < unprepared.output.size(); ++c)
        for (std::size_t i = 0; i < unprepared.output[c].size(); ++i) {
            if (i >= 11 && i < 75)
                copied &= same(unprepared.output[c][i], unprepared.input[c][i]);
            else guarded &= same(unprepared.output[c][i], sentinel);
        }
    check(copied && guarded, "processing before reprepare only copies the addressed segment");
    check(allocations == 0, "unprepared native passthrough allocates nothing");

    engine.prepare(48000, 64);
    engine.bindParameters(decls);
    check(latency.latencySamples() == 0, "reopened default core starts with zero intrinsic delay");
    adi::dsp::EqEight reference;
    setDefaults(reference, decls);
    reference.prepare(48000);
    Audio resumed(128);
    std::array<std::array<float, 128>, 2> expected{};
    engine.process(resumed.io(0, 128));
    reference.process(resumed.in[0], resumed.in[1], expected[0].data(), expected[1].data(), 128);
    bool equal = true;
    for (std::size_t c = 0; c < expected.size(); ++c)
        for (std::size_t i = 0; i < expected[c].size(); ++i)
            equal &= same(resumed.output[c][i], expected[c][i]);
    check(equal, "close/reopen retains neither parameter changes nor previous filter history");
}

void testNativeRateNormalization() {
    const auto decls = declarations("EQEight.pd");
    struct RateCase { double requested; double normalized; };
    constexpr std::array<RateCase, 2> rates{{{48000.5, 48000}, {0, 44100}}};
    for (const auto& rate : rates) {
        LibPdEngine engine(ADI_DEVICE_PATCH_DIR, "EQEight.pd", 2, 2);
        PdLatencyReceiver latency;
        if (!open(engine, latency)) continue;
        engine.bindParameters(decls);
        check(engine.sendParameter(3, 1731.f) && engine.sendParameter(4, 7.25f),
              "rate normalization fixture uses a nonflat frequency response");
        engine.prepare(rate.requested, 128);
        adi::dsp::EqEight reference;
        setDefaults(reference, decls);
        reference.set(static_cast<adi::dsp::EqEight::Param>(2), 1731.f);
        reference.set(static_cast<adi::dsp::EqEight::Param>(3), 7.25f);
        reference.prepare(rate.normalized);
        Audio audio(128);
        std::array<std::array<float, 128>, 2> expected{};
        engine.process(audio.io(0, 128));
        reference.process(audio.in[0], audio.in[1], expected[0].data(), expected[1].data(), 128);
        bool equal = true;
        for (std::size_t c = 0; c < expected.size(); ++c)
            for (std::size_t i = 0; i < expected[c].size(); ++i)
                equal &= same(audio.output[c][i], expected[c][i]);
        check(equal, "native rate normalization matches Pd's integer rate and invalid-rate fallback");
    }
}

const char* identityPatch =
    "#N canvas 0 0 500 300 12;\n"
    "#X obj 10 10 adc~ 1 2;\n"
    "#X obj 10 70 *~ 1;\n"
    "#X obj 130 70 *~ 1;\n"
    "#X obj 10 130 dac~ 1 2;\n"
    "#X obj 260 10 r \\$0-gain;\n"
    "#X obj 260 60 r \\$0-query_latency;\n"
    "#X obj 260 100 f 0;\n"
    "#X obj 260 140 s \\$0-report_latency;\n"
    "#X connect 0 0 1 0;\n"
    "#X connect 0 1 2 0;\n"
    "#X connect 1 0 3 0;\n"
    "#X connect 2 0 3 1;\n"
    "#X connect 4 0 1 1;\n"
    "#X connect 4 0 2 1;\n"
    "#X connect 5 0 6 0;\n"
    "#X connect 6 0 7 0;\n";

void testWholeBlocks(const TempDir& temp) {
    writeFile(temp.path / "identity.pd", identityPatch);
    LibPdEngine engine(temp.path.string(), "identity.pd", 2, 2,
                       Execution::PureData, BlockMode::WholeBlocks);
    PdLatencyReceiver latency;
    if (!open(engine, latency)) return;
    check(!engine.isNative(), "ordinary user patch stays on Pure Data");
    engine.prepare(48000, 64);
    engine.requestLatencyReport();
    check(engine.adapterLatencySamples() == 0 && latency.latencySamples() == 0,
          "whole-block user patch reports zero intrinsic and adapter delay");
    constexpr std::array<int, 4> lengths{64, 128, 256, 1024};
    for (int frames : lengths) {
        Audio audio(static_cast<std::size_t>(frames + 23));
        engine.process(audio.io(11, frames));
        bool equal = true;
        bool guarded = true;
        for (std::size_t c = 0; c < audio.output.size(); ++c)
            for (std::size_t i = 0; i < audio.output[c].size(); ++i) {
                if (i >= 11 && i < static_cast<std::size_t>(frames + 11))
                    equal &= same(audio.output[c][i], audio.input[c][i]);
                else guarded &= same(audio.output[c][i], sentinel);
            }
        check(equal, "whole Pd ticks return same-frame audio, including oversized process calls");
        check(guarded, "whole-block processing respects nonzero offset and end guards");
    }
    check(engine.counters().starved == 0, "whole-block rendering never starves");

    check(engine.sendFloat("gain", 2.f), "user patch's gain control is connected");
    Audio doubled(128);
    engine.process(doubled.io(0, 128, true));
    bool transformed = true;
    Audio original(128);
    for (std::size_t c = 0; c < doubled.input.size(); ++c)
        for (std::size_t i = 0; i < doubled.input[c].size(); ++i)
            transformed &= same(doubled.input[c][i], original.input[c][i] * 2.f);
    check(transformed, "in-place whole-block user processing runs Pd, not a passthrough copy");

    Audio rejected(128);
    const auto before = engine.counters();
    engine.process(rejected.io(7, 63));
    const auto after = engine.counters();
    bool silent = true;
    bool guarded = true;
    for (const auto& channel : rejected.output)
        for (std::size_t i = 0; i < channel.size(); ++i) {
            if (i >= 7 && i < 70) silent &= channel[i] == 0.f;
            else guarded &= same(channel[i], sentinel);
        }
    check(silent && guarded, "invalid whole-block segment is silenced within its exact bounds");
    check(after.rejectedSegments == before.rejectedSegments + 1 && after.ticks == before.ticks,
          "rejected remainder is counted and never advances the Pd tick");
    check(engine.adapterLatencySamples() == 0, "remainder does not switch latency while playing");
    check(engine.sendFloat("gain", 1.f), "user patch gain returns to unity");
    Audio resumed(64);
    engine.process(resumed.io(0, 64));
    bool recovered = true;
    for (std::size_t c = 0; c < resumed.output.size(); ++c)
        for (std::size_t i = 0; i < resumed.output[c].size(); ++i)
            recovered &= same(resumed.output[c][i], resumed.input[c][i]);
    check(recovered, "next valid whole block has no stale remainder or shifted samples");
}

void testRecognition(const TempDir& temp) {
    const auto canonical = readFile(std::filesystem::path(ADI_DEVICE_PATCH_DIR) / "EQEight.pd");
    writeFile(temp.path / "EQEight.pd", identityPatch);
    {
        LibPdEngine engine(temp.path.string(), "EQEight.pd", 2, 2);
        PdLatencyReceiver latency;
        if (open(engine, latency)) {
            check(!engine.isNative(), "a user patch named EQEight is never replaced by native EQ");
            engine.prepare(48000, 64);
            check(engine.adapterLatencySamples() == 64, "ordinary patches retain stable buffered mode");
        }
    }
    writeFile(temp.path / "EQEight.pd", canonical + "#X text 10 980 User edit;\n");
    {
        LibPdEngine engine(temp.path.string(), "EQEight.pd", 2, 2);
        PdLatencyReceiver latency;
        if (open(engine, latency))
            check(!engine.isNative(), "even a textual edit invalidates the compiled patch identity");
    }
    writeFile(temp.path / "EQEight.pd", canonical);
    writeFile(temp.path / "adi.param.pd",
              readFile(std::filesystem::path(ADI_PD_PATCH_DIR) / "adi.param.pd") +
                  "#X text 10 490 Edited helper;\n");
    {
        LibPdEngine engine(temp.path.string(), "EQEight.pd", 2, 2);
        PdLatencyReceiver latency;
        if (open(engine, latency))
            check(!engine.isNative(), "modified local parameter helper prevents native substitution");
    }
    {
        LibPdEngine engine(ADI_DEVICE_PATCH_DIR, "EQEight.pd", 2, 2, Execution::PureData);
        PdLatencyReceiver latency;
        if (open(engine, latency))
            check(!engine.isNative(), "explicit PureData always exercises the actual canonical patch");
    }
}
} // namespace

int main() {
#if defined(_MSC_VER) && defined(_DEBUG)
    _CrtSetAllocHook(allocationHook);
#endif
    testNativeNull(false);
    testNativeNull(true);
    testLatencyAndInstances();
    testRealtimeControlsAndTempo();
    testNativeLifecycle();
    testNativeRateNormalization();
    TempDir temp;
    testWholeBlocks(temp);
    testRecognition(temp);
    std::printf("%s -- %d checks, %d failure(s)\n", failures == 0 ? "PASS" : "FAIL",
                checks, failures);
    return failures == 0 ? 0 : 1;
}
