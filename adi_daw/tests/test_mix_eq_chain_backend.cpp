// SPDX-License-Identifier: AGPL-3.0-or-later
// Generated component contracts only. No plug-in SDK, host or reference files.
#include "adi/mix/eq_chain/Backend.hpp"
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <new>
#include <thread>
#include <type_traits>

namespace {
thread_local bool audioCall = false, refuseAllocation = false;
std::atomic<std::uint64_t> audioAllocations{0}, audioDeallocations{0};
}
void *operator new(std::size_t bytes) {
    if (refuseAllocation) {
        refuseAllocation = false;
        throw std::bad_alloc();
    }
    if (audioCall)
        audioAllocations.fetch_add(1, std::memory_order_relaxed);
    if (void *memory = std::malloc(bytes ? bytes : 1))
        return memory;
    throw std::bad_alloc();
}
void *operator new[](std::size_t bytes) { return ::operator new(bytes); }
void operator delete(void *memory) noexcept {
    if (audioCall && memory)
        audioDeallocations.fetch_add(1, std::memory_order_relaxed);
    std::free(memory);
}
void operator delete[](void *memory) noexcept { ::operator delete(memory); }
void operator delete(void *memory, std::size_t) noexcept { ::operator delete(memory); }
void operator delete[](void *memory, std::size_t) noexcept { ::operator delete(memory); }

namespace {
namespace chain = adi::mix::eq_chain;
namespace eq = adi::mix::eq;
namespace focus = adi::mix::focus;
constexpr int frames = 64;
int checks = 0, failures = 0;
std::uint64_t comparedSamples = 0;
void check(bool condition, const char *label) {
    ++checks;
    if (!condition) {
        ++failures;
        std::printf("FAIL %s\n", label);
    }
}
struct AudioScope {
    AudioScope() { audioCall = true; }
    ~AudioScope() { audioCall = false; }
};
template <class Sample> struct Audio {
    std::array<std::array<Sample, frames>, 2> samples{};
    void signal(std::uint64_t block, bool key = false) {
        for (std::size_t c = 0; c < samples.size(); ++c)
            for (std::size_t i = 0; i < samples[c].size(); ++i) {
                const auto t = static_cast<double>(block * frames + i);
                samples[c][i] = static_cast<Sample>(
                    key ? .19 + .11 * std::cos(t * .071 + static_cast<double>(c))
                        : .2 * std::sin(t * .041 * (static_cast<double>(c) + 1.)));
            }
    }
    std::array<Sample *, 2> pointers(int offset = 0) {
        return {samples[0].data() + offset, samples[1].data() + offset};
    }
    std::array<const Sample *, 2> keyPointers(int offset = 0) const {
        return {samples[0].data() + offset, samples[1].data() + offset};
    }
    bool operator==(const Audio &other) const {
        return std::memcmp(samples.data(), other.samples.data(), sizeof(samples)) == 0;
    }
};
template <class Sample>
bool run(chain::Backend &backend, Audio<Sample> &audio, const Audio<Sample> *key,
         const chain::Settings &settings, std::uint64_t block, int latency, int offset = 0,
         int count = frames, int keyChannels = 2) {
    auto main = audio.pointers(offset);
    const auto side = key ? key->keyPointers(offset) : std::array<const Sample *, 2>{};
    AudioScope guard;
    return backend.process(main.data(), key ? side.data() : nullptr, key ? keyChannels : 0, count,
                           settings, block, latency);
}
bool waitOwner(chain::Backend &backend) {
    // A bounded test wait, not a timing/performance assertion. Main is the only
    // scheduling caller; the worker has no polling loop.
    for (int attempt = 0; attempt < 5000; ++attempt) {
        backend.pumpMainThread();
        const auto status = backend.status();
        if (status.failure != chain::BackendFailure::None)
            return false;
        if (status.completedWork >= status.requestedWork && !status.ownerBusy && status.ownerParked)
            return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return false;
}
bool remainsParked(chain::Backend &backend, const chain::BackendStatus &parked, int latency) {
    // Functional scheduling window: expose missing waits or short timed polling,
    // without deriving CPU use or claiming to exclude arbitrary polling periods.
    const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(30);
    bool stable = true;
    do {
        backend.pumpMainThread();
        const auto status = backend.status();
        stable = stable && status.ownerParked && !status.ownerBusy &&
                 status.ownerLoopVisits == parked.ownerLoopVisits &&
                 status.ownerWaitReturns == parked.ownerWaitReturns &&
                 status.ownerPasses == parked.ownerPasses && status.workerStarts == parked.workerStarts &&
                 status.requestedWork == parked.requestedWork && status.completedWork == parked.completedWork &&
                 status.failure == chain::BackendFailure::None && backend.advertisedLatency() == latency;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    } while (std::chrono::steady_clock::now() < end);
    return stable;
}
chain::Settings bell() {
    chain::Settings settings;
    settings.eq.bands[0].status = eq::Status::On;
    settings.eq.bands[0].gainDb = 5.f;
    settings.eq.bands[0].frequencyHz = 713.f;
    settings.eq.bands[0].q = 1.7f;
    return settings;
}
template <class Sample> void exactInitial(bool combined, double rate, int channels) {
    auto settings = bell();
    settings.carveEnabled = combined;
    settings.focusEnabled = combined;
    settings.carve.amount = .25f;
    settings.focus.nodes[0].enabled = true;
    settings.focus.nodes[0].frequencyHz = 975.;
    settings.focus.nodes[0].q = 2.4;
    settings.focus.nodes[0].thresholdDb = -40.;
    settings.focus.phaseMode = focus::PhaseMode::Rotation;
    settings.focus.rotationDegrees = combined ? 35. : 0.;
    chain::Backend backend(chain::OwnerMode::Manual);
    backend.prepare({rate, frames, channels}, settings);
    chain::Core direct;
    direct.prepare({rate, frames, channels}, settings);
    bool exact = true;
    for (std::uint64_t block = 1; block <= 24; ++block) {
        Audio<Sample> actual, key;
        actual.signal(block);
        key.signal(block, true);
        auto expected = actual;
        const auto context = direct.beginBlock(block, direct.latencySamples(), settings);
        auto main = expected.pointers();
        const auto side = key.keyPointers();
        exact = run(backend, actual, &key, settings, block, backend.advertisedLatency(), 0, 17) && exact;
        {
            AudioScope guard;
            exact = direct.process(main.data(), side.data(), 2, 17, settings, context) && exact;
        }
        main = expected.pointers(17);
        const auto tailKey = key.keyPointers(17);
        exact = run(backend, actual, &key, settings, block, backend.advertisedLatency(), 17, 47) && exact;
        {
            AudioScope guard;
            exact = direct.process(main.data(), tailKey.data(), 2, 47, settings, context) && exact;
        }
        exact = (actual == expected) && exact;
        comparedSamples += static_cast<std::uint64_t>(frames * channels);
        backend.pumpMainThread();
    }
    const auto status = backend.status();
    check(exact, "adapter preserves exact direct-core float/double samples and split physical blocks");
    check(!status.workerStarted && status.ownerPasses == 0 && status.requestedWork == 0,
          "unchanged prepared settings schedule no owner work");
}
void manualAdmission() {
    auto initial = bell(), desired = initial;
    desired.eq.lookaheadMs = 2.f;
    chain::Backend backend(chain::OwnerMode::Manual);
    backend.prepare({48000, frames, 2}, initial);
    Audio<float> audio;
    audio.signal(1);
    check(run(backend, audio, static_cast<const Audio<float> *>(nullptr), desired, 1, 0),
          "new intent continues the previous admitted audio plan");
    auto status = backend.status();
    check(status.requestedWork > status.completedWork && status.ownerPasses == 0,
          "audio publishes work but never services it");
    backend.pumpMainThread();
    check(backend.advertisedLatency() == 0 && !backend.status().workerStarted,
          "manual Main pump neither builds nor invents a pending latency");
    check(backend.serviceOwnerOnce(), "explicit owner pass prepares the candidate");
    status = backend.status();
    check(status.requestedWork == status.completedWork && status.ownerPasses == 1,
          "owner completion reports the exact fully published work version");
    backend.pumpMainThread();
    check(backend.advertisedLatency() == 0,
          "Main does not become a third Core pending-plan reader");
    audio.signal(2);
    check(run(backend, audio, static_cast<const Audio<float> *>(nullptr), desired, 2, 0, 0, 17),
          "old graph PDC holds the complete prepared candidate");
    backend.pumpMainThread();
    check(backend.advertisedLatency() == 96, "audio mirrors the ready aggregate latency to Main");
    const auto before = audio;
    check(!run(backend, audio, static_cast<const Audio<float> *>(nullptr), desired, 2, 96, 17, 47) &&
              audio == before,
          "same physical callback rejects a changed configured PDC without touching samples");
    check(run(backend, audio, static_cast<const Audio<float> *>(nullptr), desired, 2, 0, 17, 47),
          "same physical callback remains usable with its original PDC");
    audio.signal(3);
    check(run(backend, audio, static_cast<const Audio<float> *>(nullptr), desired, 3, 96),
          "a new physical callback admits the whole acknowledged latency plan");
    for (int i = 0; i < 12; ++i)
        backend.pumpMainThread();
    const auto manual = backend.status();
    check(backend.advertisedLatency() == 96 && manual.ownerPasses == 1 && !manual.ownerParked &&
              manual.ownerLoopVisits == 0 && manual.ownerWaitReturns == 0,
          "manual completion retains latency and has no threaded loop or wait observations");
}
void workerLifecycle() {
    auto settings = bell();
    chain::Backend backend;
    backend.prepare({48000, frames, 2}, settings);
    for (int i = 0; i < 20; ++i)
        backend.pumpMainThread();
    auto status = backend.status();
    check(!status.workerStarted && status.workerStarts == 0 && !status.ownerParked &&
              status.ownerLoopVisits == 0 && status.ownerWaitReturns == 0,
          "prepare and idle Main ticks do not start or visit an owner worker");
    settings.focusEnabled = true;
    settings.focus.nodes[0].enabled = true;
    settings.focus.phaseMode = focus::PhaseMode::Rotation;
    settings.focus.rotationDegrees = 60.;
    Audio<double> audio, key;
    audio.signal(1);
    key.signal(1, true);
    check(run(backend, audio, &key, settings, 1, 0) && !backend.status().workerStarted,
          "audio request does not start or notify a worker");
    check(waitOwner(backend), "a real off-audio owner completes configuration and reaches its next wait");
    const auto firstPark = backend.status();
    check(firstPark.workerStarted && firstPark.workerStarts == 1 && firstPark.ownerPasses == 1 &&
              firstPark.completedWork == firstPark.requestedWork && !firstPark.ownerBusy &&
              firstPark.ownerParked && firstPark.ownerLoopVisits == 2 && firstPark.ownerWaitReturns == 1,
          "one real request returns from one wait, services once, then parks in the next loop");
    check(!backend.serviceOwnerOnce(), "threaded mode refuses a second manual service owner");
    check(backend.advertisedLatency() == 0, "worker cannot publish latency ahead of audio readiness");
    audio.signal(2);
    check(run(backend, audio, &key, settings, 2, 0), "ready Focus offer holds behind old graph PDC");
    backend.pumpMainThread();
    const auto target = backend.advertisedLatency();
    check(target == 1024, "ready phase target is mirrored as a complete aggregate");
    audio.signal(3);
    check(run(backend, audio, &key, settings, 3, target), "real-worker candidate admits on a new callback");
    check(remainsParked(backend, firstPark, target),
          "unchanged Main ticks leave actual owner loop visits and wait returns unchanged after admission");
    settings.focus.nodes[0].q = 2.9;
    audio.signal(4);
    check(run(backend, audio, &key, settings, 4, target) && waitOwner(backend),
          "the same sleeping worker handles a later equal-latency coefficient request");
    const auto secondPark = backend.status();
    check(secondPark.workerStarts == 1 && secondPark.ownerPasses == firstPark.ownerPasses + 1 &&
              secondPark.ownerWaitReturns == firstPark.ownerWaitReturns + 1 &&
              secondPark.ownerLoopVisits == firstPark.ownerLoopVisits + 1 &&
              secondPark.completedWork == secondPark.requestedWork && !secondPark.ownerBusy && secondPark.ownerParked,
          "later intent wakes the same owner once and acknowledges a fresh park");
    check(remainsParked(backend, secondPark, target),
          "the second completed request also leaves the real worker parked through unchanged ticks");
    backend.release();
    status = backend.status();
    check(!status.prepared && !status.workerStarted && !status.ownerBusy && !status.ownerParked &&
              status.ownerWaitReturns == secondPark.ownerWaitReturns + 1 &&
              status.ownerLoopVisits == secondPark.ownerLoopVisits && status.ownerPasses == secondPark.ownerPasses,
          "stopped release joins the parked owner and counts its stop return without another service pass");
    const auto before = audio;
    check(!run(backend, audio, &key, settings, 5, target) && audio == before,
          "released adapter refuses audio without modifying buffers");
    backend.prepare({44100, frames, 2}, bell());
    status = backend.status();
    check(status.prepared && !status.workerStarted && !status.ownerParked && !status.ownerBusy &&
              status.ownerLoopVisits == 0 && status.ownerWaitReturns == 0 && status.ownerPasses == 0 &&
              status.workerStarts == 0 && status.failure == chain::BackendFailure::None,
          "stopped reprepare resets owner diagnostics only after joining the previous lifecycle");
    audio.signal(1);
    check(run(backend, audio, &key, settings, 1, 0) && waitOwner(backend),
          "a re-prepared adapter can lazily start and complete fresh real owner work");
    status = backend.status();
    check(status.workerStarts == 1 && status.ownerPasses == 1 && status.ownerWaitReturns == 1 &&
              status.ownerLoopVisits == 2 && status.ownerParked && !status.ownerBusy &&
              status.completedWork == status.requestedWork,
          "the fresh lifecycle reports its own single request rather than stale counters");
    backend.release();
}
void scalarControlsStayIdle() {
    auto settings = bell();
    settings.carveEnabled = true;
    settings.focusEnabled = true;
    settings.focus.nodes[0].enabled = true;
    settings.focus.phaseMode = focus::PhaseMode::Rotation;
    settings.focus.rotationDegrees = 35.;
    chain::Backend backend;
    backend.prepare({48000, frames, 2}, settings);
    bool okay = true;
    for (std::uint64_t block = 1; block <= 32; ++block) {
        const auto phase = static_cast<float>(block % 7) / 7.f;
        settings.eq.outputGainDb = phase * 2.f;
        settings.eq.bands[0].gainDb = phase * 5.f;
        settings.eq.bands[0].frequencyHz = 600.f + 200.f * phase;
        settings.eq.bands[0].q = 1.f + phase;
        settings.eq.bands[0].thresholdDb = -40.f + phase;
        settings.eq.bands[0].attackMs = 10.f + phase;
        settings.carve.amount = .15f + phase * .1f;
        settings.carve.pan = phase;
        settings.focus.inputGainDb = static_cast<double>(phase);
        settings.focus.outputGainDb = -static_cast<double>(phase);
        settings.focus.nodes[0].thresholdDb = -30. + static_cast<double>(phase);
        settings.focus.nodes[0].differenceDb = -4.5 + static_cast<double>(phase);
        settings.focus.nodes[0].attackMs = 10. + static_cast<double>(phase);
        settings.focus.nodes[0].releaseMs = 50. + static_cast<double>(phase);
        settings.focus.nodes[0].position = static_cast<double>(phase);
        settings.focus.rotationDegrees = 35. + static_cast<double>(phase);
        settings.focus.phaseMinimumHz = 20. + static_cast<double>(phase);
        Audio<float> audio, key;
        audio.signal(block);
        key.signal(block, true);
        okay = run(backend, audio, &key, settings, block, backend.advertisedLatency()) && okay;
        backend.pumpMainThread();
    }
    const auto status = backend.status();
    check(okay && status.requestedWork == 0 && status.completedWork == 0 &&
              status.ownerPasses == 0 && status.workerStarts == 0 && !status.workerStarted &&
              !status.ownerParked && status.ownerLoopVisits == 0 && status.ownerWaitReturns == 0,
          "ordinary EQ, Carve and Focus scalar automation leaves the owner completely idle");
}
void threeActorConvergence() {
    chain::Backend backend;
    const auto initial = bell();
    backend.prepare({48000, frames, 2}, initial);
    std::atomic<bool> done{false}, abort{false};
    std::atomic<unsigned> audioErrors{0}, completedStages{0};
    std::atomic<std::uint64_t> renderedCallbacks{0};
    std::thread audio([&] {
        auto desired = initial;
        desired.focusEnabled = true;
        desired.focus.nodes[0].enabled = true;
        desired.focus.phaseMode = focus::PhaseMode::Rotation;
        std::uint64_t physical = 0;
        for (unsigned stage = 0; stage < 4 && !abort.load(std::memory_order_acquire); ++stage) {
            desired.eq.lookaheadMs = static_cast<float>(2 * (stage + 1));
            desired.focus.rotationDegrees = 30. + 30. * static_cast<double>(stage);
            if (stage == 3) {
                desired.focus.phaseMode = focus::PhaseMode::Match;
                desired.focus.matchAmount = 1.;
            }
            const auto expectedLatency = 1024 + 96 * static_cast<int>(stage + 1);
            bool converged = false;
            for (int attempt = 0; attempt < 5000 && !abort.load(std::memory_order_acquire); ++attempt) {
                Audio<float> main, key;
                main.signal(++physical);
                key.signal(physical, true);
                const auto configured = backend.advertisedLatency();
                if (!run(backend, main, &key, desired, physical, configured)) {
                    audioErrors.fetch_add(1, std::memory_order_relaxed);
                    break;
                }
                renderedCallbacks.fetch_add(1, std::memory_order_relaxed);
                const auto status = backend.status();
                if (configured == expectedLatency && status.requestedWork >= stage + 1 &&
                    status.completedWork == status.requestedWork) {
                    converged = true;
                    break;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            if (!converged) {
                audioErrors.fetch_add(1, std::memory_order_relaxed);
                break;
            }
            completedStages.fetch_add(1, std::memory_order_release);
        }
        done.store(true, std::memory_order_release);
    });
    bool completeLatencies = true;
    int lastAdvertised = 0;
    for (int attempt = 0; attempt < 10000 && !done.load(std::memory_order_acquire); ++attempt) {
        backend.pumpMainThread();
        const auto latency = backend.advertisedLatency();
        const bool whole = latency == 0 || latency == 1120 || latency == 1216 || latency == 1312 ||
                           latency == 1408;
        completeLatencies = completeLatencies && whole && latency >= lastAdvertised;
        lastAdvertised = latency;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    abort.store(true, std::memory_order_release);
    audio.join();
    backend.pumpMainThread();
    // The actor may finish just before its worker clears the busy diagnostic;
    // one final bounded Main-driven wait verifies it really parks afterward.
    const bool parked = waitOwner(backend);
    const auto status = backend.status();
    check(audioErrors.load() == 0 && completedStages.load() == 4 && renderedCallbacks.load() >= 4,
          "concurrent audio, Main and real owner converge through four requested complete plans");
    check(completeLatencies && backend.advertisedLatency() == 1408,
          "Main sees only monotone whole aggregate latencies, never partial child sums");
    check(parked && status.requestedWork == 4 && status.completedWork == 4 && status.ownerPasses == 4 &&
              status.workerStarts == 1,
          "three-actor work versions complete exactly with one reusable parked worker");
    backend.release();
    const auto stopped = backend.status();
    check(!stopped.prepared && !stopped.workerStarted && !stopped.ownerBusy && !stopped.ownerParked,
          "three-actor teardown joins owner work after stopping its sole audio publisher");
}
void learnedStateContinuation() {
    chain::Settings settings;
    settings.eq.bands[0].status = eq::Status::On;
    settings.eq.bands[0].dynamicOn = true;
    settings.eq.bands[0].dynamicLearn = true;
    settings.eq.bands[0].targetGainDb = -12.f;
    const auto stored = settings;
    chain::Backend backend(chain::OwnerMode::Manual);
    backend.prepare({48000, frames, 2}, settings);
    bool okay = true;
    for (std::uint64_t block = 1; block <= 128; ++block) {
        Audio<float> audio;
        audio.signal(block);
        okay = run(backend, audio, static_cast<const Audio<float> *>(nullptr), settings, block, 0) && okay;
    }
    const auto threshold = backend.learnedThreshold(0), knee = backend.learnedKnee(0);
    check(okay && std::isfinite(threshold) && std::isfinite(knee) && knee >= 2.,
          "backend processing produces authored learned threshold and knee readouts");
    backend.release();
    check(backend.learnedThreshold(0) == threshold && backend.learnedKnee(0) == knee,
          "stopped release retains the EQ learning object");
    backend.prepare({96000, 128, 2}, settings);
    check(backend.learnedThreshold(0) == threshold && backend.learnedKnee(0) == knee,
          "same-instance rate/capacity reprepare preserves the authored learning law");
    settings.eq.bands[0].dynamicLearn = false;
    backend.prepare({96000, 128, 2}, settings);
    check(backend.learnedThreshold(0) == threshold && backend.learnedKnee(0) == knee,
          "disabling learn across reprepare preserves the last learned values");
    settings.eq.bands[0].dynamicLearn = true;
    backend.prepare({96000, 128, 2}, settings);
    check(backend.learnedThreshold(0) == -40. && backend.learnedKnee(0) == knee,
          "a learn rising edge retains authored threshold-reset and knee-retention semantics");
    chain::Backend fresh(chain::OwnerMode::Manual);
    fresh.prepare({96000, 128, 2}, stored);
    check(fresh.learnedThreshold(0) == -40. && fresh.learnedKnee(0) == 0. &&
              settings.eq.bands[0].thresholdDb == stored.eq.bands[0].thresholdDb &&
              settings.eq.bands[0].kneeDb == stored.eq.bands[0].kneeDb,
          "fresh parameter recall stays fresh and runtime learning never rewrites stored controls");
}
void failuresAndValidation() {
    chain::Backend backend(chain::OwnerMode::Manual);
    auto settings = bell();
    backend.prepare({48000, frames, 2}, settings);
    Audio<float> audio;
    audio.signal(1);
    const auto unchanged = audio;
    auto invalid = settings;
    invalid.focus.nodes[0].q = std::numeric_limits<double>::quiet_NaN();
    check(!run(backend, audio, static_cast<const Audio<float> *>(nullptr), invalid, 1, 0) &&
              audio == unchanged && backend.status().requestedWork == 0,
          "invalid intent cannot publish work or change samples");
    settings.focusEnabled = true;
    check(run(backend, audio, static_cast<const Audio<float> *>(nullptr), settings, 2, 0),
          "valid pending intent retains old admitted audio before owner failure");
    refuseAllocation = true;
    check(!backend.serviceOwnerOnce(), "owner allocation failure is caught outside audio");
    refuseAllocation = false;
    check(backend.status().failure == chain::BackendFailure::OwnerService,
          "failed owner work reports an explicit parked failure");
    const auto passes = backend.status().ownerPasses;
    for (int i = 0; i < 20; ++i) {
        backend.pumpMainThread();
        static_cast<void>(backend.serviceOwnerOnce());
    }
    check(backend.status().ownerPasses == passes, "failed work is not retried in an idle loop");
    audio.signal(3);
    check(run(backend, audio, static_cast<const Audio<float> *>(nullptr), settings, 3, 0),
          "failed resource admission leaves the previous audio plan usable");
    backend.prepare({48000, frames, 2}, bell());
    check(backend.status().failure == chain::BackendFailure::None &&
              backend.status().requestedWork == backend.status().completedWork,
          "explicit stopped prepare clears failed and unfinished owner work");
    bool refused = false;
    try {
        backend.prepare({std::numeric_limits<double>::quiet_NaN(), frames, 2}, bell());
    } catch (const std::exception &) {
        refused = true;
    }
    audio.signal(4);
    const auto before = audio;
    check(refused && !backend.status().prepared &&
              !run(backend, audio, static_cast<const Audio<float> *>(nullptr), bell(), 4, 0) &&
              audio == before,
          "failed stopped prepare is explicitly unprepared and refuses processing safely");
    backend.prepare({48000, frames, 2}, bell());
    check(backend.status().prepared, "an explicitly unprepared adapter can retry preparation");
}
} // namespace
int main() {
    manualAdmission();
    workerLifecycle();
    scalarControlsStayIdle();
    threeActorConvergence();
    learnedStateContinuation();
    failuresAndValidation();
    for (const auto rate : {44100., 48000., 96000.}) {
        for (const int channels : {1, 2}) {
            exactInitial<float>(false, rate, channels);
            exactInitial<double>(false, rate, channels);
        }
        exactInitial<float>(true, rate, 2);
        exactInitial<double>(true, rate, 2);
    }
    check(audioAllocations.load() == 0 && audioDeallocations.load() == 0,
          "adapter audio calls allocate and free no C++ objects");
    std::printf("EQ chain backend: %d checks, %d failures, %llu generated samples, %llu/%llu audio allocations/deallocations\n",
                checks, failures, static_cast<unsigned long long>(comparedSamples),
                static_cast<unsigned long long>(audioAllocations.load()),
                static_cast<unsigned long long>(audioDeallocations.load()));
    return failures ? 1 : 0;
}
