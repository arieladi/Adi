// SPDX-License-Identifier: AGPL-3.0-or-later
// Own-core/provider contracts only. No native host, GUI, audio device or reference plug-in.
#include "adi/mix/eq/Backend.hpp"
#include "adi/mix/eq/Core.hpp"
#include "juce/mix_function_host.hpp"
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>
#include <numbers>
#include <thread>
#include <type_traits>

namespace {
thread_local bool audioCall = false, refuseAllocation = false;
std::atomic<std::uint64_t> audioAllocations{0}, audioDeallocations{0};
}
void *operator new(std::size_t bytes) {
    if (refuseAllocation) { refuseAllocation = false; throw std::bad_alloc(); }
    if (audioCall) audioAllocations.fetch_add(1, std::memory_order_relaxed);
    if (auto *memory = std::malloc(bytes ? bytes : 1)) return memory;
    throw std::bad_alloc();
}
void *operator new[](std::size_t bytes) { return ::operator new(bytes); }
void operator delete(void *memory) noexcept {
    if (audioCall && memory) audioDeallocations.fetch_add(1, std::memory_order_relaxed);
    std::free(memory);
}
void operator delete[](void *memory) noexcept { ::operator delete(memory); }
void operator delete(void *memory, std::size_t) noexcept { ::operator delete(memory); }
void operator delete[](void *memory, std::size_t) noexcept { ::operator delete(memory); }

namespace {
namespace eq = adi::mix::eq;
constexpr int frames = 64;
int checks = 0, failures = 0;
std::uint64_t samplePairs = 0;
void check(bool okay, const char *label) {
    ++checks;
    if (!okay) { ++failures; std::printf("FAIL %s\n", label); }
}
struct AudioScope {
    AudioScope() { audioCall = true; }
    ~AudioScope() { audioCall = false; }
};
template<class Sample> struct Audio {
    std::array<std::array<Sample, frames>, 2> samples{};
    void signal(std::uint64_t block, double rate, bool key = false) {
        for (std::size_t ch = 0; ch < 2; ++ch)
            for (std::size_t i = 0; i < frames; ++i) {
                const auto t = static_cast<double>(block * frames + i) / rate;
                samples[ch][i] = static_cast<Sample>(key ? .08 + .19 * std::cos(2 * std::numbers::pi * 1600 * t + static_cast<double>(ch))
                    : .3 * std::sin(2 * std::numbers::pi * 1000 * t + .3 * static_cast<double>(ch)));
            }
    }
    std::array<Sample *, 2> pointers(int offset = 0) { return {samples[0].data() + offset, samples[1].data() + offset}; }
    std::array<const Sample *, 2> keyPointers(int offset = 0) const { return {samples[0].data() + offset, samples[1].data() + offset}; }
    bool operator==(const Audio &other) const { return std::memcmp(samples.data(), other.samples.data(), sizeof(samples)) == 0; }
};
eq::Settings settingsFor(int mode) {
    eq::Settings s; auto &band = s.bands[0];
    band.status = eq::Status::On; band.frequencyHz = 1000; band.q = 1.3f;
    band.gainDb = mode == 0 ? 5.f : 1.f;
    band.targetGainDb = -9.f; band.thresholdDb = -35.f;
    band.attackMs = 2; band.releaseMs = 35;
    band.dynamicOn = mode != 0;
    band.spectralOn = mode == 2;
    band.spectralAutomaticThreshold = false;
    band.spectralDensityPercent = 65;
    s.spectralResolution = eq::Resolution::Low;
    s.externalSidechain = true;
    return s;
}
template<class Sample>
void run(adi::mix::EqProcessingBackend &backend, Audio<Sample> &audio, const Audio<Sample> *key,
         int keyChannels, const eq::Settings &s, std::uint64_t block, int configured,
         int offset = 0, int count = frames) {
    auto main = audio.pointers(offset);
    const auto side = key ? key->keyPointers(offset) : std::array<const Sample *, 2>{};
    AudioScope scope;
    backend.process(main.data(), key ? side.data() : nullptr, keyChannels, count, s, block, configured);
}
template<class Sample>
bool reference(eq::Core &core, Audio<Sample> &audio, const Audio<Sample> *key, int keyChannels,
               const eq::Settings &s, eq::BlockContext context, int offset = 0, int count = frames) {
    auto main = audio.pointers(offset);
    const auto side = key ? key->keyPointers(offset) : std::array<const Sample *, 2>{};
    AudioScope scope;
    return core.process(main.data(), key ? side.data() : nullptr, keyChannels, count, s, context);
}
template<class Sample> void exactMatrix(double rate, int channels, int mode) {
    const auto settings = settingsFor(mode), stored = settings;
    auto backend = eq::makeBackend(); eq::Core direct;
    backend->prepare(rate, frames, channels, settings); direct.prepare({rate, frames, channels}, settings);
    bool exact = backend->supportsDouble() && backend->advertisedLatency() == direct.latencySamples();
    // No bus, a real silent bus, then a real active bus. This keeps the same
    // prepared logical layout and exercises its connection transitions.
    for (int phase = 0; phase < 3; ++phase)
        for (int repeat = 0; repeat < 24; ++repeat) {
            const auto block = static_cast<std::uint64_t>(phase * 24 + repeat + 1);
            Audio<Sample> actual, key; actual.signal(block, rate);
            if (phase == 2) key.signal(block, rate, true);
            auto expected = actual;
            const auto *bus = phase == 0 ? nullptr : &key;
            const int width = bus ? channels : 0;
            const auto configured = backend->advertisedLatency();
            const auto context = direct.beginBlock(block, configured, settings);
            run(*backend, actual, bus, width, settings, block, configured, 0, 17);
            exact = reference(direct, expected, bus, width, settings, context, 0, 17) && exact;
            run(*backend, actual, bus, width, settings, block, configured, 17, 47);
            exact = reference(direct, expected, bus, width, settings, context, 17, 47) && exact;
            exact = (actual == expected) && exact;
            samplePairs += static_cast<std::uint64_t>(frames * channels);
            backend->pumpMainThread(settings);
        }
    check(exact && backend->processingIssue().empty() && settings == stored,
          "serial EQ provider preserves exact float/double mono/stereo static/dynamic/spectral audio for absent, silent and active keys");
    backend->release();
}
template<class Sample> void silentKeyIsDistinct() {
    auto settings = settingsFor(1); settings.bands[0].gainDb = 0;
    auto absent = eq::makeBackend(), silent = eq::makeBackend();
    absent->prepare(48000, frames, 2, settings); silent->prepare(48000, frames, 2, settings);
    double difference = 0;
    for (std::uint64_t block = 1; block <= 64; ++block) {
        Audio<Sample> a, zero; a.signal(block, 48000); auto b = a;
        run(*absent, a, static_cast<const Audio<Sample> *>(nullptr), 0, settings, block, 0);
        run(*silent, b, &zero, 2, settings, block, 0);
        for (std::size_t ch = 0; ch < 2; ++ch) for (std::size_t i = 0; i < frames; ++i)
            difference = std::max(difference, std::abs(static_cast<double>(a.samples[ch][i]) - static_cast<double>(b.samples[ch][i])));
    }
    check(difference > 1.e-5, "authored external On with no bus uses programme, while an actual silent bus remains silent");
}
template<class Sample> void malformedPlanes() {
    const auto settings = settingsFor(1); auto backend = eq::makeBackend(); backend->prepare(48000, frames, 2, settings);
    Audio<Sample> actual, key; actual.signal(1, 48000); key.signal(1, 48000, true); const auto before = actual;
    auto main = actual.pointers(); const auto side = key.keyPointers();
    std::array<const Sample *, 2> partial{side[0], nullptr};
    {
        AudioScope scope;
        backend->process(main.data(), partial.data(), 2, frames, settings, 1, 0);
        backend->process(main.data(), nullptr, 1, frames, settings, 1, 0);
        backend->process(main.data(), side.data(), 3, frames, settings, 1, 0);
        backend->process(main.data(), side.data(), 2, frames + 1, settings, 1, 0);
    }
    check(actual == before, "malformed positive-width/partial-null keys and oversized spans retain the legacy safe rejection");
}
template<class Sample> void spectralKeyIsDistinct() {
    auto settings = settingsFor(2);
    settings.bands[0].gainDb = 0;
    settings.bands[0].targetGainDb = -18;
    settings.bands[0].thresholdDb = -65;
    settings.bands[0].spectralDensityPercent = 100;
    auto active = eq::makeBackend(), silent = eq::makeBackend();
    eq::Core directActive, directSilent;
    active->prepare(48000, frames, 2, settings); silent->prepare(48000, frames, 2, settings);
    directActive.prepare({48000, frames, 2}, settings); directSilent.prepare({48000, frames, 2}, settings);
    bool exact = true; double difference = 0;
    for (std::uint64_t block = 1; block <= 128; ++block) {
        Audio<Sample> a, key, zero; a.signal(block, 48000); auto b = a, expectedActive = a, expectedSilent = a;
        for (std::size_t ch = 0; ch < 2; ++ch) for (std::size_t i = 0; i < frames; ++i) {
            const auto t = static_cast<double>(block * frames + i) / 48000.;
            key.samples[ch][i] = static_cast<Sample>(.75 * std::cos(2 * std::numbers::pi * 1000 * t + .3 * static_cast<double>(ch)));
        }
        const auto latency = active->advertisedLatency();
        const auto activeContext = directActive.beginBlock(block, latency, settings);
        const auto silentContext = directSilent.beginBlock(block, latency, settings);
        run(*active, a, &key, 2, settings, block, latency);
        run(*silent, b, &zero, 2, settings, block, latency);
        exact = reference(directActive, expectedActive, &key, 2, settings, activeContext) && exact;
        exact = reference(directSilent, expectedSilent, &zero, 2, settings, silentContext) && exact;
        exact = (a == expectedActive && b == expectedSilent) && exact;
        for (std::size_t ch = 0; ch < 2; ++ch) for (std::size_t i = 0; i < frames; ++i)
            difference = std::max(difference, std::abs(static_cast<double>(a.samples[ch][i]) - static_cast<double>(b.samples[ch][i])));
    }
    check(exact && difference > 1.e-5,
          "active spectral gain/key response differs from a connected silent key and remains exact against the authored cores");
}
template<class Sample> void continuation() {
    auto settings = settingsFor(1); settings.bands[0].dynamicLearn = true;
    auto backend = eq::makeBackend(); eq::Core direct;
    backend->prepare(48000, frames, 2, settings); direct.prepare({48000, frames, 2}, settings);
    bool exact = true;
    const auto render = [&](double rate, std::uint64_t block) {
        Audio<Sample> actual; actual.signal(block, rate); auto expected = actual;
        const auto context = direct.beginBlock(block, direct.latencySamples(), settings);
        run(*backend, actual, static_cast<const Audio<Sample> *>(nullptr), 0, settings, block, backend->advertisedLatency());
        exact = reference(direct, expected, static_cast<const Audio<Sample> *>(nullptr), 0, settings, context) && exact;
        exact = (actual == expected) && exact;
    };
    for (std::uint64_t block = 1; block <= 128; ++block) render(48000, block);
    const auto threshold = direct.learnedThreshold(0), knee = direct.learnedKnee(0);
    check(exact && std::isfinite(threshold) && std::isfinite(knee) && knee >= 2., "learning fixture reaches a real authored learned state");
    backend->release(); backend->prepare(96000, 128, 2, settings); direct.prepare({96000, 128, 2}, settings);
    check(direct.learnedThreshold(0) == threshold && direct.learnedKnee(0) == knee, "standalone authored reprepare preserves learned detector state");
    for (std::uint64_t block = 129; block <= 160; ++block) render(96000, block);
    settings.bands[0].dynamicLearn = false;
    backend->release(); backend->prepare(96000, frames, 2, settings); direct.prepare({96000, frames, 2}, settings);
    for (std::uint64_t block = 161; block <= 192; ++block) render(96000, block);
    settings.bands[0].dynamicLearn = true;
    backend->prepare(44100, frames, 2, settings); direct.prepare({44100, frames, 2}, settings);
    for (std::uint64_t block = 193; block <= 224; ++block) render(44100, block);
    check(exact && backend->processingIssue().empty(),
          "release/rate/capacity and learn-edge reprepare keep exact audio against the same retained authored EQ object");
}
template<class Sample> void asynchronousPdc() {
    auto settings = settingsFor(0); auto backend = eq::makeBackend(); eq::Core direct;
    backend->prepare(48000, frames, 2, settings); direct.prepare({48000, frames, 2}, settings);
    std::uint64_t physical = 0; int configured = backend->advertisedLatency();
    bool exact = true, complete = true;
    for (int stage = 0; stage < 4; ++stage) {
        settings.lookaheadMs = stage == 0 ? 2.f : stage == 2 ? 4.f : 0.f;
        settings.bands[0].spectralOn = stage == 1 || stage == 2;
        settings.spectralResolution = stage == 2 ? eq::Resolution::Medium : eq::Resolution::Low;
        direct.requestConfiguration(settings);
        const auto target = direct.pendingLatencyChange().samples;
        int admittedBlocks = 0; bool arrived = false;
        for (int attempt = 0; attempt < 2000 && admittedBlocks < 8; ++attempt) {
            backend->pumpMainThread(settings);
            const auto advertised = backend->advertisedLatency();
            complete = complete && (advertised == configured || advertised == target);
            if (advertised == target) { configured = target; arrived = true; }
            Audio<Sample> actual; actual.signal(++physical, 48000); auto expected = actual;
            const auto context = direct.beginBlock(physical, configured, settings);
            run(*backend, actual, static_cast<const Audio<Sample> *>(nullptr), 0, settings, physical, configured, 0, 17);
            exact = reference(direct, expected, static_cast<const Audio<Sample> *>(nullptr), 0, settings, context, 0, 17) && exact;
            backend->pumpMainThread(settings); // owner readiness may land between segments
            run(*backend, actual, static_cast<const Audio<Sample> *>(nullptr), 0, settings, physical, configured, 17, 47);
            exact = reference(direct, expected, static_cast<const Audio<Sample> *>(nullptr), 0, settings, context, 17, 47) && exact;
            exact = (actual == expected) && exact;
            if (arrived) ++admittedBlocks;
            // Bounded functional waiting for real owner progress, never a CPU/time assertion.
            if (!arrived) std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        check(arrived && admittedBlocks == 8 && backend->processingIssue().empty(), "real owner readiness reaches each complete PDC target through Main pumps and audio callbacks");
    }
    check(exact && complete, "pending lookahead/spectral plans preserve exact split audio and only whole advertised PDC totals");
}
void parkedFailure() {
    const auto initial = settingsFor(0); auto desired = initial; desired.lookaheadMs = 2.f;
    auto backend = eq::makeBackend(); backend->prepare(48000, frames, 2, initial);
    Audio<float> audio; audio.signal(1, 48000);
    run(*backend, audio, static_cast<const Audio<float> *>(nullptr), 0, desired, 1, 0);
    check(backend->processingIssue().empty(), "ordinary pending configuration is not reported as a failure");
    refuseAllocation = true; backend->pumpMainThread(desired); refuseAllocation = false;
    check(!backend->processingIssue().empty() && backend->advertisedLatency() == 0,
          "worker start failure is a nonfatal processing issue without fabricated PDC admission");
    eq::Core direct; direct.prepare({48000, frames, 2}, initial);
    audio.signal(2, 48000); auto expected = audio;
    // Use a fresh pair after stopped retry to prove the explicit failure clears.
    backend->release(); backend->prepare(48000, frames, 2, desired); direct.prepare({48000, frames, 2}, desired);
    const auto configured = backend->advertisedLatency();
    const auto context = direct.beginBlock(2, configured, desired);
    run(*backend, audio, static_cast<const Audio<float> *>(nullptr), 0, desired, 2, configured);
    const bool okay = reference(direct, expected, static_cast<const Audio<float> *>(nullptr), 0, desired, context);
    check(okay && audio == expected && backend->processingIssue().empty(), "stopped retry clears the parked issue and returns to exact prepared audio");
}
void nativeIdentityAndDuplicatePrepare() {
    adi::rows::PluginRef ref; ref.format = "internal"; ref.uid = "org.adidaw.mix.eq"; ref.name = "EQ";
    adi::engine::DeviceRequest request; request.ref = &ref; request.channels = 2; std::string error;
    auto a = adi::device::makeMixFunction(request, error), b = adi::device::makeMixFunction(request, error);
    check(a && b && a->paramCount() == 730 && a->processesDouble() && a->identity().uid == ref.uid,
          "existing native factory keeps the same identity, 730 parameters and double capability");
    if (!a || !b) return;
    a->setParam("9", adi::device::ParamValue::fromNormalized(1));
    a->setParam("14", adi::device::ParamValue::fromNormalized(.7));
    a->setParam("17", adi::device::ParamValue::fromNormalized(1));
    const auto state = a->saveState("state");
    check(!state.empty() && b->loadState("state", state) && b->saveState("state") == state,
          "existing physical-float state roundtrips unchanged through the provider replacement");
    a->prepare(48000, frames); b->prepare(48000, frames); bool exact = true;
    for (std::uint64_t block = 1; block <= 48; ++block) {
        if (block == 33) b->prepare(48000, frames);
        Audio<double> first; first.signal(block, 48000); auto second = first;
        auto process = [&](adi::device::DeviceInstance &device, Audio<double> &audio) {
            auto out = audio.pointers(); const auto in = audio.keyPointers();
            adi::engine::NodeIo io; io.in64 = in.data(); io.out64 = out.data(); io.channels = 2;
            io.frames = frames; io.renderBlock = block; io.configuredLatency = device.latencySamples();
            AudioScope scope; device.process(io);
        };
        process(*a, first); process(*b, second); exact = (first == second) && exact;
    }
    check(exact, "native same-format duplicate prepare preserves continuous detector/filter history exactly");
}
} // namespace
int main() {
    for (double rate : {44100., 48000., 96000.}) for (int channels : {1, 2}) for (int mode = 0; mode < 3; ++mode) {
        exactMatrix<float>(rate, channels, mode); exactMatrix<double>(rate, channels, mode);
    }
    silentKeyIsDistinct<float>(); silentKeyIsDistinct<double>();
    spectralKeyIsDistinct<float>(); spectralKeyIsDistinct<double>();
    malformedPlanes<float>(); malformedPlanes<double>();
    continuation<float>(); continuation<double>();
    asynchronousPdc<float>(); asynchronousPdc<double>();
    parkedFailure(); nativeIdentityAndDuplicatePrepare();
    check(audioAllocations.load() == 0 && audioDeallocations.load() == 0, "native serial provider performs no callback allocation or free");
    std::printf("Native EQ serial: %d checks, %d failures, %llu sample pairs, %llu/%llu audio allocations/deallocations\n",
                checks, failures, static_cast<unsigned long long>(samplePairs),
                static_cast<unsigned long long>(audioAllocations.load()), static_cast<unsigned long long>(audioDeallocations.load()));
    return failures ? 1 : 0;
}
