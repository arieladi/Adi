// SPDX-License-Identifier: AGPL-3.0-or-later
#include "adi/ui/analysis_comparison.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <memory>
#include <new>
#include <numbers>
#include <stdexcept>
#include <vector>

namespace {
bool failNextAllocation = false;
std::size_t failAllocationSize = 0;
int matchingAllocations = 0;
} // namespace
void *operator new(std::size_t size) {
    if (failNextAllocation) {
        failNextAllocation = false;
        throw std::bad_alloc();
    }
    if (failAllocationSize && size == failAllocationSize && ++matchingAllocations == 2)
        throw std::bad_alloc();
    if (auto *p = std::malloc(size ? size : 1))
        return p;
    throw std::bad_alloc();
}
void *operator new[](std::size_t size) { return ::operator new(size); }
void operator delete(void *p) noexcept { std::free(p); }
void operator delete[](void *p) noexcept { std::free(p); }
void operator delete(void *p, std::size_t) noexcept { std::free(p); }
void operator delete[](void *p, std::size_t) noexcept { std::free(p); }

using adi::engine::ScopePoint;
using adi::engine::ScopeSource;
using adi::engine::ScopeTap;
using adi::ui::AnalysisComparison;
using adi::ui::SharedAnalysis;
using Status = AnalysisComparison::Status;
namespace {
int checks = 0, failures = 0;
std::size_t comparedBins = 0;
void check(bool ok, const char *what) {
    ++checks;
    if (!ok) {
        ++failures;
        std::printf("FAIL %s\n", what);
    }
}
bool near(double a, double b, double tolerance = .0004) { return std::abs(a - b) < tolerance; }
AnalysisComparison::Endpoint endpoint(std::int64_t track, std::uint64_t token = 1,
                                      SharedAnalysis::Channel channel = SharedAnalysis::Channel::Mid) {
    SharedAnalysis::Request request;
    request.track = track;
    request.channel = channel;
    return {request, token};
}
bool equalBins(std::span<const float> a, std::span<const float> b) {
    if (a.size() != b.size())
        return false;
    comparedBins += a.size();
    return std::equal(a.begin(), a.end(), b.begin());
}
enum class Wave { Sine, DC, Nyquist, Silence };
struct Fixture {
    std::array<std::shared_ptr<ScopeTap>, 3> taps{std::make_shared<ScopeTap>(48000, 2),
                                                  std::make_shared<ScopeTap>(48000, 2),
                                                  std::make_shared<ScopeTap>(48000, 2)};
    int acquired = 0, released = 0;
    std::int64_t throwTrack = 0;
    std::shared_ptr<SharedAnalysis> broker = std::make_shared<SharedAnalysis>(
        [this](ScopeSource source, double) -> std::shared_ptr<const ScopeTap> {
            ++acquired;
            if (source.track == throwTrack)
                throw std::runtime_error("generated acquisition failure");
            if (source.track < 1 || source.track > 3)
                return {};
            return taps[static_cast<std::size_t>(source.track - 1)];
        },
        [this](ScopeSource) { ++released; }, SharedAnalysis::Mode::Manual);
    AnalysisComparison comparison{broker};
    void write(std::size_t index, int count = 1024, float amplitude = .5f, float rightScale = 1.f,
               Wave wave = Wave::Sine, bool advancing = true, std::int64_t stamp = -1) {
        auto &tap = *taps[index];
        std::vector<float> l(static_cast<std::size_t>(count)), r(l.size());
        for (int i = 0; i < count; ++i) {
            const auto phase = static_cast<double>(tap.written() + i);
            float value = 0;
            if (wave == Wave::Sine)
                value = amplitude * static_cast<float>(std::sin(2 * std::numbers::pi * 16 * phase / 1024));
            if (wave == Wave::DC)
                value = amplitude;
            if (wave == Wave::Nyquist)
                value = ((tap.written() + i) % 2) ? -amplitude : amplitude;
            l[static_cast<std::size_t>(i)] = value;
            r[static_cast<std::size_t>(i)] = value * rightScale;
        }
        tap.write(l.data(), r.data(), count, stamp < 0 ? tap.written() : stamp, advancing);
    }
    void tick() {
        broker->pump();
        comparison.frame();
    }
    void start(AnalysisComparison::Endpoint a = endpoint(1),
               AnalysisComparison::Endpoint b = endpoint(2, 2)) {
        check(comparison.configure(a, b) && comparison.setVisible(true), "configured visible comparison");
    }
};

void calibrationAndSharing() {
    Fixture f;
    check(!f.comparison.setVisible(true), "unconfigured model cannot acquire guessed endpoints");
    check(f.comparison.configure(endpoint(1), endpoint(2, 2)) && f.broker->stats().consumers == 0,
          "configuration while hidden creates no demand");
    check(f.comparison.setVisible(true) && f.broker->stats().consumers == 2 &&
              f.broker->stats().domains == 2 && f.broker->stats().taps == 2,
          "distinct sources use two existing domains and taps");
    auto oracleA = f.broker->acquire(endpoint(1).request);
    auto oracleB = f.broker->acquire(endpoint(2).request);
    f.write(0);
    f.write(1, 1024, .25f);
    f.tick();
    auto view = f.comparison.view();
    failAllocationSize = 513 * sizeof(float);
    matchingAllocations = 0;
    const bool secondFreezeFailure = f.comparison.setFrozen(true);
    failAllocationSize = 0;
    check(!secondFreezeFailure && matchingAllocations == 2 && !f.comparison.view().frozen &&
              f.broker->stats().consumers == 4 && f.comparison.capturedBinCount() == 0,
          "failure copying second frozen endpoint preserves both original live endpoints");
    view = f.comparison.view();
    check(view.status == Status::Independent && f.broker->stats().transforms == 2,
          "two sources plus observer leases use only two transforms");
    check(equalBins(view.a.binsDbfs, oracleA.read()->binsDbfs) &&
              equalBins(view.b.binsDbfs, oracleB.read()->binsDbfs),
          "all published bins equal existing broker spectrum oracles");
    check(near(view.a.binsDbfs[16], -6.020599913) && near(view.b.binsDbfs[16], -12.041199827),
          "existing coherent sine calibration retained");
    check(AnalysisComparison::difference(view, 16).db &&
              near(*AnalysisComparison::difference(view, 16).db, 20 * std::log10(2.0)),
          "known amplitude ratio produces analytic spectral level difference");
    check(view.a.metadata->endpoint.ownerToken == 1 && view.b.metadata->endpoint.ownerToken == 2 &&
              view.a.metadata->endpoint.request.track == 1 && view.b.metadata->endpoint.request.track == 2,
          "each picture retains its own source and owner identity");
    check(!AnalysisComparison::difference(view, 513).db, "out-of-range bin has no invented difference");
    const auto before = f.broker->stats().transforms;
    f.tick();
    check(f.broker->stats().transforms == before, "idle comparison performs no new analysis");
    check(f.comparison.configure(endpoint(1, 4), endpoint(1, 5)),
          "same-source endpoints explicitly accepted");
    f.write(0, 1024, .5f);
    f.tick();
    view = f.comparison.view();
    check(equalBins(view.a.binsDbfs, view.b.binsDbfs) && f.broker->stats().transforms == before + 4,
          "same-source pair shares actual FFT work while other observer remains idle");
    check(AnalysisComparison::difference(view, 16).db == 0.f, "same-source valid bin has exact zero delta");
    for (const auto wave : {Wave::DC, Wave::Nyquist, Wave::Silence}) {
        f.write(0, 1024, .5f, 1, wave);
        f.tick();
        view = f.comparison.view();
        const std::size_t bin = wave == Wave::Nyquist ? 512u : 0u;
        if (wave == Wave::Silence) {
            const auto delta = AnalysisComparison::difference(view, 16);
            check(delta.aCensored && delta.bCensored && !delta.db,
                  "floor bins are censored, not exact ratios");
        } else
            check(near(view.a.binsDbfs[bin], -6.020599913),
                  "DC and Nyquist retain existing half calibration");
    }
}

void calibrationValidation() {
    Fixture f;
    f.start();
    f.write(0);
    f.write(1);
    f.tick();
    auto view = f.comparison.view();
    auto m = *view.a.metadata;
    std::vector<float> bins(view.a.binsDbfs.begin(), view.a.binsDbfs.end());
    const auto valid = m;
    const auto classify = [&] { return AnalysisComparison::classify({&m, bins}, view.b); };
    check(classify() == Status::Independent, "pure picture classifier accepts generated broker output");
    for (int kind = 0; kind < 10; ++kind) {
        m = valid;
        switch (kind) {
        case 0:
            m.endpoint.ownerToken = 0;
            break;
        case 1:
            m.generation = 0;
            break;
        case 2:
            m.firstSequence = -1;
            break;
        case 3:
            m.firstSequence = std::numeric_limits<std::int64_t>::max();
            break;
        case 4:
            m.sampleRate = std::numeric_limits<double>::infinity();
            break;
        case 5:
            m.sampleRate = 0;
            break;
        case 6:
            m.coherentGain = std::numeric_limits<double>::quiet_NaN();
            break;
        case 7:
            m.coherentGain = -1;
            break;
        case 8:
            m.outputs = SharedAnalysis::Meters;
            break;
        default:
            m.pendingFrames = -1;
            break;
        }
        check(classify() == Status::Unavailable, "malformed temporal/calibration metadata rejected");
    }
    m = valid;
    bins.pop_back();
    check(classify() == Status::Unavailable, "short bin count rejected");
    bins.push_back(0);
    bins.push_back(0);
    check(classify() == Status::Unavailable, "oversized bin count rejected");
    bins.pop_back();
    bins[16] = std::numeric_limits<float>::quiet_NaN();
    check(classify() == Status::Unavailable, "nonfinite spectrum rejected");
    bins[16] = adi::dsp::kDbFloor - 1;
    check(classify() == Status::Unavailable, "below-floor spectrum rejected");
    bins[16] = view.a.binsDbfs[16];
    auto forged = view;
    std::array<float, 1> extremeA{std::numeric_limits<float>::max()};
    std::array<float, 1> extremeB{-std::numeric_limits<float>::max()};
    forged.a.binsDbfs = extremeA;
    forged.b.binsDbfs = extremeB;
    check(!AnalysisComparison::difference(forged, 0).db,
          "external extreme values cannot publish nonfinite difference");
    for (int kind = 0; kind < 5; ++kind) {
        m = valid;
        switch (kind) {
        case 0:
            m.sampleRate = 96000;
            break;
        case 1:
            m.coherentGain *= .9;
            break;
        case 2:
            m.endpoint.request.window = adi::dsp::Window::Rectangular;
            break;
        case 3:
            m.endpoint.request.channel = SharedAnalysis::Channel::Side;
            break;
        default:
            m.endpoint.request.hopSize = m.hopSize = 512;
            break;
        }
        check(classify() == Status::Incompatible,
              "different exact calibration is incompatible, not reindexed");
    }
    m = valid;
    m.endpoint.request.fftSize = 2048;
    bins.resize(1025);
    check(classify() == Status::Incompatible, "different FFT/bin count is incompatible");
    auto bad = endpoint(1);
    bad.ownerToken = 0;
    check(!f.comparison.configure(bad, endpoint(2, 2)) && f.comparison.view().status == Status::Independent,
          "invalid endpoint preserves accepted pictures");
    bad = endpoint(1);
    bad.request.outputs = SharedAnalysis::Meters;
    check(!f.comparison.configure(bad, endpoint(2, 2)),
          "component does not silently ignore non-spectrum request");
    bad = endpoint(1);
    bad.request.fftSize = 224;
    check(!f.comparison.configure(bad, endpoint(2, 2)),
          "unsupported FFT rejected before upstream transform creation");
    bad = endpoint(1);
    bad.request.point = ScopePoint::DeviceOutput;
    check(!f.comparison.configure(bad, endpoint(2, 2)), "device endpoint requires explicit device identity");
    bad.request.device = 99;
    check(f.comparison.configure(bad, endpoint(2, 2)),
          "explicit device source accepted without relabeling track output");
    check(!f.comparison.view().a.metadata, "changed source cannot display old picture with new identity");
    f.write(0);
    f.write(1);
    f.tick();
    check(f.comparison.view().a.metadata && f.comparison.view().a.metadata->endpoint.request.device == 99,
          "new device picture retains explicit endpoint identity");
}

void holdsAndFailurePreservation() {
    Fixture f;
    f.start();
    auto historyRequest = endpoint(2).request;
    historyRequest.outputs = SharedAnalysis::Spectrum | SharedAnalysis::Spectrogram | SharedAnalysis::Scope;
    auto history = f.broker->acquire(historyRequest);
    f.write(0);
    f.write(1, 1024, .25f);
    f.tick();
    const auto bins =
        std::vector<float>(f.comparison.view().b.binsDbfs.begin(), f.comparison.view().b.binsDbfs.end());
    std::weak_ptr<const SharedAnalysis::Result> oldB = history.read();
    check(f.broker->stats().historyBytes > 0, "unrelated observer has real spectrogram payload");
    check(!f.comparison.holdReference(99) && f.broker->stats().consumers == 3,
          "stale expected owner token cannot capture or release current endpoint");
    failNextAllocation = true;
    const bool heldOnFailure = f.comparison.holdReference(2);
    check(!heldOnFailure && !failNextAllocation && f.comparison.view().status == Status::Independent &&
              f.broker->stats().consumers == 3,
          "capture allocation failure preserves both live endpoints");
    check(f.comparison.holdReference(2) && f.comparison.view().status == Status::Held &&
              f.broker->stats().consumers == 2,
          "hold releases only local B lease");
    history.reset();
    check(oldB.expired() && f.broker->stats().historyBytes == 0 && f.comparison.capturedBinCount() == 513,
          "hold stores only bounded bins and metadata, not Result scope or shared history");
    f.write(0, 1024, .125f);
    f.write(1, 1024, 1);
    f.tick();
    check(equalBins(f.comparison.view().b.binsDbfs, bins),
          "held bins remain immutable while producers advance");
    check(near(*AnalysisComparison::difference(f.comparison.view(), 16).db, -6.020599913),
          "live A compares against explicit historical B amplitude");
    const auto oldA = f.comparison.endpointA();
    failNextAllocation = true;
    const bool configuredOnAllocationFailure = f.comparison.configure(endpoint(3, 3), endpoint(2, 2));
    check(!configuredOnAllocationFailure && !failNextAllocation && f.comparison.endpointA() == oldA &&
              equalBins(f.comparison.view().b.binsDbfs, bins) && f.broker->stats().consumers == 1,
          "recoverable acquisition allocation exception preserves accepted reference and demand");
    auto incompatible = endpoint(3, 3);
    incompatible.request.channel = SharedAnalysis::Channel::Side;
    check(!f.comparison.configure(incompatible, endpoint(2, 2)) && f.comparison.endpointA() == oldA &&
              equalBins(f.comparison.view().b.binsDbfs, bins),
          "incompatible A configuration preserves held reference");
    f.throwTrack = 3;
    check(!f.comparison.configure(endpoint(3, 3), endpoint(2, 2)) && f.comparison.endpointA() == oldA &&
              equalBins(f.comparison.view().b.binsDbfs, bins),
          "acquisition exception preserves accepted held configuration");
    const auto consumersBefore = f.broker->stats().consumers;
    check(!f.comparison.configure(endpoint(1), endpoint(3, 3)) && f.comparison.endpointA() == oldA &&
              equalBins(f.comparison.view().b.binsDbfs, bins) &&
              f.broker->stats().consumers == consumersBefore,
          "failure acquiring second replacement endpoint releases staged demand and preserves held B");
    f.throwTrack = 0;
    check(f.comparison.configure(endpoint(3, 3), endpoint(2, 2)) && f.comparison.view().referenceHeld &&
              !f.comparison.view().a.metadata,
          "compatible A-only replacement retains correctly labeled held B");
    f.write(2);
    f.tick();
    failNextAllocation = true;
    const bool frozenOnFailure = f.comparison.setFrozen(true);
    check(!frozenOnFailure && !failNextAllocation && !f.comparison.view().frozen &&
              f.comparison.view().referenceHeld,
          "freeze allocation failure preserves prior reference and live A");
    check(f.comparison.setFrozen(true) && f.broker->stats().consumers == 0 &&
              f.comparison.capturedBinCount() == 1026,
          "whole freeze drops remaining demand and stores two bin arrays");
    const auto frozenA =
        std::vector<float>(f.comparison.view().a.binsDbfs.begin(), f.comparison.view().a.binsDbfs.end());
    f.write(2, 1024, 1);
    f.tick();
    check(equalBins(f.comparison.view().a.binsDbfs, frozenA), "frozen A does not advance without leases");
    check(f.comparison.setVisible(false) && f.comparison.view().status == Status::Unavailable &&
              f.comparison.capturedBinCount() == 1026,
          "hide releases demand and retains explicit local snapshots");
    check(f.comparison.setVisible(true) && f.comparison.view().frozen && f.broker->stats().consumers == 0,
          "showing a frozen comparison does not resume shared work");
    check(f.comparison.setFrozen(false) && f.comparison.view().status == Status::Unavailable &&
              f.broker->stats().consumers == 1,
          "unfreeze holds B but waits for fresh A");
    f.write(2);
    f.tick();
    check(f.comparison.view().status == Status::Held, "new A resumes held comparison");
    check(f.comparison.configure(endpoint(3, 3), endpoint(2, 77)) && !f.comparison.view().referenceHeld &&
              !f.comparison.view().b.metadata,
          "B owner incarnation change clears reference before new publication");
    f.write(2);
    f.write(1);
    f.tick();
    check(f.comparison.view().b.metadata && f.comparison.view().b.metadata->endpoint.ownerToken == 77,
          "only newly admitted B result receives the new token");
    check(f.comparison.clear() && f.comparison.view().status == Status::Unavailable &&
              f.comparison.capturedBinCount() == 0,
          "clear is local and waits for fresh input");
}

void freshnessAndEpochs() {
    Fixture f;
    f.start(endpoint(1), endpoint(1, 2));
    auto observer = f.broker->acquire(endpoint(1).request);
    const auto statsBeforePosition = f.broker->stats();
    auto position = observer.sourcePosition();
    check(position.generation == f.taps[0]->generation() && position.written == 0,
          "source position reports actual ring generation and written count");
    const auto statsAfterPosition = f.broker->stats();
    check(statsBeforePosition.transforms == statsAfterPosition.transforms &&
              statsBeforePosition.cursorReads == statsAfterPosition.cursorReads &&
              statsBeforePosition.frameRequests == statsAfterPosition.frameRequests &&
              statsBeforePosition.workerPasses == statsAfterPosition.workerPasses,
          "source-position barrier neither consumes data nor requests analysis work");
    f.write(0, 1024, .5f, 1, Wave::Sine, false, 42);
    f.tick();
    check(f.comparison.view().status == Status::Independent,
          "initial parked window is independently available");
    const auto firstStamp = f.comparison.view().a.metadata->firstStamp;
    const auto firstSequence = f.comparison.view().a.metadata->firstSequence;
    f.comparison.setVisible(false);
    f.write(0, 2048, .25f, 1, Wave::Sine, false, 42);
    check(f.comparison.setVisible(true), "resume with unprocessed backlog succeeds as pending");
    f.tick();
    check(f.comparison.view().status == Status::Unavailable && observer.read()->firstSequence > firstSequence,
          "repumped pre-resume backlog cannot pass the written-sample barrier");
    auto expandedRequest = endpoint(1).request;
    expandedRequest.outputs |= SharedAnalysis::Scope;
    const auto oldRevision = observer.read()->revision;
    auto expanded = f.broker->acquire(expandedRequest);
    f.tick();
    check(observer.read()->revision > oldRevision && f.comparison.view().status == Status::Unavailable,
          "output expansion republishes old window but is not fresh audio");
    f.write(0, 256, .125f, 1, Wave::Sine, false, 42);
    f.tick();
    check(f.comparison.view().status == Status::Independent &&
              f.comparison.view().a.metadata->firstStamp == firstStamp &&
              f.comparison.view().a.metadata->firstSequence > firstSequence,
          "parked repeated stamps with newly written samples pass sequence freshness only");
    f.comparison.holdReference(2);
    check(f.comparison.resumeReference() && f.comparison.view().status == Status::Unavailable,
          "reference resume requires fresh data even while another observer keeps domain alive");
    f.tick();
    check(f.comparison.view().status == Status::Unavailable, "idle cached result cannot satisfy resume");
    f.write(0, 256, .5f, 1, Wave::Sine, false, 42);
    f.tick();
    check(f.comparison.view().status == Status::Independent,
          "post-resume write supplies both explicit leases");
    const auto oldGeneration = f.comparison.view().a.metadata->generation;
    f.taps[0] = std::make_shared<ScopeTap>(96000, 2);
    f.write(0, 1024);
    f.broker->refreshSources();
    f.write(0, 1024);
    f.tick();
    check(f.comparison.view().status == Status::Unavailable,
          "tap replacement establishes its own barrier, never admits old buffered publication");
    f.write(0, 256);
    f.tick();
    check(f.comparison.view().status == Status::Independent &&
              f.comparison.view().a.metadata->generation != oldGeneration &&
              f.comparison.view().a.metadata->sampleRate == 96000,
          "new generation requires a subsequent actual write and carries new rate");
    f.broker->refreshSources(true);
    f.comparison.frame();
    check(f.comparison.view().status == Status::Unavailable,
          "explicit source epoch reset removes stale live pictures");
    f.write(0, 1024, .5f, 1, Wave::Sine, true, 0);
    f.tick();
    check(f.comparison.view().status == Status::Independent &&
              f.comparison.view().a.metadata->firstStamp == 0,
          "seek/loop repeated timestamp is still explicitly independent");
    f.taps[0].reset();
    f.broker->refreshSources();
    f.comparison.frame();
    check(!observer.sourcePosition().generation && f.comparison.view().status == Status::Unavailable,
          "missing source has no position and revokes current live pictures");
}

void channelsQualityAndLifetime() {
    Fixture f;
    f.start(endpoint(1), endpoint(2, 2, SharedAnalysis::Channel::StereoPower));
    f.write(0, 1024, .5f, -1);
    f.write(1, 1024, .5f, -1);
    f.tick();
    auto v = f.comparison.view();
    check(v.status == Status::Incompatible && v.a.binsDbfs[16] == adi::dsp::kDbFloor &&
              near(v.b.binsDbfs[16], -6.020599913) && !AnalysisComparison::difference(v, 16).db,
          "antiphase Mid cancellation and StereoPower are distinct incompatible conventions");
    check(f.broker->stats().transforms == 2, "silent Mid performs zero FFTs; stereo power uses two");
    f.comparison.configure(endpoint(1), endpoint(2, 2));
    f.write(0);
    f.write(1);
    f.tick();
    f.comparison.holdReference(2);
    f.taps[0] = std::make_shared<ScopeTap>(44100, 2);
    f.broker->refreshSources();
    f.write(0);
    f.tick();
    f.write(0, 256);
    f.tick();
    check(f.comparison.view().status == Status::Incompatible && f.comparison.view().referenceHeld,
          "held reference with changed live rate is incompatible, never silently resampled");
    f.comparison.configure(endpoint(3, 3), endpoint(3, 4));
    auto oracle = f.broker->acquire(endpoint(3).request);
    std::array<float, 1024> corrupt{};
    corrupt[9] = std::numeric_limits<float>::infinity();
    f.taps[2]->write(corrupt.data(), nullptr, 1024, 100, true);
    f.tick();
    check(f.comparison.view().a.metadata &&
              f.comparison.view().a.metadata->nonFiniteSamples == oracle.read()->nonFiniteSamples &&
              f.comparison.view().a.metadata->nonFiniteSamples > 0,
          "sanitized input quality flags survive comparison admission");
    f.write(2, 1024, .5f, 1, Wave::Sine, true, 100000);
    f.tick();
    check(f.comparison.view().a.metadata->gapCount == oracle.read()->gapCount &&
              f.comparison.view().a.metadata->discontinuity == oracle.read()->discontinuity &&
              f.comparison.view().a.metadata->gapCount > 0,
          "gap and discontinuity metadata are not erased");
    f.write(2, 96000 + 2048, .5f, 1, Wave::Sine, true, 101024);
    f.tick();
    const auto *quality = f.comparison.view().a.metadata;
    check(quality && quality->overrunCount == oracle.read()->overrunCount && quality->overrunCount > 0 &&
              quality->budgetLimited == oracle.read()->budgetLimited &&
              quality->pendingFrames == oracle.read()->pendingFrames && quality->budgetLimited,
          "overrun and bounded worker backlog flags match actual shared analysis result");
    f.comparison.holdReference(4);
    f.comparison.setFrozen(true);
    check(f.comparison.view().frozen, "shutdown fixture has explicit frozen pictures");
    f.broker->shutdown();
    check(!f.broker->active() && !oracle.sourcePosition().generation &&
              f.comparison.view().status == Status::Unavailable && f.comparison.capturedBinCount() == 1026,
          "shutdown revokes frozen readiness without pretending retained bins are live");
    check(!f.comparison.configure(endpoint(1), endpoint(2, 2)) && !f.comparison.holdReference(4) &&
              !f.comparison.setFrozen(false),
          "shutdown rejects new actions without reviving a broker");
    f.comparison.frame();
    check(f.broker->stats().consumers == 0, "post-shutdown frame performs no new demand");

    Fixture g;
    auto observer = g.broker->acquire(endpoint(1).request);
    {
        AnalysisComparison scoped(g.broker);
        scoped.configure(endpoint(1), endpoint(2, 2));
        scoped.setVisible(true);
        check(g.broker->stats().consumers == 3,
              "destruction fixture has unrelated observer and two local leases");
    }
    check(g.broker->stats().consumers == 1 && observer.available(), "destruction releases only local leases");
    AnalysisComparison empty(nullptr);
    check(!empty.configure(endpoint(1), endpoint(2, 2)) && !empty.setVisible(true) &&
              empty.view().status == Status::Unavailable,
          "null broker has explicit unavailable contract");
}

void boundedCapture() {
    Fixture f;
    auto a = endpoint(1);
    a.request.fftSize = SharedAnalysis::kMaxFftSize;
    auto b = a;
    b.ownerToken = 2;
    f.start(a, b);
    f.write(0, SharedAnalysis::kMaxFftSize);
    f.tick();
    check(f.comparison.view().status == Status::Independent && f.broker->stats().transforms == 1,
          "maximum admitted FFT still shares one transform");
    check(f.comparison.holdReference(2) && f.comparison.capturedBinCount() == 32769,
          "maximum reference capture stores exactly N/2+1 bins");
    check(f.comparison.setFrozen(true) && f.comparison.capturedBinCount() == 2 * 32769 &&
              f.broker->stats().consumers == 0,
          "maximum whole freeze holds only two bounded bin arrays");
}
void directSpectrumAndIndependentIntervals() {
    for (const auto window : {adi::dsp::Window::Hann, adi::dsp::Window::Rectangular}) {
        Fixture f;
        auto a = endpoint(1);
        a.request.window = window;
        auto b = endpoint(2, 2);
        b.request.window = window;
        f.start(a, b);
        std::vector<float> input(1024), expected(513);
        for (std::size_t i = 0; i < input.size(); ++i)
            input[i] =
                .5f * static_cast<float>(std::sin(2 * std::numbers::pi * 16 * static_cast<double>(i) / 1024));
        adi::dsp::Spectrum spectrum(1024, window);
        spectrum.analyse(input, expected);
        std::transform(expected.begin(), expected.end(), expected.begin(), adi::dsp::toDbfs);
        f.taps[0]->write(input.data(), input.data(), 1024, 10, true);
        f.write(1, 2048, .25f, 1, Wave::Sine, true, 8000);
        f.tick();
        const auto view = f.comparison.view();
        check(equalBins(view.a.binsDbfs, expected), "all A bins match direct existing Spectrum kernel");
        check(view.status == Status::Independent && view.a.metadata->firstStamp == 10 &&
                  view.b.metadata->firstStamp == 9024 && view.a.metadata->firstSequence == 0 &&
                  view.b.metadata->firstSequence == 1024,
              "unequal producer chunks and heard intervals remain independent with unmodified metadata");
    }

    Fixture f;
    f.taps[1].reset();
    f.start();
    f.write(0);
    f.tick();
    check(f.comparison.view().a.metadata && !f.comparison.view().b.metadata &&
              f.comparison.view().status == Status::Unavailable,
          "missing B is explicit unavailable, never a fallback to A or Main");
    f.taps[1] = std::make_shared<ScopeTap>(48000, 2);
    f.broker->refreshSources();
    f.write(1);
    f.tick();
    check(f.comparison.view().status == Status::Unavailable,
          "newly available tap establishes a barrier before accepting input");
    f.write(1, 256);
    f.tick();
    check(f.comparison.view().status == Status::Independent, "subsequent B input completes explicit pair");
    f.comparison.holdReference(2);
    f.broker->shutdown();
    check(f.comparison.view().status == Status::Unavailable && f.comparison.view().referenceHeld &&
              f.comparison.capturedBinCount() == 513,
          "held-only reference readiness is revoked by shutdown while its historical bins remain local");
}
void preparedWindowGain() {
    for (const auto n : {32, 96, 160, 1024, 1536, 4096, 65536}) {
        for (const auto window : {adi::dsp::Window::Hann, adi::dsp::Window::Rectangular}) {
            Fixture f;
            auto request = endpoint(1).request;
            request.fftSize = n;
            request.window = window;
            request.outputs = SharedAnalysis::Scope;
            auto lease = f.broker->acquire(request);
            const double expected = adi::dsp::coherentGain(window, n);
            f.write(0, n, 0, 1, Wave::Silence);
            f.broker->pump();
            const auto result = lease.read();
            check(result && result->coherentGain == expected && result->binsDbfs.empty() &&
                      result->scopeLeft.size() == static_cast<std::size_t>(n) &&
                      std::all_of(result->scopeLeft.begin(), result->scopeLeft.end(),
                                  [](float sample) { return sample == 0; }) &&
                      f.broker->stats().transforms == 0,
                  "prepared scratch gain equals existing ordered window law and leaves silent Scope intact");
        }
    }
}
} // namespace
int main() {
    calibrationAndSharing();
    calibrationValidation();
    holdsAndFailurePreservation();
    freshnessAndEpochs();
    channelsQualityAndLifetime();
    boundedCapture();
    directSpectrumAndIndependentIntervals();
    preparedWindowGain();
    std::printf("AnalysisComparison: %d checks, %d failures, %zu exact compared bins\n", checks, failures,
                comparedBins);
    return failures ? 1 : 0;
}
