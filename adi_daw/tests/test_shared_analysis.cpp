// SPDX-License-Identifier: AGPL-3.0-or-later
#include "adi/ui/shared_analysis.hpp"
#include "adi/ui/focus_scope.hpp"
#include "adi/dsp/spectrum_average.hpp"
#include "adi/engine/transport.hpp"
#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <memory>
#include <limits>
#include <numbers>
#include <new>
#include <stdexcept>
#include <thread>
#include <utility>
#include <vector>

using adi::engine::ScopePoint;
using adi::engine::ScopeTap;
using adi::ui::SharedAnalysis;
namespace {
int checks = 0, failures = 0;
thread_local int failAllocationAfter = -1;
thread_local bool observeAllocationSizes = false, refusedAllocationSize = false;
thread_local std::size_t allocationSizeCount = 0, refuseAllocationBytes = 0;
thread_local std::array<std::size_t,16> allocationSizes{};
void check(bool ok, const char *what) {
    ++checks;
    if (!ok) { ++failures; std::printf("FAIL %s\n", what); }
}
bool near(double a, double b, double tolerance = 0.0003) { return std::abs(a - b) < tolerance; }
bool exactBins(std::span<const float> a, std::span<const float> b) {
    return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](float x, float y) {
        return std::bit_cast<std::uint32_t>(x) == std::bit_cast<std::uint32_t>(y);
    });
}
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
void focusScopeWindows() {
    const auto tapA = std::make_shared<ScopeTap>(48000., 1.);
    const auto tapB = std::make_shared<ScopeTap>(48000., 1.);
    const auto tapC = std::make_shared<ScopeTap>(48000., 1.);
    int attached = 0;
    SharedAnalysis broker([&](std::int64_t track, ScopePoint) {
        ++attached;
        return track == 1 ? tapA : track == 2 ? tapB : tapC;
    }, [](std::int64_t, ScopePoint) {}, SharedAnalysis::Mode::Manual);
    SharedAnalysis::Request request{1};
    request.outputs = SharedAnalysis::Scope;
    request.fftSize = 32;
    request.hopSize = 16;
    auto a = broker.acquire(request);
    request.track = 2;
    auto b = broker.acquire(request);
    request.track = 3;
    auto c = broker.acquire(request);
    std::array<float, 64> left{}, right{};
    left.fill(.5f); right.fill(.25f);
    tapA->write(left.data(), right.data(), 64, 100, true, 7);
    left.fill(-.5f); right.fill(.25f);
    tapB->write(left.data(), right.data(), 64, 108, true, 7);
    broker.pump();
    const auto ra = a.read(), rb = b.read();
    check(ra && rb && attached == 3 && ra->transportRevision == 7 && rb->transportRevision == 7 &&
          ra->firstStamp == 132 && rb->firstStamp == 140 && ra->scopeLeft.size() == 32,
          "only the latest overlapping raw windows publish and retain the tap transport epoch");
    std::array sources{ra.get(), rb.get()};
    const auto overlap = adi::ui::focusScopeRange(sources);
    check(overlap && overlap->first == 140 && overlap->frames == 24 && overlap->sampleRate == 48000.,
          "Focus aligns different stamped offsets over their exact intersection");
    if (overlap) {
        const auto result = adi::ui::focusReadSum(sources, *overlap);
        check(result.peak == std::array{0., .5} && result.rms == std::array{0., .5} &&
              broker.stats().transforms == 0,
              "sample sum cancels antiphase before measuring and scope-only demand needs no FFT");
    }
    const auto waiting = c.read();
    check(!adi::ui::focusScopeRange(std::array{ra.get(), rb.get(), waiting.get()}),
          "an absent member produces a readout gap rather than a partial sum");

    tapC->write(left.data(), right.data(), 32, 140, true, 8);
    broker.pump();
    const auto afterSeek = c.read();
    check(afterSeek && afterSeek->firstStamp == 140 && afterSeek->transportRevision == 8 &&
          !adi::ui::focusScopeRange(std::array{ra.get(), rb.get(), afterSeek.get()}),
          "the same heard stamp cannot mix audio from two transport revisions");

    tapC->write(left.data(), right.data(), 32, 140, false, 7);
    broker.pump();
    const auto parked = c.read();
    check(parked && !parked->advancing && !adi::ui::focusScopeRange(std::array{ra.get(), rb.get(), parked.get()}),
          "a parked timestamp cannot align separate live monitor callbacks");

    tapC->write(left.data(), right.data(), 32, 400, true, 7);
    broker.pump();
    const auto disjoint = c.read();
    check(disjoint && disjoint->firstStamp == 400 && disjoint->transportRevision == 7 &&
          !adi::ui::focusScopeRange(std::array{ra.get(), rb.get(), disjoint.get()}),
          "same-revision buffers with no audible intersection produce a gap");
    constexpr auto lastStamp = std::numeric_limits<std::int64_t>::max();
    check(adi::ui::focusWindowEnd(lastStamp - 32, 32) == lastStamp &&
              !adi::ui::focusWindowEnd(lastStamp - 31, 32) && !adi::ui::focusWindowEnd(0, -1),
          "Focus validates the window end before comparing it with the rendered position");
    if (ra) {
        auto badStamp = *ra;
        badStamp.firstStamp = lastStamp - 31;
        check(!adi::ui::focusScopeMember(badStamp) &&
                  !adi::ui::focusScopeRange(std::array<const SharedAnalysis::Result *, 2>{ra.get(), &badStamp}),
              "overflowing stamps cannot form an independent trace or participate in a combined range");
    }
}
void parkedSilence() {
    Fixture f;
    auto request = SharedAnalysis::Request{1};
    request.outputs = SharedAnalysis::Spectrogram;
    const auto view = f.broker.acquire(request);
    const auto quiet = [&](std::uint64_t revision) {
        f.tap->write(static_cast<const float *>(nullptr), nullptr, 480, 0, false, revision);
        f.broker.pump();
    };
    tone(*f.tap);
    f.broker.pump();
    const auto playing = view.read();
    check(playing && playing->advancing, "playing signal seeds the spectrogram");
    quiet(1); quiet(1); quiet(1);
    const auto parked = view.read();
    const auto stable = f.broker.stats();
    check(parked && !parked->advancing && parked->peak[0] == 0 && parked->sourceEpoch > playing->sourceEpoch,
          "a complete quiet window replaces the playing picture after Stop");
    // Silence extends beyond the entire ring to also test resuming without a false overrun.
    for (int block = 0; block < 1100; ++block) quiet(1);
    const auto idle = f.broker.stats();
    check(idle.workerPasses == stable.workerPasses && idle.cursorReads == stable.cursorReads &&
              idle.publications == stable.publications && view.read() == parked,
          "unchanged stopped silence has no analysis work or scrolling publications");
    const std::vector<float> monitor(1024, .25f);
    f.tap->write(monitor.data(), monitor.data(), 1024, 0, false, 1);
    f.broker.pump();
    const auto audible = view.read();
    check(audible && audible->peak[0] == .25f && !audible->advancing &&
              audible->sourceEpoch == parked->sourceEpoch && audible->overrunCount == parked->overrunCount,
          "input monitoring wakes while parked, without misreporting a skipped silent interval as an overrun");
    quiet(1); quiet(1); quiet(1);
    const auto beforeSeek = view.read();
    quiet(2); quiet(2); quiet(2);
    const auto afterSeek = view.read();
    check(beforeSeek && afterSeek && !afterSeek->advancing && afterSeek->peak[0] == 0 &&
              afterSeek->sourceEpoch > beforeSeek->sourceEpoch,
          "an idle same-stamp seek invalidates the old parked publication");
    tone(*f.tap, 1024, .5f, 1.f, 0);
    f.broker.pump();
    check(view.read() && view.read()->advancing && view.read()->peak[0] == .5f,
          "playback resumes with the first complete window");
}
void sharing() {
    Fixture f;
    f.broker.requestFrame();
    check(f.broker.stats().successfulEnqueues == 0, "Manual mode never inserts scheduler work");
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
    broker.requestFrame();
    check(broker.stats().successfulEnqueues == 0 && acquired == 0,
          "zero-consumer worker frame requests enqueue no scheduler work");
    auto lease = broker.acquire({1});
    tone(*tap);
    check(broker.stats().transforms == 0 && !lease.read(), "acquiring/writing alone starts no worker or autonomous polling");
    broker.requestFrame();
    // Functional bounded wait for the real single worker, not a duration/CPU assertion.
    const auto limit = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (!lease.read() && std::chrono::steady_clock::now() < limit) std::this_thread::yield();
    check(acquired == 2 && lease.read() && near(lease.read()->binsDbfs[16], -6.020599913),
          "a visible frame request makes the process worker publish without message-thread FFT");
    const auto completed = [&] {
        const auto stats = broker.stats();
        const auto result = lease.read();
        return result && result->pendingFrames == 0 && stats.analysisWindows == 1 &&
               stats.transforms == 1 && stats.publications == 1 && stats.cursorReads > 0;
    };
    while (!completed() && std::chrono::steady_clock::now() < limit) std::this_thread::yield();
    check(completed(), "the actual worker completes and consumes its initial source window");
    const auto settled = broker.stats();
    check(settled.successfulEnqueues == 1, "one actual frame request inserts one scheduler item");
    for (int i = 0; i < 64; ++i) broker.requestFrame();
    const auto unchanged = broker.stats();
    check(unchanged.successfulEnqueues == settled.successfulEnqueues,
          "64 unchanged-source frame requests synchronously insert no scheduler items");
    check(unchanged.workerPasses == settled.workerPasses && unchanged.cursorReads == settled.cursorReads &&
          unchanged.analysisWindows == settled.analysisWindows && unchanged.publications == settled.publications,
          "unchanged-source requests also preserve useful analysis counters");
    const auto previous = lease.read();
    tone(*tap, 256, .25f, 1.f, 1024);
    broker.requestFrame();
    check(broker.stats().successfulEnqueues == settled.successfulEnqueues + 1,
          "one new written hop accepts one genuine scheduler insertion");
    const auto hopLimit = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    const auto nextHop = [&] {
        const auto result = lease.read();
        return result && previous && result->revision > previous->revision && result->pendingFrames == 0 &&
               broker.stats().analysisWindows == settled.analysisWindows + 1;
    };
    while (!nextHop() && std::chrono::steady_clock::now() < hopLimit) std::this_thread::yield();
    check(nextHop(), "genuine queued hop produces and completes a later publication");
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
    const auto closed = broker.stats().successfulEnqueues;
    broker.requestFrame();
    check(released == 1 && !lease && !lease.read() && broker.stats().successfulEnqueues == closed,
          "broker shutdown waits for work, releases its source and permits no later enqueue");
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
void clippedSampleWindows() {
    Fixture f;
    SharedAnalysis::Request request{1};
    request.fftSize = 32;
    request.hopSize = 8;
    request.outputs = SharedAnalysis::Meters;
    auto meter = f.broker.acquire(request);
    std::array<float, 32> left{}, right{};
    left[0] = 1.f;
    left[1] = -1.f;
    left[2] = std::numeric_limits<float>::quiet_NaN();
    left[8] = 1.125f;
    right[3] = -std::numeric_limits<float>::max();
    right[4] = std::numeric_limits<float>::infinity();
    right[6] = -std::numeric_limits<float>::infinity();
    right[7] = std::nextafter(1.f, 0.f);
    f.tap->write(left.data(), right.data(), 32, 0, true);
    check(f.broker.pump() == 0, "windowed Meters need no FFT");
    const auto first = meter.read();
    check(first && first->clippedSamples == 4 && first->nonFiniteSamples == 3 &&
              first->scopeLeft.empty() && first->scopeRight.empty(),
          "Meters count both finite full-scale polarities, including clamped extremes, without raw samples");

    std::array<float, 8> nextLeft{}, nextRight{};
    nextLeft[4] = -2.f;
    nextRight[5] = 1.f;
    f.tap->write(nextLeft.data(), nextRight.data(), 7, 32, true);
    f.broker.pump();
    check(first && meter.read() == first, "partial hop keeps the last complete clip count");
    request.outputs = SharedAnalysis::Scope;
    auto scope = f.broker.acquire(request);
    f.broker.pump();
    const auto same = scope.read();
    check(same && same == meter.read() && same->firstStamp == 0 &&
              same->clippedSamples == 4 && same->nonFiniteSamples == 3 && same->scopeLeft.size() == left.size(),
          "compatible raw demand shares the same completed meter window, excluding partial samples");

    f.tap->write(nextLeft.data() + 7, nextRight.data() + 7, 1, 39, true);
    f.broker.pump();
    const auto shifted = meter.read();
    check(shifted && shifted->firstStamp == 8 && shifted->clippedSamples == 3 && shifted->nonFiniteSamples == 0,
          "the next complete window drops expired samples and counts only the new hop");
}
void exactZeroSpectrum() {
    for (const auto size : {32, 96, 1024, 1536, 65536})
        for (const auto window : {adi::dsp::Window::Hann, adi::dsp::Window::Rectangular}) {
            adi::dsp::Spectrum spectrum(size, window);
            std::vector<float> frame(static_cast<std::size_t>(size), .5f);
            std::vector<float> bins(static_cast<std::size_t>(size / 2 + 1), -1.f);
            spectrum.analyse(frame, bins);
            for (std::size_t i = 0; i < frame.size(); ++i) frame[i] = (i & 1u) ? -0.f : 0.f;
            spectrum.analyse(frame, bins);
            check(spectrum.valid() && std::all_of(bins.begin(), bins.end(), [](float value) {
                      return std::bit_cast<std::uint32_t>(value) == 0;
                  }), "prepared FFT overwrites prior tone magnitudes with positive zero for either signed-zero input");
        }
}
void exactSilentStereoLanes() {
    for (const auto size : {32, 1024})
        for (const auto window : {adi::dsp::Window::Hann, adi::dsp::Window::Rectangular}) {
            Fixture f;
            SharedAnalysis::Request request{1, ScopePoint::PostFader, size, SharedAnalysis::Channel::StereoPower, window};
            request.hopSize = size; request.outputs = 31;
            auto lease = f.broker.acquire(request);
            adi::dsp::Spectrum reference(size, window);
            const auto n = static_cast<std::size_t>(size);
            std::vector<float> l(n), r(n), cleanL(n), cleanR(n), first(n / 2 + 1), second(first.size()), expected(first.size());
            for (int part = 0; part < 10; ++part) {
                const bool zeroL = part == 2 || part == 3 || part == 7 || part == 9;
                const bool zeroR = part == 1 || part == 3 || part == 6 || part == 9;
                std::size_t invalid = 0;
                for (std::size_t i = 0; i < n; ++i) {
                    const auto phase = 2 * std::numbers::pi * static_cast<double>(i) / size;
                    l[i] = zeroL ? ((i & 1u) ? -0.f : 0.f) : static_cast<float>(.5 * std::cos(3 * phase));
                    r[i] = zeroR ? ((i & 1u) ? -0.f : 0.f) : static_cast<float>(.25 * std::sin(5 * phase));
                    if (part == 4) l[i] = (i & 1u) ? std::numeric_limits<float>::infinity() : std::numeric_limits<float>::quiet_NaN();
                    if (part == 5) r[i] = (i & 1u) ? -std::numeric_limits<float>::infinity() : std::numeric_limits<float>::quiet_NaN();
                    if (part == 6) l[i] = std::numeric_limits<float>::denorm_min();
                    if (part == 7) r[i] = -std::numeric_limits<float>::denorm_min();
                    invalid += static_cast<std::size_t>(!std::isfinite(l[i])) + static_cast<std::size_t>(!std::isfinite(r[i]));
                    cleanL[i] = std::isfinite(l[i]) ? l[i] : 0.f;
                    cleanR[i] = std::isfinite(r[i]) ? r[i] : 0.f;
                }
                reference.analyse(cleanL, first); reference.analyse(cleanR, second);
                for (std::size_t i = 0; i < first.size(); ++i)
                    expected[i] = adi::dsp::toDbfs(std::sqrt((first[i] * first[i] + second[i] * second[i]) * .5f));
                const auto stamp = static_cast<std::int64_t>(part) * size;
                const auto before = f.broker.stats().transforms;
                f.tap->write(l.data(), r.data(), size, stamp, true);
                f.broker.pump();
                const auto result = lease.read();
                const auto wanted = part == 3 || part == 9 ? 0u : part == 0 || part == 8 ? 2u : 1u;
                check(f.broker.stats().transforms - before == wanted,
                      "stereo power transforms each exact-nonzero sanitized lane once, including subnormal lanes");
                const auto sameBits = [](float a, float b) { return std::bit_cast<std::uint32_t>(a) == std::bit_cast<std::uint32_t>(b); };
                check(result && result->binsDbfs.size() == expected.size() &&
                          std::equal(expected.begin(), expected.end(), result->binsDbfs.begin(), sameBits) &&
                          std::equal(cleanL.begin(), cleanL.end(), result->scopeLeft.begin(), result->scopeLeft.end(), sameBits) &&
                          std::equal(cleanR.begin(), cleanR.end(), result->scopeRight.begin(), result->scopeRight.end(), sameBits) &&
                          result->nonFiniteSamples == invalid && result->windowSerial == static_cast<std::uint64_t>(part + 1) &&
                          result->firstStamp == stamp && result->firstSequence == stamp && result->columns.size() == static_cast<std::size_t>(part + 1),
                      "lane skipping preserves exact two-FFT combination, sanitized scope, stamps and hop history after active/silent transitions");
            }
            const auto previous = lease.read(); const auto transforms = f.broker.stats().transforms;
            auto compact = f.broker.acquire(request);
            check(f.broker.pump() == 0 && compact.read() == previous && lease.read() == previous &&
                      f.broker.stats().transforms == transforms,
                  "compatible full/compact silence pictures share the same publication without redundant transforms");
        }
}
void sharedAverage() {
    Fixture f;
    SharedAnalysis::Request request{1}; request.hopSize = 1024;
    request.outputs = SharedAnalysis::Spectrum | SharedAnalysis::Spectrogram;
    auto raw = f.broker.acquire(request);
    request.outputs = SharedAnalysis::Average1s;
    auto average = f.broker.acquire(request);
    check(average && !average.readAverage() && !raw.readAverage(), "only demanded average has an optional sidecar, initially no invented history");
    tone(*f.tap); check(f.broker.pump() == 1, "Raw, Spectrogram and average share the same first FFT");
    const auto first = average.readAverage();
    check(first && first->status == SharedAnalysis::AverageStatus::Ready && first->processedWindows == 1 &&
              first->window == raw.read() && first->binsDbfs && f.broker.stats().domains == 1 && f.broker.stats().spectralPlans == 1,
          "average sidecar points to the atomically published Raw window and one shared plan");
    auto compact = f.broker.acquire(request);
    check(compact.readAverage() == first && f.broker.pump() == 0,
          "second average consumer reuses the identical sidecar without reset or old-window integration");
    auto combinedRequest=request; combinedRequest.outputs|=SharedAnalysis::Scope;
    auto combined=f.broker.acquire(combinedRequest);
    check(!combined.readAverage() && average.readAverage()==first && compact.readAverage()==first,
          "a combined lease refuses an older sidecar lacking newly requested fields while existing average readers retain their picture");
    f.broker.pump(); const auto complete=combined.readAverage();
    check(complete && complete->window==combined.read() && complete->window->scopeLeft.size()==1024 &&
              complete->processedWindows==1 && complete->binsDbfs==first->binsDbfs && f.broker.stats().transforms==1,
          "combined fields publish together on a metadata-only pass without extra FFT or average integration");
    combined.reset();
    adi::dsp::Spectrum spectrum(1024, adi::dsp::Window::Hann);
    adi::dsp::LinearMagnitudeAverage reference;
    reference.prepare({513,48000.,1024,1.});
    std::vector<float> input(1024), linear(513), expected(513);
    for (std::size_t i=0;i<input.size();++i) input[i]=.5f*static_cast<float>(std::sin(2*std::numbers::pi*16*static_cast<double>(i)/1024));
    spectrum.analyse(input,linear); reference.processChecked(linear);
    for(std::size_t i=0;i<expected.size();++i) expected[i]=adi::dsp::toDbfs(static_cast<float>(reference.magnitudes()[i]));
    check(first && first->binsDbfs && exactBins(*first->binsDbfs,expected), "first average is the exact primitive fed linear calibrated bins, before dB conversion");
    request.outputs=SharedAnalysis::Scope;
    auto scope=f.broker.acquire(request); f.broker.pump();
    const auto republished=average.readAverage();
    check(republished && republished!=first && republished->window==raw.read() && republished->window->windowSerial==1 &&
              republished->processedWindows==1 && republished->binsDbfs==first->binsDbfs && f.broker.stats().transforms==1,
          "output-only republication shares immutable average bins and never advances the numerical history");
    const std::vector<float> zero(1024);
    f.tap->write(zero.data(),zero.data(),1024,1024,true); f.broker.pump();
    std::fill(linear.begin(),linear.end(),0.f); reference.processChecked(linear);
    for(std::size_t i=0;i<expected.size();++i) expected[i]=adi::dsp::toDbfs(static_cast<float>(reference.magnitudes()[i]));
    const auto decayed=average.readAverage();
    check(decayed && decayed->processedWindows==2 && decayed->windowSerial==2 && decayed->binsDbfs &&
              exactBins(*decayed->binsDbfs,expected) && f.broker.stats().transforms==1,
          "a new silent hop advances the average decay even when no FFT call is needed");
    average.reset();
    tone(*f.tap,1024,.25f,1.f,2048); f.broker.pump();
    check(compact.readAverage() && compact.readAverage()->processedWindows==3 && first->processedWindows==1 &&
              first->binsDbfs && exactBins(*first->binsDbfs,*republished->binsDbfs),
          "one frozen/released reader does not reset another consumer and retained snapshots remain immutable");
    compact.reset();
    request.outputs=SharedAnalysis::Average1s;
    auto resumed=f.broker.acquire(request); f.broker.pump();
    check(resumed.readAverage() && resumed.readAverage()->status==SharedAnalysis::AverageStatus::Waiting &&
              resumed.readAverage()->processedWindows==0 && !resumed.readAverage()->binsDbfs,
          "last average demand removal frees its history; renewed demand starts zero and waits for a new hop");
    tone(*f.tap,1024,.125f,1.f,3072); f.broker.pump();
    check(resumed.readAverage() && resumed.readAverage()->processedWindows==1 && raw.read()->columns.size()==4,
          "renewed average and still-live Raw history have independent lifetimes");
}
void averageActivationAndOverflow() {
    Fixture f;
    SharedAnalysis::Request request{1};
    auto raw=f.broker.acquire(request);
    tone(*f.tap); f.broker.pump();
    request.outputs=SharedAnalysis::Average1s;
    auto average=f.broker.acquire(request); f.broker.pump();
    check(average.readAverage() && average.readAverage()->status==SharedAnalysis::AverageStatus::Waiting &&
              average.readAverage()->processedWindows==0 && f.broker.stats().transforms==1,
          "first average demand cannot integrate an already completed retained Raw window");
    average.reset();
    tone(*f.tap,4096,.25f,1.f,1024);
    average=f.broker.acquire(request); f.broker.pump();
    check(average.readAverage() && average.readAverage()->status==SharedAnalysis::AverageStatus::Waiting &&
              average.readAverage()->processedWindows==0 && raw.read()->windowSerial==17,
          "queued pre-activation Raw windows do not invent prior average history");
    tone(*f.tap,256,.25f,1.f,5120); f.broker.pump();
    check(average.readAverage() && average.readAverage()->status==SharedAnalysis::AverageStatus::Ready &&
              average.readAverage()->processedWindows==1 && average.readAverage()->windowSerial==18,
          "one genuinely post-activation hop starts the average once");
    request.channel=SharedAnalysis::Channel::Left;
    auto differentDomain=f.broker.acquire(request); f.broker.pump();
    check(differentDomain.readAverage() && differentDomain.readAverage()->status==SharedAnalysis::AverageStatus::Waiting &&
              differentDomain.readAverage()->processedWindows==0,
          "a new channel domain on an already-live tap cannot reinterpret retained pre-demand audio as a first average hop");
    tone(*f.tap,256,.25f,1.f,5376); f.broker.pump();
    check(differentDomain.readAverage() && differentDomain.readAverage()->processedWindows==1,
          "a newly configured average on a shared tap starts at its own post-demand sample boundary");
    auto left=SharedAnalysis::Request{2,ScopePoint::PostFader,1024,SharedAnalysis::Channel::Left};
    left.hopSize=1024; left.outputs=SharedAnalysis::Spectrum | SharedAnalysis::Average1s;
    Fixture overflow;
    auto lease=overflow.broker.acquire(left);
    std::vector<float> huge(1024,std::numeric_limits<float>::max()*.25f), zero(1024), modest(1024,.25f);
    overflow.tap->write(huge.data(),zero.data(),1024,0,true); overflow.broker.pump();
    auto rejected=lease.readAverage();
    check(rejected && rejected->status==SharedAnalysis::AverageStatus::InvalidInput && !rejected->binsDbfs &&
              rejected->processedWindows==0 && lease.read() && !lease.read()->binsDbfs.empty(),
          "finite input that overflows the float FFT refuses only average output, without poisoning or suppressing Raw");
    const auto epoch=rejected?rejected->epoch:0;
    overflow.tap->write(huge.data(),zero.data(),1024,1024,true); overflow.broker.pump();
    rejected=lease.readAverage();
    check(rejected && rejected->status==SharedAnalysis::AverageStatus::InvalidInput && rejected->epoch==epoch &&
              rejected->processedWindows==0, "an invalid run stays explicit without repeatedly resetting the average epoch");
    overflow.tap->write(modest.data(),zero.data(),1024,2048,true); overflow.broker.pump();
    const auto recovered=lease.readAverage();
    adi::dsp::Spectrum spectrum(1024,adi::dsp::Window::Hann); std::vector<float> bins(513), expected(513);
    spectrum.analyse(modest,bins); adi::dsp::LinearMagnitudeAverage reference;
    reference.prepare({513,48000.,1024,1.}); reference.processChecked(bins);
    for(std::size_t i=0;i<bins.size();++i) expected[i]=adi::dsp::toDbfs(static_cast<float>(reference.magnitudes()[i]));
    check(recovered && recovered->status==SharedAnalysis::AverageStatus::Ready && recovered->epoch==epoch &&
              recovered->processedWindows==1 && recovered->binsDbfs && exactBins(*recovered->binsDbfs,expected),
          "a valid completed window recovers deterministically from zero after an explicit invalid-data gap");
}
void averageChronologyAndEpochs() {
    constexpr int size=1024, hop=256, total=size+30*hop;
    std::vector<float> signal(total), right(total);
    for(std::size_t i=0;i<signal.size();++i) {
        signal[i]=static_cast<float>(.25*std::sin(.137*static_cast<double>(i))+.1*std::cos(.027*static_cast<double>(i)));
        right[i]=signal[i];
    }
    adi::dsp::Spectrum spectrum(size,adi::dsp::Window::Hann);
    adi::dsp::LinearMagnitudeAverage reference; reference.prepare({513,48000.,hop,1.});
    std::vector<float> linear(513),expected(513);
    for(int start=0;start+size<=total;start+=hop) {
        spectrum.analyse(std::span(signal).subspan(static_cast<std::size_t>(start),size),linear);
        reference.processChecked(linear);
    }
    for(std::size_t i=0;i<expected.size();++i) expected[i]=adi::dsp::toDbfs(static_cast<float>(reference.magnitudes()[i]));
    for(const int fps:{30,60,144}) for(const int chunk:{17,256,513}) {
        Fixture f; SharedAnalysis::Request request{1}; request.outputs=SharedAnalysis::Average1s;
        auto average=f.broker.acquire(request); int nextFrame=1;
        for(int offset=0;offset<total;) {
            const int count=std::min(chunk,total-offset);
            f.tap->write(signal.data()+offset,right.data()+offset,count,offset,true); offset+=count;
            const int frame=offset*fps/48000;
            if(frame>=nextFrame) {f.broker.pump();nextFrame=frame+1;}
        }
        f.broker.pump(); f.broker.pump();
        const auto result=average.readAverage();
        check(result && result->status==SharedAnalysis::AverageStatus::Ready && result->processedWindows==31 &&
                  result->windowSerial==31 && result->binsDbfs && exactBins(*result->binsDbfs,expected),
              "identical real hops are bit-identical at exact 30/60/144 Hz frame cadence and different audio callback splits");
        check(result && result->window->binsDbfs.empty() && f.broker.stats().transforms==31 &&
                  f.broker.stats().domains==1 && f.broker.stats().taps==1,
              "average-only demand uses one ordinary transform path without an unused Raw dB projection");
    }
    Fixture f; SharedAnalysis::Request request{1}; request.hopSize=1024; request.outputs=SharedAnalysis::Average1s;
    auto average=f.broker.acquire(request); tone(*f.tap); f.broker.pump(); const auto old=average.readAverage();
    f.tap=std::make_shared<ScopeTap>(96000.,4.); f.broker.refreshSources();
    check(!average.readAverage(), "source replacement atomically discards both old window and old average sidecar");
    tone(*f.tap); f.broker.pump(); const auto changed=average.readAverage();
    check(old && changed && changed->epoch>old->epoch && changed->processedWindows==1 &&
              changed->window->sampleRate==96000. && changed->window->generation!=old->window->generation,
          "owner rate/generation preparation starts a zero average epoch with the actual new sample rate");
    adi::dsp::LinearMagnitudeAverage rateReference; rateReference.prepare({513,96000.,1024,1.});
    std::vector<float> rateInput(1024);
    for(std::size_t i=0;i<rateInput.size();++i)
        rateInput[i]=.5f*static_cast<float>(std::sin(2*std::numbers::pi*16*static_cast<double>(i)/1024));
    spectrum.analyse(rateInput,linear); rateReference.processChecked(linear);
    for(std::size_t i=0;i<expected.size();++i) expected[i]=adi::dsp::toDbfs(static_cast<float>(rateReference.magnitudes()[i]));
    check(changed && changed->binsDbfs && exactBins(*changed->binsDbfs,expected),
          "new-rate average values use the exact 96 kHz hop coefficient, not merely new rate metadata");
    tone(*f.tap,1024,.5f,1.f,50000); f.broker.pump(); const auto gap=average.readAverage();
    check(gap && changed && gap->epoch>changed->epoch && gap->processedWindows==1 && gap->window->gapCount==1 &&
              gap->binsDbfs && exactBins(*gap->binsDbfs,*changed->binsDbfs),
          "timeline gaps reset only average numerical history and accept the next complete contiguous window");
    const auto frozen=gap; f.broker.shutdown();
    check(!average && !average.readAverage() && frozen && frozen->binsDbfs && old->processedWindows==1,
          "shutdown revokes live sidecars while retained immutable snapshots stay valid");
    std::shared_ptr<ScopeTap> late;
    SharedAnalysis pending([&](std::int64_t,ScopePoint){return late;},[](std::int64_t,ScopePoint){},SharedAnalysis::Mode::Manual);
    auto waiting=pending.acquire(request);
    check(waiting && !waiting.available() && pending.pump()==0, "unavailable average demand starts no worker analysis or guessed-rate history");
    late=std::make_shared<ScopeTap>(96000.,4.); pending.refreshSources(); tone(*late); pending.pump();
    check(waiting.readAverage() && waiting.readAverage()->processedWindows==1 && waiting.readAverage()->window->sampleRate==96000.,
          "late source availability prepares the average on the owner at its actual rate");
    const auto retained=std::make_shared<ScopeTap>(96000.,4.); int calls=0;
    SharedAnalysis negotiating([&](adi::engine::ScopeSource,double){
        if(++calls==2) tone(*retained,65536);
        return retained;
    },[](adi::engine::ScopeSource){},SharedAnalysis::Mode::Manual);
    request.fftSize=65536; request.hopSize=0;
    auto onset=negotiating.acquire(request); negotiating.pump();
    check(calls==2 && onset.readAverage() && onset.readAverage()->processedWindows==1 &&
              onset.readAverage()->window->firstSequence==0 && onset.readAverage()->window->sampleRate==96000.,
          "first average activation preserves audio captured during actual-rate history negotiation");
    const auto small=std::make_shared<ScopeTap>(48000.,1.), large=std::make_shared<ScopeTap>(96000.,4.);
    int replacements=0;
    SharedAnalysis resizing([&](adi::engine::ScopeSource,double seconds){
        if(seconds>1.) {if(++replacements==2) tone(*large,65536);return large;}
        return small;
    },[](adi::engine::ScopeSource){},SharedAnalysis::Mode::Manual);
    auto existing=resizing.acquire({1}); tone(*small); resizing.pump();
    auto replacing=resizing.acquire(request);
    for(int pass=0;pass<4 && (!replacing.readAverage() || replacing.readAverage()->status!=SharedAnalysis::AverageStatus::Ready);++pass)
        resizing.pump();
    check(replacements==2 && replacing.readAverage() && replacing.readAverage()->processedWindows==1 &&
              replacing.readAverage()->window->firstSequence==0 && replacing.readAverage()->window->sampleRate==96000.,
          "physical source replacement during a new-domain negotiation preserves newly activated capture onset on an existing tap key");
    Fixture overrun; overrun.tap=std::make_shared<ScopeTap>(48000.,.1);
    request.fftSize=1024; request.hopSize=256;
    auto afterOverrun=overrun.broker.acquire(request); tone(*overrun.tap); overrun.broker.pump();
    const auto beforeOverrun=afterOverrun.readAverage();
    tone(*overrun.tap,10000,.25f,1.f,1024); overrun.broker.pump();
    check(beforeOverrun && afterOverrun.readAverage() && afterOverrun.readAverage()->epoch>beforeOverrun->epoch &&
              afterOverrun.readAverage()->window->overrunCount==1 && afterOverrun.readAverage()->processedWindows>0 &&
              afterOverrun.readAverage()->processedWindows<=SharedAnalysis::kMaxWindowsPerFrame,
          "ring overrun starts a bounded fresh average epoch rather than bridging discarded source history");
}
void averagePreparationFailure() {
    for(int fault:{0,1}) {
        Fixture f; SharedAnalysis::Request request{1}; request.hopSize=1024;
        auto raw=f.broker.acquire(request); tone(*f.tap); f.broker.pump();
        request.outputs=SharedAnalysis::Average1s;
        failAllocationAfter=fault; auto average=f.broker.acquire(request); failAllocationAfter=-1;
        f.broker.pump(); const auto refused=average.readAverage();
        check(average && raw.available() && refused && refused->status==SharedAnalysis::AverageStatus::Unavailable &&
                  !refused->binsDbfs && refused->window==raw.read(),
              "average object/array preparation failures leave a live Raw subscriber and explicit unavailable sidecar");
        raw.reset(); const auto before=f.broker.stats(); tone(*f.tap,1024,.25f,1.f,1024); f.broker.pump();
        check(!average.available() && f.broker.stats().workerPasses==before.workerPasses &&
                  f.broker.stats().transforms==before.transforms,
              "unprepared average-only demand does no hidden window/FFT work or worker allocation retries");
        request.outputs=SharedAnalysis::Spectrum; raw=f.broker.acquire(request); f.broker.pump();
        check(raw.available() && raw.read() && near(raw.read()->binsDbfs[16],-12.041199827) &&
                  average.readAverage() && average.readAverage()->status==SharedAnalysis::AverageStatus::Unavailable,
              "a new Raw subscriber reactivates its source independently of an unavailable average");
        average.reset(); request.outputs=SharedAnalysis::Average1s; average=f.broker.acquire(request); f.broker.pump();
        check(average.readAverage() && average.readAverage()->status==SharedAnalysis::AverageStatus::Waiting &&
                  average.readAverage()->processedWindows==0, "renewed owner demand can prepare after allocation refusal without integrating old Raw data");
        tone(*f.tap,1024,.125f,1.f,2048); f.broker.pump();
        check(average.readAverage() && average.readAverage()->status==SharedAnalysis::AverageStatus::Ready &&
                  average.readAverage()->processedWindows==1, "fresh audio recovers an explicitly retried average from zero");
    }
}
void averagePendingBoundaries() {
    for(const int queued:{0,1,2}) {
        Fixture f;
        if(queued==2) f.tap=std::make_shared<ScopeTap>(48000.,.1);
        SharedAnalysis::Request request{1}; request.outputs=SharedAnalysis::Scope;
        auto temporal=f.broker.acquire(request); tone(*f.tap); f.broker.pump();
        std::int64_t nextStamp=1024;
        if(queued==1) {tone(*f.tap,1024,.25f,1.f,50000);nextStamp=51024;}
        if(queued==2) {tone(*f.tap,10000,.25f,1.f,1024);nextStamp=11024;}
        request.outputs=SharedAnalysis::Average1s;
        auto average=f.broker.acquire(request); f.broker.pump();
        const auto waiting=average.readAverage();
        check(waiting && waiting->status==SharedAnalysis::AverageStatus::Waiting && waiting->processedWindows==0 &&
                  !waiting->binsDbfs && f.broker.stats().transforms==0,
              "temporal-only cached/gapped/overrun backlog preserves the first-average sample fence and performs no unused old-window FFT");
        tone(*f.tap,256,.25f,1.f,nextStamp); f.broker.pump();
        const auto fresh=average.readAverage();
        check(fresh && fresh->status==SharedAnalysis::AverageStatus::Ready && fresh->processedWindows==1 &&
                  f.broker.stats().transforms==1,
              "first real post-demand hop after cached/gapped/overrun backlog performs exactly one average FFT/update");
    }
}
void averageHistoryAndSanitation() {
    const auto tap=std::make_shared<ScopeTap>(48000.,1.);
    SharedAnalysis broker([&](std::int64_t,ScopePoint){return tap;},[](std::int64_t,ScopePoint){},
                          SharedAnalysis::Mode::Manual,3*513*sizeof(float));
    SharedAnalysis::Request request{1}; request.outputs=SharedAnalysis::Spectrogram|SharedAnalysis::Average1s;
    auto lease=broker.acquire(request);
    tone(*tap); broker.pump();
    for(int part=0;part<3;++part){tone(*tap,256,.5f,1.f,1024+part*256);broker.pump();}
    const auto blocked=lease.readAverage(); const auto work=broker.stats();
    check(blocked && blocked->status==SharedAnalysis::AverageStatus::Ready && blocked->processedWindows==4 &&
              blocked->window->historyLimited && blocked->window->columns.size()==2,
          "average real-hop history advances independently of a capped immutable Spectrogram publication");
    broker.pump(); const auto retried=lease.readAverage();
    check(retried && blocked && !retried->window->historyLimited && retried->window->columns.size()==3 &&
              retried->processedWindows==blocked->processedWindows && retried->epoch==blocked->epoch &&
              retried->binsDbfs==blocked->binsDbfs && broker.stats().transforms==work.transforms &&
              broker.stats().analysisWindows==work.analysisWindows,
          "history-only retry reuses average bins and numerical identity with no FFT or average update");
    Fixture f; request.hopSize=1024; request.outputs=SharedAnalysis::Spectrum|SharedAnalysis::Scope|SharedAnalysis::Average1s;
    auto average=f.broker.acquire(request); tone(*f.tap); f.broker.pump();
    adi::dsp::Spectrum spectrum(1024,adi::dsp::Window::Hann);
    adi::dsp::LinearMagnitudeAverage reference; reference.prepare({513,48000.,1024,1.});
    std::vector<float> input(1024),linear(513),expected(513);
    for(std::size_t i=0;i<input.size();++i) input[i]=.5f*static_cast<float>(std::sin(2*std::numbers::pi*16*static_cast<double>(i)/1024));
    spectrum.analyse(input,linear); reference.processChecked(linear);
    std::fill(input.begin(),input.end(),0.f); input[0]=std::numeric_limits<float>::quiet_NaN();
    input[1]=std::numeric_limits<float>::infinity(); input[2]=-std::numeric_limits<float>::infinity();
    f.tap->write(input.data(),input.data(),1024,1024,true); f.broker.pump();
    std::fill(linear.begin(),linear.end(),0.f); reference.processChecked(linear);
    for(std::size_t i=0;i<expected.size();++i) expected[i]=adi::dsp::toDbfs(static_cast<float>(reference.magnitudes()[i]));
    auto result=average.readAverage();
    check(result && result->status==SharedAnalysis::AverageStatus::Ready && result->processedWindows==2 &&
              result->window->nonFiniteSamples==6 && result->window->scopeLeft[0]==0.f &&
              result->binsDbfs && exactBins(*result->binsDbfs,expected) && f.broker.stats().transforms==1 && std::isnan(input[0]),
          "source NaN/Inf sanitation supplies a valid zero hop and decay without poisoning average state or modifying input");
    std::fill(input.begin(),input.end(),std::numeric_limits<float>::denorm_min());
    f.tap->write(input.data(),input.data(),1024,2048,true); f.broker.pump();
    spectrum.analyse(input,linear); reference.processChecked(linear);
    for(std::size_t i=0;i<expected.size();++i) expected[i]=adi::dsp::toDbfs(static_cast<float>(reference.magnitudes()[i]));
    result=average.readAverage();
    check(result && result->status==SharedAnalysis::AverageStatus::Ready && result->processedWindows==3 &&
              result->binsDbfs && exactBins(*result->binsDbfs,expected) && result->window->nonFiniteSamples==0,
          "smallest finite subnormal source input follows the existing FFT and precise average law without a new silence floor");
}
void averagePublicationFailure() {
    std::array<std::size_t,2> sizes{};
    for(int kind=0;kind<2;++kind) {
        allocationSizeCount=0; observeAllocationSizes=true;
        if(kind==0) {auto marker=std::make_shared<SharedAnalysis::AverageResult>(); observeAllocationSizes=false;}
        else {auto marker=std::make_shared<std::vector<float>>(); observeAllocationSizes=false;}
        sizes[static_cast<std::size_t>(kind)]=allocationSizeCount?allocationSizes[0]:0;
    }
    check(sizes[0] && sizes[1] && sizes[0]!=sizes[1], "failure injection calibrates opaque standard-library sidecar and bin-owner allocation sizes");
    for(const auto size:sizes) {
        Fixture f; SharedAnalysis::Request request{1}; request.hopSize=1024;
        request.outputs=SharedAnalysis::Spectrum|SharedAnalysis::Average1s;
        auto lease=f.broker.acquire(request); tone(*f.tap); f.broker.pump();
        tone(*f.tap,1024,.25f,1.f,1024);
        refuseAllocationBytes=size; refusedAllocationSize=false;
        f.broker.pump(); refuseAllocationBytes=0;
        check(refusedAllocationSize && lease.read() && lease.read()->windowSerial==2 && !lease.readAverage() &&
                  near(lease.read()->binsDbfs[16],-12.041199827),
              "optional sidecar/bin publication allocation refusal still commits the complete Raw window");
        const auto stopped=f.broker.stats(); f.broker.pump();
        check(!lease.readAverage() && f.broker.stats().workerPasses==stopped.workerPasses &&
                  f.broker.stats().publications==stopped.publications,
              "publication allocation refusal does not create an idle worker allocation retry loop");
        request.outputs=SharedAnalysis::Scope; auto scope=f.broker.acquire(request); f.broker.pump();
        const auto recovered=lease.readAverage();
        check(recovered && recovered->status==SharedAnalysis::AverageStatus::Ready && recovered->processedWindows==2 &&
                  recovered->windowSerial==2 && recovered->window==lease.read() && recovered->binsDbfs &&
                  f.broker.stats().transforms==stopped.transforms && f.broker.stats().analysisWindows==stopped.analysisWindows,
              "a genuine metadata event retries optional publication without replaying FFT or numerical average hops");
    }
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
    for (int i = 0; i < 100; ++i) broker.pump();
    check(broker.stats().workerPasses == held.workerPasses && broker.stats().publications == held.publications &&
          broker.stats().analysisWindows == held.analysisWindows && broker.stats().transforms == held.transforms,
          "a frozen allocation cap does not start any worker passes before bytes are released");
    frozen.reset();
    broker.pump();
    check(!lease.read()->historyLimited && lease.read()->columns.back().serial == 6 &&
          lease.read()->columns.front().serial == 4 && lease.read()->columns.size() == 3 &&
          lease.read()->columns[1].serial == 5 &&
          lease.read()->droppedHistoryColumns == 3 && broker.stats().historyBytes == 3 * columnBytes,
          "releasing frozen backing publishes all staged hops in order");
    auto retained = lease.read(); lease.reset();
    check(broker.stats().historyBytes == 3 * columnBytes && broker.stats().spectralPlans == 0,
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
void fullHistoryBursts() {
    const auto tap = std::make_shared<ScopeTap>(48000, 1.);
    constexpr std::size_t columnBytes = 513 * sizeof(float), capacity = 8 * columnBytes;
    SharedAnalysis broker([&](std::int64_t, ScopePoint) { return tap; }, [](std::int64_t, ScopePoint) {},
                          SharedAnalysis::Mode::Manual, capacity);
    SharedAnalysis::Request request{1};
    request.outputs = SharedAnalysis::Spectrogram | SharedAnalysis::Average1s;
    const auto lease = broker.acquire(request);
    tone(*tap, 1024 + 7 * 256);
    broker.pump();
    auto displayed = lease.read();
    check(displayed && displayed->columns.size() == 8 && broker.stats().historyBytes == capacity,
          "eight hops occupy the immutable budget");
    tone(*tap, 4 * 256, .4f, 1.f, tap->written());
    broker.pump();
    check(displayed && displayed->columns.size() == 8 && displayed->columns.front().serial == 1,
          "a view that still owns the preceding frame keeps its immutable history");
    displayed = lease.read();
    tone(*tap, 4 * 256, .3f, 1.f, tap->written());
    broker.pump();
    displayed = lease.read();
    broker.pump();
    auto newest = lease.read();
    bool continuous = newest && newest->columns.size() == 8 && newest->historySerial == 16;
    if (continuous)
        for (std::size_t i = 0; i < 8; ++i)
            continuous = continuous && newest->columns[i].serial == 9 + i &&
                         newest->columns[i].firstStamp == static_cast<std::int64_t>((8 + i) * 256);
    check(continuous && !newest->historyLimited && newest->droppedHistoryColumns == 8 &&
              broker.stats().historyPeakBytes == capacity && lease.readAverage()->processedWindows == 16,
          "successive four-hop bursts survive an exact full cap, in order and within the immutable limit");
    displayed.reset();
    tone(*tap, 1024, .2f, 1.f, tap->written() + 48000);
    broker.pump();
    const auto located = lease.read();
    check(located && located->sourceEpoch != newest->sourceEpoch && located->gapCount == newest->gapCount + 1,
          "a locate explicitly separates the two history epochs");
    newest.reset();
    broker.pump();
    check(lease.read() && !lease.read()->columns.empty() && lease.read()->columns.front().firstStamp >= 48000,
          "staged history cannot cross a real discontinuity");
}
void finiteExtremeSpectrum() {
    using Channel = SharedAnalysis::Channel;
    for (const auto channel : {Channel::Mid, Channel::Side, Channel::Left, Channel::StereoPower}) {
        const auto size = channel == Channel::StereoPower ? SharedAnalysis::kMaxFftSize : 1024;
        Fixture f;
        SharedAnalysis::Request request{1, ScopePoint::PostFader, size, channel};
        request.outputs = SharedAnalysis::Spectrum | SharedAnalysis::Spectrogram | SharedAnalysis::Average1s;
        const auto lease = f.broker.acquire(request);
        auto peak = channel == Channel::Mid || channel == Channel::Side ? std::numeric_limits<float>::max() : 1.e20f;
        std::vector<float> left(static_cast<std::size_t>(size)), right(left.size());
        for (int i = 0; i < size; ++i) {
            const auto at = static_cast<std::size_t>(i);
            left[at] = peak * static_cast<float>(std::cos(2 * std::numbers::pi * 16 * i / size));
            right[at] = channel == Channel::Side ? -left[at] : left[at];
        }
        f.tap->write(left.data(), right.data(), size, 0, true);
        f.broker.pump();
        const auto result = lease.read();
        const auto finite = [](const auto &bins) { return !bins.empty() &&
            std::all_of(bins.begin(), bins.end(), [](float bin) { return std::isfinite(bin); }); };
        check(result && finite(result->binsDbfs) && !result->columns.empty() &&
                  finite(*result->columns.back().binsDbfs) && result->nonFiniteSamples == 0 &&
                  lease.readAverage()->status == SharedAnalysis::AverageStatus::InvalidInput,
              "finite extreme input cannot overflow any published spectrum or accepted average");
    }
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

void stereoMeterSharingAndIntervals() {
    using Meter = adi::dsp::StereoEnvelopeMeter;
    Fixture f;
    auto meter = f.broker.acquireStereoMeter(1, {});
    auto mirror = f.broker.acquireStereoMeter(1, {});
    auto midSide = f.broker.acquireStereoMeter(1, {true, Meter::Weighting::C});
    const auto start = f.broker.stats();
    check(start.consumers == 3 && start.domains == 2 && start.taps == 1 && start.spectralPlans == 0 && f.acquireCalls == 1,
          "equal meter settings share one reading, and both profiles share one physical tap without FFT");
    std::array<float, 641> left{}, right{};
    for (std::size_t i = 0; i < left.size(); ++i) {
        left[i] = .6f * static_cast<float>(std::sin(.17 * static_cast<double>(i)));
        right[i] = -.5f * left[i];
    }
    Meter scalar;
    check(scalar.prepare({48000., .6, .5, .5, 1.e-6}) && scalar.process(left, right),
          "scalar reference accepts one interval");
    const auto feed = [&](std::size_t offset, std::size_t count) {
        f.tap->write(left.data() + offset, right.data() + offset, static_cast<std::int32_t>(count),
                     static_cast<std::int64_t>(offset), true);
        f.broker.pump();
    };
    feed(0, 97);
    feed(97, left.size() - 97);
    const auto lr = meter.readStereoMeter(), ms = midSide.readStereoMeter();
    check(lr && lr == mirror.readStereoMeter() && lr->reading == scalar.reading() && lr->reading.signCoincidence < 0 &&
              ms && ms->reading.maximumPeak[0] < ms->reading.maximumPeak[1] && f.broker.stats().transforms == 0,
          "meter uses each new sample once and publishes identical readings across split callbacks");
    check(meter.restartStereoMeter() && !mirror.readStereoMeter() && midSide.readStereoMeter() == ms,
          "reset invalidates the common LR interval and leaves the other profile untouched");
    check(f.broker.pump() == 0 && !meter.readStereoMeter(), "discarded interval never replays old samples");
    tone(*f.tap, 1024, .08f, 1.f, static_cast<std::int64_t>(left.size()));
    f.broker.pump();
    const auto after = meter.readStereoMeter();
    check(after && lr && after->epoch > lr->epoch && near(after->reading.maximumPeak[0], .08),
          "the new interval holds only newly captured peaks");
    meter.reset(); mirror.reset(); midSide.reset();
    check(f.broker.stats().consumers == 0 && f.broker.stats().taps == 0 && f.releaseCalls == 1,
          "last meter consumer releases the physical tap");
}

void stereoMeterTransportContinuity() {
    using Meter = adi::dsp::StereoEnvelopeMeter;
    Fixture f;
    const auto view = f.broker.acquireStereoMeter(1, {});
    SharedAnalysis::Request request{1};
    request.fftSize = request.hopSize = 256;
    request.outputs = SharedAnalysis::Spectrum;
    const auto spectrum = f.broker.acquire(request);
    constexpr int frames = 480;
    adi::engine::Transport transport;
    transport.loop(0, frames);
    transport.play();
    std::array<float, frames> left{}, right{}, silent{};
    Meter direct;
    check(direct.prepare({48000., .6, .5, .5, 1.e-6}), "continuous stereo-meter oracle prepares");
    const auto step = [&](float l, float r, bool parkedSilence = false) {
        left.fill(l); right.fill(r);
        const auto &oracleLeft = parkedSilence ? silent : left;
        const auto &oracleRight = parkedSilence ? silent : right;
        f.tap->write(parkedSilence ? nullptr : left.data(), parkedSilence ? nullptr : right.data(),
                     frames, transport.position(), transport.playing(), transport.revision());
        transport.advance(frames);
        f.broker.pump();
        (void)direct.process(oracleLeft, oracleRight);
        return view.readStereoMeter();
    };
    const auto first = step(.9f, .45f);
    const auto initialSpectrum = spectrum.read();
    const auto wrapped = step(.1f, -.05f);
    check(first && wrapped && wrapped->epoch == first->epoch && wrapped->reading == direct.reading() &&
              wrapped->reading.maximumPeak[0] == .9f && wrapped->reading.peakHold[0] == .9f,
          "the same-revision loop wrap preserves exact continuous envelope and held peaks");
    check(initialSpectrum && spectrum.read() && spectrum.read()->sourceEpoch > initialSpectrum->sourceEpoch,
          "a spectrum sharing the source starts a new epoch at the loop wrap");

    transport.play(false);
    const auto stopped = step(0.f, 0.f, true);
    check(wrapped && stopped && stopped->epoch == wrapped->epoch && stopped->reading == direct.reading() &&
              stopped->reading.maximumPeak[0] == .9f && std::isfinite(stopped->reading.peakHold[0]) &&
              stopped->reading.peak[0] < wrapped->reading.peak[0],
          "Stop decays the envelope continuously and preserves its interval maximum and holds");
    transport.play();
    const auto restarted = step(.2f, .1f);
    check(stopped && restarted && restarted->epoch == stopped->epoch && restarted->reading == direct.reading() &&
              restarted->reading.maximumPeak[0] == .9f,
          "Play maintains the measurement interval across the engine's advancing revision change");

    transport.locate(0);
    direct.reset();
    const auto sought = step(.125f, .0625f);
    check(restarted && sought && sought->epoch > restarted->epoch && sought->reading == direct.reading() &&
              sought->reading.maximumPeak[0] == .125f,
          "a same-stamp seek clears highs from the prior interval");
    transport.play(false);
    transport.locate(0);
    direct.reset();
    const auto skippedSeek = step(0.f, 0.f, true);
    check(sought && skippedSeek && skippedSeek->epoch > sought->epoch &&
              skippedSeek->reading == Meter::Reading{},
          "an advancing flip cannot mask a seek also issued before the next audio block");
}

void parkedStereoMeter() {
    using Meter = adi::dsp::StereoEnvelopeMeter;
    Fixture f;
    auto view = f.broker.acquireStereoMeter(1, {false, Meter::Weighting::A});
    const auto quiet = [&](std::uint64_t revision) {
        f.tap->write(static_cast<const float *>(nullptr), nullptr, 480, 0, false, revision);
        f.broker.pump();
    };
    quiet(1);
    const auto parked = view.readStereoMeter();
    const auto before = f.broker.stats();
    for (int block = 0; block < 1100; ++block) quiet(1);
    const auto after = f.broker.stats();
    check(parked && parked->reading == Meter::Reading{} && view.readStereoMeter() == parked &&
          after.workerPasses == before.workerPasses && after.cursorReads == before.cursorReads &&
          after.publications == before.publications,
          "empty stereo meter does no analysis across a parked ring wrap");
    const std::array<float, 480> left = [] { std::array<float, 480> values{}; values.fill(.1f); return values; }();
    Meter direct;
    const auto ready = direct.prepare({48000., .6, .5, .5, 1.e-6, false, Meter::Weighting::A}) &&
                       direct.process(left, left);
    f.tap->write(left.data(), left.data(), static_cast<int>(left.size()), 0, false, 1);
    f.broker.pump();
    const auto sounding = view.readStereoMeter();
    check(ready && sounding && parked && sounding->epoch == parked->epoch && sounding->reading == direct.reading(),
          "a parked meter picks up live monitoring at the first audible sample without a false gap");
    quiet(1);
    const auto settled = view.readStereoMeter();
    check(settled && sounding && settled->reading.peak[0] < sounding->reading.peak[0] &&
          settled->reading.maximumPeak == sounding->reading.maximumPeak,
          "a non-neutral meter still decays and keeps its stored maximum");
    check(view.restartStereoMeter(), "reset starts a neutral interval");
    quiet(1);
    const auto oldInterval = view.readStereoMeter();
    quiet(2);
    const auto afterSeek = view.readStereoMeter();
    check(oldInterval && afterSeek && afterSeek->epoch > oldInterval->epoch &&
          afterSeek->reading == Meter::Reading{},
          "a new parked source epoch invalidates the old interval");
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

void continuousCapture() {
    using Scope = adi::dsp::TriggeredScope;
    for (auto trigger : {Scope::Trigger::Free, Scope::Trigger::Rising, Scope::Trigger::Falling, Scope::Trigger::Internal}) {
        Fixture f;
        SharedAnalysis::CaptureRequest req;
        req.source = {1, ScopePoint::PostFader};
        req.settings.trigger = trigger;
        req.settings.columns = 29;
        req.settings.columnsPerSample = .19;
        req.settings.internalCyclesPerSample = .009;
        req.settings.removeDc = true;
        req.settings.holdoffSamples = 17;
        auto main = f.broker.acquireCapture(req);
        req.settings.sampleRate = 96000.;
        auto mini = f.broker.acquireCapture(req);
        req.channel = SharedAnalysis::Channel::Right;
        auto other = f.broker.acquireCapture(req);
        check(f.broker.stats().taps == 1 && f.broker.stats().domains == 2 && f.broker.stats().consumers == 3,
              "capture readers share physical source and identical normalized profile");
        Scope oracleL, oracleR;
        req.settings.sampleRate = 48000.;
        check(oracleL.prepare(req.settings) && oracleR.prepare(req.settings), "capture oracle prepared");
        std::int64_t sequence = 0;
        for (int n : {1, 17, 127, 128, 129, 256, 511, 4307}) {
            std::vector<float> l(static_cast<std::size_t>(n)), r(l.size());
            for (int j = 0; j < n; ++j) {
                const auto i = static_cast<std::size_t>(j);
                l[i] = float((sequence + j) % 193 - 96) / 100.f;
                r[i] = .5f - float((sequence + j) % 43) / 42.f;
            }
            r.front() = std::numeric_limits<float>::quiet_NaN();
            f.tap->write(l.data(), r.data(), n, 0, false);
            r.front() = 0;
            const bool inputOk = oracleL.processChecked(l, sequence) && oracleR.processChecked(r, sequence);
            sequence += n;
            const auto fft = f.broker.pump();
            auto matches = [&](const std::shared_ptr<const SharedAnalysis::CaptureResult> &snap, const Scope &oracle) {
                const auto a = oracle.current(), b = oracle.previous();
                return snap && snap->sampleRate == 48000. && snap->currentSerial == a.serial &&
                       snap->previousSerial == b.serial && snap->currentTriggerSequence == a.triggerSequence &&
                       snap->previousTriggerSequence == b.triggerSequence &&
                       std::ranges::equal(snap->current, a.columns) && std::ranges::equal(snap->previous, b.columns);
            };
            check(inputOk && fft == 0 && matches(main.read(), oracleL) && matches(other.read(), oracleR) &&
                  mini.read() == main.read(), "continuous parked input matches both direct capture streams across reads");
        }
        const auto retained = main.read();
        if (!retained) {
            check(false, "continuous capture produced a publication");
            continue;
        }
        const auto copied = retained->current;
        const auto idle = f.broker.stats();
        f.broker.pump();
        check(f.broker.stats().cursorReads == idle.cursorReads && idle.spectralPlans == 0 && idle.transforms == 0,
              "idle capture has no cursor reads and scope-only has no FFT plan");
        f.broker.refreshSources(true);
        check(!main.read() && main.sourcePosition().written == sequence, "seek invalidates both retained sweeps");
        std::array<float, 3> pulse{.3f, -.6f, .7f};
        f.tap->write(pulse.data(), nullptr, 3, 0, false, 2);
        f.broker.pump();
        const auto after = main.read();
        check(after && !after->current.empty() && after->sourceEpoch != retained->sourceEpoch &&
              after->transportRevision == 2 && after->current.front().firstSequence >= sequence &&
              retained->current == copied, "fresh result never joins old timeline and borrowed GUI pictures remain immutable");
        mini.reset(); other.reset(); main.reset();
        check(f.broker.stats().consumers == 0 && f.releaseCalls == 1, "last capture detaches exactly once");
    }
}
void captureBoundaries() {
    using Channel = SharedAnalysis::Channel;
    std::shared_ptr<ScopeTap> tap;
    int release = 0;
    bool refuse = true;
    SharedAnalysis *owner = nullptr;
    SharedAnalysis broker([&](adi::engine::ScopeSource, double) -> std::shared_ptr<const ScopeTap> {
        if (refuse) throw std::bad_alloc();
        return tap;
    }, [&](adi::engine::ScopeSource) {
        ++release;
        if (refuse) check(owner->stats().taps == 0, "failed attach is gone before a reentrant release callback");
    }, SharedAnalysis::Mode::Manual);
    owner = &broker;
    SharedAnalysis::CaptureRequest req;
    req.source = {22, ScopePoint::DeviceInput, 31}; req.settings.columns = 87; req.settings.columnsPerSample = .75;
    bool refused = false;
    try { (void)broker.acquireCapture(req); } catch (const std::bad_alloc &) { refused = true; }
    check(refused && release == 1 && broker.stats().consumers == 0 && broker.stats().taps == 0 &&
          broker.stats().domains == 0, "failed capture attach leaves no pending demand");
    refuse = false;
    auto capture = broker.acquireCapture(req);
    check(capture && !capture.available() && !capture.read(), "missing device source is pending");
    tap = std::make_shared<ScopeTap>(44100., .04);
    tone(*tap, 64); broker.refreshSources();
    check(capture.available() && capture.sourcePosition().written == 64 && !capture.read(), "late source fences old samples");
    tone(*tap, 240, .25f, 1.f, 64);
    broker.pump();
    const auto first = capture.read();
    check(first && !first->previous.empty() && first->sampleRate == 44100. && first->previous.front().firstSequence >= 64,
          "scope uses negotiated physical rate and fresh history");
    tone(*tap, 4096, .1f, 1.f, 304);
    broker.pump();
    const auto over = capture.read();
    check(first && over && !over->previous.empty() && over->overrunCount == 1 && over->sourceEpoch != first->sourceEpoch &&
          over->previous.front().firstSequence >= tap->oldest(), "overrun starts new trigger epoch at first retained data");
    tone(*tap, 5, .2f, 1.f, 90000);
    broker.pump();
    const auto gap = capture.read();
    check(over && gap && !gap->current.empty() && gap->gapCount > over->gapCount &&
          gap->previous.empty() && gap->current.front().firstSequence == 4400,
          "valid first payload of discontinuity is captured");
    tap = std::make_shared<ScopeTap>(96000., .25);
    broker.refreshSources();
    check(!capture.read(), "new physical source revokes old publication");
    auto raw = broker.acquire({22, ScopePoint::DeviceInput, 1024, Channel::Mid, adi::dsp::Window::Hann, 31});
    tone(*tap, 1024);
    const auto transforms = broker.pump();
    check(transforms == 1 && raw.read() && capture.read() && capture.read()->sampleRate == 96000. &&
          broker.stats().taps == 1 && broker.stats().spectralPlans == 1,
          "scope and spectrum keep independent numerics on one tap");
    raw.reset();
    check(capture.available() && broker.stats().spectralPlans == 0, "closing spectrum does not stop scope capture");
    tone(*tap, 7, .3f, 1.f, 1024);
    const auto good = capture.read();
    failAllocationAfter = 0;
    broker.pump();
    failAllocationAfter = -1;
    check(capture.read() == good, "publication allocation refusal retains last good picture");
    tone(*tap, 16, .2f, 1.f, 1031);
    broker.pump();
    check(good && capture.read() && capture.read()->revision > good->revision, "new data retries after publication failure");
    auto invalid = req;
    invalid.settings.gain = std::numeric_limits<float>::quiet_NaN();
    check(!broker.acquireCapture(invalid), "nonfinite acquisition settings refused before source attach");
    invalid = req; invalid.channel = Channel::Mid;
    check(!broker.acquireCapture(invalid), "unsupported lanes are refused explicitly");
    capture = broker.acquireCapture(req);
    check(broker.stats().consumers == 1, "capture move assignment releases previous reader");
    broker.shutdown();
    check(!capture && !capture.read() && release == 2, "shutdown revokes pending releases while retained result survives");
}

void captureSharesLiveTap() {
    Fixture f;
    SharedAnalysis::CaptureRequest request;
    request.source = {1, ScopePoint::PostFader};
    request.settings.columns = 16;
    request.settings.columnsPerSample = .5;
    auto capture = f.broker.acquireCapture(request);
    auto peaks = f.broker.acquireStereoMeter(1, {});
    auto spectrum = f.broker.acquire({1});
    check(f.acquireCalls == 1 && f.broker.stats().taps == 1 && f.broker.stats().domains == 3,
          "capture, FFT and stereo meter share just one source lease");
    std::array<float, 1024> left{}, right{};
    left.fill(.5f); right.fill(-.25f);
    f.tap->write(left.data(), right.data(), 1024, 100, false, 5);
    f.broker.pump();
    const auto frozen = capture.read();
    const auto level = peaks.readStereoMeter();
    const auto bins = spectrum.read();
    check(frozen && !frozen->previous.empty() && level && bins &&
              frozen->generation == level->generation && level->generation == bins->generation &&
              frozen->transportRevision == 5 && bins->transportRevision == 5,
          "three analysis types publish for the same physical generation and transport");
    const auto previous = frozen ? frozen->previous : std::vector<adi::dsp::TriggeredScope::Column>{};
    f.tap->write(left.data(), right.data(), 3, 100, false, 6);
    f.broker.pump();
    const auto afterSeek = capture.read();
    check(frozen && afterSeek && afterSeek->transportRevision == 6 &&
              afterSeek->generation == frozen->generation && afterSeek->sourceEpoch > frozen->sourceEpoch &&
              afterSeek->previous.empty() && !afterSeek->current.empty() &&
              afterSeek->current.front().firstSequence == 1024 && frozen->previous == previous,
          "same-position seek starts an independent sweep without mutating a frozen publication");
    capture.reset();
    spectrum.reset();
    check(f.releaseCalls == 0 && f.broker.stats().domains == 1 && f.broker.stats().spectralPlans == 0,
          "hiding capture and spectrum leaves the shared stereo meter active");
    f.tap->write(left.data(), right.data(), 1024, 100, false, 6);
    f.broker.pump();
    const auto start = f.tap->written();
    capture = f.broker.acquireCapture(request);
    check(capture.available() && !capture.read() && capture.sourcePosition().written == start,
          "a new capture on the same live tap fences all samples written while it was hidden");
    f.tap->write(left.data(), right.data(), 3, 100, false, 6);
    f.broker.pump();
    const auto resumed = capture.read();
    check(resumed && !resumed->current.empty() && resumed->current.front().firstSequence == start &&
              resumed->previous.empty() && f.acquireCalls == 1,
          "resuming starts at the new acquisition boundary without replacing the physical tap");
    peaks.reset();
    capture.reset();
    const auto idle = f.broker.stats();
    f.tap->write(left.data(), right.data(), 1024, 100, false, 6);
    f.broker.pump();
    check(f.releaseCalls == 1 && idle.consumers == 0 && idle.taps == 0 &&
              idle.workerPasses == f.broker.stats().workerPasses && idle.cursorReads == f.broker.stats().cursorReads,
          "last hidden analyser releases the physical tap and leaves no capture or analysis work");
}

} // namespace
void *operator new(std::size_t bytes) {
    if(observeAllocationSizes && allocationSizeCount<allocationSizes.size()) allocationSizes[allocationSizeCount++]=bytes;
    if(refuseAllocationBytes && bytes==refuseAllocationBytes && !refusedAllocationSize) {
        refusedAllocationSize=true; throw std::bad_alloc{};
    }
    if(failAllocationAfter==0) throw std::bad_alloc{};
    if(failAllocationAfter>0) --failAllocationAfter;
    if(auto *p=std::malloc(bytes?bytes:1)) return p;
    throw std::bad_alloc{};
}
void *operator new[](std::size_t bytes) {return ::operator new(bytes);}
void operator delete(void *p) noexcept {std::free(p);}
void operator delete[](void *p) noexcept {::operator delete(p);}
void operator delete(void *p,std::size_t) noexcept {::operator delete(p);}
void operator delete[](void *p,std::size_t) noexcept {::operator delete(p);}
int main() {
    try { continuousCapture(); captureBoundaries(); captureSharesLiveTap(); focusScopeWindows(); parkedSilence(); sharing(); configurationsAndMoves(); refreshAndLifetime(); refusalsAndBounds(); shutdown(); workerLifecycle();
          sharedModesAndCursors(); clippedSampleWindows(); exactZeroSpectrum(); exactSilentStereoLanes(); sharedAverage(); averageActivationAndOverflow(); averageChronologyAndEpochs(); averagePreparationFailure(); averagePendingBoundaries(); averageHistoryAndSanitation(); averagePublicationFailure(); boundsAndDeviceSources(); historyRenegotiation(); fairFrameBudget(); lazyPlansAndIdleDemand(); boundedImmutableHistory(); fullHistoryBursts(); finiteExtremeSpectrum(); actualRateHistory(); lateRateAndFailedNegotiation(); stereoMeterSharingAndIntervals(); stereoMeterTransportContinuity(); parkedStereoMeter(); }
    catch (const std::exception &e) { ++failures; std::printf("FAIL exception: %s\n", e.what()); }
    std::printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
