// SPDX-License-Identifier: AGPL-3.0-or-later
#include "adi/dsp/stereo_envelope_meter.hpp"
#include "adi/dsp/auto_gain.hpp"
#include "adi/mix/filters/FlushDenormals.hpp"
#include <algorithm>
#include <cmath>
#include <complex>
#include <limits>
#include <numbers>

namespace adi::dsp {
namespace {
bool positiveFinite(double value) noexcept { return std::isfinite(value) && value > 0; }

// The high-frequency sections follow the meter response on the digital axis.
// A bilinear low-pass alone loses too much level as frequency nears Nyquist.
struct ResponseAnchor {
    double rate;
    // Unity-leading numerator followed by the monic denominator.
    std::array<double, 4> rolloff, shelf;
};
constexpr std::array anchors{
    ResponseAnchor{44100., {.521594658, .010672324, -.145444335, -.050643500},
                           {-1.730961667, .762915883, -1.662914589, .711834621}},
    ResponseAnchor{48000., {.507795859, .005923535, -.222481331, -.039080738},
                           {-1.752801805, .780034167, -1.690063828, .731871861}},
    ResponseAnchor{96000., {.435979172, -.026418561, -.841848440, .160410000},
                           {-1.876356543, .883558260, -1.844510492, .855744087}}
};
using Polynomial = std::array<double, 3>;
Polynomial warp(Polynomial p, double shift) noexcept {
    // Substitute z^-1 with (shift + z^-1) / (1 + shift*z^-1).
    const auto square = shift * shift;
    return {p[0] + shift * p[1] + square * p[2],
            2. * shift * (p[0] + p[2]) + (1. + square) * p[1],
            square * p[0] + shift * p[1] + p[2]};
}
std::array<double, 5> upperSection(double rate, bool shelf) noexcept {
    const auto at = [rate, shelf](const ResponseAnchor &point) {
        const auto &p = shelf ? point.shelf : point.rolloff;
        const double shift = (point.rate - rate) / (point.rate + rate);
        const auto b = warp({1., p[0], p[1]}, shift);
        const auto a = warp({1., p[2], p[3]}, shift);
        return std::array{b[0] / a[0], b[1] / a[0], b[2] / a[0], a[1] / a[0], a[2] / a[0]};
    };
    if (rate <= anchors.front().rate) return at(anchors.front());
    if (rate >= anchors.back().rate) return at(anchors.back());
    const std::size_t upper = rate <= anchors[1].rate ? 1 : 2;
    const auto &lo = anchors[upper - 1], &hi = anchors[upper];
    const double position = (rate - lo.rate) / (hi.rate - lo.rate);
    auto value = at(lo);
    const auto next = at(hi);
    for (std::size_t i = 0; i < value.size(); ++i) value[i] = std::lerp(value[i], next[i], position);
    return value;
}

constexpr double referenceKShelfHz = 1681.974450955533;
Biquad referenceKShelf(double sampleRate) noexcept {
    // The reference RMS meter uses the RBJ shelf with a specified midpoint.
    // Express the shelf with the bilinear tangent, using the gain-adjusted
    // midpoint to obtain the denominator's pole frequency.
    const auto midpointGain = std::pow(10., 3.999843853973347 / 40.);
    const auto pole = std::sqrt(midpointGain) * std::tan(std::numbers::pi * referenceKShelfHz / sampleRate);
    const auto damping = pole / .7071752369554196;
    const auto square = pole * pole;
    const auto highGain = midpointGain * midpointGain;
    const auto a0 = 1. + damping + square;
    return {(highGain + midpointGain * damping + square) / a0,
            2. * (square - highGain) / a0,
            (highGain - midpointGain * damping + square) / a0,
            2. * (square - 1.) / a0,
            (1. - damping + square) / a0};
}
}

bool StereoMeterWeighting::prepare(double sampleRate, Weighting weighting, Purpose purpose) noexcept {
    if (!positiveFinite(sampleRate) || weighting < Weighting::Standard || weighting > Weighting::K ||
        (weighting != Weighting::Standard && sampleRate <= 2000.)) return false;
    std::array<Biquad, 4> filter{};
    std::size_t count = 0;
    if (weighting != Weighting::Standard) {
        const bool k = weighting == Weighting::K;
        count = 2;
        // The reference shelf needs its midpoint below Nyquist. Keep the fitted
        // response for Filter Listen and taps below that cutoff.
        if (k && purpose == Purpose::Measurement && sampleRate > 2. * referenceKShelfHz) {
            filter[0] = referenceKShelf(sampleRate);
            filter[1] = AutoGain::kHighPass(sampleRate);
        } else {
            const auto pole = [sampleRate](double corner) {
                const auto warped = std::tan(std::numbers::pi * corner / sampleRate);
                return (1. - warped) / (1. + warped);
            };
            const auto hp = pole(k ? 38.1 : 20.6);
            const auto upper = upperSection(sampleRate, k);
            filter[0] = {1., -2., 1., -2. * hp, hp * hp};
            filter[1] = {upper[0], upper[1], upper[2], upper[3], upper[4]};
            if (weighting == Weighting::A) {
                filter[2] = {1., -1., 0., -pole(107.7), 0.};
                filter[3] = {1., -1., 0., -pole(737.9), 0.};
                count = 4;
            }
        }
        const auto z = std::polar(1., -2. * std::numbers::pi * 1000. / sampleRate);
        const auto z2 = z * z;
        double gain = 1.;
        for (std::size_t i = 0; i < count; ++i) {
            const auto &c = filter[i];
            gain *= std::abs((c.b0 + c.b1 * z + c.b2 * z2) / (1. + c.a1 * z + c.a2 * z2));
        }
        if (!positiveFinite(gain)) return false;
        filter[0].b0 /= gain;
        filter[0].b1 /= gain;
        filter[0].b2 /= gain;
    }
    coefficients_ = filter;
    sectionCount_ = count;
    return true;
}

namespace {
constexpr double crestSeconds = .050;
constexpr double holdSeconds = 3.;
constexpr double balanceSeconds = 3.;
constexpr double balanceEpsilon = 1.e-10;
constexpr double clipThreshold = .999;

void updateHold(double current, double &held, std::size_t &remaining, std::size_t duration) noexcept {
    if (current > held) {
        held = current;
        remaining = duration;
    } else if (remaining == 0 || --remaining == 0) {
        held = current;
    }
}

double dynamicLevel(double peakPower, double rmsPower) noexcept {
    return peakPower > 0. && rmsPower > 0. ? 10. * std::log10(peakPower / rmsPower) : 0.;
}
}

bool StereoEnvelopeMeter::prepare(const Settings &settings) noexcept {
    if (!positiveFinite(settings.sampleRate) || !positiveFinite(settings.peakDecaySeconds) ||
        !positiveFinite(settings.meanSquareSeconds) || !positiveFinite(settings.coincidenceSeconds) ||
        !std::isfinite(settings.coincidenceFloor) || settings.coincidenceFloor < 0)
        return false;
    const auto rate = settings.sampleRate;
    const auto interval = 1. / rate;
    const auto release = -std::expm1(std::log(.1) * interval / settings.peakDecaySeconds);
    const auto coincidenceStep = -std::expm1(-interval / settings.coincidenceSeconds);
    const auto floorPower = settings.coincidenceFloor * settings.coincidenceFloor;
    const auto rmsLength = std::max(1., std::round(rate * settings.meanSquareSeconds));
    const auto crestLength = std::max(1., std::round(rate * crestSeconds));
    const auto holdLength = std::max(1., std::round(rate * holdSeconds));
    const auto maxLength = static_cast<double>(powerHistory_.max_size());
    if (!std::isfinite(interval) || !std::isfinite(floorPower) || !(release > 0) || !(coincidenceStep > 0) ||
        rmsLength >= maxLength || crestLength >= maxLength || holdLength >= maxLength) return false;
    StereoMeterWeighting nextWeighting;
    if (!nextWeighting.prepare(rate, settings.weighting, StereoMeterWeighting::Purpose::Measurement)) return false;
    try {
        powerHistory_.resize(static_cast<std::size_t>(std::max(rmsLength, crestLength)));
    } catch (...) {
        return false;
    }

    rmsSamples_ = static_cast<std::size_t>(rmsLength);
    crestSamples_ = static_cast<std::size_t>(crestLength);
    holdSamples_ = static_cast<std::size_t>(holdLength);
    rmsScale_ = 1. / rmsLength;
    crestScale_ = 1. / crestLength;
    peakReleaseStep_ = release;
    coincidenceStep_ = coincidenceStep;
    balanceStep_ = -std::expm1(-interval / balanceSeconds);
    floorPower_ = floorPower;
    midSide_ = settings.midSide;
    linkedDynamic_ = settings.linkedDynamic;
    floatStep_ = -std::expm1(-interval / .7);
    weighting_ = nextWeighting;
    prepared_ = true;
    reset();
    return true;
}

void StereoEnvelopeMeter::reset() noexcept {
    lanes_ = {};
    filterState_ = {};
    correlationPower_ = balancePower_ = {};
    crossPower_ = 0.;
    maxCrestRatio_ = 1.;
    clipCount_ = {};
    clipping_ = {};
    // Samples left in the ring are ignored until they have been replaced.
    historyNext_ = historyFilled_ = 0;
}

bool StereoEnvelopeMeter::process(std::span<const float> left, std::span<const float> right) noexcept {
    if (!prepared_ || left.size() != right.size()) return false;
    // Validate first so a rejected block cannot mutate the sliding history.
    for (std::size_t i = 0; i < left.size(); ++i)
        if (!std::isfinite(left[i]) || !std::isfinite(right[i])) return false;
    [[maybe_unused]] mix::filters::FlushDenormals flush;
    if (weighting_.flat()) update<false>(left, right);
    else update<true>(left, right);
    return true;
}

template <bool weighted>
void StereoEnvelopeMeter::update(std::span<const float> left, std::span<const float> right) noexcept {
    const auto capacity = powerHistory_.size();
    const auto previous = [this, capacity](std::size_t samples) {
        return historyNext_ >= samples ? historyNext_ - samples : capacity - (samples - historyNext_);
    };
    for (std::size_t i = 0; i < left.size(); ++i) {
        const double l = left[i], r = right[i];
        const std::array raw{l, r};
        const auto samples = midSide_ ? std::array{.5 * (l + r), .5 * (l - r)} : raw;
        const auto rmsStart = previous(rmsSamples_), crestStart = previous(crestSamples_);
        const bool rmsFull = historyFilled_ >= rmsSamples_, crestFull = historyFilled_ >= crestSamples_;
        crossPower_ += coincidenceStep_ * (l * r - crossPower_);
        for (std::size_t lane = 0; lane < lanes_.size(); ++lane) {
            const auto rawPower = raw[lane] * raw[lane];
            correlationPower_[lane] += coincidenceStep_ * (rawPower - correlationPower_[lane]);
            balancePower_[lane] += balanceStep_ * (rawPower - balancePower_[lane]);
            const bool clipped = std::abs(raw[lane]) >= clipThreshold;
            if (clipped && !clipping_[lane]) ++clipCount_[lane];
            clipping_[lane] = clipped;

            auto &value = lanes_[lane];
            const auto magnitude = std::abs(samples[lane]);
            value.maximum = std::max(value.maximum, magnitude);
            value.peak = magnitude >= value.peak ? magnitude : std::lerp(value.peak, magnitude, peakReleaseStep_);
            updateHold(magnitude, value.heldPeak, value.peakWait, holdSamples_);
            value.floatingPeak += floatStep_ * (value.heldPeak - value.floatingPeak);

            auto measured = samples[lane];
            if constexpr (weighted) measured = weighting_.process(measured, filterState_[lane]);
            const auto power = measured * measured;
            if (rmsFull) value.rmsSum -= powerHistory_[rmsStart][lane];
            if (crestFull) value.crestSum -= powerHistory_[crestStart][lane];
            powerHistory_[historyNext_][lane] = power;
            value.rmsSum += power;
            value.crestSum += power;
            // Cancel rounding residue when the entire window is silent.
            value.silentSamples = power > 0. ? 0 : std::min(capacity, value.silentSamples + 1);
            if (value.silentSamples >= rmsSamples_) value.rmsSum = 0.;
            if (value.silentSamples >= crestSamples_) value.crestSum = 0.;
            const auto meanPower = std::max(0., value.rmsSum * rmsScale_);
            updateHold(meanPower, value.heldPower, value.rmsWait, holdSamples_);
            // Both windows must have filled before their ratio counts.
            if (historyFilled_ >= capacity - 1 && meanPower > floorPower_)
                maxCrestRatio_ = std::max(maxCrestRatio_, value.crestSum * crestScale_ / meanPower);
        }
        if (++historyNext_ == capacity) historyNext_ = 0;
        historyFilled_ = std::min(capacity, historyFilled_ + 1);
    }
}

StereoEnvelopeMeter::Reading StereoEnvelopeMeter::reading() const noexcept {
    Reading out;
    if (correlationPower_[0] > floorPower_ && correlationPower_[1] > floorPower_) {
        const auto powerProduct = correlationPower_[0] * correlationPower_[1];
        if (powerProduct > 0.)
            out.signCoincidence = std::clamp(crossPower_ / std::sqrt(powerProduct), -1., 1.);
    }
    out.balanceDb = 10. * std::log10((balancePower_[1] + balanceEpsilon) / (balancePower_[0] + balanceEpsilon));
    out.balance = std::clamp(out.balanceDb / 12., -1., 1.);
    out.clipCount = clipCount_;
    out.maxCrestFactorDb = 10. * std::log10(maxCrestRatio_);
    std::array<double, 2> rmsPower{}, peakPower{};
    for (std::size_t i = 0; i < lanes_.size(); ++i) {
        const auto &value = lanes_[i];
        rmsPower[i] = std::max(0., value.rmsSum * rmsScale_);
        peakPower[i] = value.heldPeak * value.heldPeak;
        out.peak[i] = value.peak;
        out.sineReferencedRms[i] = std::sqrt(2. * rmsPower[i]); // AES-17 sine reference
        out.maximumPeak[i] = value.maximum;
        out.peakHold[i] = value.heldPeak;
        out.rmsHold[i] = std::sqrt(2. * value.heldPower);
        out.floatingPeak[i] = value.floatingPeak;
        out.dynamicDb[i] = dynamicLevel(peakPower[i], rmsPower[i]);
    }
    if (linkedDynamic_) {
        const auto combinedPower = (rmsPower[0] + rmsPower[1]) * .5;
        const auto combinedPeakPower = (peakPower[0] + peakPower[1]) * .5;
        out.linkedSineReferencedRms = std::sqrt(2. * combinedPower);
        if (combinedPower > 0.) out.linkedDynamicDb = dynamicLevel(combinedPeakPower, combinedPower);
    }
    return out;
}
} // namespace adi::dsp
