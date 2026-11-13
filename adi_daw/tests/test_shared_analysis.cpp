// SPDX-License-Identifier: AGPL-3.0-or-later
#include "adi/ui/shared_analysis.hpp"
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <exception>
#include <memory>
#include <limits>
#include <numbers>
#include <stdexcept>
#include <thread>
#include <utility>
#include <vector>

using adi::engine::ScopePoint;
using adi::engine::ScopeTap;
using adi::ui::SharedAnalysis;
namespace {
int checks = 0, failures = 0;
void check(bool ok, const char *what) {
    ++checks;
    if (!ok) { ++failures; std::printf("FAIL %s\n", what); }
}
bool near(double a, double b, double tolerance = 0.0003) { return std::abs(a - b) < tolerance; }
void tone(ScopeTap &tap, int frames = 1024, float amplitude = .5f, float rightScale = 1.f,
          std::int64_t stamp = 0) {
    std::vector<float> left(static_cast<std::size_t>(frames)), right(left.size());
    for (int i = 0; i < frames; ++i) {
        left[static_cast<std::size_t>(i)] = amplitude * static_cast<float>(std::sin(2 * std::numbers::pi * 16 * i / 1024));
        right[static_cast<std::size_t>(i)] = left[static_cast<std::size_t>(i)] * rightScale;
    }
    tap.write(left.data(), right.data(), frames, stamp, true);
}
struct Fixture {
    std::shared_ptr<ScopeTap> tap = std::make_shared<ScopeTap>(48000, SharedAnalysis::kTapSeconds);
    int acquireCalls = 0, releaseCalls = 0;
    SharedAnalysis broker{
        [this](std::int64_t, ScopePoint) { ++acquireCalls; return tap; },
        [this](std::int64_t, ScopePoint) { ++releaseCalls; }, SharedAnalysis::Mode::Manual};
};
void sharing() {
    Fixture f;
    check(f.broker.pump() == 0 && f.broker.stats().transforms == 0, "no demand runs no transform");
    auto full = f.broker.acquire({1});
    auto compact = f.broker.acquire({1});
    check(full && compact && full.available() && compact.available(), "full and compact requests acquire valid demand");
    check(f.acquireCalls == 1 && f.broker.stats().taps == 1 && f.broker.stats().domains == 1 &&
          f.broker.stats().consumers == 2, "compatible consumers attach one tap and one domain");
    check(f.broker.pump() == 0 && !full.read(), "short/unwritten history does not publish invented data");
    tone(*f.tap);
    check(f.broker.pump() == 1, "one shared transform for one new window");
    auto first = full.read();
    check(first && first == compact.read(), "full and compact read the identical immutable publication");
    check(first && first->sampleRate == 48000 && first->firstStamp == 0 && first->binsDbfs.size() == 513 &&
          near(first->binsDbfs[16], -6.020599913), "published spectrum uses existing calibrated kernel and heard stamp");
    check(first && near(first->peak[0], .5) && near(first->rms[0], std::sqrt(.125)), "stereo peak and RMS use the same input window");
    check(f.broker.pump() == 0 && full.read() == first && f.broker.stats().transforms == 1,
          "unchanged samples do not transform or republish");
    full.reset();
    check(f.releaseCalls == 0 && compact.available() && f.broker.stats().consumers == 1,
          "hiding one consumer leaves the other attached");
    tone(*f.tap, 1024, .25f, 1.f, 1024);
    check(f.broker.pump() == 4 && compact.read()->revision == first->revision + 4 &&
          near(compact.read()->binsDbfs[16], -12.041199827), "four contiguous hops advance one shared publication");
    check(near(first->binsDbfs[16], -6.020599913), "older retained publication remains immutable");
    compact.reset();
    check(f.releaseCalls == 1 && f.broker.stats().domains == 0 && f.broker.stats().taps == 0,
          "last consumer detaches the tap");
    tone(*f.tap);
    check(f.broker.pump() == 0 && f.broker.stats().transforms == 5,
          "no hidden transform after all consumers detach");
    compact.reset();
    check(f.releaseCalls == 1, "repeated lease reset cannot detach twice");
    auto reopened = f.broker.acquire({1});
    check(reopened && !reopened.read() && f.broker.pump() == 0 && f.broker.stats().transforms == 5,
          "reopening retained tap history waits for samples written after renewed demand");
    auto reopenedSide = f.broker.acquire({1, ScopePoint::PostFader, 1024, SharedAnalysis::Channel::Side});
    check(!reopenedSide.read() && f.broker.pump() == 0,
          "a different configuration before fresh audio shares the attachment freshness gate");
    reopenedSide.reset();
    tone(*f.tap, 1024, .125f, 1.f, 200000);
    check(f.broker.pump() == 1 && reopened.read() && near(reopened.read()->binsDbfs[16], -18.06179974),
          "first post-reopen audio supplies the new publication");
    auto reopenedCompact = f.broker.acquire({1});
    check(reopenedCompact.read() == reopened.read() && f.broker.pump() == 0,
          "compatible second reopened consumer reuses the fresh result without another sample gate");
}
void configurationsAndMoves() {
    Fixture f;
    auto mid = f.broker.acquire({1});
    auto side = f.broker.acquire({1, ScopePoint::PostFader, 1024, SharedAnalysis::Channel::Side});
    auto left = f.broker.acquire({1, ScopePoint::PostFader, 1024, SharedAnalysis::Channel::Left});
    auto pre = f.broker.acquire({1, ScopePoint::PreFader});
    tone(*f.tap, 1024, .5f, -1.f);
    check(f.acquireCalls == 2 && f.broker.stats().taps == 2 && f.broker.stats().domains == 4,
          "channel configurations share a track tap; different tap points remain distinct");
    check(f.broker.pump() == 2 && mid.read() != side.read(), "distinct channel domains reuse zero results without FFT for exactly silent mid signals");
    check(mid.read()->binsDbfs[16] <= -190 && near(side.read()->binsDbfs[16], -6.020599913) &&
          near(left.read()->binsDbfs[16], -6.020599913), "mid and side definitions preserve antiphase distinction");
    auto moved = std::move(left);
    check(!left && moved && f.broker.stats().consumers == 4, "moving a lease transfers demand without reacquiring");
    moved = std::move(side);
    check(!side && moved && f.broker.stats().consumers == 3 && f.releaseCalls == 0,
          "move assignment releases its former domain but retains the shared tap");
    mid.reset(); moved.reset();
    check(f.releaseCalls == 1 && pre && f.broker.stats().taps == 1,
          "last domain on one tap does not release a different tap point");
    pre.reset();
    check(f.releaseCalls == 2 && f.broker.stats().consumers == 0, "each attached tap is released once");
}
void refreshAndLifetime() {
    Fixture f;
    auto lease = f.broker.acquire({1});
    tone(*f.tap);
    f.broker.pump();
    const auto initial = lease.read();
    f.broker.refreshSources();
    check(lease.read() == initial && f.broker.pump() == 0, "unchanged source refresh preserves publication and sample gate");
    f.broker.refreshSources(true);
    check(!lease.read() && lease.available() && f.broker.pump() == 0,
          "explicit seek invalidates publication and waits for new audio");
    tone(*f.tap, 1024, .25f, 1.f, 100000);
    f.broker.pump();
    check(lease.read() && lease.read()->sourceEpoch > initial->sourceEpoch && lease.read()->firstStamp == 100000,
          "post-seek audio belongs to a new heard-timeline epoch");
    const auto seek = lease.read();
    f.tap = std::make_shared<ScopeTap>(96000, SharedAnalysis::kTapSeconds);
    tone(*f.tap);
    f.broker.refreshSources();
    check(!lease.read() && f.broker.pump() == 0,
          "source replacement waits for data written after its new attachment");
    tone(*f.tap);
    check(f.broker.pump() == 1 && lease.read()->sampleRate == 96000 &&
          lease.read()->sourceEpoch > seek->sourceEpoch, "source/rate replacement keeps lease but replaces result domain");
    f.tap.reset();
    f.broker.refreshSources();
    check(!lease.available() && !lease.read() && f.broker.pump() == 0,
          "removed source is explicitly unavailable, never a stale result");
    lease.reset();
    check(f.releaseCalls == 1, "last demand releases an earlier attachment even after source became unavailable");

    SharedAnalysis::Lease retained;
    int released = 0;
    {
        auto tap = std::make_shared<ScopeTap>(48000, SharedAnalysis::kTapSeconds);
        SharedAnalysis broker([tap](std::int64_t, ScopePoint) { return tap; },
                              [&released](std::int64_t, ScopePoint) { ++released; }, SharedAnalysis::Mode::Manual);
        retained = broker.acquire({1}); tone(*tap); broker.pump();
        check(retained.read() != nullptr, "retained lease has a live publication before owner teardown");
    }
    check(released == 1 && !retained && !retained.available() && !retained.read(),
          "broker teardown revokes outliving leases and releases owner callbacks");
    retained.reset();
    check(released == 1, "outliving lease destruction cannot call the dead owner");
}
void refusalsAndBounds() {
    Fixture f;
    for (auto request : {SharedAnalysis::Request{0}, SharedAnalysis::Request{-1},
                         SharedAnalysis::Request{1, ScopePoint::PostFader, 1000},
                         SharedAnalysis::Request{1, ScopePoint::PostFader, 131072},
                         SharedAnalysis::Request{1, static_cast<ScopePoint>(99)}})
        check(!f.broker.acquire(request), "invalid request refuses without attaching");
    check(f.acquireCalls == 0, "all invalid request paths have zero tap side effects");
    f.tap = std::make_shared<ScopeTap>(48000, .001);
    auto shortTap = f.broker.acquire({1});
    check(shortTap && !shortTap.available() && !shortTap.read(), "ring shorter than the window is unavailable rather than silently truncated");
    shortTap.reset();
    check(f.releaseCalls == 1, "unusable but attached tap is still released");
    f.tap = std::make_shared<ScopeTap>(48000, SharedAnalysis::kTapSeconds);
    std::vector<SharedAnalysis::Lease> leases;
    leases.reserve(SharedAnalysis::kMaxDomains);
    for (std::size_t i = 0; i < SharedAnalysis::kMaxDomains; ++i)
        leases.push_back(f.broker.acquire({static_cast<std::int64_t>(i + 1), ScopePoint::PostFader, 32}));
    check(f.broker.stats().domains == SharedAnalysis::kMaxDomains && !f.broker.acquire({1000}),
          "domain capacity is explicit and bounded");
    auto duplicate = f.broker.acquire({1, ScopePoint::PostFader, 32});
    check(duplicate && f.broker.stats().consumers == SharedAnalysis::kMaxDomains + 1,
          "compatible demand still reuses a full-capacity domain table");
}
void shutdown() {
    Fixture f;
    auto lease = f.broker.acquire({1});
    tone(*f.tap); f.broker.pump();
    const auto transforms = f.broker.stats().transforms;
    f.broker.shutdown();
    check(f.releaseCalls == 1 && !lease && !lease.read() && f.broker.stats().consumers == 0,
          "explicit shutdown revokes visible leases before source owner teardown");
    check(!f.broker.acquire({1}) && f.broker.pump() == 0 &&
          f.broker.stats().transforms == transforms, "shutdown is permanent and performs no later analysis");
    const auto calls = f.acquireCalls;
    f.broker.refreshSources(); f.broker.shutdown(); lease.reset();
    check(f.releaseCalls == 1 && f.acquireCalls == calls,
          "shutdown, refresh and lease release after shutdown cannot call the owner again");
}
void workerLifecycle() {
    auto tap = std::make_shared<ScopeTap>(48000, SharedAnalysis::kTapSeconds);
    int acquired = 0, released = 0;
    SharedAnalysis broker([&](std::int64_t, ScopePoint) { ++acquired; return tap; },
                          [&](std::int64_t, ScopePoint) { ++released; });
    auto lease = broker.acquire({1});
    tone(*tap);
    check(broker.stats().transforms == 0 && !lease.read(), "acquiring/writing alone starts no worker or autonomous polling");
    broker.requestFrame();
    // Functional bounded wait for the real single worker, not a duration/CPU assertion.
    const auto limit = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (!lease.read() && std::chrono::steady_clock::now() < limit) std::this_thread::yield();
    check(acquired == 2 && lease.read() && near(lease.read()->binsDbfs[16], -6.020599913),
          "a visible frame request makes the process worker publish without message-thread FFT");
    while (broker.stats().analysisWindows == 0 && std::chrono::steady_clock::now() < limit) std::this_thread::yield();
    const auto settled = broker.stats();
    for (int i = 0; i < 64; ++i) broker.requestFrame();
    check(broker.stats().workerPasses == settled.workerPasses && broker.stats().cursorReads == settled.cursorReads &&
          broker.stats().analysisWindows == settled.analysisWindows && broker.stats().publications == settled.publications,
          "visible unchanged-source frames resolve leases but enqueue no worker analysis pass");
    const auto oldGeneration = lease.read()->generation;
    tap = std::make_shared<ScopeTap>(96000, .25);
    broker.requestFrame();
    check(!lease.read() && lease.available(), "a visible worker frame resolves changed lease generation without a project edit");
    tone(*tap); broker.requestFrame();
    const auto refreshedLimit = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (!lease.read() && std::chrono::steady_clock::now() < refreshedLimit) std::this_thread::yield();
    check(lease.read() && lease.read()->generation != oldGeneration && lease.read()->sampleRate == 96000,
          "the global worker follows renegotiated rate/history rather than an old retained ring");
    broker.shutdown();
    check(released == 1 && !lease && !lease.read(), "worker shutdown joins before the source owner is released");
    lease.reset();
    check(released == 1, "worker lease cannot call owner after shutdown");
}
void sharedModesAndCursors() {
    Fixture f;
    auto spectrum = f.broker.acquire({1});
    auto request = SharedAnalysis::Request{1}; request.outputs = SharedAnalysis::Spectrogram;
    auto spectrogram = f.broker.acquire(request);
    request.outputs = SharedAnalysis::Scope | SharedAnalysis::Phase | SharedAnalysis::Meters;
    auto temporal = f.broker.acquire(request);
    tone(*f.tap);
    check(f.broker.pump() == 1 && f.broker.stats().domains == 1, "five compatible modes share one window and one FFT");
    const auto initial = spectrum.read();
    check(initial == spectrogram.read() && initial == temporal.read() && initial->scopeLeft.size() == 1024 &&
          near(initial->correlation, 1.) && initial->columns.size() == 1 && near(initial->coherentGain, .5),
          "mode results share immutable samples, calibrated spectrum, phase and hop column");
    tone(*f.tap, 768, .5f, 1.f, 1024);
    check(f.broker.pump() == 3 && spectrogram.read()->columns.size() == 4 &&
          spectrogram.read()->columns.back().firstStamp == 768, "sample cursor emits every contiguous overlap hop with heard stamps");
    spectrum.reset(); spectrogram.reset();
    const auto before = f.broker.stats().transforms;
    tone(*f.tap, 1024, .5f, -1.f, 1792); f.broker.pump();
    check(f.broker.stats().transforms == before && temporal.read()->binsDbfs.empty() &&
          near(temporal.read()->correlation, -1.), "scope/meters/phase alone perform no FFT and preserve antiphase");
    const auto epoch = temporal.read()->sourceEpoch;
    tone(*f.tap, 1024, .25f, 1.f, 50000); f.broker.pump();
    check(temporal.read()->sourceEpoch > epoch && temporal.read()->gapCount == 1 &&
          temporal.read()->firstStamp == 50000 && near(temporal.read()->peak[0], .25),
          "a timeline discontinuity discards overlap history instead of mixing old/new samples");
    auto powerRequest = SharedAnalysis::Request{1, ScopePoint::PostFader, 1024, SharedAnalysis::Channel::StereoPower};
    auto power = f.broker.acquire(powerRequest); f.broker.pump();
    tone(*f.tap, 1024, .5f, -1.f, 51024); f.broker.pump();
    check(power.read() && near(power.read()->binsDbfs[16], -6.020599913), "stereo power spectrum does not cancel antiphase audio");
}
void boundsAndDeviceSources() {
    auto tap = std::make_shared<ScopeTap>(48000, .1);
    std::vector<adi::engine::ScopeSource> acquired, released;
    double seconds = 0;
    SharedAnalysis broker([&](adi::engine::ScopeSource source, double history) {
        acquired.push_back(source); seconds = history; return tap;
    }, [&](adi::engine::ScopeSource source) { released.push_back(source); }, SharedAnalysis::Mode::Manual);
    SharedAnalysis::Request request{1, ScopePoint::DeviceOutput}; request.device = 77;
    auto lease = broker.acquire(request);
    check(lease && acquired.size() == 1 && acquired[0].device == 77 && seconds < 1.,
          "device endpoint identity and negotiated window history reach the engine");
    // Force a genuine cursor overrun. The next pass resets overlap, reports it, and consumes
    // only bounded retained history; no inference from a newest-written counter is needed.
    tone(*tap, 10000); broker.pump();
    check(lease.read() && lease.read()->overrunCount == 1 && lease.read()->gapCount == 1,
          "ring overrun is visible in the immutable result");
    check(broker.stats().transforms <= SharedAnalysis::kMaxWindowsPerFrame,
          "one requested pass has a hard transform-window budget");
    lease.reset();
    check(released.size() == 1 && released[0] == acquired[0], "last device demand releases exactly its endpoint");
    auto fragmented = broker.acquire(request);
    const float sample = .25f;
    for (std::int64_t i = 0; i < 1000; ++i) tap->write(&sample, &sample, 1, i * 100, true);
    const auto beforeReads = broker.stats().cursorReads;
    broker.pump();
    check(broker.stats().cursorReads - beforeReads <= SharedAnalysis::kMaxReadsPerFrame && !fragmented.read(),
          "pathological timestamp boundaries respect the read budget without publishing a fictitious contiguous window");
    fragmented.reset();
    auto invalidSamples = broker.acquire(request);
    std::array<float, 1024> damaged{}; damaged[0] = std::numeric_limits<float>::quiet_NaN();
    tap->write(damaged.data(), damaged.data(), 1024, 200000, true); broker.pump();
    check(invalidSamples.read() && invalidSamples.read()->nonFiniteSamples == 2 &&
          invalidSamples.read()->peak[0] == 0 && invalidSamples.read()->binsDbfs[0] <= -190,
          "non-finite visualization samples are flagged and contained without changing source audio");
}
void historyRenegotiation() {
    std::shared_ptr<ScopeTap> tap;
    double heldSeconds = 0;
    std::vector<double> histories;
    int released = 0;
    SharedAnalysis broker([&](adi::engine::ScopeSource, double seconds) {
        if (heldSeconds != seconds) { tap = std::make_shared<ScopeTap>(48000, seconds); heldSeconds = seconds; }
        histories.push_back(seconds); return tap;
    }, [&](adi::engine::ScopeSource) { ++released; }, SharedAnalysis::Mode::Manual);
    auto large = broker.acquire({1, ScopePoint::PostFader, 32768});
    auto small = broker.acquire({1});
    tone(*tap); broker.pump();
    const auto original = small.read();
    check(original && histories.size() == 2 && near(histories.front(), .25) && near(heldSeconds, 4. * 32768 / 48000), "largest visible analysis discovers actual rate before exact history negotiation");
    large.reset();
    check(histories.size() == 3 && heldSeconds == .25 && released == 0 && small.available() && !small.read(),
          "closing the large window shrinks history while retaining the smaller source subscription");
    tone(*tap); broker.pump();
    check(small.read() && small.read()->sourceEpoch > original->sourceEpoch &&
          small.read()->generation != original->generation, "only an actual ring replacement starts a fresh analysis generation");
    auto duplicate = broker.acquire({1}); duplicate.reset();
    check(histories.size() == 3 && small.read(), "compatible consumer removal leaves equal history and publication untouched");
    small.reset();
    check(released == 1, "renegotiation still releases the endpoint exactly once at last demand");
    const auto retainedRing = std::make_shared<ScopeTap>(48000, 4.);
    int resolutions = 0;
    SharedAnalysis retained([&](adi::engine::ScopeSource, double) { ++resolutions; return retainedRing; },
                            [](adi::engine::ScopeSource) {}, SharedAnalysis::Mode::Manual);
    auto retainedLarge = retained.acquire({1, ScopePoint::PostFader, 32768});
    auto retainedSmall = retained.acquire({1}); tone(*retainedRing); retained.pump();
    const auto retainedResult = retainedSmall.read();
    retainedLarge.reset();
    check(resolutions == 3 && retainedSmall.read() == retainedResult,
          "history shrink with an externally retained physical ring does not reset its unchanged generation/publication");
}
void fairFrameBudget() {
    const auto busyTap = std::make_shared<ScopeTap>(48000, 1.);
    const auto smallTap = std::make_shared<ScopeTap>(48000, 1.);
    SharedAnalysis broker([&](std::int64_t track, ScopePoint) { return track == 1 ? busyTap : smallTap; },
                          [](std::int64_t, ScopePoint) {}, SharedAnalysis::Mode::Manual);
    auto busy = broker.acquire({1}), small = broker.acquire({2});
    tone(*busyTap, 32768); tone(*smallTap);
    check(broker.pump() == SharedAnalysis::kMaxWindowsPerFrame && busy.read() &&
          busy.read()->budgetLimited && !small.read(), "a backlogged domain is capped at the existing frame budget");
    const auto windows = broker.stats().analysisWindows;
    broker.pump();
    check(busy.read() && small.read() && small.read()->firstStamp == 0 &&
          broker.stats().analysisWindows - windows <= SharedAnalysis::kMaxWindowsPerFrame,
          "rotating domain start publishes the second source within two passes without increasing work budget");
}
void lazyPlansAndIdleDemand() {
    Fixture f;
    SharedAnalysis::Request request{1}; request.outputs = SharedAnalysis::Scope | SharedAnalysis::Meters | SharedAnalysis::Phase;
    auto temporal = f.broker.acquire(request);
    check(f.broker.stats().spectralPlans == 0, "temporal demand allocates no spectral plan");
    tone(*f.tap); f.broker.pump();
    const auto first = temporal.read();
    const auto idle = f.broker.stats();
    for (int i = 0; i < 8; ++i) f.broker.pump();
    check(first && first->binsDbfs.empty() && f.broker.stats().workerPasses == idle.workerPasses &&
          f.broker.stats().analysisWindows == idle.analysisWindows && f.broker.stats().cursorReads == idle.cursorReads &&
          temporal.read() == first, "unchanged temporal samples do no worker pass, aggregation or publication");
    // A partial hop must not overwrite the retained completed window used by new modes.
    tone(*f.tap, 128, .25f, 1.f, 1024); f.broker.pump();
    auto spectrum = f.broker.acquire({1});
    check(f.broker.stats().spectralPlans == 1 && !spectrum.read(), "first spectral consumer prepares one shared plan before worker demand");
    check(f.broker.pump() == 1 && spectrum.read() && spectrum.read()->firstSequence == 0 &&
          near(spectrum.read()->binsDbfs[16], -6.020599913) && spectrum.read()->scopeLeft == first->scopeLeft,
          "new mode at idle reuses the exact completed window rather than a partial hop");
    request.outputs = SharedAnalysis::Spectrogram;
    auto history = f.broker.acquire(request);
    const auto before = f.broker.stats();
    f.broker.pump();
    check(history.read() && history.read()->columns.size() == 1 && f.broker.stats().transforms == before.transforms &&
          f.broker.stats().analysisWindows == before.analysisWindows,
          "idle spectrogram demand reuses the already calibrated FFT without consuming another window");
    const auto sharedColumn = history.read()->columns.front().binsDbfs;
    auto compatible = f.broker.acquire(request);
    const auto compatibleResult = history.read();
    const auto compatiblePasses = f.broker.stats().workerPasses;
    f.broker.pump();
    check(compatible.read() == compatibleResult && f.broker.stats().workerPasses == compatiblePasses,
          "identical new demand reuses the publication without another analysis job");
    tone(*f.tap, 128, .25f, 1.f, 1152); f.broker.pump();
    check(history.read()->columns.size() == 2 && history.read()->columns.front().binsDbfs == sharedColumn &&
          near((*sharedColumn)[16], -6.020599913), "history snapshots share immutable existing column payloads");
    spectrum.reset(); history.reset(); compatible.reset();
    check(f.broker.stats().spectralPlans == 0, "last spectral consumer releases plan and spectral scratch while temporal demand stays");
    f.broker.pump();
    check(temporal.read() && temporal.read()->binsDbfs.empty() && temporal.read()->columns.empty(),
          "dropping spectral output updates the retained window at idle without a stale spectral claim");
    auto other = request; other.fftSize = 512; other.outputs = SharedAnalysis::Spectrum;
    auto independent = f.broker.acquire(other);
    check(f.broker.pump() > 0 && independent.read() && independent.read()->request.fftSize == 512,
          "a new independent cursor domain can consume already captured fresh samples at idle");
    f.broker.refreshSources(true); f.broker.pump();
    const auto reset = f.broker.stats();
    f.broker.pump();
    check(!temporal.read() && !independent.read() && f.broker.stats().workerPasses == reset.workerPasses,
          "an explicit epoch reset has no repeated work or replay before fresh input");
}
void boundedImmutableHistory() {
    const auto tap = std::make_shared<ScopeTap>(48000, 1.);
    constexpr std::size_t columnBytes = 513 * sizeof(float);
    SharedAnalysis broker([&](std::int64_t, ScopePoint) { return tap; }, [](std::int64_t, ScopePoint) {},
                          SharedAnalysis::Mode::Manual, 3 * columnBytes);
    SharedAnalysis::Request request{1}; request.outputs = SharedAnalysis::Spectrogram;
    auto lease = broker.acquire(request);
    tone(*tap); broker.pump();
    tone(*tap, 256, .5f, 1.f, 1024); broker.pump();
    tone(*tap, 256, .5f, 1.f, 1280); broker.pump();
    check(lease.read()->columns.size() == 3 && broker.stats().historyBytes == 3 * columnBytes,
          "history payload budget counts each shared immutable bin buffer once");
    tone(*tap, 256, .5f, 1.f, 1536); broker.pump();
    check(lease.read()->historyLimited && lease.read()->columns.size() == 2 && lease.read()->historySerial == 4 &&
          lease.read()->droppedHistoryColumns == 1 && lease.read()->gapCount == 0 && lease.read()->overrunCount == 0,
          "an exact-full budget retires an explicit resource column, distinct from source gaps");
    const auto blocked = broker.stats();
    broker.pump();
    check(!lease.read()->historyLimited && lease.read()->columns.size() == 3 && lease.read()->columns.back().serial == 4 &&
          broker.stats().transforms == blocked.transforms && broker.stats().analysisWindows == blocked.analysisWindows &&
          broker.stats().cursorReads == blocked.cursorReads && broker.stats().historyPeakBytes <= 3 * columnBytes,
          "current-publication capacity recovers on the next bounded pass without new samples or FFT");
    auto frozen = lease.read();
    tone(*tap, 256, .5f, 1.f, 1792); broker.pump();
    tone(*tap, 256, .5f, 1.f, 2048); broker.pump();
    check(lease.read()->historyLimited && lease.read()->historySerial == 6 && lease.read()->columns.size() == 1 &&
          broker.stats().historyBytes == 3 * columnBytes && frozen->columns.size() == 3 &&
          frozen->columns.back().serial == 4, "frozen snapshots remain immutable and fully charged while new history is capped");
    const auto held = broker.stats();
    broker.pump();
    check(broker.stats().publications == held.publications && broker.stats().analysisWindows == held.analysisWindows &&
          broker.stats().transforms == held.transforms, "a still-held cap retries without redundant FFT, aggregation or publication");
    frozen.reset();
    broker.pump();
    check(!lease.read()->historyLimited && lease.read()->columns.back().serial == 6 &&
          lease.read()->columns.front().serial == 4 && lease.read()->columns.size() == 2 &&
          lease.read()->droppedHistoryColumns == 4 && broker.stats().historyBytes == 2 * columnBytes,
          "releasing frozen backing resumes the newest pending hop with the missing hop left explicit");
    auto retained = lease.read(); lease.reset();
    check(broker.stats().historyBytes == 2 * columnBytes && broker.stats().spectralPlans == 0,
          "closed demand releases FFT resources while client-held history stays charged");
    retained.reset();
    check(broker.stats().historyBytes == 0, "last frozen buffer release returns all scoped history payload bytes");

    Fixture normal;
    auto full = normal.broker.acquire(request);
    tone(*normal.tap, 33536);
    for (int i = 0; i < 12; ++i) normal.broker.pump();
    check(full.read() && full.read()->columns.size() == 128 && full.read()->historySerial == 128 &&
          !full.read()->historyLimited && full.read()->droppedHistoryColumns == 0,
          "ordinary default history keeps all 128 unchanged-hop columns without resource drops");
    check(normal.broker.stats().historyBytes == 128 * columnBytes,
          "ordinary current publication shares the domain's 128 bin buffers without duplicate payload copies");
}
void actualRateHistory() {
    for (const double rate : {44100., 96000., 192000.}) {
        std::shared_ptr<ScopeTap> tap;
        std::vector<double> requests;
        double seconds = 0;
        SharedAnalysis broker([&](adi::engine::ScopeSource, double desired) {
            requests.push_back(desired);
            if (desired != seconds) { seconds = desired; tap = std::make_shared<ScopeTap>(rate, desired); }
            return tap;
        }, [](adi::engine::ScopeSource) {}, SharedAnalysis::Mode::Manual);
        auto large = broker.acquire({1, ScopePoint::PostFader, 65536});
        check(large.available() && requests.size() == 2 && requests.front() == .25 && near(seconds, 4. * 65536 / rate),
              "initial maximum window lease corrects history using the physical source rate");
        tone(*tap, 65536); broker.pump();
        const auto result = large.read();
        broker.refreshSources();
        check(result && large.read() == result && requests.size() == 3 && near(requests.back(), seconds),
              "same-rate history refresh preserves cursor, completed onset and existing source epoch");
    }
}

void lateRateAndFailedNegotiation() {
    std::shared_ptr<ScopeTap> tap;
    bool sourceExists = false;
    double seconds = 0;
    SharedAnalysis broker([&](adi::engine::ScopeSource, double desired) {
        if (!sourceExists) return tap;
        if (seconds != desired) { seconds = desired; tap = std::make_shared<ScopeTap>(192000., desired); }
        return tap;
    }, [](adi::engine::ScopeSource) {}, SharedAnalysis::Mode::Manual);
    auto pending = broker.acquire({1, ScopePoint::PostFader, 65536});
    check(pending && !pending.available(), "maximum-window demand can wait without fabricating a source rate");
    sourceExists = true; broker.refreshSources();
    check(pending.available() && near(seconds, 4. * 65536 / 192000.),
          "a late source corrects its history from actual rate before the first usable cursor");
    tone(*tap, 65536); broker.pump();
    check(pending.read() && pending.read()->firstStamp == 0 && pending.read()->firstSequence == 0 &&
          pending.read()->sampleRate == 192000., "late rate negotiation preserves the complete first acquired onset");
    const auto retained = std::make_shared<ScopeTap>(96000., 4.);
    int retainedCalls = 0;
    SharedAnalysis duringNegotiation([&](adi::engine::ScopeSource, double) {
        if (++retainedCalls == 2) tone(*retained, 65536);
        return retained;
    }, [](adi::engine::ScopeSource) {}, SharedAnalysis::Mode::Manual);
    auto onset = duringNegotiation.acquire({1, ScopePoint::PostFader, 65536});
    duringNegotiation.pump();
    check(retainedCalls == 2 && onset.read() && onset.read()->firstSequence == 0 && onset.read()->firstStamp == 0,
          "same-ring initial rate correction keeps audio captured after first discovery rather than skipping its onset");
    int acquired = 0, released = 0;
    SharedAnalysis failed([&](adi::engine::ScopeSource, double) -> std::shared_ptr<const ScopeTap> {
        if (++acquired == 2) throw std::runtime_error("history refused");
        return tap;
    }, [&](adi::engine::ScopeSource) { ++released; }, SharedAnalysis::Mode::Manual);
    bool refused = false;
    try { auto unavailable = failed.acquire({1, ScopePoint::PostFader, 65536}); }
    catch (const std::runtime_error &) { refused = true; }
    check(refused && acquired == 2 && released == 1 && failed.stats().domains == 0 && failed.stats().taps == 0,
          "failed second-stage history negotiation releases its pending lease without leaking hidden demand");
}

} // namespace
int main() {
    try { sharing(); configurationsAndMoves(); refreshAndLifetime(); refusalsAndBounds(); shutdown(); workerLifecycle();
          sharedModesAndCursors(); boundsAndDeviceSources(); historyRenegotiation(); fairFrameBudget(); lazyPlansAndIdleDemand(); boundedImmutableHistory(); actualRateHistory(); lateRateAndFailedNegotiation(); }
    catch (const std::exception &e) { ++failures; std::printf("FAIL exception: %s\n", e.what()); }
    std::printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
