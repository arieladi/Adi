// SPDX-License-Identifier: AGPL-3.0-or-later
#include "adi/dsp/stereo_envelope_meter.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <limits>
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
    Meter meter;
    check(meter.prepare(settings), "prepare step fixture");
    const std::vector<float> left(7000, .5f), right(7000, -.25f), zeros(7000);
    const double t = static_cast<double>(left.size()) / rate;
    check(meter.process(left, right), "process step");
    auto reading = meter.reading();
    const double rms = std::sqrt(2 * (1 - std::exp(-t / settings.meanSquareSeconds)));
    const double coincidence = std::expm1(-t / settings.coincidenceSeconds);
    check(reading.peak == std::array{.5, .25} && close(reading.sineReferencedRms[0], .5 * rms) &&
          close(reading.sineReferencedRms[1], .25 * rms) && close(reading.signCoincidence, coincidence),
          "independent channel levels and opposite signs");
    check(meter.reading() == reading, "multiple consumers can read the same result");
    check(meter.process(zeros, zeros), "process silence");
    reading = meter.reading();
    check(close(reading.peak[0], .5 * std::exp(-t / settings.peakDecaySeconds)) &&
          close(reading.sineReferencedRms[0], .5 * rms * std::exp(-.5 * t / settings.meanSquareSeconds)) &&
          close(reading.signCoincidence, coincidence * std::exp(-t / settings.coincidenceSeconds)),
          "three independent decay laws");
    meter.reset();
    check(meter.reading() == Meter::Reading{} && meter.process({}, {}), "gap reset and empty input");
}

void floorAndRecall() {
    Meter meter;
    check(!meter.process({}, {}) && meter.prepare(testSettings), "explicit configuration");
    const std::array<float, 1> left{.75f};
    const float edge = static_cast<float>(testSettings.coincidenceFloor);
    const float above = std::nextafter(edge, 1.f);
    const std::array<float, 3> samples{std::nextafter(edge, 0.f), edge, above};
    for (float r : samples) {
        meter.reset();
        const std::array<float, 1> right{-r};
        check(meter.process(left, right), "floor fixture");
        check((meter.reading().signCoincidence < 0) == (r > edge), "strict two-lane magnitude floor");
    }
    const auto before = meter.reading();
    const std::array<float, 2> invalid{.5f, std::numeric_limits<float>::quiet_NaN()};
    check(!meter.process(invalid, invalid) && !meter.process(left, {}) &&
          !meter.prepare({0, .3, .2, .4, 0}) && meter.reading() == before,
          "invalid data or configuration is transactional");
    meter.reset();
    const std::vector<float> nearLeft(73, -.9f), nearRight(73, -above), muted(73);
    const double elapsed = 73 / testSettings.sampleRate;
    const double step = -std::expm1(-elapsed / testSettings.coincidenceSeconds);
    check(meter.process(nearLeft, nearRight) && close(meter.reading().signCoincidence, step),
          "same sign counts fully even with very different amplitudes");
    check(meter.process(muted, nearRight) && close(meter.reading().signCoincidence, step * (1 - step)),
          "one silent channel drives coincidence toward neutral");
}

void holdsBalanceAndDynamics() {
    for (const double rate : {1000., 32000., 96000.}) {
        Meter meter; auto settings = testSettings; settings.sampleRate = rate;
        meter.prepare(settings);
        const std::array<float,1> l{1.f}, r{.5f};
        meter.process(l,r);
        const auto impulse = meter.reading();
        std::vector<float> zeros(static_cast<std::size_t>(2.*rate));
        meter.process(zeros,zeros);
        const auto held = meter.reading();
        check(held.maximumPeak==std::array{1.,.5} && held.peakHold==impulse.peakHold &&
              held.rmsHold==impulse.rmsHold, "sample and RMS maxima hold for two seconds at the source rate");
        const std::array<float,1> silence{};
        meter.process(silence,silence);
        const auto after = meter.reading();
        check(close(after.peakHold[0],held.peakHold[0]*std::exp(-1./(settings.peakDecaySeconds*rate))) &&
              close(after.rmsHold[0],held.rmsHold[0]*std::exp(-.5/(settings.meanSquareSeconds*rate))) &&
              after.maximumPeak==held.maximumPeak, "timed holds start decaying on the next sample while numeric peak persists");
        meter.reset();
        check(meter.reading()==Meter::Reading{}, "reset clears numeric maxima, holds, balance and dynamics");
        std::vector<float> left(6000,.5f), right(6000,.125f);
        meter.process(left,right);
        const auto known=meter.reading();
        check(close(known.balance, std::expm1(-6000./(settings.coincidenceSeconds*rate))), "left-to-right magnitude balance uses the source timebase");
        check(close(known.floatingPeak[0], -.5*std::expm1(-6000./(.7*rate))), "floating dynamics anchor follows each lane's peak envelope");
    }
    Meter midSide; auto settings=testSettings; settings.midSide=true; midSide.prepare(settings);
    const std::array<float,1> both{.5f}, inverted{-.5f};
    midSide.process(both,both);
    check(midSide.reading().peak==std::array{.5,0.} && midSide.reading().signCoincidence>0 && midSide.reading().balance==0,
          "M/S matrix preserves the original L/R spatial readouts");
    midSide.reset(); midSide.process(both,inverted);
    check(midSide.reading().peak==std::array{0.,.5} && midSide.reading().signCoincidence<0,
          "opposed equal channels produce only Side");
    Meter dynamics; settings.sampleRate=1000; settings.midSide=false; dynamics.prepare(settings);
    const std::array<float,1> tone{.5f}; dynamics.process(tone,tone);
    const double power=.25*-std::expm1(-1./(1000.*settings.meanSquareSeconds));
    const double smoothed=.5+-std::expm1(-1./400.)*(std::min(.5,power/.25)-.5);
    check(close(dynamics.reading().dynamicDb[0],-10.*std::log10(2.*smoothed)), "dynamics integrates the linear ratio before logarithmic display");
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
    const double decay = std::exp(-1 / (testSettings.sampleRate * testSettings.meanSquareSeconds));
    const double sum = (1 - std::exp(-double(n) / (testSettings.sampleRate * testSettings.meanSquareSeconds))) / (1 + decay);
    check(close(a.sineReferencedRms[0], .125 * std::sqrt(2 * sum)) &&
          close(a.sineReferencedRms[1], .0625 * std::sqrt(2 * sum)), "sine reference includes deterministic ripple");
}
}

int main() {
    stepAndDecay(44100);
    stepAndDecay(48000);
    stepAndDecay(96000);
    floorAndRecall();
    partitionAndSine();
    holdsBalanceAndDynamics();
    weightingCurves();
    return failures == 0 ? 0 : 1;
}
