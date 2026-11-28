// SPDX-License-Identifier: AGPL-3.0-or-later
#include "adi/ui/shared_analysis.hpp"
#include "adi/ui/focus_match.hpp"
#include "adi/ui/focus_lag.hpp"
#include "adi/ui/focus_scope.hpp"
#include "adi/dsp/spectrum_average.hpp"
#include "adi/dsp/true_peak.hpp"
#include "adi/engine/session.hpp"
#include "adi/engine/transport.hpp"
#include "adi/store.hpp"
#include "temp_directory.hpp"
#include <SQLiteCpp/SQLiteCpp.h>
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
#include <optional>
#include <stdexcept>
#include <thread>
#include <type_traits>
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
void periodScopeReference() {
    constexpr double rate = adi::dsp::kPeriodScopeSampleRate;
    std::array<float, 512> samples{};
    const auto signal = [&](double frequency, bool harmonics = false) {
        for (std::size_t i = 0; i < samples.size(); ++i) {
            const auto phase = 2. * std::numbers::pi * frequency * static_cast<double>(i) / rate + .37;
            samples[i] = static_cast<float>(std::sin(phase) + (harmonics ? .8 * std::sin(2. * phase) +
                                                              .6 * std::sin(3. * phase) : 0.));
        }
        return adi::dsp::measurePeriodScope(samples);
    };
    for (const auto frequency : {50., 55., 110., 220., 440., 880., 984., 1000.}) {
        const auto reading = signal(frequency);
        check(reading.lag > 0. && near(rate / reading.lag, frequency, frequency * .06),
              "period detection finds the first correlation maximum, including both limit tones");
        if (reading.lag > 0. && reading.start > 0.) {
            const auto previous = static_cast<std::size_t>(reading.start);
            const auto crossing = std::lerp(reading.samples[previous], reading.samples[previous + 1],
                static_cast<float>(reading.start - static_cast<double>(previous)));
            check(std::abs(crossing) < .001f && reading.start + reading.lag < static_cast<double>(samples.size()),
                  "the retriggered trace begins at a rising zero crossing with a whole captured period");
        }
    }
    const auto rich = signal(80., true);
    check(rich.lag > 0. && near(rate / rich.lag, 80., 4.8), "strong harmonics do not make the scope lock an octave high");
    check(signal(35.).lag == 0., "a tone below the 40 ms capture's usable range is rejected");
    samples.fill(0.f);
    check(adi::dsp::measurePeriodScope(samples).lag == 0., "silence has no detected period");
    std::uint32_t seed = 12345;
    for (auto &sample : samples) {
        seed = (seed * 1103515245u + 12345u) & 0x7fffffffu;
        sample = static_cast<float>(static_cast<double>(seed) / 1073741823. - 1.);
    }
    check(adi::dsp::measurePeriodScope(samples).lag == 0., "deterministic white noise has no detected period");
}

void contiguousPeriodScopeDemand() {
    constexpr int size = 4096;
    for (const double rate : {44100., 768000.}) {
        Fixture fixture;
        fixture.tap = std::make_shared<ScopeTap>(rate, .5);
        SharedAnalysis::Request request{1, ScopePoint::PostFader, size};
        auto raw = fixture.broker.acquire(request);
        const auto originalAcquires = fixture.acquireCalls;
        request.outputs = SharedAnalysis::Spectrum | SharedAnalysis::PeriodScope;
        auto scoped = fixture.broker.acquire(request);
        const auto frames = static_cast<std::int32_t>(rate * .055) + size;
        std::vector<float> left(static_cast<std::size_t>(frames)), right(left.size());
        for (std::size_t i = 0; i < left.size(); ++i) {
            const double seconds = static_cast<double>(i) / rate;
            const auto mono = .4f * static_cast<float>(std::sin(2. * std::numbers::pi * 440. * seconds));
            const auto side = .7f * static_cast<float>(std::sin(2. * std::numbers::pi * 1700. * seconds));
            left[i] = mono + side;
            right[i] = mono - side;
        }
        fixture.tap->write(left.data(), right.data(), size, 0, true, 1);
        fixture.broker.pump();
        if (rate == 768000.)
            check(scoped.read() && !scoped.read()->periodScope, "a 4096-sample high-rate FFT does not invent a complete 40 ms capture");
        for (int offset = size; offset < frames;) {
            const auto block = std::min(893, frames - offset);
            fixture.tap->write(left.data() + offset, right.data() + offset, block, offset, true, 1);
            fixture.broker.pump();
            offset += block;
        }
        const auto snapshot = scoped.read();
        check(snapshot && snapshot == raw.read() && snapshot->periodScope && fixture.acquireCalls == originalAcquires &&
                  fixture.broker.stats().domains == 1 && fixture.broker.stats().taps == 1 &&
                  fixture.broker.stats().spectralPlans == 1,
              "the period scope and the full spectrum share the same worker, reader and physical tap");
        if (!snapshot || !snapshot->periodScope) continue;
        const auto &period = *snapshot->periodScope;
        constexpr auto fixedRate = adi::dsp::kPeriodScopeSampleRate;
        const auto end = static_cast<double>(snapshot->firstSequence + size - 1);
        const auto firstCell = std::floor(end * fixedRate / rate + 1.e-8) - 511.;
        float worst = 0.f;
        for (std::size_t i = 0; i < period.samples.size(); ++i) {
            const auto time = (firstCell + static_cast<double>(i)) / fixedRate;
            const auto sine = .4f * static_cast<float>(std::sin(2. * std::numbers::pi * 440. * time));
            worst = std::max(worst, std::abs(period.samples[i] - sine));
        }
        check(period.lag > 0. && near(fixedRate / period.lag, 440., 26.4) && worst < .002f,
              "512 cells follow the running audio clock across callback splits and use same-sample stereo mono");
        if (rate == 768000.) {
            fixture.tap->write(left.data(), right.data(), size, frames + 256, true, 2);
            fixture.broker.pump();
            check(scoped.read() && !scoped.read()->periodScope && scoped.read()->sourceEpoch > snapshot->sourceEpoch,
                  "a real source discontinuity discards the earlier period window");
            for (int i = 0; i < 8; ++i) {
                fixture.tap->write(static_cast<const float *>(nullptr), nullptr, size, 0, false, 3);
                fixture.broker.pump();
            }
            const auto parked = scoped.read();
            const auto workerPasses = fixture.broker.stats().workerPasses;
            fixture.tap->write(static_cast<const float *>(nullptr), nullptr, size, 0, false, 3);
            fixture.broker.pump();
            check(parked && parked->periodScope && parked->periodScope->lag == 0. && scoped.read() == parked &&
                      fixture.broker.stats().workerPasses == workerPasses,
                  "one complete high-rate silence capture settles the parked scope and worker");
        }
        scoped.reset();
        fixture.broker.pump();
        check(raw.read() && !raw.read()->periodScope && fixture.broker.stats().consumers == 1,
              "closing the scope drops its capture while a visible Raw spectrum keeps its tap");
        raw.reset();
        check(fixture.broker.stats().domains == 0 && fixture.releaseCalls == 1,
              "the last visible analysis lease releases the source");
    }
}

void colourWaveformReference() {
    using Capture = adi::dsp::ColourWaveformCapture;
    using adi::dsp::kColourWaveformColumnsPerSecond;
    for (const double rate : {44100., 48000., 768000.}) {
        Capture whole, chunked;
        check(whole.prepare(rate) && chunked.prepare(rate), "the waveform prepares its band split at the running native rate");
        const auto frames = static_cast<std::size_t>(rate / 4.);
        std::vector<float> left(frames), right(frames);
        for (std::size_t i = 0; i < frames; ++i) {
            const auto phase = 2. * std::numbers::pi * static_cast<double>(i) / rate;
            const auto mono = .4f * static_cast<float>(std::sin(1200. * phase));
            const auto side = .7f * static_cast<float>(std::sin(300. * phase));
            left[i] = mono + side;
            right[i] = mono - side;
        }
        constexpr std::int64_t origin = 321;
        whole.feed(left, right, origin);
        const auto boundary = static_cast<std::size_t>(std::ceil(rate / kColourWaveformColumnsPerSecond));
        const auto feed = [&](std::size_t offset, std::size_t length) {
            chunked.feed(std::span<const float>(left).subspan(offset, length), std::span<const float>(right).subspan(offset, length),
                         origin + static_cast<std::int64_t>(offset));
        };
        feed(0, boundary - 1);
        check(chunked.reading().size == 0, "an incomplete column never invents future audio");
        feed(boundary - 1, 1);
        const auto first = chunked.reading();
        check(first.size == 1 && first.serial == 1 && first.firstSequence == origin &&
                  first.endSequence == origin + static_cast<std::int64_t>(boundary),
              "the first column closes on the first native sample at a 256 Hz cell boundary");
        for (auto offset = boundary; offset < frames;) {
            const auto block = std::min<std::size_t>(97, frames - offset);
            feed(offset, block);
            offset += block;
        }
        const auto batch = whole.reading(), split = chunked.reading();
        check(batch == split && split.size == 64 && split.serial == 64 &&
                  split.endSequence == origin + static_cast<std::int64_t>(frames) &&
                  near(split.columns[63].envelope, .4, .002) && split.columns[63].bands[1] > .32f,
              "one quarter-second gives 64 identical real columns through any callback partition or native rate");
    }

    constexpr double rate = 48000.;
    constexpr std::size_t frames = 14400;
    std::array<float, frames> lane{};
    for (const auto &[target, frequency] : {std::pair{0u, 50.}, std::pair{1u, 1200.}, std::pair{2u, 10000.}}) {
        Capture capture;
        check(capture.prepare(rate), "band convergence fixture prepares");
        for (std::size_t i = 0; i < lane.size(); ++i)
            lane[i] = .73f * static_cast<float>(std::sin(2. * std::numbers::pi * frequency * static_cast<double>(i) / rate));
        capture.feed(lane, lane, 0);
        const auto reading = capture.reading();
        const auto &column = reading.columns[reading.size - 1];
        const auto &bands = column.bands;
        check(near(column.envelope, .73, .012) && bands[target] > 1.35f * std::max(bands[(target + 1) % 3], bands[(target + 2) % 3]),
              "the 200/4000 Hz split gives each test tone its own band and preserves the actual mono envelope");
        if (target == 0)
            check(bands[0] + bands[1] + bands[2] > column.envelope + .12f,
                  "band peaks can sum higher than the true envelope without increasing waveform height");
    }

    Capture history;
    check(history.prepare(rate), "waveform ring fixture prepares");
    const std::vector<float> past(48000, .2f), recent(48000, .5f), now(12000, .8f);
    history.feed(past, past, 0);
    history.feed(recent, recent, static_cast<std::int64_t>(past.size()));
    history.feed(now, now, static_cast<std::int64_t>(past.size() + recent.size()));
    const auto wrapped = history.reading();
    check(wrapped.size == 512 && wrapped.serial == 576 && near(wrapped.columns[0].envelope, .2) &&
              near(wrapped.columns[192].envelope, .5) && near(wrapped.columns[511].envelope, .8),
          "the two-second ring retains the newest 512 real columns in chronological order");
}

void continuousColourWaveformDemand() {
    Fixture fixture;
    fixture.tap = std::make_shared<ScopeTap>(768000., .5);
    SharedAnalysis::Request request{1};
    request.outputs = SharedAnalysis::ColourWaveform;
    auto waveform = fixture.broker.acquire(request);
    request.fftSize = 4096;
    auto mirror = fixture.broker.acquire(request);
    constexpr int frames = 192000;
    const std::vector<float> samples(frames, -.5f);
    fixture.tap->write(samples.data(), samples.data(), frames, 0, true, 1);
    fixture.broker.pump();
    const auto first = waveform.read();
    const auto stats = fixture.broker.stats();
    check(first && first == mirror.read() && first->colourWaveform && first->colourWaveform->serial == 64 &&
              first->firstSequence + first->request.fftSize == frames && !first->pendingFrames &&
              stats.spectralPlans == 0 && stats.analysisWindows == 0 && stats.domains == 1 && stats.taps == 1,
          "one worker pass consumes a high-rate quarter-second without an FFT hop bottleneck");
    if (!first || !first->colourWaveform) return;

    constexpr int columnFrames = 3000;
    fixture.tap->write(samples.data(), samples.data(), columnFrames, 0, true, 1);
    fixture.broker.pump();
    const auto looped = waveform.read();
    fixture.tap->write(static_cast<const float *>(nullptr), nullptr, columnFrames, 0, false, 2);
    fixture.broker.pump();
    const auto stopped = waveform.read();
    check(looped && looped->colourWaveform && looped->colourWaveform->serial == 65 && looped->sourceEpoch == first->sourceEpoch &&
              stopped && stopped->colourWaveform && stopped->colourWaveform->serial == 66 && stopped->sourceEpoch == first->sourceEpoch &&
              stopped->colourWaveform->columns[65].envelope == 0.f,
          "looping and stopping leave the waveform on its sample clock and still record real silence");

    fixture.tap->write(samples.data(), samples.data(), columnFrames, 120, false, 3);
    fixture.broker.pump();
    const auto sought = waveform.read();
    check(sought && sought->colourWaveform && sought->sourceEpoch > first->sourceEpoch && sought->colourWaveform->serial == 1 &&
              sought->colourWaveform->firstSequence == frames + 2 * columnFrames &&
              sought->firstSequence + sought->request.fftSize == frames + 3 * columnFrames,
          "a real seek starts a new capture while the generic epoch fence identifies its newest completed column");

    auto raw = fixture.broker.acquire({1});
    check(fixture.broker.stats().taps == 1 && fixture.acquireCalls == 1,
          "concurrent spectrum and waveform domains share the physical tap");
    waveform.reset();
    mirror.reset();
    check(fixture.broker.stats().consumers == 1 && fixture.broker.stats().domains == 1 && fixture.releaseCalls == 0 &&
              first->colourWaveform->serial == 64,
          "hiding the waveform releases its worker domain without discarding immutable readings");
    raw.reset();
    const auto exited = fixture.broker.stats();
    fixture.tap->write(samples.data(), samples.data(), columnFrames, 120, false, 3);
    fixture.broker.pump();
    check(exited.domains == 0 && exited.taps == 0 && fixture.releaseCalls == 1 &&
              fixture.broker.stats().cursorReads == exited.cursorReads,
          "no capture work continues without any visible analysis lease");
}

void filledSpectrumCurves() {
    using Curve = SharedAnalysis::FilledCurve;
    constexpr int size = 4096, hop = 1024, rate = 48000, bin = 64;
    constexpr std::size_t passes = 9;
    const auto index = [](Curve curve) { return static_cast<std::size_t>(curve); };
    std::array<float, 3 * size> samples{};
    for (int i = 0; i < size; ++i)
        samples[static_cast<std::size_t>(size + i)] = .8f *
            static_cast<float>(std::sin(2 * std::numbers::pi * bin * i / size));

    Fixture f;
    SharedAnalysis::Request request{1, ScopePoint::PostFader, size};
    auto raw = f.broker.acquire(request);
    const auto sourceAcquisitions = f.acquireCalls;
    request.outputs = SharedAnalysis::FilledSpectrum;
    auto filled = f.broker.acquire(request);
    auto sibling = f.broker.acquire(request);
    f.tap->write(samples.data(), samples.data(), static_cast<int>(samples.size()), 0, true, 1);
    f.broker.pump();
    const auto result = filled.read();
    const auto retained = sibling.read();
    check(result && result->filledSpectrum && retained && retained->filledSpectrum &&
              result->hopSize == hop && result->sampleRate == rate && result->filledSpectrum->processedWindows == passes &&
              f.broker.stats().domains == 1 && f.broker.stats().spectralPlans == 1 && f.broker.stats().taps == 1 &&
              f.acquireCalls == sourceAcquisitions,
          "Filled Spectrum processes all nine hops in a single pass on the shared raw FFT and tap");
    if (!result || !result->filledSpectrum || !retained || !retained->filledSpectrum) return;

    adi::dsp::Spectrum transform(size, adi::dsp::Window::Hann);
    std::array<float, size / 2 + 1> magnitudes{};
    const double interval = static_cast<double>(hop) / rate / .194;
    const double powerStep = 1. - std::pow(.0001, interval);
    const double peakStep = 1. - std::pow(.1, interval);
    double realtimePower = 0., realtimePeak = 0., held = 0., totalPower = 0.;
    for (std::size_t frame = 0; frame < passes; ++frame) {
        transform.analyse(std::span<const float>(samples).subspan(frame * hop, size), magnitudes);
        const double x = magnitudes[bin];
        realtimePower += (x * x - realtimePower) * powerStep;
        realtimePeak = std::max(x, realtimePeak + (x - realtimePeak) * peakStep);
        held = std::max(held, x);
        totalPower += x * x;
    }
    const auto &traces = result->filledSpectrum->magnitudes;
    check(near(traces[index(Curve::RealtimeAverage)][bin], std::sqrt(realtimePower)) &&
              near(traces[index(Curve::RealtimeMaximum)][bin], realtimePeak) &&
              near(traces[index(Curve::HeldMaximum)][bin], held) &&
              near(traces[index(Curve::Average)][bin], std::sqrt(totalPower / passes)) && held > .79 &&
              raw.read() && raw.read()->binsDbfs[bin] < -60.f,
          "both 194 ms realtime curves and both cumulative curves retain an intermediate peak before the latest frame");

    const auto before = f.broker.stats();
    f.broker.pump();
    check(filled.read() == result && f.broker.stats().analysisWindows == before.analysisWindows,
          "reading or pumping without new audio advances no Filled Spectrum curve");
    check(filled.restartFilledSpectrum() && !filled.read() && sibling.read() == retained,
          "Filled Spectrum Clear resets just the requesting view");
    f.tap->write(samples.data(), samples.data(), size, static_cast<int>(samples.size()), true, 1);
    f.broker.pump();
    const auto fresh = filled.read(), continuing = sibling.read();
    check(fresh && fresh->filledSpectrum && fresh->filledSpectrum->processedWindows == 1 &&
              fresh->filledSpectrum->magnitudes[index(Curve::HeldMaximum)][bin] < .001f &&
              continuing && continuing->filledSpectrum &&
              near(continuing->filledSpectrum->magnitudes[index(Curve::HeldMaximum)][bin], held) &&
              near(traces[index(Curve::HeldMaximum)][bin], held),
          "the next wholly fresh window starts a new local interval while the sibling and old snapshot retain their peak");

    const auto retainsPeak = [&] {
        const auto next = sibling.read();
        return next && next->filledSpectrum && near(next->filledSpectrum->magnitudes[index(Curve::HeldMaximum)][bin], held);
    };
    f.tap->write(samples.data(), samples.data(), size, 0, true, 1);
    f.broker.pump();
    check(retainsPeak(), "a loop discontinuity keeps the peak from the same play interval");
    f.tap->write(samples.data(), samples.data(), size, size, false, 2);
    f.broker.pump();
    check(retainsPeak(), "stopping keeps the peak from the play interval");
    f.tap->write(samples.data(), samples.data(), size, size, true, 3);
    f.broker.pump();
    const auto played = sibling.read();
    check(played && played->filledSpectrum && played->filledSpectrum->processedWindows == 1 &&
              played->filledSpectrum->magnitudes[index(Curve::HeldMaximum)][bin] < .001f,
          "starting playback opens a new held interval");
}
void stripCadenceAndEpochs() {
    constexpr int period = 2400, window = 1024;
    Fixture f;
    auto strip = f.broker.acquireStrip(1), sibling = f.broker.acquireStrip(1);
    check(strip.available() && sibling.available() && !strip.readStrip() && f.acquireCalls == 1 &&
              f.broker.stats().taps == 1 && f.broker.stats().domains == 1 && f.broker.stats().spectralPlans == 1,
          "two visible strip readers share one post-fader tap and spectral domain");

    std::array<float, period> left{}, right{};
    left.fill(.5f); right.fill(.25f);
    left[period - window] = -.75f; left[period - window + 1] = .8f;
    right[period - window] = -.5f; right[period - window + 1] = .6f;
    f.tap->write(left.data(), right.data(), period - 1, 0, true, 1);
    check(f.broker.pump() == 0 && f.broker.stats().workerPasses == 0 && !strip.readStrip(),
          "a strip-only lease stays idle until its first 50 milliseconds of audio");
    f.tap->write(left.data() + period - 1, right.data() + period - 1, 1, period - 1, true, 1);
    check(f.broker.pump() == 1, "the first due strip frame uses one transform for both readers");
    const auto first = strip.readStrip();
    check(first && first == sibling.readStrip() && first->spectrumDbfs.size() == 96 && first->scope.size() == 64 &&
              first->firstStamp == period - window && first->endSequence == period && first->transportRevision == 1 &&
              first->scope.front().minimum == std::array{-.75f, -.5f} &&
              first->scope.front().maximum == std::array{.8f, .6f},
          "the strip publishes shared stamped spectrum points and signed per-channel scope extrema");

    const float *silence = nullptr;
    f.tap->write(silence, silence, period, period, true, 1);
    check(f.broker.pump() == 0, "playing silence needs no strip FFT");
    const auto smooth = strip.readStrip();
    check(first && smooth && smooth->endSequence == 2 * period &&
              near(static_cast<double>(first->spectrumDbfs[0]) - smooth->spectrumDbfs[0],
                   20. * std::log10(std::exp(.05 / .3))),
          "the captured spectrum smooths linear magnitude with a 300-millisecond time constant");

    f.tap->write(silence, silence, period, 5, true, 2);
    f.broker.pump();
    const auto sought = strip.readStrip();
    check(first && sought && sought->sourceEpoch > first->sourceEpoch && sought->transportRevision == 2 &&
              sought->firstStamp == 5 + period - window && sought->spectrumDbfs[0] == adi::dsp::toDbfs(0.f),
          "a seek clears the prior smoothing and never publishes samples across the transport epoch");

    f.tap = std::make_shared<ScopeTap>(44100, SharedAnalysis::kTapSeconds);
    f.broker.refreshSources();
    check(!strip.readStrip(), "a new tap binding clears the old strip picture before new audio");
    constexpr int newPeriod = 2205;
    f.tap->write(left.data(), right.data(), newPeriod - 1, 5, true, 2);
    check(f.broker.pump() == 0 && !strip.readStrip(), "the strip waits for 2205 samples at 44.1 kHz");
    f.tap->write(left.data() + newPeriod - 1, right.data() + newPeriod - 1, 1, 5 + newPeriod - 1, true, 2);
    check(f.broker.pump() == 1, "the new rate publishes one transform at its exact sample boundary");
    const auto replaced = strip.readStrip();
    check(sought && replaced && replaced->generation == f.tap->generation() && replaced->sampleRate == 44100. &&
              replaced->sourceEpoch > sought->sourceEpoch && replaced->endSequence == newPeriod,
          "the refreshed strip publishes the current generation and sample-rate cadence");

    f.tap->write(silence, silence, newPeriod, 0, false, 3);
    f.broker.pump();
    const auto parked = strip.readStrip();
    const auto settled = f.broker.stats();
    f.tap->write(silence, silence, newPeriod, 0, false, 3);
    f.broker.pump();
    check(parked && parked->spectrumDbfs[0] == adi::dsp::toDbfs(0.f) && strip.readStrip() == parked &&
              f.broker.stats().workerPasses == settled.workerPasses,
          "certified stopped silence clears once and then performs no continuous analysis");

    strip.reset(); sibling.reset();
    const auto hidden = f.broker.stats();
    f.tap->write(left.data(), right.data(), newPeriod, 0, true, 4);
    f.broker.pump();
    check(hidden.taps == 0 && hidden.domains == 0 && f.releaseCalls == 1 &&
              hidden.transforms == f.broker.stats().transforms && hidden.workerPasses == f.broker.stats().workerPasses,
          "releasing the last visible strip detaches the tap and leaves no work for hidden audio");
}

void stripReusesRawSpectrum() {
    Fixture f;
    auto raw = f.broker.acquire({1});
    auto strip = f.broker.acquireStrip(1);
    std::array<float, 2560> audio;
    audio.fill(.25f);
    f.tap->write(audio.data(), audio.data(), static_cast<int>(audio.size()), 0, true, 1);
    check(f.broker.pump() == 7 && f.broker.stats().transforms == 7 && f.broker.stats().spectralPlans == 1 &&
              f.broker.stats().domains == 1 && f.acquireCalls == 1,
          "a compatible raw analyzer plus strip uses only the raw domain's seven transforms");
    const auto window = raw.read();
    const auto summary = strip.readStrip();
    check(window && summary && summary->endSequence == 2560 && summary->firstStamp == window->firstStamp &&
              summary->spectrumDbfs[0] == *std::max_element(window->binsDbfs.begin(), window->binsDbfs.begin() + 5),
          "the strip projects the first shared completed raw FFT at or after its 50-millisecond boundary");
    const auto settled = f.broker.stats();
    strip.reset();
    check(f.broker.pump() == 0 && raw.read() == window,
          "closing the last Strip retains the raw analyzer's completed publication");
    const auto idle = f.broker.stats();
    check(idle.taps == 1 && idle.consumers == 1 && idle.workerPasses == settled.workerPasses &&
              idle.cursorReads == settled.cursorReads && idle.analysisWindows == settled.analysisWindows &&
              idle.transforms == settled.transforms && idle.publications == settled.publications,
          "closing a shared Strip in an idle hop needs no worker, cursor, FFT or publication");

    SharedAnalysis::Request scopeRequest{1};
    scopeRequest.outputs = SharedAnalysis::Scope;
    auto waitingScope = f.broker.acquire(scopeRequest);
    auto transientStrip = f.broker.acquireStrip(1);
    transientStrip.reset();
    const auto pending = f.broker.stats();
    f.broker.pump();
    const auto projected = waitingScope.read();
    check(projected && window && projected->windowSerial == window->windowSerial &&
              projected->scopeLeft.size() == 1024 && f.broker.stats().publications == pending.publications + 1 &&
              f.broker.stats().transforms == pending.transforms,
          "closing Strip preserves an already pending projection for another consumer");

    auto reopened = f.broker.acquireStrip(1);
    check(reopened && !reopened.readStrip(), "reopening Strip starts without the old publication");
    f.tap->write(audio.data(), audio.data(), 256, 2560, true, 1);
    check(f.broker.pump() == 1 && f.broker.stats().transforms == 8 && f.broker.stats().taps == 1,
          "the compatible raw analyzer keeps its next hop and reuses one tap");
    f.tap->write(audio.data(), audio.data(), 2048, 2816, true, 1);
    f.broker.pump();
    check(!reopened.readStrip(), "a reopened Strip waits through raw hops before its next 50 milliseconds");
    f.tap->write(audio.data(), audio.data(), 256, 4864, true, 1);
    f.broker.pump();
    const auto next = reopened.readStrip();
    check(next && next != summary && next->endSequence == 5120 &&
              f.broker.stats().transforms == 17 && f.broker.stats().taps == 1 && f.acquireCalls == 1,
          "Strip restarts at its next fresh boundary without a new tap or FFT plan");
}

void stripWorkerBudget() {
    constexpr int rate = 768000, period = rate / 20, elapsed = 3 * period;
    constexpr auto count = SharedAnalysis::kMaxWindowsPerFrame + 1;
    std::array<std::shared_ptr<ScopeTap>, count> taps;
    std::size_t acquired = 0, released = 0;
    SharedAnalysis broker(
        [&](adi::engine::ScopeSource source, double seconds) -> std::shared_ptr<const ScopeTap> {
            ++acquired;
            auto &tap = taps.at(static_cast<std::size_t>(source.track - 1));
            if (!tap && source.point == ScopePoint::PostFader) tap = std::make_shared<ScopeTap>(rate, seconds);
            return tap;
        }, [&](adi::engine::ScopeSource) { ++released; }, SharedAnalysis::Mode::Manual);
    std::array<SharedAnalysis::Lease, count> strips;
    std::vector<float> audio(static_cast<std::size_t>(elapsed), .25f);
    for (std::size_t i = 0; i < count; ++i) {
        strips[i] = broker.acquireStrip(static_cast<std::int64_t>(i + 1));
        taps[i]->write(audio.data(), audio.data(), elapsed, 0, true, 1);
    }
    check(acquired == count && broker.stats().taps == count && broker.stats().spectralPlans == count &&
              broker.pump() == SharedAnalysis::kMaxWindowsPerFrame && broker.pump() == 1 && broker.pump() == 0 &&
              broker.stats().transforms == count && broker.stats().cursorReads == count,
          "high-rate late frames get one latest FFT and bounded cursor read per strip across fair worker passes");
    check(std::ranges::all_of(strips, [&](const auto &strip) {
              const auto frame = strip.readStrip(); return frame && frame->endSequence == elapsed;
          }), "a late owner frame skips obsolete 50-millisecond pictures instead of building a backlog");
    for (auto &strip : strips) strip.reset();
    check(released == count && broker.stats().taps == 0 && broker.stats().spectralPlans == 0,
          "closing all visible strips releases every physical tap and plan");
}

struct StripAudioSource final : adi::engine::Node {
    explicit StripAudioSource(double value) : amplitude(value) {}
    bool processesDouble() const noexcept override { return true; }
    void process(const adi::engine::NodeIo &io) noexcept override {
        for (int i = io.blockOffset; i < io.blockOffset + io.frames; ++i) {
            const double sample = (io.transport->timelineSample + i) % 64 == 11 ? amplitude : 0.;
            if (io.wide()) {
                io.out64[0][i] = sample;
                io.out64[1][i] = sample * -.5;
            } else {
                io.out[0][i] = static_cast<float>(sample);
                io.out[1][i] = static_cast<float>(sample * -.5);
            }
        }
    }
    double amplitude;
};

template <class Sample> void stripLeasePreservesAudio(const adi::Store &store) {
    using namespace adi::engine;
    constexpr double rate = 48000., restartedRate = 96000.;
    constexpr int frames = 512, delay = 37, window = 1024;
    constexpr bool wide = std::is_same_v<Sample, double>;
    auto setup = [&](Session &session, StripAudioSource &a, StripAudioSource &b) {
        session.setSourcesFor([&a, &b](std::int64_t track) -> std::vector<Node *> {
            if (track == 1) return {&a};
            if (track == 2) return {&b};
            return {};
        });
        session.graph().setFadeFrames(0);
        if (!session.load(store, {}, {2, rate, frames, wide ? Precision::Float64 : Precision::Float32}))
            throw std::runtime_error(session.error());
        session.transport().play();
    };
    StripAudioSource plainA(.25), plainB(.125), watchedA(.25), watchedB(.125);
    Session plain, watched;
    setup(plain, plainA, plainB);
    setup(watched, watchedA, watchedB);
    ScopeLease demand;
    SharedAnalysis broker(
        [&](ScopeSource source, double seconds) -> std::shared_ptr<const ScopeTap> {
            if (!demand) demand = watched.requestScope({source, seconds});
            return demand.tap();
        }, [&](ScopeSource) { demand.reset(); }, SharedAnalysis::Mode::Manual);

    using Output = std::array<std::array<Sample, frames>, 2>;
    using Bits = std::conditional_t<wide, std::uint64_t, std::uint32_t>;
    const auto render = [](Session &session, int count, Output &samples) {
        Sample *planes[]{samples[0].data(), samples[1].data()};
        AudioIo io;
        io.numOut = 2;
        io.frames = count;
        if constexpr (wide) io.out64 = planes;
        else io.out = planes;
        session.process(io);
    };
    const auto latency = [](Session &session) {
        const auto *graph = session.graph().currentGraph();
        return graph ? graph->latencySamples() : -1;
    };
    Output reference{}, observed{};
    auto compare = [&](int callbacks, int expectedLatency) {
        bool exact = true;
        for (int block = 0; block < callbacks; ++block) {
            const int count = block % 2 ? 257 : frames;
            render(plain, count, reference);
            render(watched, count, observed);
            watched.collectScopeTaps();
            broker.pump();
            for (std::size_t ch = 0; ch < reference.size(); ++ch) {
                exact = exact && std::equal(reference[ch].data(), reference[ch].data() + count, observed[ch].data(),
                    [](Sample a, Sample b) { return std::bit_cast<Bits>(a) == std::bit_cast<Bits>(b); });
            }
            exact = exact && latency(plain) == expectedLatency && latency(watched) == expectedLatency;
        }
        return exact;
    };
    // The early strip's signed latency must move the other track's pulse from
    // sample 11 to sample 48 in the compensated Main output.
    check(compare(1, delay) && reference[0][11] == .25 && reference[0][11 + delay] == .125,
          "the untouched graph has the expected physical PDC on two distinguishable pulses");

    auto strip = broker.acquireStrip(1);
    check(compare(8, delay), "attaching and reading a visible strip during playback preserves every audio and PDC bit");
    const auto beforeSeek = strip.readStrip();
    const auto tapBeforeRestart = demand.tap();
    check(beforeSeek && tapBeforeRestart && watched.scopeStats().sources == 1 && tapBeforeRestart->latency() == delay &&
              beforeSeek->firstStamp == watched.transport().position() - delay - window -
                  (tapBeforeRestart->written() - beforeSeek->endSequence),
          "the real post-fader strip tap uses the audible graph's existing compensated stamps");

    plain.transport().locate(8192);
    watched.transport().locate(8192);
    check(compare(8, delay), "the active strip does not change audio or PDC after a playing seek");
    const auto afterSeek = strip.readStrip();
    check(beforeSeek && afterSeek && afterSeek->sourceEpoch > beforeSeek->sourceEpoch &&
              afterSeek->transportRevision == watched.transport().revision() && afterSeek->firstStamp >= 8192 - delay,
          "seek publishes only the new transport epoch on the existing strip lease");

    for (auto *session : {&plain, &watched}) {
        session->release();
        session->prepare(restartedRate, frames);
        session->transport().play();
    }
    broker.refreshSources();
    check(!strip.readStrip() && demand.tap() && demand.tap() != tapBeforeRestart,
          "driver restart at a new rate invalidates the previous strip generation");
    check(compare(14, 2 * delay), "a retained strip preserves audio and the rescaled PDC after the driver restart");
    const auto restarted = strip.readStrip();
    const auto lastTap = demand.tap();
    check(restarted && lastTap && restarted->sampleRate == restartedRate && restarted->generation == lastTap->generation() &&
              lastTap->latency() == 2 * delay,
          "the retained strip follows the restarted graph's physical tap and compensation");

    strip.reset();
    check(compare(2, 2 * delay), "closing the visible strip across callback boundaries preserves audio and PDC");
    const auto retiredFrames = lastTap ? lastTap->written() : -1;
    check(compare(4, 2 * delay) && lastTap && lastTap->written() == retiredFrames &&
              watched.scopeStats().consumers == 0 && watched.scopeStats().sources == 0 && broker.stats().taps == 0,
          "the detached strip stops copying frames while the running graph remains bit-identical");
}

void stripLeaseInSession() {
    adi::test::TempDirectory directory{"shared_analysis", "strip_audio"};
    adi::StoreError error{};
    auto store = adi::Store::createEmpty(directory.path() / "strip-audio.adi", error);
    if (!store) throw std::runtime_error("strip audio fixture store");
    store->db().exec("INSERT INTO tracks(id,kind,name,index_in_parent) VALUES"
                     "(1,'audio','Early',0),(2,'audio','Late',1),(9,'master','Main',2);"
                     "INSERT INTO mixer_strip(track_id,delay_samples) VALUES(1,-37);");
    stripLeasePreservesAudio<float>(*store);
    stripLeasePreservesAudio<double>(*store);
}

void realtimeMaximumSpectrum() {
    constexpr int window = 480;
    constexpr int rate = 48000;
    std::array<float, window> first{}, quiet{}, loud{};
    first.front() = .5f;
    loud.fill(.5f);
    for (const int hop : {480, 240, 120}) {
        Fixture f;
        SharedAnalysis::Request request{1, ScopePoint::PostFader, window};
        request.window = adi::dsp::Window::Rectangular;
        request.hopSize = hop;
        auto raw = f.broker.acquire(request);
        request.outputs = SharedAnalysis::RealtimeMaximum;
        auto maximum = f.broker.acquire(request);
        auto sibling = f.broker.acquire(request);
        check(raw.available() && maximum.available() && sibling.available() && f.acquireCalls == 1 &&
                  f.broker.stats().spectralPlans == 1 && f.broker.stats().domains == 1,
              "visible realtime maximum readers share the raw Spectrum tap, window and FFT plan");
        if (!maximum.available())
            continue;
        f.tap->write(first.data(), first.data(), window, 0, true, 1);
        f.broker.pump();
        const auto initial = maximum.readMaximum();
        const auto input = raw.read();
        check(initial && sibling.readMaximum() == initial && input && initial->window == input &&
                  exactBins(initial->binsDbfs, input->binsDbfs) && f.broker.stats().transforms == 1,
              "the first maximum takes the calibrated raw bins in the same hop without another FFT");
        if (!initial || initial->binsDbfs.empty())
            continue;
        const auto startDb = initial->binsDbfs.front();
        const auto before = f.broker.stats();
        f.broker.pump();
        check(maximum.readMaximum() == initial && before.analysisWindows == f.broker.stats().analysisWindows &&
                  before.publications == f.broker.stats().publications,
              "a repaint or redundant pump advances no maximum without a completed physical hop");

        for (int i = 0; i < rate / hop; ++i) {
            f.tap->write(quiet.data(), quiet.data(), hop, window + i * hop, true, 1);
            if (i % 4 == 3)
                f.broker.pump();
        }
        f.broker.pump();
        const auto decayed = maximum.readMaximum();
        check(decayed && near(decayed->binsDbfs.front(), startDb - 20., .002) &&
                  f.broker.stats().transforms == 1 && initial->binsDbfs.front() == startDb,
              "one second of physical hops decays by 20 dB and leaves the held picture immutable");
        f.tap->write(loud.data(), loud.data(), hop, window + rate, true, 1);
        f.broker.pump();
        const auto rising = maximum.readMaximum();
        const auto current = raw.read();
        check(rising && current && rising->binsDbfs.front() == current->binsDbfs.front() &&
                  rising->binsDbfs.front() > startDb && f.broker.stats().transforms == 2,
              "a louder calibrated bin rises on the next hop without waiting for a redraw");

        f.tap->write(quiet.data(), quiet.data(), window, 2200, true, 2);
        f.broker.pump();
        const auto afterSeek = maximum.readMaximum();
        check(afterSeek && afterSeek->window->transportRevision == 2 &&
                  afterSeek->window->sourceEpoch > initial->window->sourceEpoch &&
                  afterSeek->binsDbfs.front() == adi::dsp::kDbFloor,
              "a seek begins a fresh maximum without reusing an old peak");

        maximum.reset();
        check(sibling.readMaximum() == afterSeek && f.releaseCalls == 0,
              "hiding one maximum view leaves the other view's history intact");
        sibling.reset();
        raw.reset();
        const auto hidden = f.broker.stats();
        f.tap->write(loud.data(), loud.data(), window, 2200 + window, true, 2);
        f.broker.pump();
        const auto idle = f.broker.stats();
        check(hidden.consumers == 0 && hidden.spectralPlans == 0 && f.releaseCalls == 1 &&
                  hidden.cursorReads == idle.cursorReads && hidden.transforms == idle.transforms,
              "hiding the final view leaves no maximum plan, capture or FFT work");
    }
    Fixture f;
    SharedAnalysis::Request request{1, ScopePoint::PostFader, window};
    request.hopSize = window / 2;
    auto raw = f.broker.acquire(request);
    f.tap->write(loud.data(), loud.data(), window, 0, true, 1);
    f.broker.pump();
    request.outputs = SharedAnalysis::RealtimeMaximum;
    auto reopened = f.broker.acquire(request);
    f.tap->write(quiet.data(), quiet.data(), window / 2, window, true, 1);
    f.broker.pump();
    check(raw.read() && !reopened.readMaximum(),
          "a newly admitted maximum excludes windows that overlap the loud pre-admission history");
    f.tap->write(quiet.data(), quiet.data(), window / 2, window + window / 2, true, 1);
    f.broker.pump();
    const auto fresh = reopened.readMaximum();
    check(fresh && fresh->window->firstSequence == window &&
              fresh->binsDbfs.front() == adi::dsp::kDbFloor && f.acquireCalls == 1,
          "a new maximum starts at its first complete fresh window on the existing tap");

    f.tap->write(static_cast<const float *>(nullptr), nullptr, window, 2000, false, 2);
    f.broker.pump();
    const auto settled = f.broker.stats();
    for (int i = 0; i < 12; ++i) {
        f.tap->write(static_cast<const float *>(nullptr), nullptr, window, 2000, false, 2);
        f.broker.pump();
    }
    const auto parked = f.broker.stats();
    check(settled.cursorReads == parked.cursorReads && settled.transforms == parked.transforms &&
              settled.publications == parked.publications,
          "the parked maximum becomes idle once its complete quiet window is at the display floor");
}
void realtimeMaximumDecayAndAdmission() {
    constexpr int window = 480;
    constexpr int rate = 48000;
    std::array<float, window> high{}, low{}, quiet{};
    high.fill(.5f);
    low.fill(.125f);

    {
        Fixture fixture;
        SharedAnalysis::Request request{1, ScopePoint::PostFader, window};
        request.window = adi::dsp::Window::Rectangular;
        request.hopSize = window;
        request.outputs = SharedAnalysis::RealtimeMaximum;
        auto maximum = fixture.broker.acquire(request);
        fixture.tap->write(high.data(), high.data(), window, 0, true, 1);
        fixture.broker.pump();
        const auto start = maximum.readMaximum();
        for (int i = 0; i < rate / window; ++i) {
            fixture.tap->write(low.data(), low.data(), window, window * (i + 1), true, 1);
            if (i % 4 == 3)
                fixture.broker.pump();
        }
        fixture.broker.pump();
        const auto end = maximum.readMaximum();
        const auto expectedDrop = 20. * std::log10(.25 + .75 * .1);
        check(start && end && near(end->binsDbfs.front(), start->binsDbfs.front() + expectedDrop, .002),
              "realtime maximum falls toward an ongoing weaker signal on the physical hop clock");

        request.outputs |= SharedAnalysis::Spectrum;
        const auto transforms = fixture.broker.stats().transforms;
        auto combined = fixture.broker.acquire(request);
        check(!combined.readMaximum(), "combined lease withholds maximum metadata from before Raw demand");
        fixture.broker.pump();
        const auto updated = combined.readMaximum();
        check(updated && end && updated->window == combined.read() &&
                  exactBins(updated->binsDbfs, end->binsDbfs) && fixture.broker.stats().transforms == transforms,
              "adding Raw republishes the same maximum bins and a matching window without a new FFT");
    }

    {
        Fixture fixture;
        SharedAnalysis::Request request{1, ScopePoint::PostFader, window};
        request.hopSize = window / 2;
        auto raw = fixture.broker.acquire(request);
        fixture.tap->write(quiet.data(), quiet.data(), window, 0, true, 1);
        fixture.broker.pump();
        check(raw.read() != nullptr, "Raw consumed history before a queued discontinuity");
        for (int i = 0; i < 3; ++i)
            fixture.tap->write(high.data(), high.data(), window, 2000 + i * window, true, 2);
        const auto admission = fixture.tap->written();
        request.outputs = SharedAnalysis::RealtimeMaximum;
        auto maximum = fixture.broker.acquire(request);
        fixture.broker.pump();
        check(!maximum.readMaximum(), "a seek already in the backlog cannot relax the maximum admission fence");
        fixture.tap->write(quiet.data(), quiet.data(), window, 2000 + 3 * window, true, 2);
        fixture.broker.pump();
        const auto fresh = maximum.readMaximum();
        check(fresh && fresh->window->firstSequence == admission && fresh->binsDbfs.front() == adi::dsp::kDbFloor,
              "maximum starts with the first complete post-admission window after a backlogged seek");
    }

    {
        Fixture fixture;
        SharedAnalysis::Request request{1, ScopePoint::PostFader, window};
        request.hopSize = window / 2;
        auto raw = fixture.broker.acquire(request);
        const float *silent = nullptr;
        fixture.tap->write(silent, silent, window, 0, false, 1);
        fixture.broker.pump();
        const auto oldPicture = raw.read();
        const auto admission = fixture.tap->written();
        request.outputs = SharedAnalysis::RealtimeMaximum;
        auto maximum = fixture.broker.acquire(request);
        fixture.tap->write(silent, silent, window, 0, false, 1);
        fixture.broker.pump();
        const auto parked = maximum.readMaximum();
        check(oldPicture && parked && parked->window->firstSequence == admission &&
                  parked->binsDbfs.front() == adi::dsp::kDbFloor,
              "a new maximum receives a floor picture while a settled Raw reader stays parked");
        const auto settled = fixture.broker.stats();
        fixture.tap->write(silent, silent, window, 0, false, 1);
        fixture.broker.pump();
        const auto idle = fixture.broker.stats();
        check(parked && maximum.readMaximum() == parked && idle.cursorReads == settled.cursorReads &&
                  idle.publications == settled.publications && idle.transforms == 0,
              "settled parked maximum skips later certified quiet samples");
    }
}
void realtimeSigmaSpectrum() {
    constexpr int window = 480;
    constexpr std::uint64_t revision = 51;
    Fixture f;
    SharedAnalysis::Request request{1, ScopePoint::PostFader, window};
    request.window = adi::dsp::Window::Rectangular;
    request.hopSize = window;
    auto raw = f.broker.acquire(request);
    request.outputs = SharedAnalysis::RealtimeSigma;
    auto sigma = f.broker.acquire(request);
    const auto plan = f.broker.stats();
    check(raw.available() && sigma.available() && !sigma.readRealtimeSigma() && !raw.readRealtimeSigma() &&
              plan.taps == 1 && plan.domains == 1 && plan.spectralPlans == 1 && f.acquireCalls == 1,
          "realtime sigma is demand-gated and shares the Raw FFT plan and physical tap");

    const auto write = [&](float value, std::int64_t stamp, std::uint64_t epoch) {
        std::array<float, window> samples;
        samples.fill(value);
        const auto *source = value == 0.f ? nullptr : samples.data();
        f.tap->write(source, source, window, stamp, true, epoch);
        return f.broker.pump();
    };
    const auto floorWork = write(0.f, 0, revision);
    const auto initial = sigma.readRealtimeSigma();
    check(initial && initial->window == raw.read() && !initial->binsDbfs.empty() &&
              initial->binsDbfs.front() == adi::dsp::kDbFloor && floorWork == 0,
          "the first silent null-input window publishes sigma floor without running the FFT");

    // Independent values for a2 = 1 - .1^(480/(48000 * .5)).
    // Update the mean with a2, then use the new mean and the same a2
    // for the central second moment. Report only its square root in dB.
    constexpr std::array<float, 4> steps{.5f, .125f, 0.f, .5f};
    constexpr std::array<double, 4> expectedDb{-19.887759312, -19.900744366, -20.087349149, -17.308109395};
    bool exactSteps = true;
    bool sharedWindows = true;
    for (std::size_t i = 0; i < steps.size(); ++i) {
        const auto transforms = write(steps[i], static_cast<std::int64_t>(i + 1) * window, revision);
        const auto picture = sigma.readRealtimeSigma();
        exactSteps = exactSteps && picture && !picture->binsDbfs.empty() &&
                     near(picture->binsDbfs.front(), expectedDb[i], .001) &&
                     transforms == (steps[i] == 0.f ? 0u : 1u);
        sharedWindows = sharedWindows && picture && picture->window == raw.read();
    }
    const auto last = sigma.readRealtimeSigma();
    check(exactSteps && sharedWindows && f.broker.stats().transforms == 3,
          "both sigma moments advance per physical hop with the same faster coefficient and newly updated mean");
    check(initial && !initial->binsDbfs.empty() && initial->binsDbfs.front() == adi::dsp::kDbFloor,
          "subsequent sigma windows cannot mutate a previously held publication");

    request.outputs = SharedAnalysis::Scope;
    auto scope = f.broker.acquire(request);
    const auto beforeRepublish = f.broker.stats();
    f.broker.pump();
    const auto metadata = sigma.readRealtimeSigma();
    const auto republished = f.broker.stats();
    check(metadata && last && metadata->window && last->window &&
              metadata->window == raw.read() && metadata->window == scope.read() &&
              metadata->window->windowSerial == last->window->windowSerial &&
              exactBins(metadata->binsDbfs, last->binsDbfs) &&
              republished.transforms == beforeRepublish.transforms &&
              republished.analysisWindows == beforeRepublish.analysisWindows,
          "metadata-only scope publication changes no sigma history or transform");
    f.broker.pump();
    const auto idle = f.broker.stats();
    check(sigma.readRealtimeSigma() == metadata && idle.transforms == republished.transforms &&
              idle.publications == republished.publications && idle.workerPasses == republished.workerPasses,
          "unchanged workers keep the same immutable sigma publication without extra passes");
    scope.reset();

    const auto seekWork = write(0.f, 30000, revision + 1);
    const auto afterSeek = sigma.readRealtimeSigma();
    check(seekWork == 0 && metadata && afterSeek && afterSeek->window == raw.read() &&
              afterSeek->window->transportRevision == revision + 1 &&
              afterSeek->window->sourceEpoch > metadata->window->sourceEpoch &&
              !afterSeek->binsDbfs.empty() && afterSeek->binsDbfs.front() == adi::dsp::kDbFloor,
          "a silent seek clears both sigma moments before publishing the new epoch");
    write(.5f, 30000 + window, revision + 1);
    const auto restarted = sigma.readRealtimeSigma();
    check(restarted && !restarted->binsDbfs.empty() &&
              near(restarted->binsDbfs.front(), expectedDb.front(), .001),
          "the first nonzero post-seek hop repeats sigma startup numerically");

    sigma.reset();
    write(.25f, 30000 + 2 * window, revision + 1);
    check(raw.read() && !(raw.read()->outputs & SharedAnalysis::RealtimeSigma) &&
              f.broker.stats().spectralPlans == 1 && f.releaseCalls == 0,
          "closing the sigma lease leaves the independent Raw plan without sigma output");
    raw.reset();
    const auto hidden = f.broker.stats();
    write(.25f, 30000 + 3 * window, revision + 1);
    const auto still = f.broker.stats();
    check(hidden.taps == 0 && hidden.spectralPlans == 0 && f.releaseCalls == 1 &&
              still.transforms == hidden.transforms && still.cursorReads == hidden.cursorReads &&
              still.workerPasses == hidden.workerPasses && still.publications == hidden.publications,
          "closing the final lease leaves no work for subsequent input");

    Fixture parked;
    request.outputs = SharedAnalysis::RealtimeSigma;
    auto quietSigma = parked.broker.acquire(request);
    const float *silence = nullptr;
    parked.tap->write(silence, silence, window, 0, false, revision);
    parked.broker.pump();
    const auto settled = parked.broker.stats();
    const auto heldFloor = quietSigma.readRealtimeSigma();
    parked.tap->write(silence, silence, 5 * window, 0, false, revision);
    parked.broker.pump();
    const auto quiet = parked.broker.stats();
    check(heldFloor && quietSigma.readRealtimeSigma() == heldFloor && settled.transforms == 0 &&
              quiet.cursorReads == settled.cursorReads && quiet.workerPasses == settled.workerPasses,
          "a settled parked Sigma trace does not read or transform further certified silence");
    std::array<float, window> resumedInput;
    resumedInput.fill(.5f);
    parked.tap->write(resumedInput.data(), resumedInput.data(), window, 0, false, revision);
    parked.broker.pump();
    const auto resumedSigma = quietSigma.readRealtimeSigma();
    check(resumedSigma && !resumedSigma->binsDbfs.empty() &&
              near(resumedSigma->binsDbfs.front(), expectedDb.front(), .001) && parked.broker.stats().transforms == 1,
          "an audible input wakes parked Sigma with its correct first-hop value");
}

void focusScopeMidBeforeBinning() {
    using adi::ui::FocusScopeBarBlock;
    using adi::ui::FocusScopeChannels;
    using adi::ui::FocusScopeBarProjection;
    using adi::ui::FocusScopeBarSource;
    using adi::ui::FocusScopeRange;
    using adi::ui::FocusSumColumn;
    constexpr FocusScopeRange range{48000, 2, 48000.};
    constexpr std::uint64_t revision = 31;
    constexpr std::array sources{FocusScopeBarSource{{7}, 11}, FocusScopeBarSource{{8}, 15}};
    constexpr std::array left{.5f, -.5f}, inverted{-.5f, .5f}, silence{0.f, 0.f};

    struct Captured {
        FocusSumColumn member, sum;
        std::size_t midCount = 0;
        std::array<double, 2> mid{};
    };
    const auto capture = [&](FocusScopeBarProjection &projection, std::span<const float> right,
                              bool requestMid) -> std::optional<Captured> {
        const auto view = requestMid ? FocusScopeChannels::Mid : FocusScopeChannels::Stereo;
        if (!projection.prepare(range, sources, 1, revision, view)) return {};
        for (std::size_t frame = 0; frame < left.size(); ++frame) {
            const auto stamp = range.first + static_cast<std::int64_t>(frame);
            const auto quiet = std::span(silence).subspan(frame, 1);
            std::array blocks{
                FocusScopeBarBlock{sources[0], stamp, revision, range.sampleRate, true,
                                   std::span(left).subspan(frame, 1), right.subspan(frame, 1)},
                FocusScopeBarBlock{sources[1], stamp, revision, range.sampleRate, true, quiet, quiet}
            };
            if (frame == 1) {
                ++blocks[1].firstStamp;
                if (projection.append(blocks) || projection.frames() != frame) return {};
                --blocks[1].firstStamp;
            }
            if (!projection.append(blocks) || projection.frames() != frame + 1) return {};
        }
        const auto member = projection.memberColumns(0), sum = projection.sumColumns();
        if (!projection.complete() || member.size() != 1 || sum.size() != 1) return {};
        Captured result{member.front(), sum.front(), 0, {}};
        const auto mid = projection.memberMonoColumns(0, FocusScopeChannels::Mid);
        result.midCount = mid.size();
        if (mid.size() == 1) result.mid = {mid.front().low, mid.front().high};
        return result;
    };
    const auto stereoMatches = [](const std::optional<Captured> &result,
                                  std::array<double, 2> low, std::array<double, 2> high) {
        return result && result->member.low == low && result->member.high == high &&
            result->sum.low == low && result->sum.high == high;
    };
    FocusScopeBarProjection projection;
    const auto inPhase = capture(projection, left, true);
    const auto cancel = capture(projection, inverted, true);
    const auto leftOnly = capture(projection, silence, true);
    const auto unrequested = capture(projection, left, false);
    check(stereoMatches(inPhase, {-.5, -.5}, {.5, .5}) && stereoMatches(cancel, {-.5, -.5}, {.5, .5}) &&
              stereoMatches(leftOnly, {-.5, 0.}, {.5, 0.}) && stereoMatches(unrequested, {-.5, -.5}, {.5, .5}),
          "the split stamped blocks retain exact existing L/R member and sum extrema");
    check(inPhase && inPhase->midCount == 1 && inPhase->mid == std::array{-.5, .5},
          "in-phase L/R is preserved in the requested Mid column");
    check(cancel && cancel->midCount == 1 && cancel->mid == std::array{0., 0.},
          "opposed L/R with the same stereo extrema cancels before Mid binning");
    check(leftOnly && leftOnly->midCount == 1 && leftOnly->mid == std::array{-.25, .25},
          "a single active channel contributes half its amplitude to Mid");
    check(unrequested && unrequested->midCount == 0,
          "unrequested Mid has no stored columns even after re-preparation");
}

void cumulativeMaximumSpectrum() {
    constexpr int window = 1024;
    Fixture f;
    SharedAnalysis::Request request{1, ScopePoint::PostFader, window};
    request.window = adi::dsp::Window::Rectangular;
    request.hopSize = window;
    auto raw = f.broker.acquire(request);
    request.outputs = SharedAnalysis::RealtimeMaximum;
    auto realtime = f.broker.acquire(request);

    tone(*f.tap, window, .9f);
    f.broker.pump();
    const auto oldPeak = raw.read();
    request.outputs = SharedAnalysis::CumulativeMaximum;
    auto cumulative = f.broker.acquire(request);
    auto sibling = f.broker.acquire(request);
    check(cumulative.available() && sibling.available() && f.broker.stats().domains == 1 &&
              f.broker.stats().spectralPlans == 1 && f.acquireCalls == 1,
          "cumulative maximum demand shares the Raw and Realtime Maximum FFT and physical tap");
    if (!cumulative.available()) return;

    f.broker.pump();
    check(!cumulative.readMaximum(), "cumulative demand cannot use a completed window from before its admission");
    tone(*f.tap, window, .5f, 1.f, window);
    f.broker.pump();
    const auto first = cumulative.readMaximum();
    const auto rawFirst = raw.read();
    const auto siblingFirst = sibling.readMaximum();
    check(first && siblingFirst && rawFirst && first->window == rawFirst && siblingFirst->window == rawFirst &&
              exactBins(first->binsDbfs, siblingFirst->binsDbfs) && exactBins(first->binsDbfs, rawFirst->binsDbfs) && oldPeak &&
              first->binsDbfs[16] < oldPeak->binsDbfs[16] && f.broker.stats().transforms == 2,
          "the first cumulative picture contains only new calibrated bins with no second transform");
    if (!first) return;

    std::array<float, window> loudDc{}, quiet{};
    loudDc.fill(.75f);
    f.tap->write(loudDc.data(), loudDc.data(), window, 2 * window, true);
    f.broker.pump();
    const auto peak = cumulative.readMaximum();
    const auto rawDc = raw.read();
    check(peak && rawDc && peak->window == rawDc && peak->binsDbfs[0] == rawDc->binsDbfs[0] &&
              peak->binsDbfs[16] == first->binsDbfs[16] && rawDc->binsDbfs[16] < peak->binsDbfs[16],
          "cumulative maximum retains independent peak values from separate physical windows");
    if (!peak) return;

    for (int i = 3; i <= 64; ++i) {
        f.tap->write(quiet.data(), quiet.data(), window, i * window, true);
        f.broker.pump();
    }
    const auto held = cumulative.readMaximum();
    const auto decayed = realtime.readMaximum();
    check(held && decayed && exactBins(held->binsDbfs, peak->binsDbfs) &&
              decayed->binsDbfs[16] + 20.f < held->binsDbfs[16] && f.broker.stats().transforms == 3,
          "cumulative bins stay identical through quiet hops while realtime maximum decays");
    check(held == peak && raw.read() != rawDc,
          "an unchanged cumulative picture is reused while the shared realtime trace continues publishing");
    if (!held) return;

    const auto siblingHeld = sibling.readMaximum();
    cumulative.reset();
    check(siblingHeld && exactBins(siblingHeld->binsDbfs, held->binsDbfs) &&
              sibling.readMaximum() == siblingHeld && f.releaseCalls == 0,
          "removing one cumulative observer preserves the history of another");
    cumulative = f.broker.acquire(request);
    f.broker.pump();
    check(!cumulative.readMaximum(), "reopening a cumulative observer waits for a fresh window");
    f.tap->write(quiet.data(), quiet.data(), window, 65 * window, true);
    f.broker.pump();
    const auto reopened = cumulative.readMaximum();
    const auto continuing = sibling.readMaximum();
    check(reopened && continuing && reopened->binsDbfs[0] == adi::dsp::kDbFloor &&
              exactBins(continuing->binsDbfs, held->binsDbfs),
          "a fresh cumulative subscription does not inherit a sibling's earlier peaks");
    sibling.reset();

    f.tap->write(loudDc.data(), loudDc.data(), window, 0, false, 2);
    f.broker.pump();
    const auto stopped = cumulative.readMaximum();
    check(stopped && stopped->window->sourceEpoch > held->window->sourceEpoch &&
              stopped->binsDbfs[16] == adi::dsp::kDbFloor && stopped->binsDbfs[0] > adi::dsp::kDbFloor,
          "a transport discontinuity resets cumulative bins before processing the new window");
    realtime.reset();
    const float *silent = nullptr;
    f.tap->write(silent, silent, window, 0, false, 2);
    f.broker.pump();
    const auto parked = cumulative.readMaximum();
    const auto settled = f.broker.stats();
    f.tap->write(silent, silent, window, 0, false, 2);
    f.broker.pump();
    const auto idle = f.broker.stats();
    check(parked && stopped && exactBins(parked->binsDbfs, stopped->binsDbfs) &&
              parked == cumulative.readMaximum() && idle.cursorReads == settled.cursorReads &&
              idle.publications == settled.publications && idle.transforms == settled.transforms,
          "certified parked silence retains cumulative peaks without further reads, FFTs or publications");

    cumulative.reset();
    raw.reset();
    const auto hidden = f.broker.stats();
    tone(*f.tap, window, .5f);
    f.broker.pump();
    check(hidden.consumers == 0 && hidden.spectralPlans == 0 && f.releaseCalls == 1 &&
              f.broker.stats().workerPasses == hidden.workerPasses,
          "hiding the final cumulative view frees analysis work");
}

void cumulativeAverageSpectrum() {
    constexpr int window = 480;
    constexpr std::uint64_t revision = 19;
    constexpr auto output = SharedAnalysis::CumulativeAverage;
    const auto dcIs = [](const auto &picture, std::uint64_t windows, double magnitude) {
        const auto expected = magnitude == 0. ? adi::dsp::kDbFloor : 20. * std::log10(magnitude);
        return picture && picture->window && !picture->binsDbfs.empty() &&
               picture->processedWindows == windows && near(picture->binsDbfs.front(), expected, .001);
    };
    const auto write = [](Fixture &f, float level, std::int64_t stamp, bool advancing, std::uint64_t epoch) {
        std::array<float, window> audio;
        audio.fill(level);
        const auto *source = level == 0.f ? nullptr : audio.data();
        f.tap->write(source, source, window, stamp, advancing, epoch);
        return f.broker.pump();
    };

    Fixture f;
    SharedAnalysis::Request request{1, ScopePoint::PostFader, window};
    request.window = adi::dsp::Window::Rectangular;
    request.hopSize = window;
    auto raw = f.broker.acquire(request);
    write(f, .875f, 0, true, revision);
    request.outputs = output;
    auto a = f.broker.acquire(request);
    f.broker.pump();
    const auto admitted = f.broker.stats();
    check(a.available() && !a.readCumulativeAverage() && raw.read() &&
              !raw.readCumulativeAverage() && !raw.restartCumulativeAverage() &&
              admitted.taps == 1 && admitted.domains == 1 && admitted.spectralPlans == 1 && f.acquireCalls == 1,
          "cumulative average joins the Raw plan and ignores completed pre-admission audio");

    write(f, .125f, window, true, revision);
    const auto initial = a.readCumulativeAverage();
    check(dcIs(initial, 1, .125) && initial->window == raw.read(),
          "the initial cumulative average uses the first fresh calibrated immutable window");
    auto b = f.broker.acquire(request);
    f.broker.pump();
    check(b.available() && !b.readCumulativeAverage() && a.readCumulativeAverage() == initial,
          "a second cumulative average starts a new interval without changing the first");

    const auto loudWork = write(f, .5f, 2 * window, true, revision);
    const auto mixed = a.readCumulativeAverage(), firstB = b.readCumulativeAverage();
    check(dcIs(mixed, 2, (.125 + .5) / 2.) && dcIs(firstB, 1, .5) &&
              mixed->window == raw.read() && firstB->window == mixed->window && loudWork == 1 &&
              f.broker.stats().transforms == 3 && f.broker.stats().spectralPlans == 1,
          "one shared FFT gives independently admitted arithmetic means of linear magnitudes");
    const auto quietWork = write(f, 0.f, 3 * window, true, revision);
    const auto diluted = a.readCumulativeAverage(), dilutedB = b.readCumulativeAverage();
    check(dcIs(diluted, 3, (.125 + .5) / 3.) && dcIs(dilutedB, 2, .5 / 2.) &&
              diluted->window == raw.read() && quietWork == 0 && dcIs(initial, 1, .125) && dcIs(mixed, 2, .3125),
          "each silent physical hop counts without another FFT and old publications stay immutable");
    const auto unchanged = f.broker.stats();
    for (int idle = 0; idle < 3; ++idle) f.broker.pump();
    check(a.readCumulativeAverage() == diluted && b.readCumulativeAverage() == dilutedB &&
              f.broker.stats().analysisWindows == unchanged.analysisWindows &&
              f.broker.stats().publications == unchanged.publications &&
              f.broker.stats().workerPasses == unchanged.workerPasses,
          "repeated pumps cannot count a hop or publish a new average without more audio");

    const bool reset = a.restartCumulativeAverage();
    f.broker.pump();
    check(reset && !a.readCumulativeAverage() && b.readCumulativeAverage() == dilutedB &&
              f.broker.stats().transforms == unchanged.transforms,
          "Reset clears only its cumulative interval and leaves the sibling and FFT alone");
    write(f, .375f, 4 * window, true, revision);
    check(dcIs(a.readCumulativeAverage(), 1, .375) &&
              dcIs(b.readCumulativeAverage(), 3, (.5 + .375) / 3.),
          "the reset interval starts at its next complete window while the sibling continues");
    const auto beforeSeek = a.readCumulativeAverage();
    write(f, 0.f, 20000, true, revision + 1);
    const auto afterSeek = a.readCumulativeAverage(), afterSeekB = b.readCumulativeAverage();
    check(dcIs(afterSeek, 1, 0.) && dcIs(afterSeekB, 1, 0.) && beforeSeek &&
              afterSeek->window->transportRevision == revision + 1 &&
              afterSeek->window->sourceEpoch > beforeSeek->window->sourceEpoch,
          "a seek starts a silent new source epoch in both intervals");

    a.reset();
    raw.reset();
    check(b.readCumulativeAverage() == afterSeekB && f.broker.stats().consumers == 1 &&
              f.broker.stats().spectralPlans == 1 && f.releaseCalls == 0,
          "closing Raw and one average leaves the remaining cumulative observer live");
    write(f, .5f, 20000 + window, true, revision + 1);
    const auto alone = b.readCumulativeAverage();
    check(dcIs(alone, 2, .25) && alone->window->binsDbfs.empty(),
          "an average-only lease has no raw-bin publication and still counts the silent start");
    b.reset();
    const auto hidden = f.broker.stats();
    write(f, .5f, 20000 + 2 * window, true, revision + 1);
    const auto hiddenAfterWrite = f.broker.stats();
    check(hidden.consumers == 0 && hidden.taps == 0 && hidden.spectralPlans == 0 && f.releaseCalls == 1 &&
              hiddenAfterWrite.transforms == hidden.transforms && hiddenAfterWrite.cursorReads == hidden.cursorReads &&
              hiddenAfterWrite.workerPasses == hidden.workerPasses && hiddenAfterWrite.publications == hidden.publications,
          "closing the final cumulative view leaves no tap, outstanding plan or new worker work");

    Fixture overlapping;
    request.hopSize = window / 2;
    request.outputs = SharedAnalysis::Spectrum;
    auto rollingRaw = overlapping.broker.acquire(request);
    write(overlapping, .875f, 0, true, revision);
    request.outputs = output;
    auto fresh = overlapping.broker.acquire(request);
    const float *zero = nullptr;
    overlapping.tap->write(zero, zero, window / 2, window, true, revision);
    overlapping.broker.pump();
    const auto partial = rollingRaw.read();
    check(partial && partial->firstSequence == window / 2 && !fresh.readCumulativeAverage(),
          "an overlapping window cannot carry pre-admission samples into a new cumulative observer");
    overlapping.tap->write(zero, zero, window / 2, window + window / 2, true, revision);
    overlapping.broker.pump();
    const auto firstFresh = fresh.readCumulativeAverage();
    check(dcIs(firstFresh, 1, 0.) && firstFresh->window->firstSequence == window,
          "the first eligible whole silent window starts the new interval");

    Fixture parked;
    request.hopSize = window;
    auto only = parked.broker.acquire(request);
    constexpr std::int64_t parkedStamp = 800;
    write(parked, .5f, parkedStamp, false, revision);
    const auto parkedFirst = only.readCumulativeAverage();
    const auto parkedQuiet = write(parked, 0.f, parkedStamp, false, revision);
    const auto parkedSecond = only.readCumulativeAverage();
    write(parked, 0.f, parkedStamp, false, revision);
    const auto parkedThird = only.readCumulativeAverage();
    check(dcIs(parkedFirst, 1, .5) && dcIs(parkedSecond, 2, .25) && dcIs(parkedThird, 3, .5 / 3.) &&
              parkedQuiet == 0 && parked.broker.stats().transforms == 1,
          "certified parked zeros lower a nonzero cumulative mean without waking an FFT");

    Fixture belowFloor;
    auto veryQuiet = belowFloor.broker.acquire(request);
    write(belowFloor, 1e-12f, parkedStamp, false, revision);
    write(belowFloor, 0.f, parkedStamp, false, revision);
    const auto lowReading = veryQuiet.readCumulativeAverage();
    check(lowReading && lowReading->processedWindows == 2 && lowReading->binsDbfs.front() == adi::dsp::kDbFloor &&
              belowFloor.broker.stats().transforms == 1,
          "a nonzero history keeps diluting even when it reads at the display floor");

    Fixture initiallyQuiet;
    auto quietOnly = initiallyQuiet.broker.acquire(request);
    write(initiallyQuiet, 0.f, parkedStamp, false, revision);
    const auto quietStart = quietOnly.readCumulativeAverage();
    write(initiallyQuiet, 0.f, parkedStamp, false, revision);
    check(dcIs(quietStart, 1, 0.) && quietOnly.readCumulativeAverage() == quietStart &&
              initiallyQuiet.broker.stats().transforms == 0,
          "an entirely silent stopped interval retains its first floor publication");
    write(initiallyQuiet, .75f, parkedStamp, false, revision);
    check(dcIs(quietOnly.readCumulativeAverage(), 3, .25) && initiallyQuiet.broker.stats().transforms == 1,
          "the first nonzero parked hop retains earlier zeros from its same source epoch");
}

void parkedCumulativeAverage() {
    enum class Boundary { None, Burst, BurstWithScope, Locate, Join, Reset };
    constexpr int window = 480, hop = 240, block = 401, blocks = 33;
    constexpr int quietFrames = block * blocks, untilHop = hop - quietFrames % hop;
    constexpr int burstFrames = 71;
    constexpr std::int64_t stamp = 800;
    constexpr std::uint64_t revision = 4;
    const std::array<float, window> zeros{};
    std::array<float, window> signal;
    signal.fill(.5f);

    SharedAnalysis::Request request{1};
    request.outputs = SharedAnalysis::CumulativeAverage;
    request.fftSize = window;
    request.hopSize = hop;
    request.window = adi::dsp::Window::Rectangular;
    const auto floor = [](const auto &average) {
        return average && !average->binsDbfs.empty() &&
               std::ranges::all_of(average->binsDbfs, [](float db) { return db == adi::dsp::kDbFloor; });
    };
    const auto matches = [](const auto &measured, const auto &expected) {
        return measured && expected && measured->window && expected->window &&
               measured->processedWindows == expected->processedWindows &&
               measured->window->windowSerial == expected->window->windowSerial &&
               measured->window->firstSequence == expected->window->firstSequence &&
               measured->window->firstStamp == expected->window->firstStamp &&
               measured->window->sourceEpoch == expected->window->sourceEpoch &&
               measured->window->overrunCount == expected->window->overrunCount &&
               exactBins(measured->binsDbfs, expected->binsDbfs);
    };

    for (const auto boundary : {Boundary::None, Boundary::Burst, Boundary::BurstWithScope, Boundary::Locate,
                                Boundary::Join, Boundary::Reset}) {
        Fixture parked, reference;
        parked.tap = std::make_shared<ScopeTap>(48000., .25);
        reference.tap = std::make_shared<ScopeTap>(48000., .25);
        auto average = parked.broker.acquire(request);
        auto uninterrupted = reference.broker.acquire(request);
        SharedAnalysis::Lease staged, controlStaged;
        SharedAnalysis::Lease scope, controlScope;
        const bool withScope = boundary == Boundary::BurstWithScope;
        const bool burstBetweenPumps = boundary == Boundary::Burst || withScope;
        if (withScope) {
            auto scopeRequest = request;
            scopeRequest.outputs = SharedAnalysis::Scope;
            scope = parked.broker.acquire(scopeRequest);
            controlScope = reference.broker.acquire(scopeRequest);
        }
        if (boundary == Boundary::Reset) {
            staged = parked.broker.acquire(request);
            controlStaged = reference.broker.acquire(request);
        }
        const auto writePair = [&](const float *samples, int frames, bool pump = true) {
            parked.tap->write(samples, samples, frames, stamp, false, revision);
            // Non-null zero buffers give the control the same samples without a silence certificate.
            const auto *input = samples ? samples : zeros.data();
            reference.tap->write(input, input, frames, stamp, false, revision);
            if (pump) {
                parked.broker.pump();
                reference.broker.pump();
            }
        };

        writePair(nullptr, window);
        const auto first = average.readCumulativeAverage();
        const auto firstStaged = staged.readCumulativeAverage();
        const auto settled = parked.broker.stats();
        check(floor(first) && first->processedWindows == 1 && first->window && !first->window->advancing &&
                  settled.transforms == 0,
              "the first complete stopped Cumulative Average publishes an exact-zero result");
        const auto firstScope = scope.read();
        if (withScope) {
            check(first && firstScope && firstScope == first->window && exactBins(firstScope->scopeLeft, zeros) &&
                      exactBins(firstScope->scopeRight, zeros) && settled.taps == 1 && settled.domains == 1 &&
                      settled.spectralPlans == 1 && settled.consumers == 2,
                  "compatible Scope and Cumulative Average share one zero window, tap and domain");
        }

        for (int i = 0; i < blocks; ++i) writePair(nullptr, block);
        const auto idle = parked.broker.stats();
        const auto quietControl = uninterrupted.readCumulativeAverage();
        check(parked.tap->oldest() > window && floor(quietControl) &&
                  quietControl->processedWindows == 1 + quietFrames / hop &&
                  average.readCumulativeAverage() == first &&
                  idle.workerPasses == settled.workerPasses && idle.cursorReads == settled.cursorReads &&
                  idle.analysisWindows == settled.analysisWindows && idle.transforms == settled.transforms &&
                  idle.publications == settled.publications,
              "periodic pumps do no work for an already-published zero cumulative interval through a ring wrap");
        if (withScope) {
            check(firstScope && scope.read() == firstScope,
                  "a compatible Scope keeps its zero picture while the shared stopped interval wraps");
        }

        constexpr std::int64_t admission = window + quietFrames;
        const bool stagedBoundary = boundary == Boundary::Join || boundary == Boundary::Reset;
        if (boundary == Boundary::Locate) {
            constexpr std::int64_t newStamp = stamp + 2400;
            for (auto *tap : {parked.tap.get(), reference.tap.get()}) {
                tap->write(signal.data(), signal.data(), hop, newStamp, false, revision + 1);
                const auto *silence = tap == parked.tap.get() ? nullptr : zeros.data();
                tap->write(silence, silence, window - hop, newStamp, false, revision + 1);
            }
            parked.broker.pump();
            reference.broker.pump();
            const auto resumed = average.readCumulativeAverage();
            check(first && first->window && matches(resumed, uninterrupted.readCumulativeAverage()) &&
                      resumed->processedWindows == 1 && resumed->window->firstSequence == admission &&
                      resumed->window->firstStamp == newStamp && resumed->window->transportRevision == revision + 1 &&
                      resumed->window->sourceEpoch == first->window->sourceEpoch + 1 &&
                      resumed->window->overrunCount == first->window->overrunCount,
                  "a stopped Locate between pumps starts a fresh interval at the real source fence");
            continue;
        }
        if (boundary == Boundary::None) {
            writePair(signal.data(), untilHop - 1);
            check(average.readCumulativeAverage() == first && uninterrupted.readCumulativeAverage() == quietControl,
                  "stopped monitoring waits for the original partial cumulative hop");
        } else if (burstBetweenPumps) {
            writePair(signal.data(), burstFrames, false);
            writePair(nullptr, untilHop - burstFrames - 1, false);
            const auto followingQuiet = parked.tap->lastParkedSilence();
            check(followingQuiet.begin == admission + burstFrames && followingQuiet.end == admission + untilHop - 1 &&
                      average.readCumulativeAverage() == first && parked.broker.stats().workerPasses == idle.workerPasses,
                  "a short audible burst and a fresh silence certificate can both arrive between worker passes");
        } else {
            if (boundary == Boundary::Join) {
                staged = parked.broker.acquire(request);
                controlStaged = reference.broker.acquire(request);
            } else {
                check(staged.restartCumulativeAverage() && controlStaged.restartCumulativeAverage() &&
                          floor(firstStaged) && firstStaged->processedWindows == 1,
                      "resetting one zero interval preserves its previous immutable publication");
            }
            parked.broker.pump();
            reference.broker.pump();
            check(staged.available() && !staged.readCumulativeAverage() && !controlStaged.readCumulativeAverage() &&
                      average.readCumulativeAverage() == first,
                  "late admission or reset does not publish or replace a surviving cumulative interval");
            writePair(nullptr, window);
            writePair(nullptr, untilHop - 1);
            check(!staged.readCumulativeAverage() && !controlStaged.readCumulativeAverage(),
                  "a late or reset interval excludes every full window that starts before its writer fence");
        }

        writePair(burstBetweenPumps ? nullptr : signal.data(), 1);
        const auto resumed = average.readCumulativeAverage();
        const auto expected = uninterrupted.readCumulativeAverage();
        const auto activeStats = parked.broker.stats();
        constexpr auto resumedCount = std::uint64_t{2} + quietFrames / hop;
        const auto additionalHops = stagedBoundary ? std::uint64_t{window / hop} : std::uint64_t{0};
        check(first && first->window && matches(resumed, expected) &&
                  resumed->processedWindows == resumedCount + additionalHops &&
                  resumed->window->firstStamp == stamp && resumed->window->transportRevision == revision &&
                  resumed->window->sourceEpoch == first->window->sourceEpoch &&
                  resumed->window->overrunCount == first->window->overrunCount &&
                  !resumed->binsDbfs.empty() && resumed->binsDbfs.front() > adi::dsp::kDbFloor &&
                  first->processedWindows == 1 && activeStats.taps == 1 && activeStats.spectralPlans == 1 &&
                  activeStats.transforms == 1,
              burstBetweenPumps
                  ? "a burst between pumps preserves the cumulative denominator, bins and clock without an overrun"
                  : "the first resumed cumulative hop preserves the full denominator, immutable picture and source clock");
        if (withScope) {
            const auto visible = scope.read(), expectedScope = controlScope.read();
            constexpr std::size_t firstBurst = window - untilHop;
            check(resumed && visible && expectedScope && visible == resumed->window && visible != firstScope &&
                      visible->firstSequence == expectedScope->firstSequence && visible->firstStamp == expectedScope->firstStamp &&
                      visible->windowSerial == expectedScope->windowSerial && visible->transportRevision == revision &&
                      !visible->advancing && exactBins(visible->scopeLeft, expectedScope->scopeLeft) &&
                      exactBins(visible->scopeRight, expectedScope->scopeRight) && visible->scopeLeft.size() == window &&
                      visible->scopeLeft[firstBurst - 1] == 0.f && visible->scopeLeft[firstBurst] == .5f &&
                      visible->scopeLeft[firstBurst + burstFrames - 1] == .5f &&
                      visible->scopeLeft[firstBurst + burstFrames] == 0.f && activeStats.domains == 1,
                  "the shared Scope wakes on the original hop with the exact samples and source clock");
        }
        if (stagedBoundary) {
            const auto next = staged.readCumulativeAverage();
            check(resumed && matches(next, controlStaged.readCumulativeAverage()) && next->processedWindows == 1 &&
                      next->window == resumed->window && next->window->firstSequence == admission + untilHop,
                  "the first post-fence window shares the source transform without inheriting another interval's count");
        }
    }
}

void cumulativeMaximumParkedRollover() {
    constexpr int window = 1024;
    constexpr auto revision = std::uint64_t{19};
    constexpr auto stamp = std::int64_t{400};
    std::array<float, window> loud{};
    loud.fill(.75f);
    const float *quiet = nullptr;

    for (const bool restart : {false, true}) {
        Fixture f;
        f.tap = std::make_shared<ScopeTap>(48000., .25);
        SharedAnalysis::Request request{1, ScopePoint::PostFader, window};
        request.outputs = SharedAnalysis::CumulativeMaximum;
        auto retained = f.broker.acquire(request);
        auto second = restart ? f.broker.acquire(request) : SharedAnalysis::Lease{};
        f.tap->write(loud.data(), loud.data(), window, stamp, false, revision);
        f.broker.pump();
        f.tap->write(quiet, quiet, window, stamp, false, revision);
        f.broker.pump();
        const auto held = retained.readMaximum();
        check(held && held->binsDbfs[0] > -10.f, "the parked source has a cumulative peak worth retaining");
        if (!held) continue;

        const auto settled = f.broker.stats();
        const auto beyondRing = static_cast<int>(f.tap->capacity()) + window;
        f.tap->write(quiet, quiet, beyondRing, stamp, false, revision);
        f.broker.pump();
        check(settled.cursorReads == f.broker.stats().cursorReads && retained.readMaximum() == held,
              "cumulative silence stays parked after the physical ring rolls over");

        const auto admission = f.tap->written();
        if (restart)
            check(second.restartCumulativeMaximum(), "Reset accepts a parked cumulative observer");
        else
            second = f.broker.acquire(request);
        check(!second.readMaximum(), "a new cumulative interval starts without stale parked peaks");
        f.tap->write(quiet, quiet, window, stamp, false, revision);
        f.broker.pump();
        const auto fresh = second.readMaximum();
        const auto continuing = retained.readMaximum();
        const auto after = f.broker.stats();
        check(fresh && fresh->window->firstSequence >= admission && fresh->binsDbfs[0] == adi::dsp::kDbFloor,
              "a full new quiet window appears after a parked observer joins or resets");
        check(continuing && exactBins(continuing->binsDbfs, held->binsDbfs) &&
                  continuing->window->sourceEpoch == held->window->sourceEpoch &&
                  continuing->window->overrunCount == held->window->overrunCount &&
                  after.cursorReads - settled.cursorReads <= 1 + window / (window / 4) &&
                  after.transforms == settled.transforms,
              "rejoining a parked tap skips certified quiet without treating rollover as a sibling gap");

        f.tap->write(loud.data(), loud.data(), window, stamp + 17, true, revision + 1);
        f.broker.pump();
        const auto resumed = retained.readMaximum();
        check(resumed && resumed->window->transportRevision == revision + 1 &&
                  resumed->window->sourceEpoch > held->window->sourceEpoch &&
                  resumed->window->overrunCount == held->window->overrunCount,
              "skipping parked silence still detects authentic transport changes on resume");
    }
}
void focusScopeBarTiming() {
    constexpr auto quarter = adi::textproj::kPPQ;
    const adi::engine::TempoMap constant{{{0, 120., 0}}};
    using adi::ui::focusScopeBarAtTick;
    const auto start = focusScopeBarAtTick(0, 48000., constant, {});
    const auto inside = focusScopeBarAtTick(3 * quarter, 48000., constant, {});
    const auto next = focusScopeBarAtTick(4 * quarter, 48000., constant, {});
    check(start && inside && next && start->firstTick == 0 && start->nextTick == 4 * quarter &&
              start->range.first == 0 && start->range.frames == 96000 && start->range.sampleRate == 48000. &&
              inside->range.first == start->range.first && inside->range.frames == start->range.frames &&
              next->firstTick == start->nextTick && next->range.first == 96000 && next->range.frames == 96000,
          "the Focus interval stays anchored to its bar and advances at the next boundary");

    const adi::engine::TempoMap change{{{0, 120., 0}, {2 * quarter, 60., 0}}};
    const auto stepped = focusScopeBarAtTick(3 * quarter, 48000., change, {});
    check(stepped && stepped->range.first == 0 && stepped->range.frames == 144000,
          "one bar integrates the tempo on both sides of a tempo step");

    const std::vector<adi::textproj::Meter> signatures{{0, 4, 4}, {2 * quarter, 6, 8}};
    const auto fragment = focusScopeBarAtTick(quarter, 48000., constant, signatures);
    const auto compound = focusScopeBarAtTick(2 * quarter, 48000., constant, signatures);
    check(fragment && compound && fragment->nextTick == 2 * quarter && fragment->range.frames == 48000 &&
              compound->firstTick == fragment->nextTick && compound->nextTick == 5 * quarter &&
              compound->range.first == 48000 && compound->range.frames == 72000,
          "a signature change ends the short bar and starts a real six-eighths interval");

    const adi::engine::TempoMap fractional{{{0, 123., 0}}};
    const auto firstEdge = focusScopeBarAtTick(0, 48000., fractional, {});
    const auto secondEdge = focusScopeBarAtTick(4 * quarter, 48000., fractional, {});
    check(firstEdge && secondEdge && firstEdge->range.frames == 93659 && secondEdge->range.first == 93659 &&
              secondEdge->range.frames == 93658,
          "adjacent bars round absolute project samples to one shared edge");
    check(!focusScopeBarAtTick(-1, 48000., constant, {}) && !focusScopeBarAtTick(0, 0., constant, {}) &&
              !focusScopeBarAtTick(0, 48000., {}, {}) &&
              !focusScopeBarAtTick(std::numeric_limits<std::int64_t>::max(), 48000., constant, {}),
          "negative ticks, missing timing and overflowing bars produce no Focus interval");
}
void focusScopeBeatTiming() {
    constexpr auto quarter = adi::textproj::kPPQ;
    constexpr auto eighth = quarter / 2;
    constexpr auto sixteenth = quarter / 4;
    constexpr double rate = 48000.;
    using adi::ui::FocusScopeWindow;
    using adi::ui::focusScopeBarAtTick;
    using adi::ui::focusScopeWindowAtTick;
    const std::vector<adi::textproj::Meter> equalMeter;
    const adi::engine::TempoMap constant{{{0, 120., 0}}};
    constexpr auto beat = FocusScopeWindow::Beat;

    const auto quarterWindow = focusScopeWindowAtTick(quarter + sixteenth, rate, constant, equalMeter, beat);
    const auto bar = focusScopeBarAtTick(quarter + sixteenth, rate, constant, equalMeter);
    check(quarterWindow && quarterWindow->firstTick == quarter && quarterWindow->nextTick == 2 * quarter &&
              quarterWindow->range.first == 24000 && quarterWindow->range.frames == 24000 &&
              quarterWindow->range.sampleRate == rate && bar && bar->range.first == 0 && bar->range.frames == 96000,
          "a 4/4 beat follows its own sample edges without changing the ordinary bar wrapper");

    constexpr auto cut = 2 * quarter + sixteenth;
    const std::vector<adi::textproj::Meter> split{{0, 4, 4}, {cut, 6, 8}};
    const auto lastQuarter = focusScopeWindowAtTick(cut - 1, rate, constant, split, beat);
    const auto firstEighth = focusScopeWindowAtTick(cut, rate, constant, split, beat);
    const auto secondEighth = focusScopeWindowAtTick(cut + eighth, rate, constant, split, beat);
    check(lastQuarter && firstEighth && secondEighth && lastQuarter->firstTick == 2 * quarter &&
              lastQuarter->nextTick == cut && lastQuarter->range.first == 48000 && lastQuarter->range.frames == 6000 &&
              firstEighth->firstTick == cut && firstEighth->nextTick == cut + eighth &&
              firstEighth->range.first == 54000 && firstEighth->range.frames == 12000 &&
              secondEighth->firstTick == firstEighth->nextTick && secondEighth->range.first == 66000,
          "a mid-beat signature cuts the 4/4 interval and anchors the next 6/8 denominator beat at its exact edge");

    const adi::engine::TempoMap stepped{{{0, 120., 0}, {eighth, 60., 0}}};
    const auto acrossTempo = focusScopeWindowAtTick(eighth + sixteenth, rate, stepped, equalMeter, beat);
    const auto afterTempo = focusScopeWindowAtTick(quarter, rate, stepped, equalMeter, beat);
    check(acrossTempo && afterTempo && acrossTempo->firstTick == 0 && acrossTempo->nextTick == quarter &&
              acrossTempo->range.first == 0 && acrossTempo->range.frames == 36000 && afterTempo->range.first == 36000 &&
              afterTempo->range.frames == 48000,
          "the beat integrates both parts of a tempo step before rounding its absolute edge");

    const adi::engine::TempoMap fractional{{{0, 123., 0}}};
    const auto edge1 = focusScopeWindowAtTick(0, rate, fractional, equalMeter, beat);
    const auto edge2 = focusScopeWindowAtTick(quarter, rate, fractional, equalMeter, beat);
    const auto edge3 = focusScopeWindowAtTick(2 * quarter, rate, fractional, equalMeter, beat);
    check(edge1 && edge2 && edge3 && edge1->range.frames == 23415 && edge2->range.first == 23415 &&
              edge2->range.frames == 23414 && edge3->range.first == 46829 && edge3->range.frames == 23415,
          "three fractional beats share rounded project sample edges without accumulating error");
    const auto previousSample = focusScopeWindowAtSample(46828, rate, fractional, equalMeter, beat);
    const auto boundarySample = focusScopeWindowAtSample(46829, rate, fractional, equalMeter, beat);
    check(previousSample && boundarySample && previousSample->range.first == 23415 &&
              boundarySample->range.first == 46829 && boundarySample->firstTick == 2 * quarter,
          "a fresh request on a rounded-down edge starts the new beat rather than a finished one");
    const auto beforeTie = focusScopeWindowAtSample(24000, 48001., constant, equalMeter, beat);
    const auto afterTie = focusScopeWindowAtSample(24001, 48001., constant, equalMeter, beat);
    check(beforeTie && afterTie && beforeTie->firstTick == 0 && afterTie->firstTick == quarter &&
              afterTie->range.first == 24001,
          "an exact half-sample edge belongs to the higher sample with no early beat label");

    constexpr auto note = FocusScopeWindow::Sixteenth;
    const auto note1 = focusScopeWindowAtSample(0, rate, fractional, equalMeter, note);
    const auto note2 = focusScopeWindowAtSample(5854, rate, fractional, equalMeter, note);
    const auto note3 = focusScopeWindowAtSample(11707, rate, fractional, equalMeter, note);
    check(note1 && note2 && note3 && note1->range.first == 0 && note1->range.frames == 5854 &&
              note2->range.first == 5854 && note2->range.frames == 5853 &&
              note3->range.first == 11707 && note3->range.frames == 5854,
          "three fractional sixteenths share absolute sample edges, including a rounded-down start");
    const std::vector<adi::textproj::Meter> shortBar{{0, 3, 32}};
    const auto lastHalf = focusScopeWindowAtSample(8779, rate, fractional, shortBar, note);
    const auto nextBarNote = focusScopeWindowAtSample(8780, rate, fractional, shortBar, note);
    check(lastHalf && nextBarNote && lastHalf->range.first == 5854 && lastHalf->range.frames == 2926 &&
              nextBarNote->range.first == 8780 && nextBarNote->range.frames == 5854,
          "a sixteenth clips to the end of a 3/32 bar and restarts at its rounded sample edge");
    const auto splitTick = sixteenth + sixteenth / 2;
    const std::vector<adi::textproj::Meter> splitNote{{0, 4, 4}, {splitTick, 6, 8}};
    const auto beforeSplit = focusScopeWindowAtTick(splitTick - 1, rate, constant, splitNote, note);
    const auto afterSplit = focusScopeWindowAtTick(splitTick, rate, constant, splitNote, note);
    check(beforeSplit && afterSplit && beforeSplit->range.first == 6000 && beforeSplit->range.frames == 3000 &&
              afterSplit->range.first == 9000 && afterSplit->range.frames == 6000 &&
              !focusScopeWindowAtTick(std::numeric_limits<std::int64_t>::max(), rate, constant, equalMeter, note),
          "a mid-note signature starts a fresh grid and a note cannot overflow the timeline");

    constexpr auto twoBars = FocusScopeWindow::TwoBars;
    constexpr auto fourBars = FocusScopeWindow::FourBars;
    const auto steppedPair = focusScopeWindowAtTick(5 * quarter, rate, stepped, equalMeter, twoBars);
    const auto nextFour = focusScopeWindowAtSample(384000, rate, constant, equalMeter, fourBars);
    check(steppedPair && steppedPair->firstTick == 0 && steppedPair->nextTick == 8 * quarter &&
              steppedPair->range.frames == 372000 && nextFour && nextFour->firstTick == 16 * quarter &&
              nextFour->range.first == 384000 && nextFour->range.frames == 384000,
          "multi-bar windows stay on the project grid and integrate tempo changes before rounding the sample ends");
    const auto lastFourSample = focusScopeWindowAtSample(374633, rate, fractional, equalMeter, fourBars);
    const auto roundedFourEdge = focusScopeWindowAtSample(374634, rate, fractional, equalMeter, fourBars);
    check(lastFourSample && roundedFourEdge && lastFourSample->range.first == 0 &&
              roundedFourEdge->range.first == 374634 && roundedFourEdge->firstTick == 16 * quarter,
          "the rounded-down four-bar sample edge starts the new group exactly at the heard boundary");
    const std::vector<adi::textproj::Meter> groupSplit{{0, 4, 4}, {5 * quarter, 6, 8}};
    const auto truncated = focusScopeWindowAtTick(4 * quarter, rate, constant, groupSplit, twoBars);
    const auto afterGroupSplit = focusScopeWindowAtSample(120000, rate, constant, groupSplit, fourBars);
    check(truncated && truncated->firstTick == 0 && truncated->nextTick == 5 * quarter &&
              truncated->range.frames == 120000 && afterGroupSplit && afterGroupSplit->firstTick == 5 * quarter &&
              afterGroupSplit->nextTick == 17 * quarter && afterGroupSplit->range.first == 120000 &&
              afterGroupSplit->range.frames == 288000 &&
              !focusScopeWindowAtTick(std::numeric_limits<std::int64_t>::max(), rate, constant, equalMeter, fourBars),
          "a signature change ends the old collection and anchors a fresh group, even within an unfinished bar");

    const auto grid = [&](adi::ui::FocusScopeRange range, const auto &tempo, const auto &meters) {
        std::vector<std::pair<std::int64_t, bool>> marks;
        adi::ui::forEachFocusScopeGridLine(range, tempo, meters,
            [&](std::int64_t sample, bool barLine) { marks.emplace_back(sample, barLine); });
        return marks;
    };
    check(grid({0, 93659, rate}, fractional, equalMeter) ==
              std::vector<std::pair<std::int64_t, bool>>{{0, true}, {23415, false}, {46829, false}, {70244, false}} &&
              grid({46829, 23415, rate}, fractional, equalMeter) ==
              std::vector<std::pair<std::int64_t, bool>>{{46829, false}},
          "beat and bar marks use rounded heard samples and exclude the next window's first sample");
    check(grid({30000, 37000, rate}, constant, split) ==
              std::vector<std::pair<std::int64_t, bool>>{{48000, false}, {54000, true}, {66000, false}} &&
              grid({30000, 55000, rate}, stepped, equalMeter) ==
              std::vector<std::pair<std::int64_t, bool>>{{36000, false}, {84000, false}},
          "the grid follows a truncated bar at a signature change and an interval crossing a tempo change");
    constexpr std::int64_t samplesPerBeat = 750;
    constexpr std::size_t beatsPerBar = 255, visibleBars = 4;
    const std::vector<adi::textproj::Meter> dense{{0, beatsPerBar, 128}};
    const auto denseBar = grid({0, beatsPerBar * samplesPerBeat, rate}, constant, dense);
    const auto denseFourBars = grid({0, beatsPerBar * visibleBars * samplesPerBeat, rate}, constant, dense);
    const auto matchesDense = [&](const auto &marks, std::size_t beats) {
        if (marks.size() != beats) return false;
        for (std::size_t i = 0; i < marks.size(); ++i)
            if (marks[i].first != static_cast<std::int64_t>(i) * samplesPerBeat ||
                marks[i].second != (i % beatsPerBar == 0)) return false;
        return true;
    };
    check(matchesDense(denseBar, beatsPerBar), "the grid includes every beat of the largest project signature");
    check(matchesDense(denseFourBars, beatsPerBar * visibleBars),
          "a dense four-bar extent retains all of its bar and beat lines through its last sample");
    using adi::ui::FocusScopeRange;
    using adi::ui::focusScopeTimedWindow;
    const auto firstTimed = focusScopeTimedWindow(12345, rate, 85);
    const auto nextTimed = focusScopeTimedWindow(16425, rate, 85);
    check(firstTimed == FocusScopeRange{12345, 4080, rate} && nextTimed == FocusScopeRange{16425, 4080, rate} &&
              focusScopeTimedWindow(0, 44100., 85) == FocusScopeRange{0, 3749, 44100.} &&
              focusScopeTimedWindow(0, 44100., 170) == FocusScopeRange{0, 7497, 44100.} &&
              focusScopeTimedWindow(0, rate, 340) == FocusScopeRange{0, 16320, rate},
          "fixed-time windows use absolute sample starts, whole-frame rounding and contiguous ends");
    check(!focusScopeTimedWindow(-1, rate, 85) && !focusScopeTimedWindow(0, 0., 85) &&
              !focusScopeTimedWindow(0, rate, 0) &&
              !focusScopeTimedWindow(std::numeric_limits<std::int64_t>::max() - 10, rate, 85),
          "fixed-time windows reject unusable rates, lengths and timeline overflow");
}

void focusScopeBarProjection() {
    using adi::ui::FocusScopeBarBlock;
    using adi::ui::FocusScopeBarProjection;
    using adi::ui::FocusScopeBarSource;
    using adi::ui::FocusSumColumn;
    constexpr std::size_t frames = 96000, columns = 257, slice = 193;
    constexpr std::uint64_t revision = 17;
    const adi::engine::TempoMap tempo{{{0, 120., 0}}};
    const auto bar = adi::ui::focusScopeBarAtTick(4 * adi::textproj::kPPQ, 48000., tempo, {});
    check(bar && bar->range.first == 96000 && bar->range.frames == frames,
          "the second 48 kHz 120 BPM bar owns exactly 96000 samples at a nonzero stamp");
    if (!bar) return;
    constexpr std::array sources{FocusScopeBarSource{{7}, 11}, FocusScopeBarSource{{8}, 15}};
    FocusScopeBarProjection projection;
    const bool prepared = projection.prepare(bar->range, sources, columns, revision);
    check(prepared && projection.frames() == 0 && !projection.complete() &&
              projection.memberColumns(0).empty() && projection.sumColumns().empty() && !projection.sumReading(),
          "one-bar projection is ready before samples arrive and exposes no unfinished bins");
    if (!prepared) return;

    const auto values = [](std::size_t at) {
        const auto left = static_cast<float>(static_cast<int>(at % 31) - 15) / 32.f;
        const auto right = static_cast<float>(static_cast<int>(at % 19) - 9) / 32.f;
        if (at == frames - 1) return std::array{.875f, .875f, -.875f, .875f};
        if (at == frames) return std::array{0.f, 99.f, 0.f, 99.f};
        return std::array{left, right, -left, right * .5f};
    };
    std::array<float, slice> aLeft{}, aRight{}, bLeft{}, bRight{};
    const auto blocksAt = [&](std::size_t offset, std::size_t count) {
        for (std::size_t i = 0; i < count; ++i) {
            const auto input = values(offset + i);
            aLeft[i] = input[0]; aRight[i] = input[1];
            bLeft[i] = input[2]; bRight[i] = input[3];
        }
        const auto stamp = bar->range.first + static_cast<std::int64_t>(offset);
        return std::array{
            FocusScopeBarBlock{sources[0], stamp, revision, 48000., true,
                              std::span(aLeft).first(count), std::span(aRight).first(count)},
            FocusScopeBarBlock{sources[1], stamp, revision, 48000., true,
                              std::span(bLeft).first(count), std::span(bRight).first(count)}};
    };

    allocationSizeCount = 0;
    observeAllocationSizes = true;
    bool allAccepted = projection.append(blocksAt(0, slice));
    observeAllocationSizes = false;
    check(allAccepted && allocationSizeCount == 0 && projection.frames() == slice &&
              projection.memberColumns(0).size() == 1 && projection.sumColumns().size() == 1,
          "the first short block populates the current bin without allocating or padding its missing samples");
    const auto firstReading = projection.sumReading();
    check(firstReading && firstReading->peak[0] == 0., "signed cancellation works before the bar is complete");

    const auto reject = [&](std::array<FocusScopeBarBlock, 2> invalid) {
        return !projection.append(invalid) && projection.frames() == slice;
    };
    auto gap = blocksAt(slice + 1, 1);
    auto overlap = blocksAt(slice - 1, 1);
    bool refused = reject(gap) && reject(overlap);
    auto bad = blocksAt(slice, 1);
    bad[1].firstStamp += 1;
    refused = reject(bad) && refused;
    bad = blocksAt(slice, 1);
    bad[1].transportRevision += 1;
    refused = reject(bad) && refused;
    bad = blocksAt(slice, 1);
    bad[1].sampleRate = 44100.;
    refused = reject(bad) && refused;
    bad = blocksAt(slice, 1);
    bad[1].advancing = false;
    refused = reject(bad) && refused;
    bad = blocksAt(slice, 1);
    bad[1].source.generation += 1;
    refused = reject(bad) && refused;
    bad = blocksAt(slice, 1);
    std::swap(bad[0], bad[1]);
    refused = reject(bad) && refused;
    bad = blocksAt(slice, 1);
    bad[1].right = {};
    refused = reject(bad) && refused;
    bad = blocksAt(slice, 1);
    bRight[0] = std::numeric_limits<float>::quiet_NaN();
    refused = reject(bad) && refused;
    check(refused && projection.sumColumns().size() == 1 && firstReading && projection.sumReading() &&
              projection.sumReading()->peak == firstReading->peak && projection.sumReading()->rms == firstReading->rms,
          "invalid stamps, lanes, source order, data and timing leave the valid prefix untouched");

    allocationSizeCount = 0;
    observeAllocationSizes = true;
    for (std::size_t offset = slice; offset < frames - 1;) {
        const auto count = std::min(slice, frames - 1 - offset);
        allAccepted = projection.append(blocksAt(offset, count)) && allAccepted;
        offset += count;
    }
    observeAllocationSizes = false;
    check(allocationSizeCount == 0 && allAccepted && projection.frames() == frames - 1 && !projection.complete(),
          "all slices stream to the sample before the terminal edge with no heap work in append");
    check(!projection.append(blocksAt(frames - 1, 2)) && projection.frames() == frames - 1 &&
              projection.append(blocksAt(frames - 1, 1)) && projection.complete() && projection.frames() == frames &&
              !projection.append(blocksAt(frames, 1)),
          "the half-open terminal edge includes the last transient and refuses the next bar's sample");

    const auto sum = projection.sumColumns();
    const auto a = projection.memberColumns(0);
    const auto b = projection.memberColumns(1);
    double energy = 0.;
    bool matches = sum.size() == columns && a.size() == columns && b.size() == columns;
    for (std::size_t column = 0; column < columns && matches; ++column) {
        constexpr auto inf = std::numeric_limits<double>::infinity();
        std::array<FocusSumColumn, 3> expected{
            FocusSumColumn{{inf, inf}, {-inf, -inf}}, FocusSumColumn{{inf, inf}, {-inf, -inf}},
            FocusSumColumn{{inf, inf}, {-inf, -inf}}};
        for (std::size_t at = column * frames / columns; at < (column + 1) * frames / columns; ++at) {
            const auto input = values(at);
            const auto right = static_cast<double>(input[1]) + input[3];
            const std::array amps{std::array<double, 2>{input[0], input[1]},
                                  std::array<double, 2>{input[2], input[3]}, std::array{0., right}};
            energy += right * right;
            for (std::size_t lane = 0; lane < amps.size(); ++lane)
                for (std::size_t ch = 0; ch < 2; ++ch) {
                    expected[lane].low[ch] = std::min(expected[lane].low[ch], amps[lane][ch]);
                    expected[lane].high[ch] = std::max(expected[lane].high[ch], amps[lane][ch]);
                }
        }
        const std::array actual{a[column], b[column], sum[column]};
        for (std::size_t lane = 0; lane < actual.size(); ++lane)
            matches = matches && actual[lane].low == expected[lane].low && actual[lane].high == expected[lane].high;
    }
    const auto reading = projection.sumReading();
    check(matches && sum.back().high[1] == 1.75 && sum.back().high[0] == 0. &&
              reading && reading->peak == std::array{0., 1.75} &&
              reading->rms == std::array{0., std::sqrt(energy / static_cast<double>(frames))},
          "the scalar oracle confirms every unscaled stereo bin and signed peak/RMS after the 96000-frame bar");

    FocusScopeBarProjection tiny;
    auto one = bar->range;
    one.frames = 1;
    const bool oneReady = tiny.prepare(one, sources, columns, revision);
    check(oneReady && tiny.append(blocksAt(0, 1)) && tiny.complete() && tiny.sumColumns().size() == 1,
          "more requested columns than samples clamp to one nonempty bin");
}
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
void focusBarScopeBroker() {
    using Source = adi::engine::ScopeSource;
    using Status = SharedAnalysis::BarScopeStatus;
    const std::array sources{Source{1, ScopePoint::PostFader, 0}, Source{2, ScopePoint::PostFader, 0}};
    std::array<std::shared_ptr<ScopeTap>, 2> taps;
    unsigned attached = 0, released = 0;
    SharedAnalysis broker([&](Source source, double seconds) -> std::shared_ptr<const ScopeTap> {
        const auto i = static_cast<std::size_t>(source.track - 1);
        if (!taps[i]) { ++attached; taps[i] = std::make_shared<ScopeTap>(48000., seconds); }
        return taps[i];
    }, [&](Source) { ++released; }, SharedAnalysis::Mode::Manual);
    auto lease = broker.acquireBarScope(sources, 64);
    check(lease && lease.sampleRate() == 48000. && attached == 2 && taps[0]->capacity() == 12000 &&
              taps[1]->capacity() == 12000 && lease.request({0, 96000, 48000.}, 9),
          "a one-bar Focus request uses the two actual quarter-second source taps");
    if (!lease) return;

    std::array<std::int64_t, 2> heads{};
    const auto feed = [&](std::size_t i, std::int64_t end) {
        std::array<float, 256> left{}, right{};
        const auto count = static_cast<std::size_t>(end - heads[i]);
        for (std::size_t frame = 0; frame < count; ++frame) {
            const auto stamp = heads[i] + static_cast<std::int64_t>(frame);
            if (stamp == 0 || stamp == 95999) left[frame] = i == 0 ? .75f : -.75f;
            if (stamp == 95999) right[frame] = .25f;
        }
        taps[i]->write(left.data(), right.data(), static_cast<std::int32_t>(count), heads[i], true, 9);
        heads[i] = end;
    };
    feed(0, 256);
    broker.pump();
    const auto beforeWait = broker.stats();
    broker.pump();
    check(lease.status() == Status::Pending && !lease.read() && broker.stats().cursorReads == beforeWait.cursorReads,
          "a stalled peer leaves the staged fast samples intact without repeated reads");
    feed(1, 97);
    broker.pump();
    feed(1, 256);
    broker.pump();
    for (std::int64_t end = 512; end < 95999; end += 256) {
        feed(0, end);
        feed(1, end);
        broker.pump();
    }
    feed(0, 96000);
    feed(0, 96017);
    feed(1, 95999);
    broker.pump();
    check(lease.status() == Status::Pending && !lease.read(), "a bar waits for the last sample of every member");
    feed(1, 96017);
    broker.pump();
    const auto result = lease.read();
    check(lease.status() == Status::Ready && result && result->firstStamp == 0 && result->frames == 96000 &&
              result->sampleRate == 48000. && result->transportRevision == 9 && result->projection &&
              result->projection->complete() && result->projection->memberColumns(0).front().high[0] == .75 &&
              result->projection->memberColumns(1).back().low[0] == -.75 &&
              result->projection->sumReading()->peak == std::array{0., .5} && broker.stats().transforms == 0,
          "the complete bar retains its earliest peaks and cancels signed samples with no FFT");

    check(lease.request({96000, 17, 48000.}, 9), "the same lease can request the adjacent range");
    broker.pump();
    const auto adjacent = lease.read();
    check(adjacent && adjacent->firstStamp == 96000 && adjacent->frames == 17,
          "adjacent capture uses the staged unequal suffix without waiting for new input");

    auto companion = broker.acquire({1});
    check(companion.available() && attached == 2, "a companion spectrum shares the unchanged physical tap");
    lease.reset();
    const auto beforeIdle = broker.stats();
    broker.pump();
    check(beforeIdle.domains == 1 && released == 1 && !lease && result && result->projection->complete(),
          "hiding the group releases only unshared demand and preserves held immutable results");
    companion.reset();
    check(released == 2 && broker.stats().taps == 0, "the last subscriber releases each tap exactly once");
}
void focusSingleSourceScope() {
    using Channels = adi::ui::FocusScopeChannels;
    using Source = adi::engine::ScopeSource;
    using Status = SharedAnalysis::BarScopeStatus;
    constexpr int frames = 4080;
    constexpr double rate = 48000.;
    constexpr std::uint64_t revision = 53;
    const std::array sources{Source{1, ScopePoint::PostFader, 0}};
    auto tap = std::make_shared<ScopeTap>(rate, .25);
    int detached = 0;
    SharedAnalysis broker([&](Source source, double) -> std::shared_ptr<const ScopeTap> {
        return source == sources[0] ? tap : nullptr;
    }, [&](Source) { ++detached; }, SharedAnalysis::Mode::Manual);
    auto lease = broker.acquireBarScope(sources, 64);
    check(lease && broker.stats().taps == 1, "an individual Focus Scope can own one heard source");
    if (!lease) return;
    lease.setPhaseRequested(true);
    const SharedAnalysis::ScopeDifference pair{sources[0], Source{2, ScopePoint::PostFader, 0},
                                               adi::ui::FocusScopeChannels::Stereo, 1, true};
    lease.setDifferenceRequested(pair);
    check(lease.request({0, frames, rate}, revision), "one source accepts a complete 85 ms display request");
    std::array<float, frames> left{}, right{};
    left.front() = .25f; right.front() = -.125f;
    left.back() = .75f; right.back() = -.375f;
    tap->write(left.data(), right.data(), frames - 1, 0, true, revision);
    broker.pump();
    check(lease.status() == Status::Pending && !lease.read(), "the single source waits for its last sample");
    tap->write(&left.back(), &right.back(), 1, frames - 1, true, revision);
    broker.pump();
    const auto picture = lease.read();
    check(picture && picture->frames == frames && picture->firstStamp == 0 && picture->transportRevision == revision &&
              picture->projection && picture->projection->memberColumns(0).front().high[0] == .25 &&
              picture->projection->memberColumns(0).back().high[0] == .75 &&
              picture->projection->sumReading()->peak == std::array{.75, .375} &&
              picture->projection->phase(false) == -1. && !picture->projection->phase(true) &&
              !picture->differenceSpectrum(pair) && broker.stats().transforms == 0,
          "one source keeps endpoint extrema and L/R phase with no pair phase or spectrum transform");

    constexpr std::array<std::optional<Channels>, 1> sideView{Channels::Side};
    lease.setLayerChannelViews(sideView);
    check(lease.request({frames, 2, rate}, revision), "one source accepts its own layer channel view");
    constexpr std::array nextLeft{.5f, -.5f}, nextRight{-.25f, .25f};
    tap->write(nextLeft.data(), nextRight.data(), 2, frames, true, revision);
    broker.pump();
    const auto solo = lease.read();
    const auto side = solo && solo->projection ? solo->projection->memberMonoColumns(0, Channels::Side)
                                               : std::span<const adi::ui::FocusScopeBarProjection::MonoColumn>{};
    check(side.size() == 2 && side.front().high == .375 && side.back().low == -.375 &&
              solo->projection->sumReading()->peak == std::array{.5, .25} &&
              solo->projection->phase(false) == -1. && !solo->projection->phase(true) &&
              broker.stats().taps == 1 && broker.stats().transforms == 0,
          "a single Side layer preserves the stereo measurement and uses the original tap");
    lease.reset();
    check(detached == 1 && broker.stats().taps == 0 && picture && picture->projection->complete(),
          "the released single tap leaves a complete held publication");
}
void focusBarScopeMidDemand() {
    using adi::ui::FocusScopeChannels;
    using Source = adi::engine::ScopeSource;
    using Status = SharedAnalysis::BarScopeStatus;
    constexpr double rate = 48000.;
    constexpr std::uint64_t revision = 43;
    const std::array sources{Source{1, ScopePoint::PostFader, 0}, Source{2, ScopePoint::PostFader, 0}};
    const std::array taps{std::make_shared<ScopeTap>(rate, .25), std::make_shared<ScopeTap>(rate, .25)};
    unsigned acquires = 0, releases = 0;
    SharedAnalysis broker([&](Source source, double) -> std::shared_ptr<const ScopeTap> {
        ++acquires;
        return taps[static_cast<std::size_t>(source.track - 1)];
    }, [&](Source) { ++releases; }, SharedAnalysis::Mode::Manual);
    auto lease = broker.acquireBarScope(sources, 1);
    if (!lease) { check(false, "Mid uses a regular grouped Focus scope lease"); return; }

    const auto write = [&](std::int64_t stamp, float first, float second) {
        const float inverted = -first, silent = 0.f;
        taps[0]->write(&first, &inverted, 1, stamp, true, revision);
        taps[1]->write(&second, &silent, 1, stamp, true, revision);
        broker.pump();
    };
    lease.setMonoRequested(FocusScopeChannels::Mid);
    check(lease.request({0, 2, rate}, revision), "a Mid bar uses the existing capture request");
    write(0, .75f, -.5f);
    check(lease.status() == Status::Pending, "a partial Mid bar is not published");
    write(1, -.75f, .5f);
    const auto mid = lease.read();
    const auto levels = [](const auto &result) {
        return result && result->projection ? result->projection->sumMonoColumns(FocusScopeChannels::Mid)
            : std::span<const adi::ui::FocusScopeBarProjection::MonoColumn>{};
    };
    const auto combined = levels(mid);
    const auto member = mid && mid->projection ? mid->projection->memberMonoColumns(0, FocusScopeChannels::Mid)
        : std::span<const adi::ui::FocusScopeBarProjection::MonoColumn>{};
    check(combined.size() == 1 && combined.front().low == -.25 && combined.front().high == .25 &&
              member.size() == 1 && member.front().low == 0. && member.front().high == 0.,
          "the grouped publication bins signed Mid for members and the sum");

    check(lease.request({2, 2, rate}, revision), "the following bar starts on the same grouped lease");
    write(2, 1.f, .5f);
    lease.setMonoRequested(FocusScopeChannels::Stereo);
    write(3, -1.f, -.5f);
    const auto stereo = lease.read();
    check(stereo && stereo->projection && stereo->projection->sumColumns().size() == 1 &&
              stereo->projection->memberMonoColumns(0, FocusScopeChannels::Mid).empty() && levels(stereo).empty(),
          "turning off Mid discards unpublished Mid extrema without restarting the stereo reader");
    check(levels(mid).size() == 1 && levels(mid).front().high == .25,
          "an already published Mid snapshot stays immutable when the view changes");
    check(acquires == 2 && releases == 0 && broker.stats().taps == 2 &&
              broker.stats().consumers == 1 && broker.stats().transforms == 0,
          "projection changes use the same two taps and grouped reader without FFT work");
    lease.reset();
    check(releases == 2 && broker.stats().taps == 0, "hiding the last grouped reader releases both taps");
}

void focusCapturedTruePeak() {
    using adi::dsp::TruePeakMeter;
    using adi::ui::FocusCapturedTruePeak;
    struct RateCase { double rate; int factor; };
    constexpr std::array rates{RateCase{192000., 1}, RateCase{96000., 2}, RateCase{64000., 3},
        RateCase{48000., 4}, RateCase{40000., 5}, RateCase{32000., 6}, RateCase{27000., 7}, RateCase{24000., 8}};
    constexpr auto phaseDelay = TruePeakMeter::kDelay;
    constexpr auto tapsPerPhase = TruePeakMeter::kTapsPerPhase;
    const auto samplePeak = [](std::span<const double> left, std::span<const double> right) {
        double peak = 0.;
        for (const auto channel : {left, right})
            for (const auto value : channel) peak = std::max(peak, std::abs(value));
        return peak;
    };
    const auto phaseOracle = [&](std::span<const double> left, std::span<const double> right,
                                 int factor, std::span<const double> coefficients) {
        double peak = samplePeak(left, right);
        if (factor == 1) return peak;
        for (const auto channel : {left, right}) {
            const auto frames = static_cast<std::ptrdiff_t>(channel.size());
            for (std::ptrdiff_t sample = 0; sample < frames; ++sample) {
                for (int phase = 0; phase < factor; ++phase) {
                    double value = 0.;
                    for (int tap = 0; tap < tapsPerPhase; ++tap) {
                        const auto at = sample + phaseDelay - tap;
                        if (at >= 0 && at < frames)
                            value += coefficients[static_cast<std::size_t>(phase + factor * tap)] *
                                     channel[static_cast<std::size_t>(at)];
                    }
                    peak = std::max(peak, std::abs(value));
                }
            }
        }
        return peak;
    };
    const auto measure = [&](double rate, std::span<const double> left, std::span<const double> right) {
        FocusCapturedTruePeak capture(rate, left.size());
        const auto original = samplePeak(left, right);
        check(!capture.finish(original), "an incomplete captured interval cannot report a true peak");
        constexpr std::array<std::size_t, 6> chunks{7, 2, 17, 256, 1, 96};
        allocationSizeCount = 0;
        observeAllocationSizes = true;
        for (std::size_t cursor = 0, chunk = 0; cursor < left.size(); ++chunk) {
            const auto next = std::min(left.size(), cursor + chunks[chunk % chunks.size()]);
            for (; cursor < next; ++cursor) capture.append(left[cursor], right[cursor]);
            capture.flushSamples();
        }
        const auto result = capture.finish(original);
        const auto repeated = capture.finish(original);
        observeAllocationSizes = false;
        check(result && result == repeated && allocationSizeCount == 0,
              "appending and finishing the interval allocate nothing, and the result remains idempotent");
        return result;
    };

    std::array<double, 542> left{}, right{};
    constexpr std::array tone{.8, .8, -.8, -.8};
    for (std::size_t i = 0; i < left.size(); ++i) {
        left[i] = tone[i % tone.size()];
        right[i] = tone[(i + 1) % tone.size()] * (i < 24 ? .375 : 1.125);
    }
    constexpr std::array<std::size_t, 14> lengths{1, 2, 3, 4, 5, 6, 7, 8, 9, 15, 16, 17, 257, 542};
    for (const auto [rate, factor] : rates) {
        check(TruePeakMeter::factorFor(rate) == factor, "the capture fixture covers its intended oversampling factor");
        std::vector<double> coefficients;
        TruePeakMeter::design(factor, coefficients);
        bool aligned = true;
        for (const auto length : lengths) {
            const auto l = std::span(left).first(length), r = std::span(right).first(length);
            const auto expected = phaseOracle(l, r, factor, coefficients);
            const auto measured = measure(rate, l, r);
            aligned &= measured && (factor == 1 ? *measured == expected : near(*measured, expected, 2.e-12));
        }
        check(aligned, "captured stereo true peak matches the half-open phase oracle for short and long inputs");
    }

    constexpr std::array tail{.8, .8, -.8}, tailSilence{0., 0., 0.};
    constexpr std::array leading{1., -1., .25, -.25, 1., -.25}, leadingSilence{0., 0., 0., 0., 0., 0.};
    const auto silence = measure(48000., tailSilence, tailSilence);
    const auto tailPeak = measure(48000., tailSilence, tail);
    const auto leadingPeak = measure(48000., leading, leadingSilence);
    check(silence == 0. && tailPeak && near(*tailPeak, 1.14323772182338, 2.e-12),
          "a valid silent result is zero and a three-frame right-lane over requires the FIR tail");
    check(leadingPeak && near(*leadingPeak, 1.0081671313273954, 2.e-12),
          "the warmup's larger negative-time phase cannot leak into the reported interval peak");
    check(measure(1.e-300, tail, tailSilence) == measure(24000., tail, tailSilence),
          "a tiny positive synthetic rate saturates without an out-of-range integer conversion");
}

void focusBarCapturedTruePeak() {
    using namespace adi::ui;
    using Source = adi::engine::ScopeSource;
    constexpr double rate = 48000.;
    constexpr std::int32_t frames = 8192;
    constexpr std::uint64_t revision = 79;
    constexpr std::array sources{Source{1, ScopePoint::PostFader, 0}, Source{2, ScopePoint::PostFader, 0}};
    std::array taps{std::make_shared<ScopeTap>(rate, .5), std::make_shared<ScopeTap>(rate, .5)};
    unsigned opened = 0, closed = 0;
    SharedAnalysis broker([&](Source source, double) -> std::shared_ptr<const ScopeTap> {
        ++opened;
        return taps[static_cast<std::size_t>(source.track - 1)];
    }, [&](Source) { ++closed; }, SharedAnalysis::Mode::Manual);
    auto lease = broker.acquireBarScope(sources, 64);
    if (!lease) { check(false, "captured true peak uses the existing bar scope lease"); return; }
    std::array<float, frames> tone{}, zeros{};
    constexpr std::array waveform{.8f, .8f, -.8f, -.8f};
    for (std::size_t i = 0; i < tone.size(); ++i) tone[i] = waveform[i % waveform.size()];
    const auto feed = [&](std::int64_t start) {
        taps[0]->write(tone.data(), zeros.data(), frames, start, true, revision);
        taps[1]->write(zeros.data(), zeros.data(), frames, start, true, revision);
    };
    check(lease.request({0, frames, rate}, revision), "request a complete bar with true peak disabled");
    feed(0);
    broker.pump();
    const auto dry = lease.read();
    check(dry && dry->projection && !dry->projection->sumTruePeak(), "normal bars carry no captured true-peak work");

    lease.setSumTruePeakRequested(true);
    check(lease.request({frames, frames, rate}, revision), "request true peak on the same stamped sources");
    feed(frames);
    broker.pump();
    check(!lease.read() && lease.status() == SharedAnalysis::BarScopeStatus::Pending,
          "a full backed-up bar cannot exceed one worker pass's phase budget");
    for (int i = 0; i < 5 && !lease.read(); ++i) broker.pump();
    const auto measured = lease.read();
    const auto peak = measured && measured->projection ? measured->projection->sumTruePeak() : std::nullopt;
    check(peak && *peak > 1. && measured->projection->sumReading()->peak[0] == static_cast<double>(.8f) &&
              opened == 2 && broker.stats().transforms == 0 && broker.stats().taps == 2,
          "the captured stereo sum detects intersample overs using the original sources and no transform");

    check(lease.request({2 * frames, frames, rate}, revision), "start another measured bar");
    feed(2 * frames);
    broker.pump();
    lease.setSumTruePeakRequested(false);
    broker.pump();
    const auto cancelled = lease.read();
    check(cancelled && cancelled->projection && !cancelled->projection->sumTruePeak() &&
              peak && measured->projection->sumTruePeak() == peak,
          "disabling true peak cancels unfinished work without mutating the held measurement");
    lease.reset();
    check(closed == 2 && broker.stats().taps == 0, "the measured view leaves no demand when released");
}

void focusScopeDifference() {
    using namespace adi::ui;
    using Source = adi::engine::ScopeSource;
    constexpr double rate = 48000.;
    constexpr std::uint64_t revision = 61;
    constexpr std::array sources{Source{1, ScopePoint::PostFader, 0}, Source{2, ScopePoint::PostFader, 0},
                                 Source{3, ScopePoint::PostFader, 0}};
    constexpr std::array left{.75f, -.75f}, inverted{-.75f, .75f}, right{.25f, .25f}, silent{0.f, 0.f};
    SharedAnalysis::Result a, b;
    for (auto *result : {&a, &b}) {
        result->advancing = true;
        result->transportRevision = revision;
        result->sampleRate = rate;
        result->outputs = SharedAnalysis::Scope;
    }
    a.firstStamp = 1;
    b.firstStamp = 2;
    a.scopeLeft = {9.f, left[0], left[1]};
    a.scopeRight = {9.f, right[0], right[1]};
    b.scopeLeft.assign(inverted.begin(), inverted.end());
    b.scopeRight.assign(silent.begin(), silent.end());
    FocusSumColumn stereo{}, mid{}, null{};
    std::array<double, 2> stereoEnergy{}, foldedEnergy{}, nullEnergy{};
    const auto capture = [](FocusSumColumn &out) { return [&](std::size_t, const auto &column) { out = column; }; };
    allocationSizeCount = 0;
    observeAllocationSizes = true;
    const auto overlap = focusProjectDifference(a, b, 1, FocusScopeChannels::Stereo, capture(stereo), {}, {}, &stereoEnergy);
    observeAllocationSizes = false;
    check(overlap && overlap->first == 2 && overlap->frames == 2 && allocationSizeCount == 0 &&
              stereo.low == std::array{-1.5, .25} && stereo.high == std::array{1.5, .25},
          "short Difference subtracts stamped samples before extrema, with no allocation");
    check(focusReadDifferenceRms(stereoEnergy, 2) == std::array{1.5, .25},
          "the short Difference measures unclipped signed samples over the aligned intersection");
    check(focusProjectDifference(a, b, 1, FocusScopeChannels::Mid, capture(mid), {}, {}, &foldedEnergy) &&
              mid.low[0] == -.625 && mid.high[0] == .875,
          "Mid combines the paired L/R sample differences before binning");
    check(std::abs(focusReadDifferenceRms(foldedEnergy, 2)[0] - std::sqrt(37.) / 8.) < 1.e-14,
          "the short Difference folds continuously before measuring RMS");
    check(focusProjectDifference(a, a, 1, FocusScopeChannels::Stereo, capture(null), {}, {}, &nullEnergy) &&
              null.low == std::array{0., 0.} && null.high == std::array{0., 0.} &&
              focusReadDifferenceRms(nullEnergy, 2) == std::array{0., 0.},
          "a waveform subtracts from itself to exact silence");
    ++b.transportRevision;
    bool wrongRevision = false;
    check(!focusProjectDifference(a, b, 1, FocusScopeChannels::Stereo, [&](std::size_t, const auto &) { wrongRevision = true; }) &&
              !wrongRevision, "Difference never renders across two transport revisions");

    std::array taps{std::make_shared<ScopeTap>(rate, .25), std::make_shared<ScopeTap>(rate, .25),
                    std::make_shared<ScopeTap>(rate, .25)};
    unsigned opens = 0, closes = 0;
    SharedAnalysis broker([&](Source source, double) -> std::shared_ptr<const ScopeTap> {
        ++opens;
        return taps[static_cast<std::size_t>(source.track - 1)];
    }, [&](Source) { ++closes; }, SharedAnalysis::Mode::Manual);
    auto lease = broker.acquireBarScope(sources, 1);
    if (!lease) { check(false, "Difference uses the existing grouped reader"); return; }
    const auto write = [&](std::int64_t stamp, std::size_t i) {
        taps[0]->write(&left[i], &right[i], 1, stamp, true, revision);
        taps[1]->write(&inverted[i], &silent[i], 1, stamp, true, revision);
        taps[2]->write(&left[i], &right[i], 1, stamp, true, revision);
        broker.pump();
    };
    const SharedAnalysis::ScopeDifference first{sources[0], sources[1], FocusScopeChannels::Stereo, 1};
    const SharedAnalysis::ScopeDifference second{sources[0], sources[2], FocusScopeChannels::Mid, 2};
    lease.setDifferenceRequested(first);
    check(lease.request({0, 2, rate}, revision), "Difference requests a normal stamped bar");
    write(0, 0);
    write(1, 1);
    const auto read = lease.read();
    const auto values = read && read->projection ? read->projection->differenceColumns(first)
        : std::span<const FocusSumColumn>{};
    check(values.size() == 1 && values.front().low == stereo.low && values.front().high == stereo.high &&
              read->projection->differenceColumns(second).empty(),
          "the grouped result contains the same A minus B as the short projection and excludes C");
    check(read && read->projection && read->projection->differenceRms(first) == std::array{1.5, .25} &&
              !read->projection->differenceRms(second),
          "the grouped immutable RMS matches the short path and belongs only to its measured pair");

    check(lease.request({2, 2, rate}, revision), "Difference keeps its grouped cursor across bars");
    write(2, 0);
    lease.setDifferenceRequested(second);
    write(3, 1);
    const auto replaced = lease.read();
    check(replaced && replaced->projection && replaced->projection->differenceColumns(first).empty() &&
              replaced->projection->differenceColumns(second).empty() && values.size() == 1 &&
              values.front().low == stereo.low,
          "changing B discards the incomplete comparison and does not mutate a published bar");
    check(replaced && replaced->projection && !replaced->projection->differenceRms(first) &&
              !replaced->projection->differenceRms(second) && read && read->projection &&
              read->projection->differenceRms(first) == std::array{1.5, .25},
          "discarding an unfinished pair clears only its RMS and leaves the previous publication intact");
    check(lease.request({4, 2, rate}, revision), "new selection begins on the next full bar");
    write(4, 0);
    write(5, 1);
    const auto next = lease.read();
    const auto nextValues = next && next->projection ? next->projection->differenceColumns(second)
        : std::span<const FocusSumColumn>{};
    check(nextValues.size() == 1 && nextValues.front().low[0] == 0. && nextValues.front().high[0] == 0. &&
              opens == 3 && closes == 0 && broker.stats().consumers == 1 && broker.stats().transforms == 0,
          "new B and Mid produce a null without reacquiring sources or requesting FFT work");
}

void focusScopeSideDemand() {
    using namespace adi::ui;
    using Source = adi::engine::ScopeSource;
    using Channels = FocusScopeChannels;
    constexpr double rate = 48000.;
    constexpr std::uint64_t revision = 63;
    constexpr std::array sources{Source{1, ScopePoint::PostFader, 0}, Source{2, ScopePoint::PostFader, 0}};
    std::array taps{std::make_shared<ScopeTap>(rate, .25), std::make_shared<ScopeTap>(rate, .25)};
    unsigned opens = 0;
    SharedAnalysis broker([&](Source source, double) -> std::shared_ptr<const ScopeTap> {
        ++opens;
        return taps[static_cast<std::size_t>(source.track - 1)];
    }, [](Source) {}, SharedAnalysis::Mode::Manual);
    auto lease = broker.acquireBarScope(sources, 1);
    if (!lease) { check(false, "Side uses the normal grouped Scope lease"); return; }

    const auto write = [&](std::int64_t stamp, float left, float right, float other) {
        const float silence = 0.f;
        taps[0]->write(&left, &right, 1, stamp, true, revision);
        taps[1]->write(&silence, &other, 1, stamp, true, revision);
        broker.pump();
    };
    const auto lane = [](const auto &result, Channels channel) {
        return result && result->projection ? result->projection->sumMonoColumns(channel)
            : std::span<const FocusScopeBarProjection::MonoColumn>{};
    };

    lease.setMonoRequested(Channels::Side);
    check(lease.request({0, 2, rate}, revision), "Side starts on the existing stamped bar");
    write(0, .75f, -.75f, .25f);
    write(1, -.75f, .75f, 0.f);
    const auto first = lease.read();
    const auto side = lane(first, Channels::Side);
    check(side.size() == 1 && side.front().low == -.75 && side.front().high == .625 &&
              lane(first, Channels::Mid).empty(),
          "the Side lane bins signed corresponding samples and never retains an unrequested Mid lane");
    const auto member = first && first->projection ? first->projection->memberMonoColumns(0, Channels::Side)
        : std::span<const FocusScopeBarProjection::MonoColumn>{};
    check(member.size() == 1 && member.front().low == -.75 && member.front().high == .75,
          "Side retains opposite polarity even though both independent lanes have identical extrema");

    check(lease.request({2, 2, rate}, revision), "the next Side bar starts normally");
    write(2, .75f, -.75f, .25f);
    lease.setMonoRequested(Channels::Mid);
    write(3, -.75f, .75f, 0.f);
    const auto switched = lease.read();
    check(switched && switched->projection && switched->projection->sumColumns().size() == 1 &&
              lane(switched, Channels::Mid).empty() && lane(switched, Channels::Side).empty() &&
              side.size() == 1 && side.front().high == .625,
          "changing the fold drops incomplete bins and keeps old published bins immutable");

    SharedAnalysis::Result a, b;
    for (auto *result : {&a, &b}) {
        result->advancing = true;
        result->transportRevision = revision;
        result->sampleRate = rate;
        result->outputs = SharedAnalysis::Scope;
    }
    a.scopeLeft = {.75f, -.75f};
    a.scopeRight = {-.75f, .75f};
    b.scopeLeft = {0.f, 0.f};
    b.scopeRight = {.25f, 0.f};
    FocusSumColumn difference;
    const auto range = focusProjectDifference(a, b, 1, Channels::Side,
        [&](std::size_t, const auto &column) { difference = column; });
    check(range && difference.low == std::array{-.75, 0.} && difference.high == std::array{.875, 0.} &&
              opens == 2 && broker.stats().taps == 2 && broker.stats().transforms == 0,
          "short Side Difference subtracts A minus B before folding and needs no additional acquisition");
}

void focusScopeLayerChannels() {
    using Channels = adi::ui::FocusScopeChannels;
    using Projection = adi::ui::FocusScopeBarProjection;
    using Source = adi::engine::ScopeSource;
    using Status = SharedAnalysis::BarScopeStatus;
    constexpr double rate = 48000.;
    constexpr std::uint64_t revision = 67;
    constexpr std::array sources{Source{1, ScopePoint::PostFader, 0}, Source{2, ScopePoint::PostFader, 0}};
    const std::array taps{std::make_shared<ScopeTap>(rate, .25), std::make_shared<ScopeTap>(rate, .25)};
    unsigned opens = 0;
    SharedAnalysis broker([&](Source source, double) -> std::shared_ptr<const ScopeTap> {
        ++opens;
        return source == sources[0] ? taps[0] : taps[1];
    }, [](Source) {}, SharedAnalysis::Mode::Manual);
    auto lease = broker.acquireBarScope(sources, 1);
    check(static_cast<bool>(lease), "independent channel views use the existing grouped Scope lease");
    if (!lease) return;

    const auto write = [&](std::int64_t stamp, std::int32_t count) {
        constexpr std::array aLeft{.75f, -.75f}, aRight{.25f, -.25f};
        constexpr std::array bLeft{.25f, -.25f}, bRight{-.75f, .75f};
        taps[0]->write(aLeft.data(), aRight.data(), count, stamp, true, revision);
        taps[1]->write(bLeft.data(), bRight.data(), count, stamp, true, revision);
        broker.pump();
    };
    const auto layer = [](const auto &picture, std::size_t member, Channels view) {
        return picture && picture->projection ? picture->projection->memberMonoColumns(member, view)
                                               : std::span<const Projection::MonoColumn>{};
    };
    const auto symmetric = [](std::span<const Projection::MonoColumn> bins, double peak) {
        return bins.size() == 1 && bins.front().low == -peak && bins.front().high == peak;
    };
    constexpr std::array<std::optional<Channels>, 2> mixed{Channels::Mid, Channels::Side};
    lease.setLayerChannelViews(mixed);
    lease.setPhaseRequested(true);
    constexpr SharedAnalysis::ScopeDifference difference{sources[0], sources[1], Channels::Stereo, 1, false};
    lease.setDifferenceRequested(difference);
    check(lease.request({0, 2, rate}, revision), "two independently folded layers share one stamped capture");
    write(0, 2);
    const auto held = lease.read();
    check(symmetric(layer(held, 0, Channels::Mid), .5) && symmetric(layer(held, 1, Channels::Side), .5) &&
              layer(held, 0, Channels::Side).empty() && layer(held, 1, Channels::Mid).empty(),
          "each layer stores only its requested fold of corresponding samples");
    if (held && held->projection) {
        const auto &projection = *held->projection;
        const auto sum = projection.sumColumns();
        const auto residual = projection.differenceColumns(difference);
        check(sum.size() == 1 && sum.front().low == std::array{-1., -.5} &&
                  sum.front().high == std::array{1., .5} && projection.sumMonoColumns(Channels::Mid).empty() &&
                  projection.sumMonoColumns(Channels::Side).empty() && projection.phase(false) == -1. &&
                  projection.phase(true) == -1. && residual.size() == 1 &&
                  residual.front().low == std::array{-.5, -1.} && residual.front().high == std::array{.5, 1.},
              "layer views leave stereo Sum, Difference and the original phase operands intact");
    } else {
        check(false, "the comparison capture is available with independent layer views");
    }

    check(lease.request({2, 2, rate}, revision), "the next mixed capture starts normally");
    write(2, 1);
    check(lease.status() == Status::Pending && !lease.read(), "a partial mixed capture stays unpublished");
    constexpr std::array<std::optional<Channels>, 2> next{Channels::Side, std::nullopt};
    lease.setLayerChannelViews(next);
    check(lease.status() == Status::Missed && !lease.read(), "a changed layer view retires only the in-flight capture");
    lease.setMonoRequested(Channels::Mid);
    check(lease.request({3, 2, rate}, revision), "a new channel layout starts without reacquiring the sources");
    write(3, 2);
    const auto changed = lease.read();
    check(changed && changed->firstStamp == 3 && symmetric(layer(changed, 0, Channels::Side), .25) &&
              symmetric(layer(changed, 1, Channels::Mid), .25) && layer(changed, 0, Channels::Mid).empty() &&
              symmetric(changed->projection->sumMonoColumns(Channels::Mid), .25) &&
              symmetric(layer(held, 0, Channels::Mid), .5) && symmetric(layer(held, 1, Channels::Side), .5) &&
              opens == 2 && broker.stats().taps == 2 && broker.stats().transforms == 0,
          "a revised override and inherited channel use the same taps and leave the held picture intact");

    check(lease.request({5, 2, rate}, revision), "the next capture starts with the explicit Side override");
    write(5, 1);
    lease.setMonoRequested(Channels::Stereo);
    check(lease.status() == Status::Missed && !lease.read() && lease.request({6, 2, rate}, revision),
          "changing the inherited fold fences an incomplete mixed capture");
    write(6, 2);
    const auto inheritedStereo = lease.read();
    check(symmetric(layer(inheritedStereo, 0, Channels::Side), .25) &&
              layer(inheritedStereo, 1, Channels::Mid).empty() && opens == 2 && broker.stats().transforms == 0,
          "the new inherited view preserves the explicit Side layer without changing sources or FFT work");
}

void focusBarScopeBrokerBoundaries() {
    using Source = adi::engine::ScopeSource;
    using Status = SharedAnalysis::BarScopeStatus;
    const std::array sources{Source{1, ScopePoint::PostFader, 0}, Source{2, ScopePoint::PostFader, 0}};
    std::array<Source, 3> releases{};
    std::size_t detached = 0;
    bool failSecond = true;
    std::array taps{std::make_shared<ScopeTap>(48000., .25), std::make_shared<ScopeTap>(44100., .25),
                    std::make_shared<ScopeTap>(48000., .25)};
    SharedAnalysis broker([&](Source source, double) -> std::shared_ptr<const ScopeTap> {
        if (source.track == 2 && failSecond) throw std::bad_alloc();
        return taps[static_cast<std::size_t>(source.track - 1)];
    }, [&](Source source) {
        if (detached < releases.size()) releases[detached] = source;
        ++detached;
    }, SharedAnalysis::Mode::Manual);
    auto companion = broker.acquire({3});
    bool refused = false;
    try { (void)broker.acquireBarScope(sources, 8); } catch (const std::bad_alloc &) { refused = true; }
    check(refused && detached == 2 && releases[0] == sources[0] && releases[1] == sources[1] &&
              companion.available() && broker.stats().taps == 1,
          "failed grouped attach releases its new provider demand and preserves an existing subscriber");
    failSecond = false;
    auto lease = broker.acquireBarScope(sources, 8);
    check(lease && lease.sampleRate() == 0. && !lease.request({0, 32, 48000.}),
          "different tap rates expose no coherent rate or accepted request");
    if (!lease) return;

    taps[1] = std::make_shared<ScopeTap>(48000., .25);
    broker.refreshSources();
    check(lease.sampleRate() == 48000. && lease.request({100, 32, 48000.}, 9), "rate is available after rebinding both sources");
    std::array<float, 32> samples{};
    for (const auto &tap : std::span(taps).first(2)) tap->write(samples.data(), nullptr, 32, 100, true, 9);
    broker.pump();
    const auto prior = lease.read();
    check(prior && lease.status() == Status::Ready, "one aligned stamped group produces a completed result");
    broker.refreshSources(true);
    check(lease.status() == Status::Missed && !lease.read() && prior && prior->firstStamp == 100,
          "a forced format epoch drops the group result while an older publication remains immutable");

    check(lease.request({200, 32, 48000.}), "a valid bar can be requested before its first source arrives");
    taps[0]->write(samples.data(), nullptr, 32, 201, true, 10);
    broker.pump();
    check(lease.status() == Status::Missed && !lease.read(), "starting after the first bar sample misses instead of truncating");

    check(lease.request({300, 32, 48000.}), "an unresolved revision can be learned from heard samples");
    taps[0]->write(samples.data(), nullptr, 32, 300, true, 11);
    taps[1]->write(samples.data(), nullptr, 32, 300, true, 12);
    broker.pump();
    check(lease.status() == Status::Missed && !lease.read(), "equal stamps cannot align two transport revisions");

    check(lease.request({400, 32, 48000.}), "the group can wait for a fresh complete interval");
    taps[0]->write(samples.data(), nullptr, 32, 400, true, 12);
    broker.pump();
    lease.reset();
    const auto idle = broker.stats();
    taps[1]->write(samples.data(), nullptr, 32, 400, true, 12);
    broker.pump();
    check(idle.domains == 1 && idle.taps == 1 && detached == 4 && companion.available() &&
              broker.stats().cursorReads == idle.cursorReads && broker.stats().publications == idle.publications,
          "closing a partially staged group leaves no queued reads and retains the companion");
}
void focusBarScopeWaitsOnAllSources() {
    using Source = adi::engine::ScopeSource;
    using Status = SharedAnalysis::BarScopeStatus;
    constexpr auto rate = 48000.;
    constexpr std::int32_t block = 256;
    constexpr std::int64_t first = 64 * block;
    const std::array sources{Source{1, ScopePoint::PostFader, 0}, Source{2, ScopePoint::PostFader, 0}};
    const std::array taps{std::make_shared<ScopeTap>(rate, .25), std::make_shared<ScopeTap>(rate, .25)};
    SharedAnalysis broker([&](Source source, double) -> std::shared_ptr<const ScopeTap> {
        return source == sources.front() ? taps.front() : taps.back();
    }, [](Source) {}, SharedAnalysis::Mode::Manual);
    auto lease = broker.acquireBarScope(sources, 8);
    check(first > taps.front()->capacity() && lease.request({first, 2 * block, rate}, 2),
          "a new full Scope window may start beyond the retention of either current source");
    const std::array<float, block> samples{};
    bool pending = true;
    for (std::int64_t stamp = 0; stamp < first + 2 * block; stamp += block) {
        for (const auto &tap : taps) tap->write(samples.data(), nullptr, block, stamp, true, 2);
        broker.pump();
        if (stamp < first) pending = pending && lease.status() == Status::Pending;
    }
    const auto bar = lease.read();
    check(pending && lease.status() == Status::Ready && bar && bar->firstStamp == first &&
              bar->frames == 2 * block && bar->transportRevision == 2 &&
              broker.stats().taps == 2 && broker.stats().transforms == 0,
          "waiting for a future Scope window keeps every source cursor live and publishes the exact interval");
}
void focusBarScopeResumesAtRequestedRevision() {
    using Source = adi::engine::ScopeSource;
    using Status = SharedAnalysis::BarScopeStatus;
    constexpr double rate = 48000.;
    constexpr std::int32_t block = 16;
    const std::array sources{Source{1, ScopePoint::PostFader, 0}, Source{2, ScopePoint::PostFader, 0}};
    const std::array taps{std::make_shared<ScopeTap>(rate, .25), std::make_shared<ScopeTap>(rate, .25)};
    SharedAnalysis broker([&](Source source, double) -> std::shared_ptr<const ScopeTap> {
        return source == sources.front() ? taps.front() : taps.back();
    }, [](Source) {}, SharedAnalysis::Mode::Manual);
    auto lease = broker.acquireBarScope(sources, 8);
    std::array<float, block> samples;
    const auto write = [&](std::int64_t first, std::uint64_t revision, float sample) {
        samples.fill(sample);
        for (const auto &tap : taps) tap->write(samples.data(), samples.data(), block, first, true, revision);
    };
    check(lease.request({0, block, rate}, 9), "request the grouped waveform before a seek");
    write(0, 9, .75f);
    broker.pump();
    check(lease.status() == Status::Ready, "the preceding epoch has a complete waveform");
    write(block, 9, .75f);
    write(2 * block, 9, .75f);
    check(lease.request({block, 2 * block, rate}, 10), "request the exact new revision at overlapping heard stamps");
    broker.pump();
    check(lease.status() == Status::Pending && !lease.read(), "older audio at the requested stamps is skipped without losing the request");
    write(block, 10, -.25f);
    broker.pump();
    check(lease.status() == Status::Pending && !lease.read(), "a new revision may start at the request's first sample and still waits for its tail");
    write(2 * block, 10, -.25f);
    broker.pump();
    const auto captured = lease.read();
    const auto sum = captured && captured->projection ? captured->projection->sumReading() : std::nullopt;
    check(captured && captured->transportRevision == 10 && captured->firstStamp == block &&
              captured->frames == 2 * block && sum && sum->peak == std::array{.5, .5},
          "the complete requested epoch includes its first sample without an old peak or additional tap");
    check(lease.request({3 * block, block, rate}, 0, 11), "a seek floor leaves the newer aligned revision open");
    write(3 * block, 12, .25f);
    broker.pump();
    const auto later = lease.read();
    check(later && later->transportRevision == 12 && later->firstStamp == 3 * block && broker.stats().taps == 2,
          "a transport action after the seek can advance the epoch without forcing new taps or missing the start");
}
void focusScopeSumProjection() {
    using namespace adi::ui;
    SharedAnalysis::Result a, b;
    for (auto *source : {&a, &b}) {
        source->advancing = true;
        source->transportRevision = 7;
        source->sampleRate = 48000.;
        source->outputs = SharedAnalysis::Scope;
    }
    a.firstStamp = 100;
    b.firstStamp = 101;
    a.scopeLeft = {9.f, .8f, .8f, -.8f, .5f, -.5f};
    b.scopeLeft = {-.8f, .8f, -.8f, -.5f, .5f, 9.f};
    a.scopeRight = {9.f, .25f, .25f, -.25f, .5f, -.5f};
    b.scopeRight = {.25f, -.25f, .25f, .5f, -.5f, 9.f};
    const std::array<const SharedAnalysis::Result *, 2> sources{&a, &b};
    const auto range = focusScopeRange(sources);
    check(range && range->first == 101 && range->frames == 5,
          "the sum projection fixture spans exactly the five shared samples");
    if (!range) return;

    std::array<FocusSumColumn, 2> columns;
    std::size_t emitted = 0;
    allocationSizeCount = 0;
    observeAllocationSizes = true;
    const auto reading = focusProjectSum(sources, *range, columns.size(),
        [&](std::size_t column, const FocusSumColumn &extrema) { columns[column] = extrema; ++emitted; });
    observeAllocationSizes = false;
    constexpr auto hot = 2. * static_cast<double>(.8f);
    check(emitted == columns.size() && allocationSizeCount == 0 &&
              columns[0].low == std::array{0., 0.} && columns[0].high == std::array{hot, .5} &&
              columns[1].low == std::array{-hot, -1.} && columns[1].high == std::array{0., 1.},
          "stereo bins cancel in sample order and retain peaks beyond unity without allocating");
    const auto numeric = focusReadSum(sources, *range);
    check(reading.peak == std::array{hot, 1.} && reading.peak == numeric.peak && reading.rms == numeric.rms,
          "the plotted stereo extrema and numeric peak/RMS agree on the same unclipped sample sum");
}
void focusScopeLayerPreview() {
    using namespace adi::ui;
    SharedAnalysis::Result a, b;
    for (auto *source : {&a, &b}) {
        source->advancing = true;
        source->transportRevision = 70;
        source->sampleRate = 48000.;
        source->outputs = SharedAnalysis::Scope;
    }
    a.firstStamp = 7;
    a.scopeLeft = {99.f, 1.f, 2.f, 3.f, 4.f};
    a.scopeRight = {99.f, .125f, .25f, .375f, .5f};
    b.firstStamp = 8;
    b.scopeLeft = {4.f, 3.f, 2.f, 1.f, 99.f};
    b.scopeRight = {.5f, .375f, .25f, .125f, 99.f};
    constexpr std::array adjustments{FocusScopeLayerAdjustment{1, true}, FocusScopeLayerAdjustment{-1, false}};
    const std::array<const SharedAnalysis::Result*, 2> sources{&a, &b};
    const auto range = focusScopeRange(sources);
    check(range && range->first == 8 && range->frames == 4,
          "preview keeps the common stamped interval");
    if (!range) return;
    check(focusScopeDisplayedSample(a.scopeLeft, 1, 4, 0, adjustments[0]) == 0.f &&
              focusScopeDisplayedSample(a.scopeLeft, 1, 4, 1, adjustments[0]) == -1.f &&
              focusScopeDisplayedSample(b.scopeLeft, 0, 4, 3, adjustments[1]) == 0.f,
          "opposite shifts pad the displayed interval rather than using samples outside it");

    std::array<FocusSumColumn, 2> sum{};
    FocusSumColumn difference{};
    allocationSizeCount = 0;
    observeAllocationSizes = true;
    const auto reading = focusProjectSum(sources, *range, sum.size(),
        [&](std::size_t column, const auto &extrema) { sum[column] = extrema; }, adjustments);
    const auto differenceRange = focusProjectDifference(a, b, 1, FocusScopeChannels::Stereo,
        [&](std::size_t, const auto &extrema) { difference = extrema; }, adjustments[0], adjustments[1]);
    observeAllocationSizes = false;
    check(differenceRange && differenceRange->first == range->first && differenceRange->frames == range->frames &&
              sum[0].low == std::array{1., .125} && sum[0].high == std::array{3., .375} &&
              sum[1].low == std::array{-3., -.375} && sum[1].high == std::array{-1., -.125} &&
              reading.peak == std::array{3., .375} && reading.rms[0] == std::sqrt(5.) &&
              difference.low == std::array{-3., -.375} && difference.high == difference.low && allocationSizeCount == 0,
          "short Sum, Difference and readouts operate on shifted signed samples without allocation");
}
void focusScopePhaseOperands() {
    using namespace adi::ui;
    constexpr std::int64_t stamp = 64;
    constexpr double rate = 48000.;
    constexpr std::uint64_t revision = 7;
    constexpr std::array<float, 4> a{.25f, -.5f, .125f, -.25f};
    constexpr std::array<float, 4> b{-.125f, .25f, -.0625f, .125f};
    const FocusScopeRange range{stamp, a.size(), rate};
    const std::array<FocusScopeBarSource, 2> sources{{{{1, ScopePoint::PostFader, 0}, 1},
                                                      {{2, ScopePoint::PostFader, 0}, 1}}};
    const std::array<FocusScopeBarBlock, 2> original{{{sources[0], stamp, revision, rate, true, a, a},
                                                    {sources[1], stamp, revision, rate, true, b, b}}};
    FocusScopeBarProjection bar;
    check(bar.prepare(range, sources, a.size(), revision) && bar.append(original) && !bar.phase(true),
          "an unrequested bar phase reading stays empty");
    check(bar.prepare(range, sources, a.size(), revision, FocusScopeChannels::Stereo, {}, true) &&
              bar.append(original) && bar.phase(true) == -1. && bar.phase(false) == 1.,
          "an opposite Mid pair and its same-polarity stereo sum use different operands");
    auto inverted = original;
    inverted[1].inverted = true;
    check(bar.prepare(range, sources, a.size(), revision, FocusScopeChannels::Stereo, {}, true) &&
              bar.append(inverted) && bar.phase(true) == 1.,
          "the phase reading sees visual polarity applied before the Mid operands");
    bar.retainPhase(false);
    check(!bar.phase(false) && !bar.phase(true), "withdrawing phase demand drops the preview value");
}

void focusBarLayerPreview() {
    using namespace adi::ui;
    using Source = adi::engine::ScopeSource;
    using Status = SharedAnalysis::BarScopeStatus;
    constexpr std::size_t frames = 1024;
    constexpr std::int64_t first = 10000;
    constexpr double rate = 48000.;
    constexpr std::uint64_t revision = 71;
    constexpr std::array sources{Source{1, ScopePoint::PostFader, 0}, Source{2, ScopePoint::PostFader, 0}};
    std::array taps{std::make_shared<ScopeTap>(rate, .25), std::make_shared<ScopeTap>(rate, .25)};
    unsigned opened = 0, closed = 0;
    SharedAnalysis broker([&](Source source, double) -> std::shared_ptr<const ScopeTap> {
        ++opened;
        return taps[static_cast<std::size_t>(source.track - 1)];
    }, [&](Source) { ++closed; }, SharedAnalysis::Mode::Manual);
    auto lease = broker.acquireBarScope(sources, 4);
    if (!lease) { check(false, "adjusted preview uses the existing bar lease"); return; }
    constexpr std::array adjustments{FocusScopeLayerAdjustment{257, true}, FocusScopeLayerAdjustment{-259, false}};
    const SharedAnalysis::ScopeDifference pair{sources[0], sources[1], FocusScopeChannels::Stereo, 3};
    lease.setLayerAdjustments(adjustments);
    lease.setDifferenceRequested(pair);
    check(lease.request({first, frames, rate}, revision), "a shifted bar captures the existing source pair");
    std::array<std::array<float, frames>, 2> signals{};
    signals[0][1] = .75f;
    signals[0][766] = .25f;
    signals[0][767] = .5f; // Positively shifted out of the bar.
    signals[1][0] = 1.f;   // Negatively shifted out of the bar.
    signals[1][517] = .75f;
    signals[1][1023] = .125f;
    for (std::size_t offset = 0; offset < frames; offset += 128) {
        for (std::size_t i = 0; i < taps.size(); ++i)
            taps[i]->write(signals[i].data() + offset, signals[i].data() + offset, 128,
                           first + static_cast<std::int64_t>(offset), true, revision);
        broker.pump();
    }
    const auto shown = lease.read();
    check(shown && shown->projection && shown->projection->complete(),
          "opposite bar offsets stream through multiple tap reads and finish with padding");
    if (!shown || !shown->projection) return;
    const auto sum = shown->projection->sumColumns();
    const auto difference = shown->projection->differenceColumns(pair);
    const auto moved = shown->projection->memberColumns(0);
    check(sum.size() == 4 && difference.size() == 4 && moved.size() == 4 &&
              moved[1].low == std::array{-.75, -.75} && moved[3].low == std::array{-.25, -.25} &&
              sum[1].low == std::array{0., 0.} && sum[1].high == sum[1].low &&
              sum[2].high == std::array{.125, .125} && sum[3].low == std::array{-.25, -.25} &&
              difference[1].low == std::array{-1.5, -1.5},
          "full-bar member, Sum and Difference keep the same shifted sample and sign at tap boundaries");
    lease.setLayerAdjustments(adjustments);
    check(lease.read() == shown, "unchanged preview keeps the published bar");
    lease.setLayerAdjustments({});
    check(lease.status() == Status::Missed && !lease.read() && shown->projection->complete(),
          "changing preview invalidates the current bar while existing readers keep an immutable result");
    check(lease.request({first + static_cast<std::int64_t>(frames), 4, rate}, revision),
          "the same taps can capture the next bar after clearing preview");
    constexpr std::array raw{.25f, .5f, .75f, 1.f};
    for (const auto &tap : taps)
        tap->write(raw.data(), raw.data(), 4, first + static_cast<std::int64_t>(frames), true, revision);
    broker.pump();
    const auto next = lease.read();
    check(next && next->projection && next->projection->sumColumns().back().high == std::array{2., 2.} &&
              opened == 2 && closed == 0 && broker.stats().transforms == 0,
          "resetting preview restores the raw projection without another tap or transform");
}
void focusBarResidualSpectrum() {
    using namespace adi::ui;
    using Source = adi::engine::ScopeSource;
    using Channels = FocusScopeChannels;
    using Status = SharedAnalysis::BarScopeStatus;
    constexpr std::int32_t fftSize = 1024;
    constexpr std::size_t fft = static_cast<std::size_t>(fftSize), frames = fft + 26, bin = 17;
    constexpr double rate = 96000.;
    constexpr std::uint64_t revision = 72;
    constexpr std::array sources{Source{1, ScopePoint::PostFader, 0}, Source{2, ScopePoint::PostFader, 0}};
    std::array taps{std::make_shared<ScopeTap>(rate, .25), std::make_shared<ScopeTap>(rate, .25)};
    unsigned acquired = 0, released = 0;
    SharedAnalysis broker([&](Source source, double) -> std::shared_ptr<const ScopeTap> {
        ++acquired;
        return taps[static_cast<std::size_t>(source.track - 1)];
    }, [&](Source) { ++released; }, SharedAnalysis::Mode::Manual);
    auto lease = broker.acquireBarScope(sources, 8);
    if (!lease) { check(false, "residual spectrum attaches to the existing bar lease"); return; }

    using StereoBar = std::array<std::array<float, frames>, 2>;
    const StereoBar silent{};
    StereoBar tone{};
    for (std::size_t i = 0; i < frames; ++i) {
        const auto phase = static_cast<double>(i) * (2. * std::numbers::pi * bin / fft);
        tone[0][i] = i < frames - fft ? 1.e9f : static_cast<float>(.5 * std::cos(phase));
        tone[1][i] = i < frames - fft ? 1.e9f : static_cast<float>(-.25 * std::cos(phase));
    }
    std::int64_t stamp = 10000;
    std::uint64_t selection = 0;
    const auto select = [&](Channels fold, bool spectrum = true) {
        const SharedAnalysis::ScopeDifference selected{sources[0], sources[1], fold, ++selection, spectrum};
        lease.setDifferenceRequested(selected);
        return selected;
    };
    const auto feed = [&](const StereoBar &a, const StereoBar &b, std::size_t start, std::size_t finish,
                          std::uint64_t revisionB) {
        constexpr std::size_t chunk = 129;
        for (auto i = start; i < finish; i += chunk) {
            const auto count = static_cast<std::int32_t>(std::min(chunk, finish - i));
            const auto first = stamp + static_cast<std::int64_t>(i);
            taps[0]->write(a[0].data() + i, a[1].data() + i, count, first, true, revision);
            taps[1]->write(b[0].data() + i, b[1].data() + i, count, first, true, revisionB);
            broker.pump();
        }
    };
    const auto writeBar = [&](const StereoBar &a, const StereoBar &b) {
        const auto accepted = lease.request({stamp, frames, rate}, revision);
        feed(a, b, 0, frames, revision);
        stamp += static_cast<std::int64_t>(frames);
        return accepted ? lease.read() : std::shared_ptr<const SharedAnalysis::BarScopeResult>{};
    };

    const auto waveformOnly = select(Channels::Stereo, false);
    const auto waveform = writeBar(tone, silent);
    check(waveform && !waveform->differenceSpectrum(waveformOnly) && broker.stats().transforms == 0,
          "difference waveform does no spectrum work until explicitly requested");
    const auto identical = select(Channels::Stereo);
    const auto equal = writeBar(tone, tone);
    const auto *equalBins = equal ? equal->differenceSpectrum(identical) : nullptr;
    check(equalBins && equalBins->fftSize == fftSize && equalBins->binsDbfs.empty() && broker.stats().transforms == 0,
          "identical aligned sources publish an empty residual without a transform");

    constexpr std::array views{Channels::Left, Channels::Right, Channels::Mid, Channels::Side, Channels::Stereo};
    const std::array expected{.5, .25, .125, .375, std::sqrt((.5 * .5 + .25 * .25) * .5)};
    for (std::size_t i = 0; i < views.size(); ++i) {
        const auto shown = select(views[i]);
        const auto before = broker.stats().transforms;
        const auto current = writeBar(tone, silent);
        const auto *read = current ? current->differenceSpectrum(shown) : nullptr;
        check(read && read->fftSize == fftSize && current->sampleRate == rate && read->binsDbfs.size() == fft / 2 + 1 &&
                  near(read->binsDbfs[bin], 20. * std::log10(expected[i])) && read->binsDbfs.front() < -70.f &&
                  !current->differenceSpectrum(identical) && broker.stats().transforms - before == (i + 1 == views.size() ? 2u : 1u),
              "residual analyses the latest power-of-two tail with the selected calibrated stereo or mono reading");
    }

    // Equal magnitudes cancel if frequency-domain magnitudes are subtracted.
    // These operands instead add to a full-scale sine when subtracted in time.
    StereoBar inverted{};
    for (std::size_t ch = 0; ch < inverted.size(); ++ch)
        std::ranges::transform(tone[ch], inverted[ch].begin(), [](float value) { return -value; });
    const auto opposite = select(Channels::Left);
    const auto summed = writeBar(tone, inverted);
    const auto *summedBins = summed ? summed->differenceSpectrum(opposite) : nullptr;
    check(summedBins && summedBins->binsDbfs.size() == fft / 2 + 1 && near(summedBins->binsDbfs[bin], 0.),
          "equal-magnitude opposite-phase sources produce a full-scale residual after raw subtraction");

    StereoBar adjusted{};
    for (std::size_t ch = 0; ch < adjusted.size(); ++ch)
        for (std::size_t i = 1; i < frames; ++i) adjusted[ch][i] = -tone[ch][i - 1];
    constexpr std::array moves{FocusScopeLayerAdjustment{1, true}, FocusScopeLayerAdjustment{}};
    lease.setLayerAdjustments(moves);
    check(lease.status() == Status::Missed && !lease.read(), "a layer edit cannot keep the old residual live");
    const auto shifted = select(Channels::Stereo);
    const auto transforms = broker.stats().transforms;
    const auto aligned = writeBar(tone, adjusted);
    const auto *alignedBins = aligned ? aligned->differenceSpectrum(shifted) : nullptr;
    check(alignedBins && alignedBins->binsDbfs.empty() && broker.stats().transforms == transforms,
          "the residual captures the same adjusted and padded sample pair as the displayed waveform");
    lease.setLayerAdjustments({});

    const auto oldPair = select(Channels::Left);
    check(lease.request({stamp, frames, rate}, revision), "residual capture can begin on an existing source lease");
    feed(tone, silent, 0, 512, revision);
    const auto newPair = select(Channels::Right);
    feed(tone, silent, 512, frames, revision);
    stamp += static_cast<std::int64_t>(frames);
    const auto abandoned = lease.read();
    check(abandoned && !abandoned->differenceSpectrum(oldPair) && !abandoned->differenceSpectrum(newPair) &&
              broker.stats().transforms == transforms,
          "changing a fold mid-bar cannot publish the old or incomplete residual");

    StereoBar hugeA{}, hugeB{};
    for (std::size_t ch = 0; ch < hugeA.size(); ++ch) {
        hugeA[ch].fill(std::numeric_limits<float>::max());
        hugeB[ch].fill(-std::numeric_limits<float>::max());
    }
    const auto guarded = writeBar(hugeA, hugeB);
    const auto *guardedBins = guarded ? guarded->differenceSpectrum(newPair) : nullptr;
    check(guardedBins && guardedBins->binsDbfs.size() == fft / 2 + 1 &&
              std::ranges::all_of(guardedBins->binsDbfs, [](float value) { return std::isfinite(value); }) &&
              near(guardedBins->binsDbfs.front(), 240.),
          "overflow-scale finite differences use the shared spectral input guard");

    check(lease.request({stamp, frames, rate}, revision), "residual uses the existing stamped capture");
    feed(tone, silent, 0, frames, revision + 1);
    check(lease.status() == Status::Missed && !lease.read() && acquired == 2 && released == 0 &&
              broker.stats().transforms == transforms + 1,
          "transport mismatch refuses the residual and no extra tap or transform is needed");
}
void focusScopeFlatPath() {
    using adi::ui::FocusScopePathReducer;
    using Point = adi::ui::FocusScopePoint;
    const auto capture = [](std::span<const Point> points) {
        FocusScopePathReducer reducer;
        std::vector<Point> path;
        int subpaths = 0;
        const auto emit = [&](Point point, bool first) {
            path.push_back(point);
            if (first) ++subpaths;
        };
        for (const auto point : points) reducer.add(point, emit);
        reducer.finish(emit);
        return std::pair{path, subpaths};
    };
    const auto scalar = [](std::span<const Point> points) {
        std::vector<Point> compact(points.begin(), points.end());
        compact.erase(std::unique(compact.begin(), compact.end()), compact.end());
        std::vector<Point> expected;
        for (std::size_t first = 0; first < compact.size();) {
            auto last = first + 1;
            while (last < compact.size() && compact[last].y == compact[first].y) ++last;
            expected.push_back(compact[first]);
            if (last != first + 1) expected.push_back(compact[last - 1]);
            first = last;
        }
        return expected;
    };

    const auto empty = capture({});
    const std::array single{Point{3.25f, 6.f}};
    const auto point = capture(single);
    check(empty.first.empty() && empty.second == 0 && point.first == scalar(single) && point.second == 1,
          "empty and one-point Focus paths preserve their subpath boundary");

    std::array<Point, 1024> zeros{};
    for (std::size_t i = 0; i < zeros.size(); ++i) zeros[i].x = static_cast<float>(i) * .25f;
    const auto cancelled = capture(zeros);
    check(cancelled.second == 1 && cancelled.first == scalar(zeros) && cancelled.first.size() == 2,
          "an exactly flat cancellation trace keeps its two original caps");

    const std::array spikes{Point{0.f, 1.f}, Point{1.f, 1.f}, Point{2.f, 1.f}, Point{2.f, -4.f},
        Point{2.f, 8.f}, Point{3.f, 2.f}, Point{4.f, 2.f}, Point{4.f, 2.f}, Point{5.f, 2.f},
        Point{6.f, 9.f}, Point{7.f, 2.f}, Point{8.f, 2.f}, Point{9.f, 2.f}};
    const auto measured = capture(spikes);
    check(measured.second == 1 && measured.first == scalar(spikes) && measured.first.size() < spikes.size(),
          "the scalar oracle confirms same-x span order and an isolated peak among compressed runs");

    const std::array sloped{Point{0.f, 0.f}, Point{1.f, .0001f}, Point{2.f, 0.f}, Point{3.f, .0002f}};
    const auto quiet = capture(sloped);
    check(quiet.second == 1 && quiet.first == scalar(sloped) && quiet.first.size() == sloped.size(),
          "subpixel nonflat motion still emits every original vertex");
}
void focusMatchWindows() {
    using namespace adi::ui;
    SharedAnalysis::Result a;
    a.request.track = 1;
    a.request.fftSize = 1024;
    a.sampleRate = 48000;
    a.coherentGain = .5;
    a.hopSize = 256;
    a.outputs = SharedAnalysis::Spectrum;
    a.advancing = true;
    a.firstStamp = 768;
    a.transportRevision = 7;
    a.generation = 10;
    a.windowSerial = 5;
    a.binsDbfs.assign(513, adi::dsp::kDbFloor);
    auto b = a;
    b.request.track = 2;
    b.generation = 20;
    a.binsDbfs[0] = b.binsDbfs[0] = -12.f;
    a.binsDbfs[20] = -18.f; b.binsDbfs[20] = -24.f;
    a.binsDbfs[22] = -30.f; b.binsDbfs[22] = -27.f;
    b.binsDbfs[29] = -24.f;
    a.binsDbfs[50] = std::numeric_limits<float>::quiet_NaN(); b.binsDbfs[50] = -6.f;
    a.binsDbfs[512] = b.binsDbfs[512] = -15.f;
    const auto match = focusReadMatch(a, b);
    check(match && match->main == 1 && match->target == 2 && match->mainGeneration == 10 &&
              match->targetGeneration == 20 && match->deltaDb[20] == 6.f && match->deltaDb[22] == -3.f,
          "Focus Match proposes the signed measured difference toward Main");
    check(match && !match->deltaDb[0] && !match->deltaDb[29] && !match->deltaDb[50] && !match->deltaDb[512],
          "DC, censored, nonfinite and beyond-20-kHz bins are omitted from the preview");
    const auto swaps = focusReadMatch(b, a);
    check(swaps && swaps->deltaDb[20] == -6.f && swaps->deltaDb[22] == 3.f,
          "changing Match direction reverses the result");
    const auto reject = [&](const auto &edit) {
        auto different = b;
        edit(different);
        return !focusMatchCompatible(a, different) && !focusReadMatch(a, different);
    };
    check(reject([](auto &r) { ++r.firstStamp; }) &&
          reject([](auto &r) { ++r.transportRevision; }) &&
          reject([](auto &r) { r.advancing = false; }) &&
          reject([](auto &r) { r.sampleRate = 44100; }),
          "Focus Match requires exactly simultaneous playing windows at one rate");
    check(reject([](auto &r) { r.request.track = 1; }) &&
          reject([](auto &r) { r.request.point = ScopePoint::ChainInput; }) &&
          reject([](auto &r) { r.coherentGain = .625; }) &&
          reject([](auto &r) { r.nonFiniteSamples = 1; }) &&
          reject([](auto &r) { r.outputs = SharedAnalysis::Scope; }),
          "same-source, unequal calibrated data, or sanitised input cannot be captured as a Match");
    std::fill(b.binsDbfs.begin(), b.binsDbfs.end(), adi::dsp::kDbFloor);
    check(!focusReadMatch(a, b), "no measurable overlap leaves Match empty");
}
// File scope: GCC refuses a local variable, even a static constexpr one, in
// the lambda's default argument below ([dcl.fct.default]).
constexpr std::uint64_t focusLagRevision = 19;
void focusLagPreviewRequests() {
    using namespace adi::ui;
    constexpr std::uint64_t revision = focusLagRevision;
    SharedAnalysis::Result main, target;
    main.request.track = 81;
    main.generation = 42;
    main.firstStamp = 14000;
    main.firstSequence = 9000;
    main.sampleRate = 48000.;
    main.windowSerial = 5;
    main.transportRevision = revision;
    main.advancing = true;
    target = main;
    target.request.track = 82;
    target.generation = 43;
    target.firstStamp = 14256;
    target.firstSequence = 6256;
    const std::array heads{SharedAnalysis::Lease::SourcePosition{42, 11200},
                           SharedAnalysis::Lease::SourcePosition{43, 9120}};
    const auto plan = [&](const SharedAnalysis::Result *a, const SharedAnalysis::Result *b,
                          std::int64_t heard = 15400, std::uint64_t settled = focusLagRevision) {
        return focusLagPreviewRequest({a, b}, heads, settled, heard);
    };

    allocationSizeCount = 0;
    observeAllocationSizes = true;
    const auto request = plan(&main, &target);
    observeAllocationSizes = false;
    check(request && request->sources[0] == FocusScopeBarSource{main.request.source(), 42} &&
              request->sources[1] == FocusScopeBarSource{target.request.source(), 43} &&
              request->transportRevision == revision && request->firstStamp == 15024 - 4096 &&
              request->heardEnd == 15400 && request->maxLagSamples == 960 && allocationSizeCount == 0,
          "Align plans from the earlier published end and existing source identities without allocating");
    check(request && request->firstStamp < main.firstStamp && request->firstStamp < target.firstStamp,
          "the two preview publications may be shorter than the worker's independently verified raw interval");
    const auto heldBack = plan(&main, &target, 14800);
    check(heldBack && heldBack->firstStamp == 14800 - 4096 && heldBack->heardEnd == 14800,
          "the heard frontier clamps the request before either publication end");
    check(!plan(&main, &target, target.firstStamp) && !plan(&main, &target, 4095) &&
              !plan(&main, &target, -1),
          "entirely future publications, an incomplete interval, and a negative frontier are refused");
    auto recentMain = main, recentTarget = target;
    recentMain.firstStamp = 3600;
    recentTarget.firstStamp = 3072;
    const auto boundary = plan(&recentMain, &recentTarget, 4200);
    check(boundary && boundary->firstStamp == 0,
          "the earlier target end can select the first complete timeline window");

    check(!plan(nullptr, &target) && !plan(&main, nullptr) &&
              !plan(&main, &target, 15400, 0) && !plan(&main, &target, 15400, revision + 1),
          "both live publications and the settled matching transport revision are required");
    auto invalid = target;
    invalid.transportRevision -= 1;
    check(!plan(&main, &invalid), "a retained target from another transport revision cannot launch a preview");
    invalid = target;
    invalid.advancing = false;
    check(!plan(&main, &invalid), "parked audio cannot launch a timing preview");
    invalid = target;
    invalid.discontinuity = true;
    check(!plan(&main, &invalid), "a publication marked discontinuous cannot witness the new raw interval");
    invalid = target;
    invalid.windowSerial = 0;
    check(!plan(&main, &invalid), "the source needs a completed analysis window");
    invalid = target;
    invalid.generation += 1;
    auto retiredHeads = heads;
    retiredHeads[0].generation = 0;
    check(!plan(&main, &invalid) && !focusLagPreviewRequest({&main, &target}, retiredHeads, revision, 15400),
          "a replaced or detached tap cannot keep the old source identity eligible");
    invalid = target;
    invalid.request.track = main.request.track;
    check(!plan(&main, &invalid), "a track never compares to itself");
    invalid = target;
    invalid.request.point = ScopePoint::PreFader;
    check(!plan(&main, &invalid), "only the channel post-fader signal is eligible");
    invalid = target;
    invalid.request.device = 703;
    check(!plan(&main, &invalid), "a bound device is not a channel post-fader source");
    invalid = target;
    invalid.firstSequence = heads[1].written - target.request.fftSize + 1;
    check(!plan(&main, &invalid), "a completed analysis window cannot be ahead of its current tap head");
    auto insufficientHeads = heads;
    insufficientHeads[1].written = 4095;
    invalid.firstSequence = 1000;
    check(!focusLagPreviewRequest({&main, &invalid}, insufficientHeads, revision, 15400),
          "the live tap must have produced enough samples for a full worker window");
    invalid = target;
    invalid.firstStamp = std::numeric_limits<std::int64_t>::max() - 4;
    check(!plan(&main, &invalid, std::numeric_limits<std::int64_t>::max()),
          "the publication end cannot overflow the sample timeline");
    invalid = target;
    invalid.request.fftSize = 0;
    check(!plan(&main, &invalid), "a zero-length publication cannot witness a raw interval");
    invalid = target;
    invalid.sampleRate = 44100.;
    check(!plan(&main, &invalid), "different device rates are refused");

    for (const auto &pair : {std::pair{44100., 882}, std::pair{96000., 1920}}) {
        auto fastMain = main, fastTarget = target;
        fastMain.sampleRate = fastTarget.sampleRate = pair.first;
        const auto faster = plan(&fastMain, &fastTarget);
        check(faster && faster->maxLagSamples == pair.second,
              "the low-rate 20 ms search is rounded in device samples");
    }
    for (const auto rate : {0., 1., std::numeric_limits<double>::infinity(),
                            std::numeric_limits<double>::quiet_NaN()}) {
        auto invalidMain = main, invalidTarget = target;
        invalidMain.sampleRate = invalidTarget.sampleRate = rate;
        check(!plan(&invalidMain, &invalidTarget), "a non-finite or unusable search radius is refused");
    }
}
void focusLagPreviewCases() {
    using namespace adi::ui;
    constexpr auto size = FocusLagRequest::kFrames;
    constexpr std::int64_t start = 12345;
    FocusLagRequest request;
    request.sources = {FocusScopeBarSource{{1}, 10}, FocusScopeBarSource{{2}, 11}};
    request.transportRevision = 9;
    request.firstStamp = start;
    request.heardEnd = start + static_cast<std::int64_t>(size);
    request.maxLagSamples = 960;
    const auto publication = [&](std::size_t side) {
        const auto &identity = request.sources[side];
        SharedAnalysis::Result result;
        result.request.track = identity.source.track;
        result.request.fftSize = static_cast<std::int32_t>(size);
        result.request.outputs = SharedAnalysis::Scope;
        result.outputs = SharedAnalysis::Scope;
        result.generation = identity.generation;
        result.windowSerial = 1;
        result.firstStamp = start;
        result.transportRevision = request.transportRevision;
        result.sampleRate = 48000.;
        result.advancing = true;
        result.scopeLeft.resize(size);
        result.scopeRight.resize(size);
        return result;
    };
    auto a = publication(0), b = publication(1);
    std::array<float, size> noise{};
    const auto broadband = [](auto &samples, std::uint32_t seed) {
        for (auto &sample : samples) {
            seed ^= seed << 13;
            seed ^= seed >> 17;
            seed ^= seed << 5;
            sample = static_cast<float>(seed & 0xffffu) / 32768.f - 1.f;
        }
    };
    broadband(noise, 0x12345678u);
    const auto delay = [&](std::int32_t lag, float polarity) {
        for (std::size_t i = 0; i < size; ++i) {
            const auto n = static_cast<std::int32_t>(i) - lag;
            const float aValue = noise[i] * .4f;
            const float bValue = n >= 0 && static_cast<std::size_t>(n) < size
                ? polarity * noise[static_cast<std::size_t>(n)] * .4f : 0.f;
            a.scopeLeft[i] = a.scopeRight[i] = aValue;
            b.scopeLeft[i] = b.scopeRight[i] = bValue;
        }
    };
    delay(137, 1.f);
    allocationSizeCount = 0;
    observeAllocationSizes = true;
    const auto late = focusFindLag(request, a, b);
    observeAllocationSizes = false;
    check(late && late->lagSamples == 137 && near(late->correlation, 1., 1e-12) &&
              near(1000. * late->lagSamples / a.sampleRate, 2.85416666666667, 1e-12) && allocationSizeCount == 0,
          "a stamped broadband copy 137 samples late suggests an advance of 2.8541667 ms without allocations");
    // Use raw moments as an independent check of the zero-lag Pearson correlation.
    const auto currentCorrelation = [&] {
        double sx = 0., sy = 0., sxx = 0., syy = 0., sxy = 0.;
        const auto margin = static_cast<std::size_t>(request.maxLagSamples);
        for (auto i = margin; i < size - margin; ++i) {
            const double x = a.scopeLeft[i], y = b.scopeLeft[i];
            sx += x;
            sy += y;
            sxx += x * x;
            syy += y * y;
            sxy += x * y;
        }
        const auto count = static_cast<double>(size - 2 * margin);
        return (sxy - sx * sy / count) / std::sqrt((sxx - sx * sx / count) * (syy - sy * sy / count));
    };
    check(late && std::abs(late->unalignedCorrelation) < .2 &&
              near(late->unalignedCorrelation, currentCorrelation(), 1e-12),
          "the Align rail describes current correlation separately from the suggested correction");
    delay(137, -1.f);
    const auto inverted = focusFindLag(request, a, b);
    check(inverted && inverted->lagSamples == 137 && near(inverted->correlation, -1., 1e-12),
          "inverting the target retains its timing and reports a separately signed correlation");
    check(inverted && late && near(inverted->unalignedCorrelation, -late->unalignedCorrelation, 1e-12),
          "the current correlation rail also retains the measured polarity");
    delay(0, 1.f);
    const auto alreadyAligned = focusFindLag(request, a, b);
    check(alreadyAligned && alreadyAligned->lagSamples == 0 && near(alreadyAligned->unalignedCorrelation, 1., 1e-12),
          "an already aligned pair measures full current correlation and zero timing offset");
    delay(-137, 1.f);
    const auto early = focusFindLag(request, a, b);
    check(early && early->lagSamples == -137 && near(early->correlation, 1., 1e-12),
          "a negative lag correctly identifies an early target");

    bool gainInvariant = true;
    for (const auto db : {-20.f, -10.f, 0.f, 10.f}) {
        delay(137, 1.f);
        const auto gain = std::pow(10.f, db / 20.f);
        for (auto &sample : b.scopeLeft) sample *= gain;
        b.scopeRight = b.scopeLeft;
        const auto shifted = focusFindLag(request, a, b);
        gainInvariant = gainInvariant && shifted && shifted->lagSamples == 137 &&
            near(shifted->correlation, 1., 1e-10);
    }
    check(gainInvariant, "a -20 to +10 dB change does not move or weaken a clean lag");

    const auto addEqualPowerNoise = [&](std::uint32_t seed) {
        constexpr std::size_t lag = 137;
        const auto radius = static_cast<std::size_t>(request.maxLagSamples);
        const auto first = radius + lag, count = size - 2 * radius;
        std::array<float, size> interference;
        broadband(interference, seed);
        // Normalize AC energy over the same matched interior where SNR is measured.
        const auto power = [first, count](std::span<const float> samples) {
            double sum = 0., squared = 0.;
            for (const double sample : samples.subspan(first, count)) {
                sum += sample;
                squared += sample * sample;
            }
            return squared - sum * sum / static_cast<double>(count);
        };
        const auto scale = std::sqrt(power(b.scopeLeft) / power(interference));
        for (std::size_t i = 0; i < size; ++i)
            b.scopeLeft[i] += static_cast<float>(scale * interference[i]);
        b.scopeRight = b.scopeLeft;
    };
    delay(137, 1.f);
    addEqualPowerNoise(0x31a59e7bu);
    const auto cleanPeak = focusFindLag(request, a, b);
    check(cleanPeak && cleanPeak->lagSamples == 137 && cleanPeak->correlation >= .70,
          "the first independent 0 dB SNR window retains its distinct timing peak");
    delay(137, 1.f);
    addEqualPowerNoise(0x31a59e86u);
    const auto quieterPeak = focusFindLag(request, a, b);
    check(quieterPeak && quieterPeak->lagSamples == 137 && near(quieterPeak->correlation, .68817616, 2e-6),
          "a long 0 dB SNR window with a distinct peak may read just below .70");

    request.maxLagSamples = 2016;
    a.sampleRate = b.sampleRate = 192000.;
    delay(137, 1.f);
    addEqualPowerNoise(0x31a59e7bu);
    check(!focusFindLag(request, a, b), "a 64-sample interior keeps the conservative correlation floor");
    request.maxLagSamples = 960;
    a.sampleRate = b.sampleRate = 48000.;

    delay(137, 1.f);
    std::array<float, size> interference;
    broadband(interference, 0x31a59e7bu);
    for (std::size_t i = 0; i < size; ++i) {
        if (i + 271 < size) b.scopeLeft[i] += .32f * noise[i + 271];
        b.scopeLeft[i] += .25f * interference[i];
    }
    b.scopeRight = b.scopeLeft;
    check(!focusFindLag(request, a, b),
          "a near-floor peak with a substantial competing echo still declines to suggest a lag");

    delay(960, 1.f);
    check(!focusFindLag(request, a, b), "a true peak at the positive search boundary remains ineligible");
    delay(-960, 1.f);
    check(!focusFindLag(request, a, b), "a true peak at the negative search boundary remains ineligible");
    for (std::size_t i = 0; i < size; ++i) {
        const auto phase = 2. * std::numbers::pi * static_cast<double>(i % 800) / 800.;
        const auto value = .4f * static_cast<float>(std::sin(phase));
        a.scopeLeft[i] = a.scopeRight[i] = b.scopeLeft[i] = b.scopeRight[i] = value;
    }
    check(!focusFindLag(request, a, b), "periodic aliases at -800, 0 and +800 cannot produce a timing suggestion");
    delay(137, 1.f);
    for (std::size_t i = 0; i + 271 < size; ++i) {
        b.scopeLeft[i] += .384f * noise[i + 271];
        b.scopeRight[i] = b.scopeLeft[i];
    }
    request.maxLagSamples = 200;
    const auto onlyLatePeak = focusFindLag(request, a, b);
    check(onlyLatePeak && onlyLatePeak->lagSamples == 137 && onlyLatePeak->correlation > .70,
          "the two-echo fixture has a strong first timing peak inside the narrower search");
    request.maxLagSamples = 960;
    check(!focusFindLag(request, a, b), "a second distinct nearby-strength peak makes the wider search ambiguous");
    delay(137, 1.f);
    broadband(b.scopeLeft, 0x31a59e7bu);
    b.scopeRight = b.scopeLeft;
    check(!focusFindLag(request, a, b), "unrelated broadband windows have no sufficiently correlated candidate");
    delay(137, 1.f);
    std::fill(b.scopeLeft.begin(), b.scopeLeft.end(), 0.f);
    b.scopeRight = b.scopeLeft;
    check(!focusFindLag(request, a, b), "silence has no timing peak");
    std::fill(b.scopeLeft.begin(), b.scopeLeft.end(), .625f);
    b.scopeRight = b.scopeLeft;
    check(!focusFindLag(request, a, b), "DC has no timing peak after centering");
    delay(137, 1.f);
    for (auto &sample : b.scopeLeft) sample *= 1e-8f;
    b.scopeRight = b.scopeLeft;
    check(!focusFindLag(request, a, b), "a perfectly correlated signal below the AC level floor is declined");
    delay(137, 1.f);
    for (std::size_t i = 0; i < size; ++i) b.scopeRight[i] = -b.scopeLeft[i];
    check(!focusFindLag(request, a, b), "opposed channels give an undefined mid timing suggestion");

    delay(137, 1.f);
    --b.transportRevision;
    check(!focusFindLag(request, a, b), "different transport revisions cannot be compared despite equal timestamps");
    a.transportRevision = b.transportRevision;
    check(!focusFindLag(request, a, b), "matching old revisions are refused against the current transport witness");
    a.transportRevision = b.transportRevision = request.transportRevision;
    --b.generation;
    check(!focusFindLag(request, a, b), "a reused channel ID cannot disguise an obsolete tap generation");
    b.generation = request.sources[1].generation;
    --a.firstStamp; --b.firstStamp;
    check(!focusFindLag(request, a, b), "a prior same-revision window does not cover the exact requested stamp range");
    a.firstStamp = b.firstStamp = request.firstStamp;
    --request.heardEnd;
    check(!focusFindLag(request, a, b), "the requested range cannot contain a future or unplayed sample");
    ++request.heardEnd;
    b.sampleRate = 44100.;
    check(!focusFindLag(request, a, b), "a mixed sample-rate pair cannot produce a sample offset");
    b.sampleRate = a.sampleRate;
    request.maxLagSamples = 961;
    check(!focusFindLag(request, a, b), "the explicit search radius may not exceed 20 ms at 48 kHz");
    request.maxLagSamples = 960;
    b.scopeRight.back() = std::numeric_limits<float>::quiet_NaN();
    check(!focusFindLag(request, a, b), "non-finite raw input invalidates the whole requested interval");
}
void focusLagWorkerJob(adi::ui::FocusLagRequest request, const std::shared_ptr<ScopeTap> &a,
                       const std::shared_ptr<ScopeTap> &b) {
    using Source = adi::engine::ScopeSource;
    using Status = SharedAnalysis::FocusLagStatus;
    const auto provider = [&](Source source, double) -> std::shared_ptr<const ScopeTap> { return source.track == 1 ? a : b; };
    unsigned acquired = 0, released = 0;
    SharedAnalysis broker([&](Source source, double seconds) { ++acquired; return provider(source, seconds); },
                          [&](Source) { ++released; }, SharedAnalysis::Mode::Manual);
    check(!broker.requestFocusLag(request) && !acquired, "Align does not attach a tap to make an explicit request eligible");
    SharedAnalysis reference(provider, [](Source) {}, SharedAnalysis::Mode::Manual);
    auto fft = broker.acquire({1});
    auto referenceFft = reference.acquire({1});
    auto scopeRequest = SharedAnalysis::Request{2};
    scopeRequest.outputs = SharedAnalysis::Scope;
    auto scope = broker.acquire(scopeRequest);
    auto referenceScope = reference.acquire(scopeRequest);
    check(fft.available() && referenceFft.available() && scope.available() && referenceScope.available(),
          "both brokers have matching ordinary visual subscriptions");
    std::array<float, 1024> extraAudio;
    extraAudio.fill(.125f);
    a->write(extraAudio.data(), nullptr, static_cast<std::int32_t>(extraAudio.size()), request.heardEnd, true,
             request.transportRevision);

    auto lease = broker.requestFocusLag(request);
    check(lease && lease.status() == Status::Pending && !lease.read() && !broker.requestFocusLag(request),
          "one explicit Align task is queued and a duplicate does not schedule a second job");
    const auto referenceTransforms = reference.pump();
    const auto companionTransforms = broker.pump();
    const auto result = lease.read();
    check(lease.status() == Status::Ready && result && result->lagSamples == 137 && near(result->correlation, 1., 1e-12),
          "one explicit Align task publishes the result after ordinary spectral work");
    check(referenceTransforms == 1 && companionTransforms == referenceTransforms,
          "the Align task leaves the ordinary spectrum FFT count unchanged");
    check(acquired == 2 && released == 0, "the Align task borrows both ordinary sources without adding subscriptions");
    reference.pump();
    broker.pump();
    const auto idle = broker.stats();
    broker.pump();
    check(lease.read() == result && idle.workerPasses == broker.stats().workerPasses &&
              reference.stats().transforms == idle.transforms,
          "a completed Align task stays immutable and performs no work on idle frames");

    auto cancelled = broker.requestFocusLag(request);
    check(cancelled && cancelled.status() == Status::Pending, "a fresh explicit request can be cancelled before the worker runs");
    cancelled.reset();
    broker.pump();
    check(!cancelled && broker.stats().workerPasses == idle.workerPasses && released == 0,
          "closing Align cancels the pending task while other visual demand remains attached");

    auto stale = broker.requestFocusLag(request);
    broker.refreshSources(true);
    broker.pump();
    check(stale && stale.status() == Status::Refused && !stale.read() && lease.status() == Status::Refused && !lease.read() &&
              result && result->lagSamples == 137,
          "a source binding epoch invalidates queued and completed leases without mutating an already held result");
    auto wrongGeneration = request;
    ++wrongGeneration.sources[1].generation;
    check(!broker.requestFocusLag(wrongGeneration), "a request for an obsolete tap generation is refused before scheduling");
    auto shortHistory = request;
    shortHistory.firstStamp += 20000;
    shortHistory.heardEnd += 20000;
    auto refused = broker.requestFocusLag(shortHistory);
    broker.pump();
    check(refused && refused.status() == Status::Refused && !refused.read(),
          "an accepted task with insufficient stamped history returns a clean refusal");

    auto rebound = broker.requestFocusLag(request);
    scope.reset();
    check(!broker.requestFocusLag(request) && acquired >= 2 && released == 1,
          "Align needs both existing source demands and never revives a released tap");
    scope = broker.acquire(scopeRequest);
    broker.pump();
    check(scope.available() && rebound && rebound.status() == Status::Refused && !rebound.read(),
          "reattachment of the same physical tap cannot revive a request from the former binding");
    auto beforeSeek = broker.requestFocusLag(request);
    b->write(static_cast<const float *>(nullptr), nullptr, 1,
             request.firstStamp + static_cast<std::int64_t>(adi::ui::FocusLagRequest::kFrames), true,
             request.transportRevision + 1);
    broker.pump();
    check(beforeSeek && beforeSeek.status() == Status::Refused && !beforeSeek.read(),
          "an actual seek in the same bound tap invalidates the queued preview");
}
void focusLagTapReads() {
    using namespace adi::ui;
    constexpr auto frames = FocusLagRequest::kFrames;
    constexpr auto frameCount = static_cast<std::int32_t>(frames);
    constexpr std::int64_t first = 25000, end = first + frameCount;
    constexpr std::int32_t delay = 137;
    constexpr std::uint64_t revision = 9;
    std::array<float, frames> main, target{};
    std::uint32_t bits = 0xdeadbeefu;
    for (std::size_t i = 0; i < frames; ++i) {
        bits = bits * 1664525u + 1013904223u;
        main[i] = static_cast<float>(bits >> 16) / 65536.f - .5f;
        if (i >= static_cast<std::size_t>(delay)) target[i] = main[i - static_cast<std::size_t>(delay)];
    }
    auto a = std::make_shared<ScopeTap>(48000., .25);
    auto b = std::make_shared<ScopeTap>(48000., .25);
    a->write(main.data(), nullptr, frameCount, first, true, revision);
    a->write(static_cast<const float *>(nullptr), nullptr, 64, end, true, revision);
    constexpr std::int32_t split = 1536;
    b->write(target.data(), nullptr, split, first, true, revision);
    b->write(target.data() + split, nullptr, frameCount - split, first + split, true, revision);
    FocusLagRequest request{{FocusScopeBarSource{{1}, a->generation()}, FocusScopeBarSource{{2}, b->generation()}},
                            revision, first, end + 64, 960};
    allocationSizeCount = 0;
    observeAllocationSizes = true;
    const auto measured = focusFindLagOnTaps(request, {a, b});
    observeAllocationSizes = false;
    check(measured && measured->lagSamples == delay && near(measured->correlation, 1., 1e-12) &&
              allocationSizeCount == 0 && a->written() == frameCount + 64 && b->written() == frameCount,
          "unequal physical heads resolve the same exact heard interval without allocation or tap mutation");
    request.heardEnd = end - 1;
    check(!focusFindLagOnTaps(request, {a, b}), "Align declines a requested interval beyond the heard position");
    request.heardEnd = end;
    const auto ahead = focusFindLagOnTaps(request, {a, b});
    check(ahead && ahead->lagSamples == delay,
          "a later same-revision head can locate the exact already-heard interval");
    request.heardEnd = end + 768;

    const auto alternate = [&](double rate = 48000., double seconds = .25) {
        auto tap = std::make_shared<ScopeTap>(rate, seconds);
        request.sources[1].generation = tap->generation();
        return tap;
    };
    auto other = alternate();
    other->write(target.data(), nullptr, frameCount - 1, first, true, revision);
    check(!focusFindLagOnTaps(request, {a, other}), "one absent sample makes the requested history incomplete");
    check(!focusFindLagOnTaps(request, {a, b}), "the old tap cannot stand in for its new generation");
    other = alternate();
    other->write(target.data(), nullptr, split, first, true, revision - 1);
    other->write(target.data() + split, nullptr, frameCount - split, first + split, true, revision);
    check(!focusFindLagOnTaps(request, {a, other}), "a matching final stamp cannot hide a prior transport boundary");
    other = alternate();
    other->write(target.data(), nullptr, frameCount, first, false, revision);
    check(!focusFindLagOnTaps(request, {a, other}), "parked audio does not identify a heard interval");
    other = alternate(44100.);
    other->write(target.data(), nullptr, frameCount, first, true, revision);
    check(!focusFindLagOnTaps(request, {a, other}), "mixed sample rates cannot share a lag sample grid");
    other = alternate(48000., .1);
    other->write(target.data(), nullptr, frameCount, first, true, revision);
    other->write(static_cast<const float *>(nullptr), nullptr, 768, end, true, revision);
    check(other->oldest() > 0 && !focusFindLagOnTaps(request, {a, other}),
          "a ring that has overwritten the first sample refuses the exact wanted interval");
    request.sources[1].generation = b->generation();
    request.heardEnd = end + 64;
    focusLagWorkerJob(request, a, b);
}
void focusLagHighRateTaps() {
    using namespace adi::ui;
    using Status = SharedAnalysis::FocusLagStatus;
    constexpr std::int64_t first = 1100000;
    constexpr std::uint64_t revision = 43;
    for (const auto rate : {192000., 768000.}) {
        const auto radius = static_cast<std::int32_t>(std::lround(.020 * rate));
        const auto frameCount = static_cast<std::int32_t>(rate / 8.);
        std::vector<float> main(static_cast<std::size_t>(frameCount)), target(main.size());
        std::uint32_t noise = 0x7c612889u;
        for (auto &sample : main) {
            noise = noise * 1664525u + 1013904223u;
            sample = static_cast<float>(noise >> 16) / 65536.f - .5f;
        }
        const auto a = std::make_shared<ScopeTap>(rate, .25);
        const auto b = std::make_shared<ScopeTap>(rate, .25);
        const auto write = [&](std::int64_t stamp, std::uint64_t epoch, std::int32_t lag, float polarity,
                               float leadingOutlier = 0.f) {
            for (std::int32_t i = 0; i < frameCount; ++i) {
                const auto from = i - lag;
                target[static_cast<std::size_t>(i)] = from >= 0 && from < frameCount
                    ? polarity * main[static_cast<std::size_t>(from)] : 0.f;
            }
            if (leadingOutlier != 0.f) target[static_cast<std::size_t>(frameCount - 4 * radius)] = leadingOutlier;
            a->write(main.data(), nullptr, frameCount, stamp, true, epoch);
            const auto split = frameCount / 3;
            b->write(target.data(), nullptr, split, stamp, true, epoch);
            b->write(target.data() + split, nullptr, frameCount - split, stamp + split, true, epoch);
        };
        const auto plan = [&](std::int64_t stamp, std::uint64_t epoch) {
            // The ordinary spectrum windows only witness the common heard end.
            // The worker must obtain the longer interval from both stamped taps.
            std::array<SharedAnalysis::Result, 2> windows;
            const std::array taps{a, b};
            std::array<SharedAnalysis::Lease::SourcePosition, 2> heads;
            for (std::size_t side = 0; side < taps.size(); ++side) {
                auto &window = windows[side];
                window.request.track = side == 0 ? 1 : 2;
                window.request.fftSize = 1024;
                window.windowSerial = 1;
                window.firstStamp = stamp + frameCount - window.request.fftSize;
                window.firstSequence = taps[side]->written() - window.request.fftSize;
                window.generation = taps[side]->generation();
                window.sampleRate = rate;
                window.advancing = true;
                window.transportRevision = epoch;
                heads[side] = {window.generation, taps[side]->written()};
            }
            return focusLagPreviewRequest({&windows[0], &windows[1]}, heads, epoch, stamp + frameCount);
        };

        const bool maximumRate = rate == 768000.;
        const auto lag = maximumRate ? 137 - radius : radius - 137;
        write(first, revision, lag, maximumRate ? -1.f : 1.f);
        const auto request = plan(first, revision);
        check(request && request->maxLagSamples == radius &&
                  request->firstStamp == first + frameCount - 4 * radius,
              "high-rate Align plans the whole 20 ms search with 40 ms of common support");
        if (!request) continue;
        if (maximumRate) {
            a->write(static_cast<const float *>(nullptr), nullptr, 32, first + frameCount, true, revision);
            const auto preview = focusFindLagOnTaps(*request, {a, b});
            check(preview && preview->lagSamples == lag && preview->correlation < -.99999 &&
                      a->written() == frameCount + 32 && b->written() == frameCount,
                  "768 kHz recovers the exact negative lag and inverted polarity from unequal continuous heads");

            auto wrongRate = std::make_shared<ScopeTap>(384000., .25);
            wrongRate->write(target.data(), nullptr, frameCount, first, true, revision);
            auto mixed = *request;
            mixed.sources[1].generation = wrongRate->generation();
            check(!focusFindLagOnTaps(mixed, {a, wrongRate}), "the high-rate path refuses a mixed-rate tap pair");

            const auto later = first + 3 * frameCount;
            write(later, revision + 1, radius, 1.f);
            const auto edge = plan(later, revision + 1);
            check(edge && !focusFindLagOnTaps(*edge, {a, b}),
                  "a strongest peak on the high-rate search boundary is refused");

            for (std::size_t i = 0; i < main.size(); ++i)
                main[i] = static_cast<float>(i % 127) / 127.f - .5f;
            write(later + frameCount, revision + 2, lag, 1.f);
            const auto periodic = plan(later + frameCount, revision + 2);
            check(periodic && !focusFindLagOnTaps(*periodic, {a, b}),
                  "multiple equal high-rate alignment peaks do not produce an offset");
            continue;
        }

        SharedAnalysis broker(
            [&](adi::engine::ScopeSource source, double) -> std::shared_ptr<const ScopeTap> {
                return source.track == 1 ? a : b;
            }, [](adi::engine::ScopeSource) {}, SharedAnalysis::Mode::Manual);
        auto mainLease = broker.acquire({1});
        auto targetLease = broker.acquire({2});
        check(mainLease.available() && targetLease.available(), "both high-rate worker sources are already leased");
        auto measured = broker.requestFocusLag(*request);
        broker.pump();
        const auto ready = measured.read();
        check(measured && measured.status() == Status::Ready && ready && ready->lagSamples == lag &&
                  ready->correlation > .99999,
              "the 192 kHz worker publishes the exact positive lag outside the old range");

        auto stale = broker.requestFocusLag(*request);
        const auto afterSeek = first + 3 * frameCount;
        write(afterSeek, revision + 1, -lag, 1.f);
        broker.pump();
        check(stale && stale.status() == Status::Refused && !stale.read(),
              "a high-rate seek refuses a pending request from the previous transport revision");
        const auto fresh = plan(afterSeek, revision + 1);
        auto recovered = fresh ? broker.requestFocusLag(*fresh) : SharedAnalysis::FocusLagLease{};
        broker.pump();
        const auto next = recovered.read();
        check(recovered && recovered.status() == Status::Ready && next && next->lagSamples == -lag &&
                  next->correlation > .99999,
              "the same high-rate taps can measure a new heard interval after the seek refusal");

        for (auto &sample : main) sample *= 5.e-6f;
        write(afterSeek + frameCount, revision + 2, lag, 1.f, 10000.f);
        const auto quiet = plan(afterSeek + frameCount, revision + 2);
        const auto quietPreview = quiet ? focusFindLagOnTaps(*quiet, {a, b}) : std::nullopt;
        check(quietPreview && quietPreview->lagSamples == lag && quietPreview->correlation > .99999,
              "an outlier outside the winning window does not erase its quiet target variance");
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
void parkedSpectrogramWake() {
    constexpr int window = 480, hop = 240, block = 401, blocks = 33;
    constexpr int quietFrames = block * blocks, untilHop = hop - quietFrames % hop;
    constexpr std::int64_t stamp = 800;
    constexpr std::uint64_t revision = 62;
    const std::array<float, window> zeros{};
    std::array<float, window> left, right;
    left.fill(.5f);
    right.fill(-.25f);

    for (const bool joinScope : {false, true}) {
        Fixture parked, reference;
        parked.tap = std::make_shared<ScopeTap>(48000., .25);
        reference.tap = std::make_shared<ScopeTap>(48000., .25);
        SharedAnalysis::Request request{1, ScopePoint::PostFader, window};
        request.hopSize = hop;
        request.window = adi::dsp::Window::Rectangular;
        request.outputs = SharedAnalysis::Spectrogram;
        auto view = parked.broker.acquire(request);
        auto control = reference.broker.acquire(request);
        const auto write = [=](Fixture &fixture, const float *l, const float *r, int frames) {
            fixture.tap->write(l, r, frames, stamp, false, revision);
            fixture.broker.pump();
        };
        write(parked, nullptr, nullptr, window);
        write(reference, zeros.data(), zeros.data(), window);
        const auto first = view.read();
        const auto settled = parked.broker.stats();
        check(first && first->columns.size() == 1 && first->columns.back().firstSequence == 0 &&
                  settled.analysisWindows == 1 && settled.transforms == 0 && settled.spectralPlans == 1,
              "the stopped Spectrogram seeds one retained quiet column without an FFT");

        for (int i = 0; i < blocks; ++i) {
            write(parked, nullptr, nullptr, block);
            write(reference, zeros.data(), zeros.data(), block);
        }
        const auto idle = parked.broker.stats();
        const auto controlQuiet = control.read();
        check(first && view.read() == first && controlQuiet &&
                  controlQuiet->firstSequence == quietFrames - quietFrames % hop &&
                  parked.tap->oldest() > window && idle.workerPasses == settled.workerPasses &&
                  idle.cursorReads == settled.cursorReads && idle.transforms == settled.transforms &&
                  idle.analysisWindows == settled.analysisWindows && idle.publications == settled.publications,
              "the Spectrogram stays still with no worker work across a certified silent tap wrap");

        SharedAnalysis::Lease scope;
        if (joinScope) {
            request.outputs = SharedAnalysis::Scope;
            scope = parked.broker.acquire(request);
            parked.broker.pump();
            const auto joined = view.read(), scopeWindow = scope.read();
            const auto stats = parked.broker.stats();
            check(first && scopeWindow && joined == scopeWindow && scopeWindow->scopeLeft.size() == window &&
                      joined->historySerial == first->historySerial && joined->columns.size() == 1 &&
                      joined->columns.back().serial == first->columns.back().serial &&
                      joined->columns.back().binsDbfs == first->columns.back().binsDbfs &&
                      stats.taps == 1 && stats.domains == 1 && stats.consumers == 2 &&
                      stats.spectralPlans == idle.spectralPlans && stats.cursorReads == idle.cursorReads &&
                      stats.analysisWindows == idle.analysisWindows && stats.transforms == idle.transforms,
                  "a compatible Scope joins the retained quiet window without appending a Spectrogram column");
        }
        const auto held = view.read();
        const auto beforeWake = parked.broker.stats();
        write(parked, left.data(), right.data(), untilHop - 1);
        write(reference, left.data(), right.data(), untilHop - 1);
        const auto partial = parked.broker.stats();
        check(held && view.read() == held && control.read() == controlQuiet &&
                  partial.transforms == beforeWake.transforms && partial.publications == beforeWake.publications,
              "a partial resumed Spectrogram hop does not advance the held raster");

        write(parked, left.data(), right.data(), 1);
        write(reference, left.data(), right.data(), 1);
        const auto audible = view.read(), expected = control.read();
        const auto wake = parked.broker.stats();
        check(first && held && audible && expected && audible != held && expected != controlQuiet &&
                  audible->firstSequence == quietFrames + untilHop && audible->firstSequence == expected->firstSequence &&
                  audible->firstStamp == stamp && audible->windowSerial == expected->windowSerial &&
                  audible->transportRevision == revision && !audible->advancing && !audible->discontinuity &&
                  audible->sourceEpoch == first->sourceEpoch && audible->overrunCount == first->overrunCount &&
                  audible->peak == std::array{.5f, .25f} && exactBins(audible->binsDbfs, expected->binsDbfs) &&
                  wake.analysisWindows == beforeWake.analysisWindows + 1 && wake.transforms == beforeWake.transforms + 1 &&
                  wake.publications == beforeWake.publications + 1,
              "the first audible Spectrogram window completes at the original hop with exact bins and stamps");
        check(first && audible && expected && audible->historySerial == first->historySerial + 1 &&
                  audible->columns.size() == first->columns.size() + 1 && !expected->columns.empty() &&
                  audible->columns.back().firstSequence == expected->columns.back().firstSequence &&
                  audible->columns.back().firstStamp == stamp && audible->columns.back().binsDbfs &&
                  expected->columns.back().binsDbfs &&
                  exactBins(*audible->columns.back().binsDbfs, *expected->columns.back().binsDbfs) &&
                  audible->droppedHistoryColumns == first->droppedHistoryColumns,
              "the sweep advances just one real column after the pause with no synthetic gap");
    }
}
void spectrogramHeardGrid() {
    using Source = adi::engine::ScopeSource;
    constexpr int window = 1024, hop = 256;
    constexpr std::int64_t preStart = 17, postStart = 154, aligned = hop, end = aligned + window;
    constexpr std::uint64_t revision = 37;
    auto preTap = std::make_shared<ScopeTap>(48000., .25);
    auto postTap = std::make_shared<ScopeTap>(48000., .25);
    SharedAnalysis broker([&](Source source, double) -> std::shared_ptr<const ScopeTap> {
        if (source == Source{1, ScopePoint::ChainInput, 0}) return preTap;
        if (source == Source{1, ScopePoint::PreFader, 0}) return postTap;
        return {};
    }, [](Source) {}, SharedAnalysis::Mode::Manual);
    SharedAnalysis::Request request{1, ScopePoint::ChainInput, window};
    request.hopSize = hop;
    request.window = adi::dsp::Window::Rectangular;
    request.outputs = SharedAnalysis::Spectrogram;
    auto ordinary = broker.acquire(request);
    request.heardSpectrogramGrid = true;
    request.outputs |= SharedAnalysis::Spectrum;
    auto pre = broker.acquire(request);
    request.point = ScopePoint::PreFader;
    auto post = broker.acquire(request);
    check(ordinary && pre && post && broker.stats().taps == 2 && broker.stats().domains == 3,
          "aligned pre and post use two taps while ordinary history shares the existing pre tap");
    if (!ordinary || !pre || !post) return;
    request.outputs = SharedAnalysis::Spectrum;
    check(!broker.acquire(request), "heard-grid requests require Spectrogram output");
    request.outputs = SharedAnalysis::Spectrogram | SharedAnalysis::Scope;
    check(!broker.acquire(request), "heard-grid requests cannot change an ordinary Scope window");

    const auto write = [&](ScopeTap &tap, std::int64_t first, std::int64_t finish, float amplitude,
                           std::uint64_t epoch, bool advancing = true) {
        const auto frames = static_cast<std::int32_t>(finish - first);
        std::array<float, window + hop> samples;
        for (std::int32_t i = 0; i < frames; ++i) {
            const auto phase = static_cast<double>((first + i) % window);
            samples[static_cast<std::size_t>(i)] = amplitude * static_cast<float>(
                std::sin(2. * std::numbers::pi * 16. * phase / window));
        }
        tap.write(samples.data(), samples.data(), frames, first, advancing, epoch);
    };
    const auto hasColumn = [](const auto &result, std::int64_t stamp, std::uint64_t epoch) {
        return result && result->advancing && result->transportRevision == epoch && result->firstStamp == stamp &&
               result->columns.size() == 1 && result->columns.front().firstStamp == stamp &&
               result->columns.front().binsDbfs && result->columns.front().binsDbfs->size() > 16;
    };
    const auto difference = [](const auto &before, const auto &after) {
        return static_cast<double>((*after->columns.front().binsDbfs)[16]) -
               (*before->columns.front().binsDbfs)[16];
    };
    write(*preTap, 83, 83 + window, .875f, revision - 1, false);
    write(*postTap, 83, 83 + window, .875f, revision - 1, false);
    broker.pump();
    check(!pre.read() && !post.read(), "comparison history discards parked samples even when they contain a signal");

    write(*preTap, preStart, end - 1, .25f, revision);
    write(*postTap, postStart, end - 1, .5f, revision);
    broker.pump();
    check(hasColumn(ordinary.read(), preStart, revision) && !pre.read() && !post.read(),
          "ordinary history starts at sample 17 while the aligned windows wait for their last sample");
    write(*preTap, end - 1, end, .25f, revision);
    write(*postTap, end - 1, end, .5f, revision);
    broker.pump();
    const auto firstPre = pre.read(), firstPost = post.read();
    const auto gain = 20. * std::log10(2.);
    check(hasColumn(firstPre, aligned, revision) && hasColumn(firstPost, aligned, revision) &&
              firstPre->firstSequence - firstPost->firstSequence == postStart - preStart &&
              near(difference(firstPre, firstPost), gain),
          "staggered taps share sample 256 and the expected +6.02 dB transfer");

    write(*preTap, preStart, end, .5f, revision + 1);
    broker.pump();
    const auto soughtPre = pre.read();
    check(hasColumn(soughtPre, aligned, revision + 1) && hasColumn(post.read(), aligned, revision) &&
              post.read() == firstPost,
          "overlapping heard stamps after a seek still expose distinct transport revisions until the other tap arrives");
    write(*postTap, postStart, end, .25f, revision + 1);
    broker.pump();
    const auto soughtPost = post.read();
    check(hasColumn(soughtPre, aligned, revision + 1) && hasColumn(soughtPost, aligned, revision + 1) &&
              near(difference(soughtPre, soughtPost), -gain),
          "both new tap epochs recover the same grid and the expected -6.02 dB transfer");

    postTap = std::make_shared<ScopeTap>(48000., .25);
    broker.refreshSources();
    check(!post.read() && pre.read() == soughtPre, "replacing the post tap clears only its previous columns");
    write(*postTap, postStart, end, .25f, revision + 1);
    broker.pump();
    const auto rebound = post.read();
    check(soughtPost && hasColumn(rebound, aligned, revision + 1) && rebound->generation != soughtPost->generation,
          "the replacement tap establishes its own phase at the same exact heard sample");

    ordinary.reset(); pre.reset(); post.reset();
    const auto hidden = broker.stats();
    write(*preTap, end, end + window, .5f, revision + 1);
    write(*postTap, end, end + window, .25f, revision + 1);
    broker.pump();
    const auto after = broker.stats();
    check(hidden.taps == 0 && hidden.consumers == 0 && after.transforms == hidden.transforms &&
              after.publications == hidden.publications && after.cursorReads == hidden.cursorReads &&
              after.workerPasses == hidden.workerPasses,
          "releasing the aligned and ordinary readers leaves no tap, FFT or worker work");
}

void parkedWindowHopPhase() {
    constexpr int window = 480, hop = 240, block = 401, blocks = 33;
    constexpr int quietFrames = block * blocks, untilHop = hop - quietFrames % hop;
    constexpr std::int64_t stamp = 800;
    constexpr std::uint64_t revision = 61;
    constexpr auto realtime = SharedAnalysis::RealtimeMaximum | SharedAnalysis::RealtimeSigma;
    constexpr auto ordinary = SharedAnalysis::Spectrum | SharedAnalysis::Scope | SharedAnalysis::Phase;
    constexpr auto bandedPhase = SharedAnalysis::Phase | SharedAnalysis::PhaseBands;
    constexpr std::array<std::uint32_t, 8> outputs{
        SharedAnalysis::Scope, SharedAnalysis::Phase, SharedAnalysis::Spectrum, ordinary,
        SharedAnalysis::RealtimeMaximum, SharedAnalysis::RealtimeSigma, ordinary | realtime, bandedPhase};
    const std::array<float, window> zeros{};
    std::array<float, window> left, right;
    left.fill(.5f);
    right.fill(-.25f);
    for (const auto output : outputs) {
        Fixture parked, reference;
        parked.tap = std::make_shared<ScopeTap>(48000., .25);
        reference.tap = std::make_shared<ScopeTap>(48000., .25);
        SharedAnalysis::Request request{1, ScopePoint::PostFader, window};
        request.hopSize = hop;
        request.window = adi::dsp::Window::Rectangular;
        request.outputs = output;
        if (output & SharedAnalysis::PhaseBands) request.channel = SharedAnalysis::Channel::StereoPower;
        auto view = parked.broker.acquire(request);
        auto uninterrupted = reference.broker.acquire(request);
        const float *silence = nullptr;
        const auto write = [=](Fixture &fixture, const float *l, const float *r, int count) {
            fixture.tap->write(l, r, count, stamp, false, revision);
            fixture.broker.pump();
        };
        write(parked, silence, silence, window);
        write(reference, zeros.data(), zeros.data(), window);
        const auto first = view.read();
        const auto firstMaximum = view.readMaximum(), firstControlMaximum = uninterrupted.readMaximum();
        const auto firstSigma = view.readRealtimeSigma(), firstControlSigma = uninterrupted.readRealtimeSigma();
        const auto settled = parked.broker.stats();
        for (int i = 0; i < blocks; ++i) {
            write(parked, silence, silence, block);
            // A written zero buffer is the same audio, minus the parked-silence certificate.
            write(reference, zeros.data(), zeros.data(), block);
        }
        const auto idle = parked.broker.stats();
        const auto controlQuiet = uninterrupted.read();
        check(first && view.read() == first && controlQuiet && controlQuiet->firstSequence == quietFrames - quietFrames % hop &&
                  parked.tap->oldest() > window && settled.workerPasses == idle.workerPasses &&
                  settled.cursorReads == idle.cursorReads && settled.transforms == idle.transforms &&
                  settled.analysisWindows == idle.analysisWindows && settled.publications == idle.publications &&
                  view.readMaximum() == firstMaximum && view.readRealtimeSigma() == firstSigma,
              "windowed monitors hold one quiet frame with no extra work across a certified ring wrap");

        write(parked, left.data(), right.data(), untilHop - 1);
        write(reference, left.data(), right.data(), untilHop - 1);
        check(view.read() == first && uninterrupted.read() == controlQuiet,
              "an incomplete same-stamp monitor hop still holds the previous picture");
        write(parked, left.data(), right.data(), 1);
        write(reference, left.data(), right.data(), 1);
        const auto audible = view.read(), expected = uninterrupted.read();
        const auto transforms = (output & SharedAnalysis::PhaseBands) != 0 ? 2u
            : (output & (SharedAnalysis::Spectrum | realtime)) != 0 ? 1u : 0u;
        check(first && audible && expected && audible != first && expected != controlQuiet &&
                  audible->windowSerial == expected->windowSerial &&
                  audible->firstSequence == quietFrames + untilHop && audible->firstSequence == expected->firstSequence &&
                  audible->firstStamp == stamp && audible->transportRevision == revision && !audible->advancing &&
                  audible->sourceEpoch == first->sourceEpoch && audible->overrunCount == first->overrunCount &&
                  audible->peak == std::array{.5f, .25f} && exactBins(audible->rms, expected->rms) &&
                  audible->correlation == expected->correlation && exactBins(audible->binsDbfs, expected->binsDbfs) &&
                  exactBins(audible->scopeLeft, expected->scopeLeft) && exactBins(audible->scopeRight, expected->scopeRight) &&
                  parked.broker.stats().transforms == transforms,
              "windowed monitors wake at the uninterrupted hop with matching samples, stamps and levels");
        if (output & SharedAnalysis::PhaseBands)
            check(first && first->phaseBands && settled.transforms == 0 && audible && expected &&
                      audible->phaseBands && expected->phaseBands && *audible->phaseBands == *expected->phaseBands &&
                      std::ranges::none_of(*first->phaseBands, [](const auto &band) { return band.has_value(); }) &&
                      std::ranges::any_of(*audible->phaseBands, [](const auto &band) { return band && *band < -.999f; }),
                  "the displayed Phase bands retain an empty floor, then match the original hop with one stereo FFT pair");
        const auto exactTrace = [&](const auto &resumed, const auto &control, const auto &initial, const auto &controlInitial) {
            return resumed && initial && control && controlInitial && resumed != initial &&
                   resumed->window == audible && control->window == expected &&
                   exactBins(initial->binsDbfs, controlInitial->binsDbfs) &&
                   exactBins(resumed->binsDbfs, control->binsDbfs);
        };
        if (output & SharedAnalysis::RealtimeMaximum)
            check(exactTrace(view.readMaximum(), uninterrupted.readMaximum(), firstMaximum, firstControlMaximum),
                  "parked maximum keeps exact floor and first audible bins without an additional transform");
        if (output & SharedAnalysis::RealtimeSigma)
            check(exactTrace(view.readRealtimeSigma(), uninterrupted.readRealtimeSigma(), firstSigma, firstControlSigma),
                  "parked Sigma keeps exact floor and first audible bins without an additional transform");
    }
}

template <SharedAnalysis::Output Initial>
void parkedRealtimeModeJoins() {
    constexpr auto opposite = Initial == SharedAnalysis::RealtimeMaximum ? SharedAnalysis::RealtimeSigma
                                                                         : SharedAnalysis::RealtimeMaximum;
    constexpr int window = 480, hop = 240, block = 401, blocks = 33;
    constexpr int quietFrames = block * blocks, monitor = hop - quietFrames % hop;
    constexpr std::int64_t stamp = 800, admission = window + quietFrames;
    constexpr std::uint64_t revision = 21;
    Fixture parked, reference;
    parked.tap = std::make_shared<ScopeTap>(48000., .25);
    reference.tap = std::make_shared<ScopeTap>(48000., .25);
    SharedAnalysis::Request request{1, ScopePoint::PostFader, window};
    request.hopSize = hop;
    request.window = adi::dsp::Window::Rectangular;
    request.outputs = Initial;
    auto existing = parked.broker.acquire(request), control = reference.broker.acquire(request);
    const auto readExisting = [](const auto &lease) {
        if constexpr (Initial == SharedAnalysis::RealtimeMaximum) return lease.readMaximum();
        else return lease.readRealtimeSigma();
    };
    const auto readOpposite = [](const auto &lease) {
        if constexpr (opposite == SharedAnalysis::RealtimeMaximum) return lease.readMaximum();
        else return lease.readRealtimeSigma();
    };
    const float *silence = nullptr;
    const std::array<float, window> zeros{};
    const auto writePair = [&](const float *z, const float *referenceSamples, int frames) {
        parked.tap->write(z, z, frames, stamp, false, revision);
        reference.tap->write(referenceSamples, referenceSamples, frames, stamp, false, revision);
        parked.broker.pump();
        reference.broker.pump();
    };
    writePair(silence, zeros.data(), window);
    const auto first = readExisting(existing);
    for (int i = 0; i < blocks; ++i) writePair(silence, zeros.data(), block);
    auto sameOutput = parked.broker.acquire(request);
    check(first && readExisting(existing) == first && readExisting(sameOutput) == first &&
              parked.tap->oldest() > window,
          "a duplicate realtime observer shares the realized floor without waking a wrapped tap");

    request.outputs = SharedAnalysis::Scope | opposite;
    auto joined = parked.broker.acquire(request), joinedControl = reference.broker.acquire(request);
    parked.broker.pump();
    reference.broker.pump();
    const auto rebased = readExisting(existing), expected = readExisting(control);
    const auto before = parked.broker.stats();
    check(first && rebased && expected && rebased->window == joined.read() && rebased->window == existing.read() &&
              readExisting(sameOutput) == rebased && rebased->window->windowSerial == expected->window->windowSerial &&
              rebased->window->firstSequence == quietFrames - quietFrames % hop &&
              rebased->window->firstSequence == expected->window->firstSequence &&
              rebased->window->firstStamp == stamp && rebased->window->transportRevision == revision &&
              rebased->window->sourceEpoch == first->window->sourceEpoch &&
              rebased->window->overrunCount == first->window->overrunCount &&
              exactBins(rebased->binsDbfs, first->binsDbfs) && exactBins(rebased->window->scopeLeft, zeros) &&
              !readOpposite(joined) && !readOpposite(joinedControl) &&
              before.taps == 1 && before.domains == 1 && before.spectralPlans == 1 && before.transforms == 0,
          "a new scope and opposite trace catch up the realized holder without admitting stale samples");

    std::array<float, window> signal;
    signal.fill(.5f);
    writePair(signal.data(), signal.data(), monitor - 1);
    check(readExisting(existing) == rebased && !readOpposite(joined),
          "a partial resumed hop retains the published realtime metadata and keeps the fresh mode gated");
    writePair(signal.data(), signal.data(), 1);
    const auto audible = readExisting(existing), expectedAudible = readExisting(control);
    check(audible && expectedAudible && audible != rebased && audible->window == joined.read() &&
              audible->window->firstSequence == quietFrames + monitor &&
              audible->window->windowSerial == expectedAudible->window->windowSerial &&
              exactBins(audible->binsDbfs, expectedAudible->binsDbfs) && !readOpposite(joined) &&
              parked.broker.stats().transforms == before.transforms + 1,
          "the surviving realtime mode wakes on the original hop while the new mode rejects overlapping history");
    writePair(signal.data(), signal.data(), window);
    const auto fresh = readOpposite(joined), freshControl = readOpposite(joinedControl);
    check(fresh && freshControl && fresh->window == joined.read() && fresh->window->firstSequence >= admission &&
              fresh->window->firstSequence == freshControl->window->firstSequence &&
              fresh->window->windowSerial == freshControl->window->windowSerial &&
              exactBins(fresh->binsDbfs, freshControl->binsDbfs),
          "the new mode first reads a wholly post-admission window on the existing tap and hop grid");
}

void parkedStereoBalance() {
    constexpr int window = 480, historyFrames = 96000, historyBlocks = historyFrames / window;
    constexpr std::int64_t stoppedStamp = 800;
    constexpr std::uint64_t revision = 59;
    Fixture f;
    f.tap = std::make_shared<ScopeTap>(48000., .25);
    SharedAnalysis::Request request{1, ScopePoint::PostFader, window};
    request.hopSize = window;
    auto spectrum = f.broker.acquire(request);
    spectrum.setStereoBalanceEnabled(true);
    std::array<float, window> left, right;
    left.fill(.5f); right.fill(.25f);
    f.tap->write(left.data(), right.data(), window, stoppedStamp, false, revision);
    f.broker.pump();
    const auto monitored = spectrum.read();
    check(monitored && !monitored->advancing && monitored->balanceRms2s &&
              (*monitored->balanceRms2s)[0] == .5f && (*monitored->balanceRms2s)[1] == .25f &&
              f.broker.stats().taps == 1 && f.broker.stats().spectralPlans == 1,
          "a visible pinned-spectrum balance starts on the same parked stereo trigger and Spectrum plan");

    const auto writeQuiet = [&] {
        const float *zero = nullptr;
        f.tap->write(zero, zero, window, stoppedStamp, false, revision);
    };
    for (int block = 0; block < historyBlocks - 1; ++block) {
        writeQuiet();
        if (block % 4 == 3) f.broker.pump();
    }
    f.broker.pump();
    const auto tail = spectrum.read();
    const auto leftTailRms = .5 * std::sqrt(static_cast<double>(window) / historyFrames);
    check(tail && tail->balanceRms2s && near((*tail->balanceRms2s)[0], leftTailRms, 1e-7) &&
              near((*tail->balanceRms2s)[1], leftTailRms * .5, 1e-7),
          "the last audible block remains in the balance history one block before two seconds of silence");
    writeQuiet();
    f.broker.pump();
    const auto atZero = spectrum.read();
    const auto settled = f.broker.stats();
    check(atZero && atZero->balanceRms2s && *atZero->balanceRms2s == std::array{0.f, 0.f} &&
              atZero->firstSequence == historyFrames && settled.transforms == 1,
          "the visible balance first reaches exact zero at the complete two-second quiet boundary");

    const auto beyondRing = f.tap->capacity() / window + 8;
    for (std::int64_t block = 0; block < beyondRing; ++block) {
        writeQuiet();
        if (block % 4 == 3) f.broker.pump();
    }
    const float *zero = nullptr;
    f.tap->write(zero, zero, window / 3, stoppedStamp, false, revision);
    f.broker.pump();
    const auto idle = f.broker.stats();
    check(atZero && f.tap->oldest() > atZero->firstSequence && spectrum.read() == atZero &&
              idle.workerPasses == settled.workerPasses && idle.cursorReads == settled.cursorReads &&
              idle.analysisWindows == settled.analysisWindows && idle.transforms == settled.transforms &&
              idle.publications == settled.publications,
          "a visible zero balance stops work while certified parked silence rolls past the physical ring");

    const auto monitorStart = f.tap->written();
    f.tap->write(left.data(), right.data(), window, stoppedStamp, false, revision);
    f.broker.pump();
    const auto woke = spectrum.read();
    // The first complete hop starts with the 160 quiet samples that preceded
    // monitoring. Only its remaining samples belong in the resumed reading.
    constexpr auto quietTail = window / 3;
    const auto firstMonitorRms = .5 * std::sqrt(static_cast<double>(window - quietTail) / historyFrames);
    check(atZero && woke && !woke->advancing && woke->transportRevision == revision &&
              woke->firstSequence == monitorStart - quietTail && woke->sourceEpoch == atZero->sourceEpoch &&
              woke->overrunCount == atZero->overrunCount && woke->balanceRms2s &&
              near((*woke->balanceRms2s)[0], firstMonitorRms, 1e-7) &&
              near((*woke->balanceRms2s)[1], firstMonitorRms * .5, 1e-7) &&
              f.broker.stats().transforms == settled.transforms + 1,
          "same-stamp stopped monitoring wakes immediately with the full trailing denominator and no false overrun");

    constexpr std::int64_t resumedStamp = 12000;
    f.tap->write(left.data(), right.data(), window, resumedStamp, true, revision + 1);
    f.broker.pump();
    const auto resumed = spectrum.read();
    check(woke && resumed && resumed->advancing && resumed->firstStamp == resumedStamp &&
              resumed->transportRevision == revision + 1 && resumed->sourceEpoch > woke->sourceEpoch &&
              resumed->balanceRms2s && (*resumed->balanceRms2s)[0] == .5f && (*resumed->balanceRms2s)[1] == .25f,
          "advancing playback starts cleanly with its first complete stereo balance window");
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
void phaseBandCorrelation() {
    constexpr int samples = 4096;
    Fixture f;
    SharedAnalysis::Request request{1, ScopePoint::PostFader, samples};
    request.hopSize = samples / 2;
    request.outputs = SharedAnalysis::Phase;
    auto broad = f.broker.acquire(request);
    std::array<float, samples> left{}, right{}, silent{};
    for (std::size_t i = 0; i < left.size(); ++i) {
        const auto phase = 2. * std::numbers::pi * static_cast<double>(i) / samples;
        const auto low = static_cast<float>(.25 * std::sin(32. * phase));
        const auto high = static_cast<float>(.25 * std::sin(384. * phase));
        left[i] = low + high;
        right[i] = low - high;
    }
    f.tap->write(left.data(), right.data(), samples, 0, true, 1);
    check(f.broker.pump() == 0 && broad.read() && !broad.read()->phaseBands && !f.broker.stats().spectralPlans,
          "broadband phase draws its raw scope and correlation without a spectral plan");

    const auto acquireCalls = f.acquireCalls;
    request.outputs |= SharedAnalysis::PhaseBands;
    auto bands = f.broker.acquire(request), sibling = f.broker.acquire(request);
    check(bands.available() && sibling.available() && f.acquireCalls == acquireCalls && f.broker.stats().taps == 1 &&
              f.broker.stats().domains == 1 && f.broker.stats().spectralPlans == 1 && !bands.read(),
          "visible detailed phase readers share one lazy plan on the existing source");
    check(f.broker.pump() == 2, "a completed detailed phase picture needs one pair of real transforms");
    const auto picture = bands.read();
    const auto correlation = [&](const auto &result, std::size_t band, double expected) {
        return result && result->phaseBands && (*result->phaseBands)[band] &&
               near(*(*result->phaseBands)[band], expected, 1.e-5);
    };
    check(picture && picture == sibling.read() && picture == broad.read() && picture->firstStamp == 0 &&
              picture->scopeLeft.size() == samples && correlation(picture, 4, 1.) && correlation(picture, 16, -1.) &&
              !(*picture->phaseBands)[10] && near(picture->correlation, 0.),
          "375 Hz and 4500 Hz expose opposite band signs without changing the broadband measurement");

    f.tap->write(left.data(), right.data(), samples, samples, true, 1);
    check(f.broker.pump() == 2 && bands.read() && bands.read()->firstStamp == samples,
          "multiple completed hops measure only the final visible phase window");
    f.tap->write(left.data(), silent.data(), samples / 2, 0, true, 2);
    check(f.broker.pump() == 0 && !bands.read(),
          "a partial seek window cannot retain the previous band's phase data");
    f.tap->write(left.data() + samples / 2, silent.data(), samples / 2, samples / 2, true, 2);
    const auto beforeQuiet = f.broker.stats().transforms;
    f.broker.pump();
    const auto oneSided = bands.read();
    const auto allAbsent = [](const auto &result) {
        return result && result->phaseBands &&
               std::ranges::none_of(*result->phaseBands, [](const auto &value) { return value.has_value(); });
    };
    check(allAbsent(oneSided) && oneSided->transportRevision == 2 &&
              f.broker.stats().transforms == beforeQuiet,
          "a one-sided source marks every band unavailable without executing either transform");
    f.tap->write(static_cast<const float *>(nullptr), nullptr, samples, samples, false, 2);
    f.broker.pump();
    const auto parked = bands.read();
    const auto parkedStats = f.broker.stats();
    f.tap->write(static_cast<const float *>(nullptr), nullptr, samples, samples, false, 2);
    f.broker.pump();
    check(allAbsent(parked) && bands.read() == parked &&
              f.broker.stats().workerPasses == parkedStats.workerPasses,
          "parked silence retains one empty band picture without repeat work");

    f.tap = std::make_shared<ScopeTap>(96000, SharedAnalysis::kTapSeconds);
    f.broker.refreshSources();
    check(!bands.read(), "a replacement sample rate invalidates prior band data");
    f.tap->write(left.data(), right.data(), samples, 0, true, 3);
    check(f.broker.pump() == 2 && correlation(bands.read(), 7, 1.) && correlation(bands.read(), 19, -1.),
          "the band mapping follows the new source rate");
    bands.reset();
    check(f.broker.stats().spectralPlans == 1 && sibling.read(), "closing one view keeps its sibling's band plan");
    sibling.reset();
    f.tap->write(left.data(), right.data(), samples, samples, true, 3);
    check(f.broker.pump() == 0 && !f.broker.stats().spectralPlans && broad.read() && !broad.read()->phaseBands &&
              f.broker.stats().taps == 1,
          "the last detailed view releases the plan while the broadband view remains FFT-free");

    Fixture beyondBins;
    beyondBins.tap = std::make_shared<ScopeTap>(768000, .25);
    request.fftSize = 32;
    request.hopSize = 32;
    auto tiny = beyondBins.broker.acquire(request);
    beyondBins.tap->write(left.data(), right.data(), 32, 0, true, 1);
    check(beyondBins.broker.pump() == 0 && allAbsent(tiny.read()),
          "a resolution with no in-band FFT bins skips the stereo transforms");

    Fixture nyquist;
    nyquist.tap = std::make_shared<ScopeTap>(32000, .6);
    request.fftSize = samples;
    request.hopSize = samples;
    request.window = adi::dsp::Window::Rectangular;
    auto highBand = nyquist.broker.acquire(request);
    for (std::size_t i = 0; i < left.size(); ++i) {
        const auto angle = 2. * std::numbers::pi * 2016. * static_cast<double>(i) / samples;
        const auto tone = static_cast<float>(.25 * std::sin(angle));
        const float edge = i % 2 ? -.25f : .25f;
        left[i] = tone + edge;
        right[i] = tone - edge;
    }
    const auto lastBand = SharedAnalysis::kPhaseBandCount - 1;
    nyquist.tap->write(left.data(), left.data(), samples, 0, true, 4);
    check(nyquist.broker.pump() == 2 && correlation(highBand.read(), lastBand, 1.),
          "the penultimate-frequency and Nyquist tones settle together in one coherent band");
    nyquist.tap->write(left.data(), right.data(), samples, samples, true, 4);
    // The same-amplitude Nyquist tone has twice the power of the ordinary sine.
    // Its reversed polarity contributes -2 for the ordinary tone's +1.
    check(nyquist.broker.pump() == 2 && correlation(highBand.read(), lastBand, -1. / 3.) &&
              highBand.read()->firstStamp == samples,
          "the mixed high band counts the real-only Nyquist bin once and other bins twice");
}
void phaseBandSpatialMeasurements() {
    constexpr int window = 4096;
    constexpr std::size_t lowBand = 4, highBand = 16;
    constexpr auto ordinary = SharedAnalysis::Phase | SharedAnalysis::PhaseBands;
    constexpr auto detailed = ordinary | SharedAnalysis::PhaseBandDetail;
    Fixture f;
    SharedAnalysis::Request request{1, ScopePoint::PostFader, window, SharedAnalysis::Channel::StereoPower};
    request.hopSize = window / 2;
    request.window = adi::dsp::Window::Rectangular;
    request.outputs = ordinary;
    auto phase = f.broker.acquire(request);
    std::array<float, window> left{}, right{}, tone{};
    for (std::size_t i = 0; i < left.size(); ++i) {
        const auto angle = 2. * std::numbers::pi * static_cast<double>(i) / window;
        tone[i] = static_cast<float>(.25 * std::sin(32. * angle));
        const auto high = static_cast<float>(.25 * std::sin(384. * angle));
        left[i] = tone[i] + high;
        right[i] = tone[i] - high;
    }
    const auto bandIs = [](const auto &values, std::size_t band, double expected) {
        return values[band] && near(*values[band], expected, 1.e-5);
    };
    const auto spatialIs = [&](const auto &result, std::size_t band, double width, double pan) {
        return result && result->phaseBandSpatial &&
               bandIs(result->phaseBandSpatial->width, band, width) &&
               bandIs(result->phaseBandSpatial->pan, band, pan);
    };
    const auto empty = [](const auto &values) {
        return std::ranges::none_of(values, [](const auto &value) { return value.has_value(); });
    };
    f.tap->write(left.data(), right.data(), window, 0, true, 1);
    check(f.broker.pump() == 2, "ordinary Phase first computes one stereo pair");
    const auto original = phase.read();
    const auto baseline = f.broker.stats();
    // Rate discovery can call the owner twice while retaining one physical tap.
    const auto acquireCalls = f.acquireCalls;
    check(original && original->phaseBands && !original->phaseBandSpatial &&
              bandIs(*original->phaseBands, lowBand, 1.) && bandIs(*original->phaseBands, highBand, -1.),
          "the existing Phase picture measures correlation without publishing spatial detail");

    request.outputs = detailed;
    auto detail = f.broker.acquire(request);
    check(f.broker.pump() == 0, "adding spatial detail to a retained stereo frame reuses its L/R transforms");
    const auto stereo = detail.read();
    const auto stereoStats = f.broker.stats();
    check(original && stereo && stereo == phase.read() && stereo->windowSerial == original->windowSerial &&
              stereo->firstSequence == original->firstSequence && stereo->sourceEpoch == original->sourceEpoch &&
              original->phaseBands && stereo->phaseBands && *stereo->phaseBands == *original->phaseBands &&
              spatialIs(stereo, lowBand, 0., 0.) && spatialIs(stereo, highBand, 1., 0.) &&
              !stereo->phaseBandSpatial->width[10] && !stereo->phaseBandSpatial->pan[10],
          "spatial detail distinguishes in-phase and antiphase width at the same centred pan and leaves empty ERBs blank");
    check(stereoStats.taps == baseline.taps && stereoStats.domains == baseline.domains &&
              stereoStats.spectralPlans == baseline.spectralPlans && stereoStats.spectralPlans == 1 &&
              f.acquireCalls == acquireCalls,
          "ordinary Phase and spatial detail share one tap, domain and lazy plan");

    for (std::size_t i = 0; i < left.size(); ++i) {
        left[i] = tone[i];
        right[i] = .5f * tone[i];
    }
    f.tap->write(left.data(), right.data(), window, 10000, true, 2);
    check(f.broker.pump() == 2, "new stereo width and pan still use one pair of transforms");
    const auto unequal = detail.read();
    check(spatialIs(unequal, lowBand, .1, -.6) && unequal->phaseBands &&
              bandIs(*unequal->phaseBands, lowBand, 1.),
          "a two-to-one amplitude ratio gives ten percent side energy and signed pan without changing correlation");

    detail.reset();
    left.fill(0.f);
    f.tap->write(left.data(), tone.data(), window, 20000, true, 3);
    check(f.broker.pump() == 0, "correlation alone still needs no FFT for a one-sided source");
    const auto mono = phase.read();
    check(mono && mono->phaseBands && empty(*mono->phaseBands) && !mono->phaseBandSpatial,
          "the current one-sided Phase frame has neither valid correlation nor undemanded spatial fields");

    detail = f.broker.acquire(request);
    check(f.broker.pump() == 1, "same-window spatial upgrade transforms only the live lane");
    const auto rightOnly = detail.read();
    check(mono && spatialIs(rightOnly, lowBand, .5, 1.) && rightOnly == phase.read() &&
              rightOnly->windowSerial == mono->windowSerial && rightOnly->firstSequence == mono->firstSequence &&
              rightOnly->phaseBands && empty(*rightOnly->phaseBands),
          "right-only energy has full right pan and equal Mid and Side energy, with correlation unavailable");
    auto sibling = f.broker.acquire(request);
    check(f.broker.pump() == 0 && sibling.read() == rightOnly && f.broker.stats().spectralPlans == 1 &&
              f.acquireCalls == acquireCalls,
          "a compatible second view shares the immutable one-sided measurements without transforming again");

    const float *silence = nullptr;
    f.tap->write(silence, silence, window, 30000, false, 4);
    check(f.broker.pump() == 0, "full silence invokes no band transforms in spatial detail");
    const auto silent = detail.read();
    const auto parked = f.broker.stats();
    check(silent && silent->phaseBands && empty(*silent->phaseBands) &&
              (!silent->phaseBandSpatial ||
               (empty(silent->phaseBandSpatial->width) && empty(silent->phaseBandSpatial->pan))),
          "a fully silent Phase result contains no width, pan or correlation values");
    f.tap->write(silence, silence, window, 30000, false, 4);
    check(f.broker.pump() == 0 && detail.read() == silent && f.broker.stats().cursorReads == parked.cursorReads &&
              f.broker.stats().workerPasses == parked.workerPasses,
          "a visible, settled spatial detail stops work in certified parked silence");

    detail.reset();
    sibling.reset();
    phase.reset();
    const auto hidden = f.broker.stats();
    f.tap->write(tone.data(), right.data(), window, 40000, true, 5);
    check(f.broker.pump() == 0 && hidden.taps == 0 && hidden.spectralPlans == 0 && f.releaseCalls == 1 &&
              f.broker.stats().cursorReads == hidden.cursorReads,
          "removing the last Phase consumer retires the shared plan and tap before more samples arrive");
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
        for (const auto window : {adi::dsp::Window::Hann, adi::dsp::Window::Rectangular,
                                  adi::dsp::Window::Hamming, adi::dsp::Window::Blackman}) {
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
void spectrumLevels() {
    constexpr int rate = 48000, window = 480, hop = 240, crestFrames = rate / 20;
    constexpr int warmup = rate / 8, earlyImpulse = warmup - crestFrames;
    constexpr std::uint64_t revision = 19;
    Fixture f;
    SharedAnalysis::Request request{1, ScopePoint::PostFader, window};
    request.hopSize = hop;
    auto raw = f.broker.acquire(request), levels = f.broker.acquire(request);
    const auto before = f.broker.stats();
    std::vector<float> left, right;
    std::int64_t nextStamp = 0;
    auto write = [&](int count, float l, float r, std::uint64_t rev) {
        left.assign(static_cast<std::size_t>(count), l);
        right.assign(static_cast<std::size_t>(count), r);
        f.tap->write(left.data(), right.data(), count, nextStamp, true, rev);
        nextStamp += count;
    };
    auto feed = [&](int count, float l, float r, int block) {
        while (count > 0) {
            const auto frames = std::min(count, block);
            write(frames, l, r, revision);
            f.broker.pump();
            count -= frames;
        }
    };
    const auto reading = [](const auto &picture, double rmsL, double rmsR, double crestL, double crestR) {
        return picture && picture->spectrumLevels &&
            near(picture->spectrumLevels->integratedRms[0], rmsL, 1.e-8) &&
            near(picture->spectrumLevels->integratedRms[1], rmsR, 1.e-8) &&
            near(picture->spectrumLevels->maxCrestDb[0], crestL, 1.e-7) &&
            near(picture->spectrumLevels->maxCrestDb[1], crestR, 1.e-7);
    };

    // The existing tap already holds nonzero pre-admission audio, and its
    // last 240 frames will overlap the first new FFT picture.
    write(window + hop, .875f, .75f, revision);
    levels.setSpectrumLevelsEnabled(true);
    f.broker.pump();
    const auto preAdmission = raw.read();
    check(preAdmission && !preAdmission->spectrumLevels && f.broker.stats().spectrumLevelHistories == 1,
          "Spectrum level demand fences at the current writer, excluding unread earlier audio");
    const auto partialBefore = f.broker.stats();
    feed(hop - 1, .25f, .125f, hop);
    check(raw.read() == preAdmission && f.broker.stats().publications == partialBefore.publications,
          "new level samples alone do not publish a partial Spectrum picture");
    feed(1, .25f, .125f, hop);
    const auto first = levels.read();
    check(reading(first, .25, .125, 0., 0.) && first == raw.read() &&
              first->firstSequence == window && f.broker.stats().transforms == partialBefore.transforms + 1,
          "the first shared level picture counts only its newly admitted samples");
    feed(rate - hop, .25f, .125f, 377);
    const auto baseline = raw.read();
    check(reading(baseline, .25, .125, 0., 0.),
          "integrated levels count each sample once across arbitrary writes and overlapped FFT hops");

    feed(crestFrames, .5f, .125f, 613);
    const auto loud = levels.read();
    const auto peakCrest = 10. * std::log10(3.5);
    check(reading(loud, std::sqrt(1. / 14.), .125, peakCrest, 0.),
          "the 50 millisecond running RMS and integrated denominator give the calibrated held crest");
    feed(crestFrames, .25f, .125f, 421);
    const auto tail = levels.read();
    check(reading(tail, std::sqrt((rate + 5. * crestFrames) / (16. * (rate + 2 * crestFrames))),
                  .125, peakCrest, 0.) && tail == raw.read() && tail != loud,
          "the maximum ratio holds as the integrated RMS changes; a held short RMS alone would drift");

    // A new transport epoch clears the held ratio and the RMS interval. An
    // impulse among otherwise fairly quiet samples would exceed 0 dB if the
    // startup guard erroneously accepted it before the rolling RMS cleared.
    const auto previousEpoch = tail ? tail->sourceEpoch : 0;
    nextStamp = 200000;
    write(window, 0.f, 0.f, revision + 1);
    f.broker.pump();
    const auto reset = raw.read();
    check(reading(reset, 0., 0., 0., 0.) &&
              reset->sourceEpoch > previousEpoch && reset->firstStamp == 200000,
          "a timeline break clears both integrated RMS and held crest at the first full window");
    for (int elapsed = window; elapsed < warmup; elapsed += hop) {
        left.assign(hop, 0.f); right.assign(hop, 0.f);
        if (elapsed == earlyImpulse) left.front() = .5f;
        f.tap->write(left.data(), right.data(), hop, nextStamp, true, revision + 1);
        nextStamp += hop;
        f.broker.pump();
    }
    const auto warmed = raw.read();
    check(reading(warmed, .5 / std::sqrt(static_cast<double>(warmup)), 0., 0., 0.),
          "a startup impulse cannot set a crest after its 50 millisecond energy has cleared");
    left.assign(hop, .5f); right.assign(hop, .25f);
    left[0] = std::numeric_limits<float>::quiet_NaN();
    left[1] = std::numeric_limits<float>::infinity();
    right[2] = -std::numeric_limits<float>::infinity();
    f.tap->write(left.data(), right.data(), hop, nextStamp, true, revision + 1);
    nextStamp += hop;
    f.broker.pump();
    const auto finite = raw.read();
    const auto integratedL = std::sqrt(.25 * (hop - 1) / (warmup + hop));
    const auto integratedR = .25 * std::sqrt(static_cast<double>(hop - 1) / (warmup + hop));
    const auto crestAfterWarmup = 10. * std::log10(static_cast<double>(warmup + hop) / crestFrames);
    const auto expectedCrestL = crestAfterWarmup + 10. * std::log10(static_cast<double>(hop - 2) / (hop - 1));
    check(reset && reading(finite, integratedL, integratedR, expectedCrestL, crestAfterWarmup) &&
              finite->nonFiniteSamples == 3 && finite->sourceEpoch == reset->sourceEpoch,
          "nonfinite lane samples contribute zero power and still occupy their time positions");

    const auto measured = f.broker.stats();
    check(measured.taps == before.taps && measured.domains == before.domains &&
              measured.spectralPlans == before.spectralPlans && f.acquireCalls == 1,
          "visible level measurement shares the tap and Spectrum transform plan");
    levels.reset();
    write(hop, .25f, .125f, revision + 1);
    f.broker.pump();
    const auto undemanded = raw.read();
    const auto dropped = f.broker.stats();
    check(undemanded && !undemanded->spectrumLevels && dropped.spectrumLevelHistories == 0 &&
              dropped.taps == 1 && dropped.spectralPlans == 1 && dropped.transforms == measured.transforms + 1,
          "dropping the last levels reader frees its bounded history while ordinary Spectrum stays live");
    raw.reset();
    check(f.broker.stats().taps == 0 && f.broker.stats().spectrumLevelHistories == 0 && f.releaseCalls == 1,
          "hiding the final Spectrum releases the shared tap and level history");
}

void spectrumSamplePeaksAndClips() {
    constexpr int window = 480, hop = window / 2;
    constexpr auto extreme = std::numeric_limits<float>::max();
    Fixture f;
    SharedAnalysis::Request request{1, ScopePoint::PostFader, window, SharedAnalysis::Channel::Left};
    request.hopSize = hop;
    auto raw = f.broker.acquire(request), levels = f.broker.acquire(request);
    request.outputs = SharedAnalysis::Meters;
    auto meters = f.broker.acquire(request);
    std::array<float, window> left{}, right{};
    const auto write = [&](int count, std::int64_t stamp, std::uint64_t revision = 1) {
        f.tap->write(left.data(), right.data(), count, stamp, true, revision);
    };
    const auto agrees = [](const auto &picture, std::array<float, 2> peak, std::array<std::uint64_t, 2> clips) {
        return picture && picture->spectrumLevels && picture->spectrumLevels->samplePeak == peak &&
               picture->spectrumLevels->sampleClips == clips;
    };

    left.fill(1.5f);
    right.fill(-4.f);
    write(window, 0);
    levels.setSpectrumLevelsEnabled(true);
    f.broker.pump();
    const auto excluded = raw.read();
    const auto before = f.broker.stats();
    check(excluded && !excluded->spectrumLevels && excluded->clippedSamples == 2 * window,
          "held sample levels exclude unread audio already at the writer when admitted");

    left.fill(.25f);
    right.fill(.5f);
    left[0] = 1.f;
    left[1] = -1.f;
    left[3] = 1.125f;
    left[4] = -extreme;
    left[5] = std::numeric_limits<float>::quiet_NaN();
    left[6] = std::numeric_limits<float>::infinity();
    right[0] = -1.f;
    right[1] = -1.25f;
    right[7] = std::nextafter(1.f, 0.f);
    right[8] = -std::numeric_limits<float>::infinity();
    write(hop, window);
    f.broker.pump();
    const auto peak = levels.read();
    check(agrees(peak, {extreme, 1.25f}, {2, 1}) && peak == raw.read(),
          "held sample levels preserve the finite peak per lane and count each strict over once");
    check(peak && peak == meters.read() && peak->clippedSamples == 2 * hop + 6 && peak->nonFiniteSamples == 3,
          "current-window Meters still count both full-scale polarities and exclude invalid samples");

    left.fill(.25f);
    right.fill(.5f);
    write(window, window + hop);
    f.broker.pump();
    const auto held = levels.read();
    const auto after = f.broker.stats();
    check(agrees(held, {extreme, 1.25f}, {2, 1}) && held->clippedSamples == 0,
          "the level interval holds its peak and count after hot samples leave the overlapping FFT window");
    check(after.taps == before.taps && after.domains == before.domains &&
              after.spectralPlans == before.spectralPlans && after.transforms == before.transforms + 3,
          "sample level statistics reuse the active tap, domain and one transform per audible hop");

    right.fill(-1.f);
    write(window, 10000, 2);
    f.broker.pump();
    const auto sought = levels.read();
    check(agrees(sought, {.25f, 1.f}, {0, 0}) && held && sought->sourceEpoch > held->sourceEpoch &&
              sought->clippedSamples == window,
          "the new transport interval clears held peaks and strict overs while Meters still counts equality");

    levels.setSpectrumLevelsEnabled(false);
    left.fill(1.5f);
    right.fill(-4.f);
    write(hop, 10000 + window, 2);
    f.broker.pump();
    const auto disabled = raw.read();
    check(disabled && !disabled->spectrumLevels && f.broker.stats().spectrumLevelHistories == 0,
          "turning off the last sample-level reader drops the held statistics while Spectrum stays live");
    levels.setSpectrumLevelsEnabled(true);
    left.fill(.125f);
    right.fill(.25f);
    write(hop, 10000 + window + hop, 2);
    f.broker.pump();
    check(agrees(levels.read(), {.125f, .25f}, {0, 0}),
          "a new statistics request after disable starts at its own writer fence");
}

void restartSpectrumLevels() {
    constexpr int window = 480, hop = 240;
    constexpr std::uint64_t revision = 55;
    Fixture f;
    SharedAnalysis::Request request{1, ScopePoint::PostFader, window};
    request.hopSize = hop;
    request.window = adi::dsp::Window::Rectangular;
    auto raw = f.broker.acquire(request), levels = f.broker.acquire(request);
    levels.setSpectrumLevelsEnabled(true);
    std::array<float, window> left{}, right{};
    std::int64_t stamp = 0;
    const auto write = [&](int frames, float l, float r) {
        left.fill(l); right.fill(r);
        f.tap->write(left.data(), right.data(), frames, stamp, true, revision);
        stamp += frames;
    };
    for (int block = 0; block < 25; ++block) {
        write(window, .25f, .125f);
        f.broker.pump();
    }
    write(window, 1.25f, -1.5f);
    f.broker.pump();
    const auto held = levels.read();
    check(held && held == raw.read() && held->spectrumLevels &&
              held->spectrumLevels->samplePeak == std::array{1.25f, 1.5f} &&
              held->spectrumLevels->sampleClips == std::array<std::uint64_t, 2>{window, window} &&
              held->spectrumLevels->maxCrestDb[0] > 0.,
          "the clear fixture seeds held stereo statistics and a shared Raw publication");

    auto sibling = f.broker.acquire(request);
    sibling.setSpectrumLevelsEnabled(true);
    check(!raw.restartSpectrumLevels() && !levels.restartSpectrumLevels() && !sibling.restartSpectrumLevels() &&
              f.broker.stats().spectrumLevelHistories == 1 && levels.read() == held && sibling.read() == held,
          "a non-owner or either of two opt-in owners cannot clear another view's statistics");
    sibling.reset();

    write(hop, .75f, -.625f); // buffered before Reset, but the worker has not read it
    const auto fence = f.tap->written();
    const auto before = f.broker.stats();
    allocationSizeCount = 0;
    observeAllocationSizes = true;
    const bool restarted = levels.restartSpectrumLevels();
    observeAllocationSizes = false;
    const auto immediate = f.broker.stats();
    check(restarted && allocationSizeCount == 0 && held == raw.read() &&
              immediate.workerPasses == before.workerPasses && immediate.cursorReads == before.cursorReads &&
              immediate.publications == before.publications && immediate.transforms == before.transforms &&
              immediate.taps == before.taps && immediate.spectralPlans == before.spectralPlans &&
              immediate.spectrumLevelHistories == 1,
          "a sole owner's Reset changes no immutable Raw picture and schedules no work or allocation itself");

    f.broker.pump();
    const auto buffered = raw.read();
    check(held && buffered && buffered != held && !buffered->spectrumLevels &&
              buffered->generation == held->generation && buffered->sourceEpoch == held->sourceEpoch &&
              buffered->windowSerial == held->windowSerial + 1 && buffered->firstSequence == fence - window &&
              buffered->firstStamp == fence - window && buffered->peak == std::array{1.25f, 1.5f},
          "Raw still publishes the pre-reset buffered audio without counting it in the restarted statistics");

    write(hop, .125f, .25f);
    f.broker.pump();
    const auto fresh = levels.read();
    const auto after = f.broker.stats();
    check(held && held->spectrumLevels && fresh && fresh->spectrumLevels && fresh == raw.read() &&
              fresh->peak == std::array{.75f, .625f} && fresh->spectrumLevels->samplePeak == std::array{.125f, .25f} &&
              fresh->spectrumLevels->sampleClips == std::array<std::uint64_t, 2>{0, 0} &&
              fresh->spectrumLevels->integratedRms == std::array{.125, .25} &&
              fresh->spectrumLevels->maxCrestDb == std::array{0., 0.} &&
              held->spectrumLevels->sampleClips == std::array<std::uint64_t, 2>{window, window} &&
              fresh->windowSerial == held->windowSerial + 2 && fresh->sourceEpoch == held->sourceEpoch &&
              fresh->overrunCount == held->overrunCount && after.transforms == before.transforms + 2 &&
              after.taps == 1 && after.spectralPlans == 1 && f.acquireCalls == 1 && f.releaseCalls == 0,
          "the first post-reset interval excludes earlier audio without resetting Raw, its FFT plan or continuity");
}

void restartParkedSpectrumLevels() {
    constexpr int window = 480, hop = 240, block = 401, blocks = 33;
    constexpr int quietFrames = block * blocks, untilHop = hop - quietFrames % hop;
    constexpr std::int64_t stamp = 800;
    constexpr std::uint64_t revision = 58;
    Fixture f;
    f.tap = std::make_shared<ScopeTap>(48000., .25);
    SharedAnalysis::Request request{1, ScopePoint::PostFader, window};
    request.hopSize = hop;
    auto raw = f.broker.acquire(request), levels = f.broker.acquire(request);
    levels.setSpectrumLevelsEnabled(true);
    const float *silence = nullptr;
    const auto write = [&](int frames, const float *l, const float *r) {
        f.tap->write(l, r, frames, stamp, false, revision);
        f.broker.pump();
    };
    write(window, silence, silence);
    const auto held = raw.read();
    const auto settled = f.broker.stats();
    for (int i = 0; i < blocks; ++i)
        write(block, silence, silence);
    check(held && held->spectrumLevels && raw.read() == held && f.tap->oldest() > window &&
              f.broker.stats().workerPasses == settled.workerPasses,
          "Reset can be reached after a long certified parked interval has wrapped the tap");

    const bool restarted = levels.restartSpectrumLevels();
    f.broker.pump();
    const auto immediate = f.broker.stats();
    check(restarted && raw.read() == held && immediate.workerPasses == settled.workerPasses &&
              immediate.cursorReads == settled.cursorReads && immediate.publications == settled.publications &&
              immediate.transforms == settled.transforms,
          "Reset reconciles the parked cursor without demanding a read, FFT or publication");

    std::array<float, hop> left{}, right{};
    left.fill(.5f); right.fill(.25f);
    write(untilHop - 1, left.data(), right.data());
    check(raw.read() == held, "an incomplete monitoring hop cannot publish after a parked Reset");
    write(1, left.data(), right.data());
    const auto audible = raw.read();
    check(held && audible && audible->spectrumLevels && audible != held &&
              audible->spectrumLevels->integratedRms == std::array{.5, .25} &&
              audible->spectrumLevels->samplePeak == std::array{.5f, .25f} &&
              audible->spectrumLevels->sampleClips == std::array<std::uint64_t, 2>{0, 0} &&
              audible->windowSerial == held->windowSerial + quietFrames / hop + 1 &&
              audible->firstSequence == quietFrames + untilHop && audible->firstStamp == stamp &&
              audible->sourceEpoch == held->sourceEpoch && audible->overrunCount == held->overrunCount &&
              f.broker.stats().transforms == 1,
          "first same-stamp monitoring after a parked Reset retains the original hop and a fresh statistics denominator");
}

void spectrumTruePeak() {
    constexpr int window = 480, hop = 240, toneFrames = 4 * hop;
    constexpr std::uint64_t revision = 63;
    constexpr std::array chunks{61, 317, 7, 179, 396};
    const float toneLeft = 1.2f * std::sqrt(.5f), toneRight = .65f * std::sqrt(.5f);
    for (const int rate : {44100, 48000, 96000, 192000}) {
        Fixture f;
        f.tap = std::make_shared<ScopeTap>(static_cast<double>(rate), .25);
        SharedAnalysis::Request request{1, ScopePoint::PostFader, window, SharedAnalysis::Channel::Left};
        request.hopSize = hop;
        request.window = adi::dsp::Window::Rectangular;
        auto raw = f.broker.acquire(request), levels = f.broker.acquire(request);
        raw.setSpectrumTruePeakEnabled(true);
        levels.setSpectrumLevelsEnabled(true);
        std::array<float, toneFrames> left{}, right{};
        std::int64_t stamp = 0;
        const auto write = [&](const float *l, const float *r, int count) {
            f.tap->write(l, r, count, stamp, true, revision);
            stamp += count;
        };
        const auto constant = [&](int frames, float l, float r) {
            left.fill(l); right.fill(r);
            write(left.data(), right.data(), frames);
        };
        constant(window, .25f, .125f);
        f.broker.pump();
        const auto initial = raw.read();
        check(initial && initial->spectrumLevels && !initial->spectrumLevels->truePeak &&
                  f.broker.stats().spectrumTruePeakHistories == 0,
              "True Peak is off by default and a Raw lease cannot enable a levels observer's true peak");

        constant(hop, 1.5f, -1.75f); // old programme remains unread when True Peak is enabled
        levels.setSpectrumTruePeakEnabled(true);
        f.broker.pump();
        const auto baseline = raw.read();
        const auto measured = f.broker.stats();
        check(baseline && baseline->spectrumLevels &&
                  (!baseline->spectrumLevels->truePeak || *baseline->spectrumLevels->truePeak == std::array{0., 0.}) &&
                  baseline->spectrumLevels->samplePeak == std::array{1.5f, 1.75f} &&
                  baseline->spectrumLevels->sampleClips == std::array<std::uint64_t, 2>{hop, hop} &&
                  measured.spectrumTruePeakHistories == 1 && measured.taps == 1 && measured.spectralPlans == 1,
              "opting into True Peak fences off buffered old programme without restarting sample statistics");

        adi::dsp::TruePeakMeter leftOracle, rightOracle;
        leftOracle.prepare(rate, 1); rightOracle.prepare(rate, 1);
        for (std::size_t i = 0; i < left.size(); ++i) {
            const auto sign = (i % 4 < 2) ? 1.f : -1.f;
            left[i] = sign * toneLeft; right[i] = sign * toneRight;
        }
        const float *leftLane = left.data(), *rightLane = right.data();
        leftOracle.process(&leftLane, 1, toneFrames);
        rightOracle.process(&rightLane, 1, toneFrames);
        int cursor = 0;
        for (const int count : chunks) {
            write(left.data() + cursor, right.data() + cursor, count);
            cursor += count;
            f.broker.pump();
        }
        const auto peaked = levels.read();
        const auto afterTone = f.broker.stats();
        const auto programmes = window + hop + toneFrames;
        const auto integratedL = std::sqrt((window * .0625 + hop * 2.25 +
            toneFrames * static_cast<double>(toneLeft) * toneLeft) / programmes);
        const auto integratedR = std::sqrt((window * .015625 + hop * 3.0625 +
            toneFrames * static_cast<double>(toneRight) * toneRight) / programmes);
        check(peaked && peaked == raw.read() && peaked->spectrumLevels && peaked->spectrumLevels->truePeak &&
                  *peaked->spectrumLevels->truePeak == std::array{leftOracle.peak(), rightOracle.peak()} &&
                  peaked->spectrumLevels->samplePeak == std::array{1.5f, 1.75f} &&
                  peaked->spectrumLevels->sampleClips == std::array<std::uint64_t, 2>{hop, hop} &&
                  near(peaked->spectrumLevels->integratedRms[0], integratedL, 1.e-9) &&
                  near(peaked->spectrumLevels->integratedRms[1], integratedR, 1.e-9) &&
                  afterTone.taps == measured.taps && afterTone.spectralPlans == measured.spectralPlans &&
                  afterTone.transforms == measured.transforms + toneFrames / hop,
              "irregular writes produce canonical per-channel True Peak without an extra FFT or altered sample history");
        check(toneLeft < 1.f && (rate >= 176400 ?
                  leftOracle.peak() == toneLeft && rightOracle.peak() == toneRight :
                  leftOracle.peak() > 1.05 && leftOracle.peak() < 1.3 && rightOracle.peak() > toneRight),
              "Fs/4 creates an intersample over on L below 176.4 kHz and follows the canonical high-rate sample path");

        const bool reset = levels.restartSpectrumLevels();
        check(reset && raw.read() == peaked && f.broker.stats().spectrumTruePeakHistories == 1,
              "local statistics Reset preserves the running Raw result and the opted-in observer");
        constant(hop, 0.f, 0.f);
        f.broker.pump();
        const auto quiet = raw.read();
        check(peaked && quiet && quiet->spectrumLevels && quiet->spectrumLevels->truePeak &&
                  *quiet->spectrumLevels->truePeak == std::array{0., 0.} &&
                  quiet->spectrumLevels->samplePeak == std::array{0.f, 0.f} &&
                  quiet->peak[0] == toneLeft && quiet->sourceEpoch == peaked->sourceEpoch,
              "Reset clears FIR and held state, even while the overlapping Raw window still contains the old tone");

        constant(hop, .125f, .25f);
        f.broker.pump();
        levels.setSpectrumTruePeakEnabled(false);
        constant(hop, .0625f, .125f);
        f.broker.pump();
        const auto disabled = raw.read();
        check(disabled && disabled->spectrumLevels && !disabled->spectrumLevels->truePeak &&
                  disabled->spectrumLevels->samplePeak == std::array{.125f, .25f} &&
                  f.broker.stats().spectrumTruePeakHistories == 0 && f.broker.stats().spectrumLevelHistories == 1,
              "disabling True Peak frees only that observer's meter and keeps held sample history");

        levels.setSpectrumTruePeakEnabled(true);
        const auto shown = f.broker.stats();
        levels.reset();
        const auto hidden = f.broker.stats();
        check(shown.spectrumTruePeakHistories == 1 && hidden.spectrumTruePeakHistories == 0 &&
                  hidden.spectrumLevelHistories == 0 && hidden.taps == 1 && hidden.spectralPlans == 1 &&
                  f.acquireCalls == 1 && f.releaseCalls == 0,
              "hiding the levels view drops both optional histories while an ordinary Raw reader shares the tap");
    }
}

void parkedSpectrumLevels(bool withTruePeak) {
    constexpr int window = 480, hop = 240, block = 401, blocks = 33;
    constexpr int quietFrames = block * blocks, untilHop = hop - quietFrames % hop;
    Fixture parked, reference;
    parked.tap = std::make_shared<ScopeTap>(48000., .25);
    reference.tap = std::make_shared<ScopeTap>(48000., .25);
    SharedAnalysis::Request request{1};
    request.fftSize = window; request.hopSize = hop;
    auto levels = parked.broker.acquire(request), continuous = reference.broker.acquire(request);
    levels.setSpectrumLevelsEnabled(true); continuous.setSpectrumLevelsEnabled(true);
    levels.setSpectrumTruePeakEnabled(withTruePeak); continuous.setSpectrumTruePeakEnabled(withTruePeak);
    const std::array<float, window> zeros{};
    std::array<float, window> live;
    live.fill(.5f);
    const auto append = [](Fixture &fixture, const float *samples, int count) {
        fixture.tap->write(samples, samples, count, 0, false, 2);
        fixture.broker.pump();
    };
    append(parked, nullptr, window); append(reference, zeros.data(), window);
    const auto start = levels.read();
    const auto settled = parked.broker.stats();
    for (int i = 0; i < blocks; ++i) {
        append(parked, nullptr, block); append(reference, zeros.data(), block);
    }
    const auto idle = parked.broker.stats();
    check(parked.tap->oldest() > window && start && start->spectrumLevels && levels.read() == start &&
              start->spectrumLevels->truePeak.has_value() == withTruePeak &&
              idle.cursorReads == settled.cursorReads && idle.analysisWindows == settled.analysisWindows &&
              idle.transforms == settled.transforms && idle.publications == settled.publications,
          "an exact-zero level interval stays parked without further reads or publications at a ring wrap");
    append(parked, live.data(), untilHop - 1); append(reference, live.data(), untilHop - 1);
    check(levels.read() == start, "stopped monitoring preserves the level display until the original hop completes");
    append(parked, live.data(), 1); append(reference, live.data(), 1);
    const auto resumed = levels.read(), oracle = continuous.read();
    check(start && resumed && oracle && resumed->spectrumLevels && oracle->spectrumLevels &&
              resumed->firstSequence == oracle->firstSequence &&
              resumed->sourceEpoch == start->sourceEpoch && resumed->overrunCount == start->overrunCount &&
              resumed->spectrumLevels->integratedRms == oracle->spectrumLevels->integratedRms &&
              resumed->spectrumLevels->maxCrestDb == oracle->spectrumLevels->maxCrestDb &&
              resumed->spectrumLevels->samplePeak == std::array{.5f, .5f} &&
              resumed->spectrumLevels->samplePeak == oracle->spectrumLevels->samplePeak &&
              resumed->spectrumLevels->sampleClips == std::array<std::uint64_t, 2>{} &&
              resumed->spectrumLevels->sampleClips == oracle->spectrumLevels->sampleClips &&
              resumed->spectrumLevels->truePeak.has_value() == withTruePeak &&
              resumed->spectrumLevels->truePeak == oracle->spectrumLevels->truePeak &&
              resumed->spectrumLevels->maxCrestDb.front() > 0.,
          "parked levels count the omitted zeros for the exact uninterrupted RMS and crest on wake");
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
void parkedRealtimeAverage(bool burstBetweenPumps) {
    constexpr int window = 480, hop = 240, block = 401, blocks = 33;
    constexpr int quietFrames = block * blocks, untilHop = hop - quietFrames % hop;
    constexpr std::int64_t stamp = 800;
    constexpr std::uint64_t revision = 4;
    Fixture parked, reference;
    parked.tap = std::make_shared<ScopeTap>(48000., .25);
    reference.tap = std::make_shared<ScopeTap>(48000., .25);
    SharedAnalysis::Request request{1};
    request.outputs = SharedAnalysis::Average1s;
    request.fftSize = window;
    request.hopSize = hop;
    request.window = adi::dsp::Window::Rectangular;
    auto average = parked.broker.acquire(request);
    auto uninterrupted = reference.broker.acquire(request);
    const float *silence = nullptr;
    const std::array<float, window> zeros{};
    const auto write = [](Fixture &fixture, const float *samples, int count, std::int64_t position,
                          std::uint64_t transportRevision) {
        fixture.tap->write(samples, samples, count, position, false, transportRevision);
        fixture.broker.pump();
    };
    const auto atFloor = [](const auto &result) {
        return result && result->binsDbfs && !result->binsDbfs->empty() &&
            std::ranges::all_of(*result->binsDbfs, [](float db) { return db == adi::dsp::kDbFloor; });
    };

    write(parked, silence, window, stamp, revision);
    write(reference, zeros.data(), window, stamp, revision);
    const auto first = average.readAverage();
    const auto settled = parked.broker.stats();
    check(atFloor(first) && first->status == SharedAnalysis::AverageStatus::Ready &&
              first->processedWindows == 1 && first->window && !first->window->advancing &&
              settled.transforms == 0,
          "a first certified quiet window publishes a ready realtime average at the floor");

    for (int i = 0; i < blocks; ++i) {
        write(parked, silence, block, stamp, revision);
        // Actual zero-filled buffers cannot earn the producer's null-channel certificate.
        write(reference, zeros.data(), block, stamp, revision);
    }
    const auto idle = parked.broker.stats();
    const auto controlQuiet = uninterrupted.readAverage();
    check(parked.tap->oldest() > window && atFloor(controlQuiet) &&
              controlQuiet->processedWindows == 1 + quietFrames / hop && average.readAverage() == first &&
              idle.workerPasses == settled.workerPasses && idle.cursorReads == settled.cursorReads &&
              idle.analysisWindows == settled.analysisWindows && idle.transforms == settled.transforms &&
              idle.publications == settled.publications,
          "an exactly zero realtime average holds one ready publication through a certified parked ring wrap");

    std::array<float, window> signal;
    signal.fill(.5f);
    if (burstBetweenPumps) {
        constexpr int burstFrames = 71;
        for (auto *fixture : {&parked, &reference}) {
            const float *quietInput = fixture == &parked ? silence : zeros.data();
            fixture->tap->write(signal.data(), signal.data(), burstFrames, stamp, false, revision);
            fixture->tap->write(quietInput, quietInput, untilHop - burstFrames, stamp, false, revision);
        }
        const auto quiet = parked.tap->lastParkedSilence();
        check(quiet.begin == window + quietFrames + burstFrames && quiet.end == window + quietFrames + untilHop &&
                  average.readAverage() == first && parked.broker.stats().workerPasses == idle.workerPasses,
              "an audible burst and new quiet span both arrive before the realtime average resumes");
        parked.broker.pump();
        reference.broker.pump();
    } else {
        write(parked, signal.data(), untilHop - 1, stamp, revision);
        write(reference, signal.data(), untilHop - 1, stamp, revision);
        check(average.readAverage() == first && uninterrupted.readAverage() == controlQuiet,
              "same-stamp monitoring does not publish before the original partial hop completes");
        write(parked, signal.data(), 1, stamp, revision);
        write(reference, signal.data(), 1, stamp, revision);
    }
    const auto resumed = average.readAverage();
    const auto control = uninterrupted.readAverage();
    check(first && first->window && resumed && control && resumed->window && control->window &&
              resumed->status == SharedAnalysis::AverageStatus::Ready && resumed->epoch == first->epoch &&
              resumed->processedWindows == 2 + quietFrames / hop &&
              resumed->processedWindows == control->processedWindows && resumed->windowSerial == control->windowSerial &&
              resumed->window->firstSequence == quietFrames + untilHop &&
              resumed->window->firstSequence == control->window->firstSequence && resumed->window->firstStamp == stamp &&
              !resumed->window->advancing && resumed->window->sourceEpoch == first->window->sourceEpoch &&
              resumed->window->overrunCount == first->window->overrunCount && resumed->binsDbfs && control->binsDbfs &&
              !resumed->binsDbfs->empty() && exactBins(*resumed->binsDbfs, *control->binsDbfs) && resumed->binsDbfs->front() > adi::dsp::kDbFloor &&
              parked.broker.stats().transforms == 1 && reference.broker.stats().transforms == 1,
          burstBetweenPumps
              ? "a burst between pumps preserves the realtime average hop count, clock and bins without an overrun"
              : "parked monitoring matches the uninterrupted hop count, clock and bins without a false overrun");

    // A new transport revision clears the old state. This small but genuinely
    // nonzero window produces a smoother state below the visible dB floor.
    signal.fill(1.e-12f);
    write(parked, signal.data(), window, stamp, revision + 1);
    const auto tiny = average.readAverage();
    write(parked, silence, window, stamp, revision + 1);
    const auto fullyQuiet = average.readAverage();
    const auto beforeDecay = parked.broker.stats();
    write(parked, silence, hop, stamp, revision + 1);
    const auto decay = average.readAverage();
    const auto afterDecay = parked.broker.stats();
    check(resumed && atFloor(tiny) && tiny->epoch > resumed->epoch && tiny->processedWindows == 1 &&
              atFloor(fullyQuiet) && fullyQuiet->processedWindows == 3 && atFloor(decay) &&
              decay != fullyQuiet && decay->epoch == tiny->epoch && decay->processedWindows == 4 &&
              afterDecay.workerPasses == beforeDecay.workerPasses + 1 &&
              afterDecay.analysisWindows == beforeDecay.analysisWindows + 1 &&
              afterDecay.transforms == beforeDecay.transforms,
          "a visible floor does not park a mathematically nonzero exponential average");
}

void parkedRealtimeAverageJoinsScope() {
    constexpr int window = 480, hop = 240, block = 401, blocks = 33;
    constexpr int quietFrames = block * blocks, monitor = hop - quietFrames % hop;
    constexpr std::int64_t stamp = 800;
    constexpr std::uint64_t revision = 4;
    Fixture parked, reference;
    parked.tap = std::make_shared<ScopeTap>(48000., .25);
    reference.tap = std::make_shared<ScopeTap>(48000., .25);
    SharedAnalysis::Request request{1};
    request.outputs = SharedAnalysis::Average1s;
    request.fftSize = window;
    request.hopSize = hop;
    request.window = adi::dsp::Window::Rectangular;
    auto average = parked.broker.acquire(request);
    auto uninterrupted = reference.broker.acquire(request);
    const float *silence = nullptr;
    const std::array<float, window> zeros{};
    const auto writePair = [&](const float *parkedInput, const float *referenceInput, int frames) {
        parked.tap->write(parkedInput, parkedInput, frames, stamp, false, revision);
        reference.tap->write(referenceInput, referenceInput, frames, stamp, false, revision);
        parked.broker.pump();
        reference.broker.pump();
    };

    writePair(silence, zeros.data(), window);
    const auto first = average.readAverage();
    for (int i = 0; i < blocks; ++i)
        writePair(silence, zeros.data(), block);
    check(first && average.readAverage() == first && parked.tap->oldest() > window,
          "the realtime average is still parked when a second output joins its wrapped tap");

    request.outputs = SharedAnalysis::Scope;
    const auto scope = parked.broker.acquire(request), controlScope = reference.broker.acquire(request);
    parked.broker.pump();
    reference.broker.pump();
    const auto joined = average.readAverage(), control = uninterrupted.readAverage();
    const auto stable = parked.broker.stats();
    check(first && first->window && joined && joined->window && control && control->window &&
              joined->status == SharedAnalysis::AverageStatus::Ready && joined->epoch == first->epoch &&
              joined->processedWindows == 1 + quietFrames / hop && joined->processedWindows == control->processedWindows &&
              joined->windowSerial == control->windowSerial && joined->window == scope.read() &&
              control->window == controlScope.read() && joined->window->overrunCount == first->window->overrunCount &&
              joined->window->sourceEpoch == first->window->sourceEpoch &&
              joined->window->firstSequence == control->window->firstSequence &&
              exactBins(joined->window->scopeLeft, zeros) && exactBins(joined->window->scopeRight, zeros) &&
              stable.taps == 1 && stable.domains == 1 && stable.spectralPlans == 1 && stable.transforms == 0,
          "a same-domain Scope admission catches up certified hops while retaining the Average epoch and zero window");

    std::array<float, monitor> signal;
    signal.fill(.5f);
    writePair(signal.data(), signal.data(), monitor);
    const auto resumed = average.readAverage(), expected = uninterrupted.readAverage();
    check(first && first->window && resumed && expected && resumed->window && expected->window &&
              resumed->epoch == first->epoch && resumed->processedWindows == 2 + quietFrames / hop &&
              resumed->windowSerial == expected->windowSerial && resumed->window->firstSequence == quietFrames + monitor &&
              resumed->window->overrunCount == first->window->overrunCount && resumed->window == scope.read() &&
              resumed->binsDbfs && expected->binsDbfs && exactBins(*resumed->binsDbfs, *expected->binsDbfs) &&
              resumed->window->scopeLeft.size() == window && exactBins(resumed->window->scopeLeft, expected->window->scopeLeft),
          "monitoring after a parked Scope join keeps exact average and scope alignment with uninterrupted processing");
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
void deviceSpectrumLease() {
    using Request = SharedAnalysis::DeviceSpectrumRequest;
    using adi::engine::ScopeSource;
    const ScopeSource endpoint{4, ScopePoint::DeviceOutput, 77};
    auto tap = std::make_shared<ScopeTap>(48000, .25);
    std::vector<ScopeSource> attached, detached;
    SharedAnalysis broker([&](ScopeSource source, double) {
        attached.push_back(source);
        return source == endpoint ? tap : nullptr;
    }, [&](ScopeSource source) { detached.push_back(source); }, SharedAnalysis::Mode::Manual);
    const Request settings{4, 77, 1024, Request::Channel::Left, 3, true};
    auto invalid = settings;
    invalid.fftSize = 1536;
    check(!broker.acquireDeviceSpectrum(invalid), "device spectrum refuses an unsupported block size");
    invalid = settings; invalid.averageBlocks = 0;
    check(!broker.acquireDeviceSpectrum(invalid), "device spectrum rejects zero averaging blocks");
    invalid.averageBlocks = Request::kMaximumAverageBlocks + 1;
    check(!broker.acquireDeviceSpectrum(invalid), "device spectrum bounds the number of averaged blocks");
    invalid = settings; invalid.device = 0;
    check(!broker.acquireDeviceSpectrum(invalid) && attached.empty(), "invalid spectrum requests do not establish taps");

    auto device = broker.acquireDeviceSpectrum(settings);
    auto sibling = broker.acquireDeviceSpectrum(settings);
    SharedAnalysis::Request shared{4, ScopePoint::DeviceOutput, 1024, SharedAnalysis::Channel::Left};
    shared.device = 77;
    auto raw = broker.acquire(shared);
    check(device.available() && sibling.available() && raw.available() && attached == std::vector{endpoint} &&
              broker.stats().spectralPlans == 1 && broker.stats().domains == 1,
          "two device views and a compatible spectrum request use one device tap and FFT plan");
    check(!device.readBlockAverage() && !device.readMaximum(), "a new lease never reads an earlier average or peak");

    tone(*tap, 1024, .5f, 0.f);
    check(broker.pump() == 1 && raw.read() && device.read() == raw.read() && !device.readBlockAverage(),
          "one completed block is shared but does not complete a three-block average");
    if (!raw.read() || raw.read()->binsDbfs.size() != 513) {
        check(false, "natural one-sided order includes DC and Nyquist");
        return;
    }
    const auto first = raw.read();
    const auto &bins = first->binsDbfs;
    check(std::distance(bins.begin(), std::max_element(bins.begin(), bins.end())) == 16 &&
              near(bins[16], -6.020599913) && near(16 * first->sampleRate / first->request.fftSize, 750.),
          "natural DC-to-Nyquist bin order and full-scale sine calibration");
    tone(*tap, 256, .25f, 0.f, 1024);
    const auto nextTransforms = broker.pump();
    const auto second = raw.read();
    tone(*tap, 256, .125f, 0.f, 1280);
    const auto finalTransforms = broker.pump();
    const auto third = raw.read();
    const auto average = device.readBlockAverage();
    check(nextTransforms == 1 && finalTransforms == 1 && second && third && average &&
              average == sibling.readBlockAverage() && average->window == third && average->binsDbfs.size() == 513,
          "duplicate averaging demand emits one complete group with the exact window and no additional FFT");
    if (!second || !third || !average) return;
    const auto magnitude = [](double db) { return std::pow(10., db / 20.); };
    const double expected = 20 * std::log10((magnitude(bins[16]) + magnitude(second->binsDbfs[16]) +
                                           magnitude(third->binsDbfs[16])) / 3.);
    check(near(average->binsDbfs[16], expected) && device.readMaximum() &&
              near(device.readMaximum()->binsDbfs[16], bins[16]),
          "average uses linear block magnitudes and maximum retains the largest raw block");
    check(device.restartCumulativeMaximum() && !device.readMaximum() && sibling.readMaximum() &&
              device.readBlockAverage() == average, "restart clears only this lease's peak hold");

    std::array<float, 1024> left{}, right{};
    right.fill(.25f);
    tap->write(left.data(), right.data(), 1024, 2700, true, 9);
    broker.pump();
    check(!device.readBlockAverage() && average->window == third,
          "an epoch change drops the partial group and preserves any frozen result");
    auto rightSettings = settings;
    rightSettings.channel = Request::Channel::Right;
    rightSettings.averageBlocks = 1;
    rightSettings.maximum = false;
    auto rightLease = broker.acquireDeviceSpectrum(rightSettings);
    tap->write(left.data(), right.data(), 1024, 3724, true, 9);
    broker.pump();
    check(rightLease.read() && near(rightLease.read()->binsDbfs[0], -12.041199826) &&
              !rightLease.readBlockAverage() && attached == std::vector{endpoint} && broker.stats().spectralPlans == 2,
          "right and left have independent transforms on the same tap and one block reads directly");
    device.reset(); sibling.reset(); raw.reset(); rightLease.reset();
    const auto closed = broker.stats();
    tap->write(left.data(), right.data(), 1024, 4748, true, 9);
    broker.pump();
    const auto idle = broker.stats();
    check(detached == std::vector{endpoint} && !closed.taps && !closed.domains && !closed.spectralPlans &&
              closed.cursorReads == idle.cursorReads && closed.workerPasses == idle.workerPasses &&
              closed.transforms == idle.transforms,
          "closing the last panel releases the tap and schedules no analysis work for new audio");
}
void parkedDeviceSpectrum() {
    using Request = SharedAnalysis::DeviceSpectrumRequest;
    auto tap = std::make_shared<ScopeTap>(48000, .25);
    SharedAnalysis broker([&](adi::engine::ScopeSource, double) { return tap; },
                          [](adi::engine::ScopeSource) {}, SharedAnalysis::Mode::Manual);
    auto lease = broker.acquireDeviceSpectrum({1, 7, 1024, Request::Channel::Left, 3, true});
    const float *silent = nullptr;
    tap->write(silent, silent, 1536, 0, false, 1);
    check(broker.pump() == 0 && lease.readBlockAverage() && lease.readMaximum() &&
              lease.readBlockAverage()->binsDbfs[16] == adi::dsp::kDbFloor,
          "a parked device spectrum completes a silent average without executing FFTs");
    const auto parked = broker.stats();
    const auto quiet = lease.readBlockAverage();
    for (int block = 0; block < 64; ++block) {
        tap->write(silent, silent, 256, 0, false, 1);
        broker.pump();
    }
    const auto idle = broker.stats();
    check(quiet && lease.readBlockAverage() == quiet && parked.cursorReads == idle.cursorReads &&
              parked.workerPasses == idle.workerPasses && parked.transforms == idle.transforms,
          "a visible settled device spectrum does no work over certified parked silence");
    std::array<float, 1536> waveform{};
    for (std::size_t i = 0; i < waveform.size(); ++i)
        waveform[i] = .25f * static_cast<float>(std::sin(2. * std::numbers::pi * 16. * static_cast<double>(i) / 1024.));
    tap->write(waveform.data(), waveform.data(), 1536, 0, true, 2);
    broker.pump();
    const auto running = lease.readBlockAverage();
    check(running && running != quiet && running->window->transportRevision == 2 &&
              near(running->binsDbfs[16], -12.041199826) && broker.stats().transforms == idle.transforms + 3,
          "resuming starts a complete new group without analysing the skipped parked blocks");
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

bool matchingStereoMeterReadings(const adi::dsp::StereoEnvelopeMeter::Reading &actual,
                                 const adi::dsp::StereoEnvelopeMeter::Reading &expected) {
    const bool linkedMatches = actual.linkedDynamicDb.has_value() == expected.linkedDynamicDb.has_value() &&
        (!actual.linkedDynamicDb || near(*actual.linkedDynamicDb, *expected.linkedDynamicDb));
    return actual.peak == expected.peak && actual.sineReferencedRms == expected.sineReferencedRms &&
           actual.signCoincidence == expected.signCoincidence && actual.balance == expected.balance &&
           actual.maximumPeak == expected.maximumPeak && actual.peakHold == expected.peakHold &&
           actual.rmsHold == expected.rmsHold && actual.floatingPeak == expected.floatingPeak &&
           actual.clipCount == expected.clipCount && actual.linkedSineReferencedRms == expected.linkedSineReferencedRms &&
           near(actual.dynamicDb[0], expected.dynamicDb[0]) && near(actual.dynamicDb[1], expected.dynamicDb[1]) &&
           near(actual.balanceDb, expected.balanceDb) && near(actual.maxCrestFactorDb, expected.maxCrestFactorDb) && linkedMatches;
}

void stereoMeterReferenceProfiles() {
    using Meter = adi::dsp::StereoEnvelopeMeter;
    for (const int sampleRate : {44100, 96000}) {
        const auto tap = std::make_shared<ScopeTap>(sampleRate, SharedAnalysis::kTapSeconds);
        SharedAnalysis broker([&](std::int64_t, ScopePoint) { return tap; }, [](std::int64_t, ScopePoint) {},
                              SharedAnalysis::Mode::Manual);
        const auto dbfs = broker.acquireStereoMeter(1, {});
        const auto kScale = broker.acquireStereoMeter(1, {false, Meter::Weighting::A, false, true});
        const auto flatKScale = broker.acquireStereoMeter(1, {false, Meter::Weighting::Standard, false, true});
        std::vector<float> loud(static_cast<std::size_t>(sampleRate), -.5f);
        tap->write(loud.data(), loud.data(), sampleRate, 0, true, 1);
        broker.pump();
        const auto before = dbfs.readStereoMeter(), kBefore = kScale.readStereoMeter();
        check(before && kBefore && kBefore == flatKScale.readStereoMeter() && before != kBefore &&
                  before->sampleRate == sampleRate && kBefore->sampleRate == sampleRate &&
                  broker.stats().domains == 2 && broker.stats().taps == 1 && broker.stats().spectralPlans == 0,
              "K scales bypass the selected weighting and share one unweighted profile on the physical tap");
        if (!before || !kBefore) continue;
        check(near(20. * std::log10(before->reading.peak[0]), -6.0206, .1) &&
                  near(20. * std::log10(before->reading.sineReferencedRms[0] / std::sqrt(2.)), -6.0206, .1) &&
                  near(20. * std::log10(kBefore->reading.sineReferencedRms[0] / std::sqrt(2.)), -6.0206, .1),
              "both profiles read the negative half-scale plateau within 0.1 dB of the reference's raw level");

        const auto quietFrames = sampleRate * 3 / 5;
        std::vector<float> quiet(static_cast<std::size_t>(quietFrames));
        tap->write(quiet.data(), quiet.data(), quietFrames, sampleRate, true, 1);
        broker.pump();
        const auto after = dbfs.readStereoMeter(), kAfter = kScale.readStereoMeter();
        check(after && kAfter && near(after->reading.sineReferencedRms[0] / before->reading.sineReferencedRms[0], std::sqrt(.4)) &&
                  kAfter->reading.sineReferencedRms[0] < .001 &&
                  near(20. * std::log10(after->reading.peak[0] / before->reading.peak[0]), -40., .1) &&
                  near(20. * std::log10(kAfter->reading.peak[0] / kBefore->reading.peak[0]), -20., .1) &&
                  near(after->reading.peakHold[0], .5) && near(kAfter->reading.peakHold[0], .5),
              "600 ms of silence leaves 40% of dBFS window power, expires the K window, and exercises both peak release laws");
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
    check(scalar.prepare({48000., .3, 1., .5, 1.e-6}) && scalar.process(left, right),
          "scalar reference accepts one interval");
    const auto feed = [&](std::size_t offset, std::size_t count) {
        f.tap->write(left.data() + offset, right.data() + offset, static_cast<std::int32_t>(count),
                     static_cast<std::int64_t>(offset), true);
        f.broker.pump();
    };
    feed(0, 97);
    feed(97, left.size() - 97);
    const auto lr = meter.readStereoMeter(), ms = midSide.readStereoMeter();
    check(lr && lr == mirror.readStereoMeter() && matchingStereoMeterReadings(lr->reading, scalar.reading()) &&
              lr->reading.signCoincidence < 0 &&
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
    check(direct.prepare({48000., .3, 1., .5, 1.e-6}), "continuous stereo-meter oracle prepares");
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
    check(first && wrapped && wrapped->epoch == first->epoch && matchingStereoMeterReadings(wrapped->reading, direct.reading()) &&
              wrapped->reading.maximumPeak[0] == .9f && wrapped->reading.peakHold[0] == .9f,
          "the same-revision loop wrap preserves exact continuous envelope and held peaks");
    check(initialSpectrum && spectrum.read() && spectrum.read()->sourceEpoch > initialSpectrum->sourceEpoch,
          "a spectrum sharing the source starts a new epoch at the loop wrap");

    transport.play(false);
    const auto stopped = step(0.f, 0.f, true);
    check(wrapped && stopped && stopped->epoch == wrapped->epoch && matchingStereoMeterReadings(stopped->reading, direct.reading()) &&
              stopped->reading.maximumPeak[0] == .9f && std::isfinite(stopped->reading.peakHold[0]) &&
              stopped->reading.peak[0] < wrapped->reading.peak[0],
          "Stop decays the envelope continuously and preserves its interval maximum and holds");
    transport.play();
    const auto restarted = step(.2f, .1f);
    check(stopped && restarted && restarted->epoch == stopped->epoch &&
              matchingStereoMeterReadings(restarted->reading, direct.reading()) &&
              restarted->reading.maximumPeak[0] == .9f,
          "Play maintains the measurement interval across the engine's advancing revision change");

    transport.locate(0);
    direct.reset();
    const auto sought = step(.125f, .0625f);
    check(restarted && sought && sought->epoch > restarted->epoch && matchingStereoMeterReadings(sought->reading, direct.reading()) &&
              sought->reading.maximumPeak[0] == .125f,
          "a same-stamp seek clears highs from the prior interval");
    transport.play(false);
    transport.locate(0);
    direct.reset();
    const auto skippedSeek = step(0.f, 0.f, true);
    check(sought && skippedSeek && skippedSeek->epoch > sought->epoch &&
              matchingStereoMeterReadings(skippedSeek->reading, Meter::Reading{}),
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
    check(parked && matchingStereoMeterReadings(parked->reading, Meter::Reading{}) && view.readStereoMeter() == parked &&
          after.workerPasses == before.workerPasses && after.cursorReads == before.cursorReads &&
          after.publications == before.publications,
          "empty stereo meter does no analysis across a parked ring wrap");
    const std::array<float, 480> left = [] { std::array<float, 480> values{}; values.fill(.1f); return values; }();
    Meter direct;
    const auto ready = direct.prepare({48000., .3, 1., .5, 1.e-6, false, Meter::Weighting::A}) &&
                       direct.process(left, left);
    f.tap->write(left.data(), left.data(), static_cast<int>(left.size()), 0, false, 1);
    f.broker.pump();
    const auto sounding = view.readStereoMeter();
    check(ready && sounding && parked && sounding->epoch == parked->epoch &&
              matchingStereoMeterReadings(sounding->reading, direct.reading()),
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
          matchingStereoMeterReadings(afterSeek->reading, Meter::Reading{}),
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
#if defined(__GNUC__) && !defined(__clang__)
#define ADI_TEST_NOINLINE __attribute__((noinline))
#else
#define ADI_TEST_NOINLINE
#endif
ADI_TEST_NOINLINE void *operator new(std::size_t bytes) {
    if(observeAllocationSizes && allocationSizeCount<allocationSizes.size()) allocationSizes[allocationSizeCount++]=bytes;
    if(refuseAllocationBytes && bytes==refuseAllocationBytes && !refusedAllocationSize) {
        refusedAllocationSize=true; throw std::bad_alloc{};
    }
    if(failAllocationAfter==0) throw std::bad_alloc{};
    if(failAllocationAfter>0) --failAllocationAfter;
    if(auto *p=std::malloc(bytes?bytes:1)) return p;
    throw std::bad_alloc{};
}
ADI_TEST_NOINLINE void *operator new[](std::size_t bytes) {return ::operator new(bytes);}
ADI_TEST_NOINLINE void operator delete(void *p) noexcept {std::free(p);}
ADI_TEST_NOINLINE void operator delete[](void *p) noexcept {::operator delete(p);}
ADI_TEST_NOINLINE void operator delete(void *p,std::size_t) noexcept {::operator delete(p);}
ADI_TEST_NOINLINE void operator delete[](void *p,std::size_t) noexcept {::operator delete(p);}
#undef ADI_TEST_NOINLINE
int main() {
    try { stripCadenceAndEpochs(); stripReusesRawSpectrum(); stripWorkerBudget(); stripLeaseInSession(); phaseBandSpatialMeasurements();
          focusCapturedTruePeak(); focusBarCapturedTruePeak(); focusScopeLayerChannels(); spectrogramHeardGrid();
          continuousCapture(); captureBoundaries(); captureSharesLiveTap(); focusScopeBarTiming(); focusScopeBeatTiming(); focusScopeBarProjection(); focusScopeMidBeforeBinning(); focusScopeWindows(); focusBarScopeBroker(); focusSingleSourceScope(); focusBarScopeMidDemand(); focusScopeDifference(); focusScopeSideDemand(); focusBarScopeBrokerBoundaries(); focusBarScopeWaitsOnAllSources(); focusBarScopeResumesAtRequestedRevision(); focusScopeSumProjection(); focusScopeLayerPreview(); focusBarLayerPreview(); focusBarResidualSpectrum(); focusScopePhaseOperands(); focusScopeFlatPath(); focusLagPreviewRequests(); focusLagPreviewCases(); focusLagTapReads(); focusLagHighRateTaps(); focusMatchWindows(); parkedSilence(); parkedSpectrogramWake(); parkedWindowHopPhase();
          parkedRealtimeModeJoins<SharedAnalysis::RealtimeMaximum>(); parkedRealtimeModeJoins<SharedAnalysis::RealtimeSigma>(); parkedStereoBalance(); sharing(); configurationsAndMoves(); refreshAndLifetime(); refusalsAndBounds(); shutdown(); workerLifecycle();
          sharedModesAndCursors(); phaseBandCorrelation(); clippedSampleWindows(); exactZeroSpectrum(); exactSilentStereoLanes(); periodScopeReference(); contiguousPeriodScopeDemand(); colourWaveformReference(); continuousColourWaveformDemand(); filledSpectrumCurves(); spectrumLevels(); spectrumSamplePeaksAndClips(); restartSpectrumLevels(); restartParkedSpectrumLevels(); spectrumTruePeak(); parkedSpectrumLevels(false); parkedSpectrumLevels(true); realtimeMaximumSpectrum(); realtimeMaximumDecayAndAdmission(); realtimeSigmaSpectrum(); cumulativeMaximumSpectrum(); cumulativeAverageSpectrum(); parkedCumulativeAverage(); cumulativeMaximumParkedRollover(); sharedAverage(); parkedRealtimeAverage(false); parkedRealtimeAverage(true); parkedRealtimeAverageJoinsScope(); averageActivationAndOverflow(); averageChronologyAndEpochs(); averagePreparationFailure(); averagePendingBoundaries(); averageHistoryAndSanitation(); averagePublicationFailure(); deviceSpectrumLease(); parkedDeviceSpectrum(); boundsAndDeviceSources(); historyRenegotiation(); fairFrameBudget(); lazyPlansAndIdleDemand(); boundedImmutableHistory(); fullHistoryBursts(); finiteExtremeSpectrum(); actualRateHistory(); lateRateAndFailedNegotiation(); stereoMeterReferenceProfiles(); stereoMeterSharingAndIntervals(); stereoMeterTransportContinuity(); parkedStereoMeter(); }
    catch (const std::exception &e) { ++failures; std::printf("FAIL exception: %s\n", e.what()); }
    std::printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
