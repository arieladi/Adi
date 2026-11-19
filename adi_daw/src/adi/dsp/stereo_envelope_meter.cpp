// SPDX-License-Identifier: AGPL-3.0-or-later
#include "adi/dsp/stereo_envelope_meter.hpp"
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
}

bool StereoEnvelopeMeter::prepare(const Settings &s) noexcept {
    if (!positiveFinite(s.sampleRate) || !positiveFinite(s.peakDecaySeconds) ||
        !positiveFinite(s.meanSquareSeconds) || !positiveFinite(s.coincidenceSeconds) ||
        !std::isfinite(s.coincidenceFloor) || s.coincidenceFloor < 0)
        return false;
    const double interval = 1.0 / s.sampleRate;
    const double decay = std::exp(-interval / s.peakDecaySeconds);
    const double powerStep = -std::expm1(-interval / s.meanSquareSeconds);
    const double coincidenceStep = -std::expm1(-interval / s.coincidenceSeconds);
    if (!std::isfinite(interval) || !(decay < 1) || !(powerStep > 0) || !(coincidenceStep > 0))
        return false;
    if (s.sampleRate >= static_cast<double>(std::numeric_limits<std::int64_t>::max()) * .5) return false;
    if (s.weighting < Weighting::Standard || s.weighting > Weighting::K ||
        (s.weighting != Weighting::Standard && s.sampleRate <= 2000.)) return false;
    std::array<Biquad, 4> filter{};
    std::size_t count = 0;
    if (s.weighting != Weighting::Standard) {
        const bool k = s.weighting == Weighting::K;
        const auto pole = [rate = s.sampleRate](double corner) {
            const auto warped = std::tan(std::numbers::pi * corner / rate);
            return (1. - warped) / (1. + warped);
        };
        const auto hp = pole(k ? 38.1 : 20.6);
        const auto upper = upperSection(s.sampleRate, k);
        filter[0] = {1., -2., 1., -2. * hp, hp * hp};
        filter[1] = {upper[0], upper[1], upper[2], upper[3], upper[4]};
        count = 2;
        if (s.weighting == Weighting::A) {
            filter[2] = {1., -1., 0., -pole(107.7), 0.};
            filter[3] = {1., -1., 0., -pole(737.9), 0.};
            count = 4;
        }
        const auto z = std::polar(1., -2. * std::numbers::pi * 1000. / s.sampleRate);
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
    weighting_ = filter;
    sectionCount_ = count;
    peakDecay_ = decay;
    powerStep_ = powerStep;
    coincidenceStep_ = coincidenceStep;
    floor_ = s.coincidenceFloor;
    midSide_ = s.midSide;
    drStep_ = -std::expm1(-interval / .4);
    floatStep_ = -std::expm1(-interval / .7);
    holdSamples_ = static_cast<std::int64_t>(2. * s.sampleRate);
    prepared_ = true;
    reset();
    return true;
}

void StereoEnvelopeMeter::reset() noexcept {
    lanes_ = {};
    filterState_ = {};
    balance_ = coincidence_ = 0;
}

bool StereoEnvelopeMeter::process(std::span<const float> left, std::span<const float> right) noexcept {
    if (!prepared_ || left.size() != right.size()) return false;
    if (sectionCount_ == 0) return update<false>(left, right);
    [[maybe_unused]] mix::filters::FlushDenormals flush;
    return update<true>(left, right);
}

template <bool weighted>
bool StereoEnvelopeMeter::update(std::span<const float> left, std::span<const float> right) noexcept {
    auto lanes = lanes_;
    auto filterState = filterState_;
    double coincidence = coincidence_, balance = balance_;
    for (std::size_t i = 0; i < left.size(); ++i) {
        const double l = left[i], r = right[i];
        if (!std::isfinite(l) || !std::isfinite(r)) return false;
        const double absL = std::abs(l), absR = std::abs(r);
        const double larger = std::max(absL, absR), smaller = std::min(absL, absR);
        const double sign = smaller > floor_ ? ((l < 0) == (r < 0) ? 1. : -1.) : 0.;
        coincidence += coincidenceStep_ * (sign - coincidence);
        const double offset = larger > floor_ ? std::clamp((absR - absL) / larger * (4. / 3.), -1., 1.) : 0.;
        balance += coincidenceStep_ * (offset - balance);
        const std::array samples = midSide_ ? std::array{.5 * (l + r), .5 * (l - r)}
                                           : std::array{l, r};
        for (std::size_t lane = 0; lane < lanes.size(); ++lane) {
            auto &m = lanes[lane];
            const double x = std::abs(samples[lane]);
            double measured = samples[lane];
            if constexpr (weighted)
                for (std::size_t section = 0; section < sectionCount_; ++section) {
                    const auto &c = weighting_[section];
                    auto &state = filterState[lane][section];
                    const auto result = c.b0 * measured + state.first;
                    state.first = c.b1 * measured - c.a1 * result + state.second;
                    state.second = c.b2 * measured - c.a2 * result;
                    measured = result;
                }
            m.maximum = std::max(m.maximum, x);
            m.peak = std::max(x, m.peak * peakDecay_);
            m.power += powerStep_ * (measured * measured - m.power);
            m.floatingPeak += floatStep_ * (m.peak - m.floatingPeak);
            const double ratio = m.peak > floor_ ? std::min(.5, m.power / (m.peak * m.peak)) : .5;
            m.ratio += drStep_ * (ratio - m.ratio);
            if (m.peak >= m.heldPeak) { m.heldPeak = m.peak; m.peakWait = holdSamples_; }
            else if (m.peakWait > 0) --m.peakWait;
            else m.heldPeak = std::max(m.peak, m.heldPeak * peakDecay_);
            if (m.power >= m.heldPower) { m.heldPower = m.power; m.rmsWait = holdSamples_; }
            else if (m.rmsWait > 0) --m.rmsWait;
            else m.heldPower = std::max(m.power, m.heldPower * (1. - powerStep_));
        }
    }
    lanes_ = lanes;
    if constexpr (weighted) filterState_ = filterState;
    coincidence_ = coincidence;
    balance_ = balance;
    return true;
}

StereoEnvelopeMeter::Reading StereoEnvelopeMeter::reading() const noexcept {
    Reading out;
    out.signCoincidence = coincidence_;
    out.balance = balance_;
    for (std::size_t i = 0; i < lanes_.size(); ++i) {
        const auto &m = lanes_[i];
        out.peak[i] = m.peak;
        out.sineReferencedRms[i] = std::sqrt(2. * m.power);
        out.maximumPeak[i] = m.maximum;
        out.peakHold[i] = m.heldPeak;
        out.rmsHold[i] = std::sqrt(2. * m.heldPower);
        out.floatingPeak[i] = m.floatingPeak;
        out.dynamicDb[i] = -10. * std::log10(std::max(2. * m.ratio, 1.e-30));
    }
    return out;
}
} // namespace adi::dsp
