// SPDX-License-Identifier: AGPL-3.0-or-later
#include "adi/dsp/stereo_envelope_meter.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <limits>
#include <numbers>
#include <span>
#include <vector>

namespace {
using Meter = adi::dsp::StereoEnvelopeMeter;
int failures = 0;
void check(bool pass, const char *what) {
    if (!pass) { ++failures; std::fprintf(stderr, "FAIL %s\n", what); }
}
bool close(double a, double b) { return std::abs(a - b) < 2e-11; }
constexpr Meter::Settings testSettings{32000, .35, .19, .42, 1.0 / 4096.0};

void stepAndDecay(double rate) {
    auto settings = testSettings;
    settings.sampleRate = rate;
    settings.meanSquareSeconds = .2;
    const auto length = static_cast<std::size_t>(std::llround(.2 * rate));
    Meter meter;
    check(meter.prepare(settings), "prepare reference sliding-window fixture");
    const std::vector<float> left(length, .5f), right(length, -.25f), zeros(length);
    check(meter.process(left, right), "process one full integration window");
    const auto warm = meter.reading();
    check(close(warm.sineReferencedRms[0], std::sqrt(.5)) && close(warm.sineReferencedRms[1], std::sqrt(.125)) &&
              close(warm.signCoincidence, -1.), "steady RMS and normalised opposite-channel correlation");
    check(meter.reading() == warm, "multiple consumers can read the same result");
    const auto half = std::span(zeros).first(length / 2);
    check(meter.process(half, half), "process half a silent window");
    const auto faded = meter.reading();
    const auto seconds = static_cast<double>(half.size()) / rate;
    const auto expectedPeak = .5 * std::pow(.1, seconds / settings.peakDecaySeconds);
    check(close(faded.peak[0], expectedPeak) && close(faded.sineReferencedRms[0], .5) &&
              close(faded.sineReferencedRms[1], .25),
          "peak-only release and exactly half the preceding window's power");
    check(meter.process(half, half) && meter.reading().sineReferencedRms[0] < 1.e-9,
          "a full silent window has no remaining RMS");
    meter.reset();
    check(meter.reading() == Meter::Reading{} && meter.process({}, {}), "reset and empty input are neutral");
}

void floorAndRecall() {
    Meter meter;
    check(!meter.process({}, {}) && meter.prepare(testSettings), "explicit configuration");
    constexpr std::size_t count = 32000;
    const std::vector<float> left(count, .75f);
    const std::vector<float> below(count, static_cast<float>(.5 * testSettings.coincidenceFloor));
    const std::vector<float> above(count, static_cast<float>(-2 * testSettings.coincidenceFloor));
    check(meter.process(left, below) && std::abs(meter.reading().signCoincidence) < 1.e-10,
          "the weighted energy of both channels must clear the correlation floor");
    meter.reset();
    check(meter.process(left, above) && close(meter.reading().signCoincidence, -1.),
          "a polarity-inverted copy correlates fully despite its different amplitude");
    const auto before = meter.reading();
    const std::array<float, 2> invalid{.5f, std::numeric_limits<float>::quiet_NaN()};
    check(!meter.process(invalid, invalid) && !meter.process(left, {}) &&
          !meter.prepare({0, .3, .2, .4, 0}) && meter.reading() == before,
          "invalid data and settings are transactional");
}

void holdsBalanceAndDynamics() {
    for (const double rate : {1000., 32000., 96000.}) {
        Meter meter;
        auto settings = testSettings;
        settings.sampleRate = rate;
        settings.coincidenceSeconds = .5;
        check(meter.prepare(settings), "prepare reference hold fixture");
        const std::array<float, 1> leftImpulse{1.f}, rightImpulse{.5f}, silent{};
        check(meter.process(leftImpulse, rightImpulse), "meter accepts a one-sample peak");
        const auto impulse = meter.reading();
        const std::vector<float> silence(static_cast<std::size_t>(std::llround(3. * rate)) - 1);
        check(meter.process(silence, silence), "advance to one sample before hold expiry");
        const auto held = meter.reading();
        check(held.maximumPeak == std::array{1., .5} && held.peakHold == impulse.peakHold &&
                  held.rmsHold == impulse.rmsHold, "the held values still show the pulse just before three seconds");
        check(meter.process(silent, silent), "advance to the hold deadline");
        const auto after = meter.reading();
        check(after.peakHold[0] < 1.e-8 && after.rmsHold[0] < 1.e-9 &&
                  after.maximumPeak == impulse.maximumPeak, "holds drop to their current value at exactly three seconds");
        meter.reset();
        check(meter.reading() == Meter::Reading{}, "reset clears values and held statistics");

        const auto oneSecond = static_cast<std::size_t>(std::llround(rate));
        const std::vector<float> left(oneSecond, .5f), right(oneSecond, -.25f);
        check(meter.process(left, left), "initial equal-power stereo");
        const auto initial = meter.reading();
        check(close(initial.signCoincidence, 1.) && std::abs(initial.balance) < 1.e-10,
              "mono reads +1 correlation and centre balance");
        check(meter.process(left, right), "change polarity and level for one second");
        const auto changed = meter.reading();
        const auto balanceFirst = -std::expm1(-1. / 3.);
        const auto leftPower = .25 * -std::expm1(-2. / 3.);
        const auto rightPower = (.25 * std::exp(-1. / 3.) + .0625) * balanceFirst;
        const auto expectedBalance = 10. * std::log10((rightPower + 1.e-10) / (leftPower + 1.e-10));
        const auto corrFirst = -std::expm1(-2.);
        const auto corrOld = corrFirst * std::exp(-2.);
        const auto numerator = .25 * corrOld - .125 * corrFirst;
        const auto denominator = std::sqrt(.25 * (corrOld + corrFirst) * (.25 * corrOld + .0625 * corrFirst));
        check(std::abs(changed.balance - expectedBalance / 12.) < 1.e-8 &&
                  std::abs(changed.signCoincidence - numerator / denominator) < 1.e-8,
              "500 ms correlation and 3 s dB balance follow independent running sample-rate clocks");
    }
    auto settings = testSettings;
    settings.midSide = true;
    Meter midSide;
    check(midSide.prepare(settings), "prepare M/S meter");
    const std::vector<float> both(1024, .5f), inverted(1024, -.5f);
    check(midSide.process(both, both) && midSide.reading().peak == std::array{.5, 0.} &&
              close(midSide.reading().signCoincidence, 1.) && std::abs(midSide.reading().balance) < 1.e-10,
          "M/S matrix preserves the original L/R spatial readouts");
    midSide.reset();
    check(midSide.process(both, inverted) && midSide.reading().peak == std::array{0., .5} &&
              close(midSide.reading().signCoincidence, -1.), "opposite equal channels produce only Side");
}

double dbAmplitude(double amplitude) { return 20. * std::log10(amplitude); }

void releaseAffectsPeakOnly() {
    Meter slow, fast;
    Meter::Settings settings{1000., 1., .6, .5, 1.e-6};
    settings.linkedDynamic = true;
    check(slow.prepare(settings), "prepare a long peak release");
    settings.peakDecaySeconds = .05;
    check(fast.prepare(settings), "prepare a short peak release");
    std::array<float, 100> left{}, right{};
    left.fill(.125f);
    right.fill(.0625f);
    left.front() = .875f;
    right.front() = -.375f;
    check(slow.process(left, right) && fast.process(left, right), "process the same transient at both release speeds");
    const auto a = slow.reading(), b = fast.reading();
    check(a.peak[0] - b.peak[0] > .4 && close(a.sineReferencedRms[0], b.sineReferencedRms[0]) &&
              close(a.peakHold[0], b.peakHold[0]) && close(a.floatingPeak[0], b.floatingPeak[0]) &&
              std::abs(a.dynamicDb[0] - b.dynamicDb[0]) < .001 && a.linkedDynamicDb && b.linkedDynamicDb &&
              std::abs(*a.linkedDynamicDb - *b.linkedDynamicDb) < .001,
          "peak release shapes only the peak bar, with identical RMS, Dynamic and held readings");
}

void referencePeakRmsAndDynamic() {
    constexpr double rate = 48000.;
    constexpr std::size_t frames = 48000;
    const Meter::Settings settings{rate, .3, 1., .5, 1.e-6};
    Meter meter;
    check(meter.prepare(settings), "prepare the default window and release");
    std::vector<float> left(frames), right(frames);
    constexpr std::array<float, 4> sine{0.f, .5f, 0.f, -.5f};
    for (std::size_t i = 0; i < frames; ++i) {
        left[i] = sine[i % sine.size()];
        right[i] = i % 2 == 0 ? .25f : -.25f;
    }
    check(meter.process(left, right), "read steady sine and square during one integration");
    const auto waveforms = meter.reading();
    check(std::abs(waveforms.dynamicDb[0] - 3.0103) < .1 && std::abs(waveforms.dynamicDb[1]) < .1,
          "Dynamic reads crest from raw power, about 3 dB for a sine and zero for a square");
    meter.reset();
    // The same peak and RMS levels documented by the side-by-side Max reading.
    // A rectangular pulse train fixes both quantities independently.
    constexpr double peakDb = -.1;
    const auto amplitude = std::pow(10., peakDb / 20.);
    for (std::size_t channel = 0; channel < 2; ++channel) {
        auto &samples = channel == 0 ? left : right;
        std::ranges::fill(samples, 0.f);
        const auto rmsDb = channel == 0 ? -3.6 : -3.7;
        const auto duty = .5 * std::pow(10., (rmsDb - peakDb) / 10.);
        std::fill_n(samples.begin(), static_cast<std::size_t>(std::llround(duty * rate)), static_cast<float>(amplitude));
    }
    check(meter.process(left, right), "process a deterministic reference-level pair");
    const auto measured = meter.reading();
    check(std::abs(dbAmplitude(measured.maximumPeak[0]) + .1) < .1 &&
              std::abs(dbAmplitude(measured.maximumPeak[1]) + .1) < .1 &&
              std::abs(dbAmplitude(measured.sineReferencedRms[0]) + 3.6) < .1 &&
              std::abs(dbAmplitude(measured.sineReferencedRms[1]) + 3.7) < .1,
          "peak and sine-referenced RMS agree with the reference readings within 0.1 dB");
}

void linkedDynamics() {
    constexpr double rate = 1000.;
    constexpr std::size_t length = 1000;
    const auto leftGain = std::pow(10., -10. / 20.);
    const auto rightGain = std::pow(10., -20. / 20.);
    std::array<float, length> left{}, right{};
    for (std::size_t i = 0; i < length; ++i) {
        constexpr std::array<float, 4> cycle{0.f, 1.f, 0.f, -1.f};
        left[i] = static_cast<float>(leftGain * cycle[i % cycle.size()]);
        right[i] = static_cast<float>(rightGain * cycle[i % cycle.size()]);
    }
    Meter regular, linked, split;
    Meter::Settings settings{rate, .3, .5, .5, 1.e-6};
    check(regular.prepare(settings) && regular.process(left, right), "prepare regular stereo for Link comparison");
    settings.linkedDynamic = true;
    check(linked.prepare(settings) && split.prepare(settings) && linked.process(left, right), "prepare Link meter");
    for (std::size_t pos = 0; pos < length;) {
        const auto chunk = std::min(length - pos, 1 + pos % 97);
        check(split.process(std::span(left).subspan(pos, chunk), std::span(right).subspan(pos, chunk)), "partitioned Link");
        pos += chunk;
    }
    const auto lane = regular.reading(), whole = linked.reading(), chunks = split.reading();
    check(whole.maximumPeak == lane.maximumPeak && whole.sineReferencedRms == lane.sineReferencedRms &&
              chunks.sineReferencedRms == whole.sineReferencedRms && whole.linkedDynamicDb && chunks.linkedDynamicDb &&
              std::abs(*whole.linkedDynamicDb - 3.0103) < .1 && std::abs(*chunks.linkedDynamicDb - *whole.linkedDynamicDb) < 1.e-8 &&
              std::abs(dbAmplitude(whole.linkedSineReferencedRms) - 10. * std::log10((.1 + .01) / 2.)) < .1,
          "Link combines the same stereo powers across arbitrary blocks and preserves a sine's dynamic difference");
    const auto zeros = std::array<float, 500>{};
    check(linked.process(zeros, zeros) && !linked.reading().linkedDynamicDb,
          "Link becomes neutral after a whole integration window of silence");
}

void referenceClipsAndCrest() {
    Meter meter;
    check(meter.prepare({1000., .3, .2, .5, 1.e-6, true}), "prepare crest and raw clip event fixture");
    const std::array<float, 7> left{-1.f, -1.f, 0.f, 1.f, -1.f, 0.f, -.999f};
    const std::array<float, 7> right{0.f, 0.f, .998f, 0.f, 1.f, 1.f, 0.f};
    check(meter.process(std::span(left).first(4), std::span(right).first(4)) &&
              meter.process(std::span(left).subspan(4), std::span(right).subspan(4)) &&
              meter.reading().clipCount == std::array<std::uint64_t, 2>{3, 1},
          "both polarities count once per raw L/R clip event across block boundaries, even in M/S mode");
    meter.reset();
    const std::array<float, 150> silence{};
    std::array<float, 50> burst{};
    burst.fill(.5f);
    check(meter.process(silence, silence) && meter.process(burst, burst), "fill one RMS window with a 50 ms burst at the end");
    const auto recorded = meter.reading();
    check(std::abs(recorded.maxCrestFactorDb - 10. * std::log10(4.)) < .1 &&
              recorded.clipCount == std::array<std::uint64_t, 2>{}, "crest compares the 50 ms and integrated RMS window powers");
    check(meter.process(silence, silence) && std::abs(meter.reading().maxCrestFactorDb - recorded.maxCrestFactorDb) < 1.e-8,
          "the maximum crest survives quieter readings until reset");
    meter.reset();
    check(meter.reading() == Meter::Reading{}, "reset also clears clip events and the maximum crest");
}

void weightingCurves() {
    using Weighting = Meter::Weighting;
    constexpr std::array rates{44100., 48000., 96000.};
    constexpr std::array frequency{50., 125., 500., 1000., 4000., 12000., 20000.};
    constexpr std::array weights{Weighting::A, Weighting::C, Weighting::K};
    struct GainRange { double minimum, maximum; };
    constexpr GainRange expected[3][7] = {
        {{-33., -27.}, {-19., -14.}, {-4., -2.}, {-.035, .035}, {0., 2.}, {-6., -2.}, {-13., -7.}},
        {{-3., -.5}, {-.5, .2}, {-.3, .3}, {-.035, .035}, {-2., 0.}, {-8., -4.}, {-14., -9.}},
        {{-7., -3.}, {-3., -.5}, {-1.5, -.2}, {-.035, .035}, {2.5, 4.}, {2.5, 4.}, {2.5, 4.}}
    };
    for (std::size_t rate = 0; rate < rates.size(); ++rate) {
        auto settings = testSettings;
        settings.sampleRate = rates[rate];
        settings.meanSquareSeconds = .05;
        const auto period = static_cast<std::size_t>(settings.sampleRate);
        std::vector<float> left(period), right(period);
        for (std::size_t f = 0; f < frequency.size(); ++f) {
            for (std::size_t i = 0; i < period; ++i) {
                left[i] = static_cast<float>(.1 * std::sin(6.283185307179586 * frequency[f] *
                                                           static_cast<double>(i) / settings.sampleRate));
                right[i] = -.5f * left[i];
            }
            settings.weighting = Weighting::Standard;
            Meter reference;
            check(reference.prepare(settings) && reference.process(left,right), "raw meter tone warms");
            const auto raw = reference.reading();
            for (std::size_t mode = 0; mode < weights.size(); ++mode) {
                settings.weighting = weights[mode];
                Meter meter;
                check(meter.prepare(settings) && meter.process(left,right), "weighted meter tone warms");
                const auto first = meter.reading();
                check(first.peak == raw.peak && first.peakHold == raw.peakHold &&
                          first.maximumPeak == raw.maximumPeak && first.floatingPeak == raw.floatingPeak &&
                          first.signCoincidence == raw.signCoincidence && first.balance == raw.balance,
                      "weighting is passive and preserves unweighted spatial and peak values");
                double power = 0.;
                constexpr std::size_t count = 157;
                bool accepted = true;
                for (std::size_t i = 0; i < count; ++i) {
                    const auto firstFrame = i * period / count;
                    const auto frames = (i + 1) * period / count - firstFrame;
                    accepted &= meter.process(std::span(left).subspan(firstFrame,frames),
                                              std::span(right).subspan(firstFrame,frames));
                    const auto value = meter.reading().sineReferencedRms[0];
                    power += value * value;
                }
                const double gain = 10. * std::log10(power / (count * .01));
                const auto range = expected[mode][f];
                if (!accepted || !std::isfinite(gain) || gain < range.minimum || gain > range.maximum) {
                    std::fprintf(stderr, "FAIL %.0f Hz at %.0f mode %zu expected [%g, %g] measured %.6f\n",
                                 frequency[f], rates[rate], mode, range.minimum, range.maximum, gain);
                    ++failures;
                }
            }
        }
    }
}

void referenceWeightingCurves() {
    using Weighting = Meter::Weighting;
    struct Point { double hz, gainDb; };
    constexpr std::array aCurve{Point{31.5, -39.4}, Point{63., -26.2}, Point{100., -19.1},
        Point{200., -10.9}, Point{500., -3.2}, Point{1000., 0.}, Point{2000., 1.2},
        Point{4000., 1.0}, Point{8000., -1.1}, Point{10000., -2.5}};
    constexpr std::array cCurve{Point{31.5, -3.0}, Point{63., -.8}, Point{125., -.2},
        Point{250., 0.}, Point{1000., 0.}, Point{4000., -.8}, Point{8000., -3.0}, Point{10000., -4.4}};
    // The Max test reports the raw K curve. The RMS meter normalises it at 1 kHz.
    constexpr std::array kCurve{Point{20., -13.76}, Point{100., -1.62}, Point{1000., 0.},
        Point{4000., 3.44}, Point{10000., 3.56}};
    struct Curve { Weighting mode; std::span<const Point> points; };
    const std::array curves{Curve{Weighting::A, aCurve}, Curve{Weighting::C, cCurve}, Curve{Weighting::K, kCurve}};
    // The sample-rate ladder in adi/settings/registry.cpp.
    constexpr std::array rates{44100., 48000., 88200., 96000., 176400., 192000., 352800., 384000., 705600., 768000.};
    constexpr double amplitude = .1;
    constexpr int probes = 101;
    for (const double rate : rates) {
        std::vector<float> tone;
        for (const auto &curve : curves) {
            for (const auto &point : curve.points) {
                auto settings = testSettings;
                settings.sampleRate = rate;
                settings.meanSquareSeconds = .015;
                settings.weighting = curve.mode;
                const auto warmup = static_cast<std::size_t>(std::llround(.2 * rate));
                // An integer number of power periods makes the result independent of the filter's phase.
                const auto powerHz = 2. * point.hz;
                const auto duration = std::ceil(.08 * powerHz) / powerHz;
                tone.resize(warmup + static_cast<std::size_t>(std::llround(duration * rate)));
                for (std::size_t frame = 0; frame < tone.size(); ++frame)
                    tone[frame] = static_cast<float>(amplitude * std::sin(2. * std::numbers::pi * point.hz *
                                                                          static_cast<double>(frame) / rate));
                Meter meter;
                const std::span<const float> input(tone);
                bool processed = meter.prepare(settings) && meter.process(input.first(warmup), input.first(warmup));
                auto begin = warmup;
                double power = 0.;
                for (int probe = 1; probe <= probes; ++probe) {
                    const auto end = warmup + static_cast<std::size_t>(std::llround(duration * rate * probe / probes));
                    const auto block = input.subspan(begin, end - begin);
                    processed &= meter.process(block, block);
                    const auto rms = meter.reading().sineReferencedRms[0];
                    power += rms * rms;
                    begin = end;
                }
                const auto measuredDb = 10. * std::log10(power / (probes * amplitude * amplitude));
                const auto tolerance = curve.mode == Weighting::K ? .09 : point.hz >= 8000. && rate < 96000. ? 1.5 : .35;
                if (!processed || !std::isfinite(measuredDb) || std::abs(measuredDb - point.gainDb) > tolerance) {
                    std::fprintf(stderr, "FAIL weighting %u at %.0f Hz rate, %.1f Hz tone: %.3f dB, expected %.3f +/- %.2f\n",
                        static_cast<unsigned>(curve.mode), rate, point.hz, measuredDb, point.gainDb, tolerance);
                    ++failures;
                }
            }
        }
    }
}

void partitionAndSine() {
    constexpr std::size_t n = 64000;
    constexpr std::array<float, 4> cycle{0, .125f, 0, -.125f};
    std::vector<float> left(n), right(n);
    for (std::size_t i = 0; i < n; ++i) { left[i] = cycle[i % 4]; right[i] = left[i] * .5f; }
    Meter whole, split;
    check(whole.prepare(testSettings) && split.prepare(testSettings) && whole.process(left, right), "sine");
    for (std::size_t pos = 0; pos < n;) {
        const std::size_t length = std::min(n - pos, 1 + pos % 173);
        check(split.process(std::span(left).subspan(pos, length), std::span(right).subspan(pos, length)), "partition");
        pos += length;
    }
    const auto a = whole.reading();
    check(a == split.reading(), "partition invariance");
    // At Fs/4 the squared sine alternates 0 and A^2. Sum the geometric series at its final peak.
    check(close(a.sineReferencedRms[0], .125) && close(a.sineReferencedRms[1], .0625),
          "a whole sliding window of Fs/4 sine calibrates to its peak amplitude");
}
}

int main() {
    stepAndDecay(44100);
    stepAndDecay(48000);
    stepAndDecay(96000);
    floorAndRecall();
    partitionAndSine();
    holdsBalanceAndDynamics();
    linkedDynamics();
    releaseAffectsPeakOnly();
    referencePeakRmsAndDynamic();
    referenceClipsAndCrest();
    weightingCurves();
    referenceWeightingCurves();
    return failures == 0 ? 0 : 1;
}
