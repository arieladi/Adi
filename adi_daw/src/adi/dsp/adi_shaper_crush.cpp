// SPDX-License-Identifier: GPL-3.0-or-later
#include "adi/dsp/adi_shaper_crush.hpp"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace adi::dsp {
namespace {
// The small offset above 3 kHz is audible on long fractional hold captures.
constexpr double holdLow = 8.82;
constexpr double holdMid = 3000.00500679;
constexpr double holdHigh = 44100.0;
constexpr double holdRatio = (holdHigh - holdMid) / (holdMid - holdLow);
constexpr double holdScale = (holdMid - holdLow) / (holdRatio - 1.0);
constexpr double totalLevelRange = 32767.0;
constexpr double linearLevelRange = 14.0 * totalLevelRange / 32768.0;
constexpr unsigned clockIntervals = 256;
const std::array<double, clockIntervals + 1> exponent = [] {
    std::array<double, clockIntervals + 1> table{};
    for (unsigned i = 0; i <= clockIntervals; ++i)
        table[i] = std::pow(holdRatio, 2.0 * i / clockIntervals);
    return table;
}();
const double logStep = 2 * std::log(holdRatio) / clockIntervals;

// Tabulate the bilinear one-pole gain and its derivative over [0, Nyquist].
// A moving contour then uses one cubic interpolation for all sixteen poles.
constexpr unsigned prefilterIntervals = 256;
constexpr double prefilterBinWidth = .5 / prefilterIntervals;
constexpr float prefilterMaximum = .999686062335968f;
struct PrefilterPoint { double gain, slope; };
const auto prefilterCurve = [] {
    std::array<PrefilterPoint, prefilterIntervals + 1> curve{};
    for (unsigned i = 0; i <= prefilterIntervals; ++i) {
        const double angle = std::numbers::pi * (i * prefilterBinWidth - .25);
        const double gain = .5 * (1.0 + std::tan(angle));
        const double slope = std::numbers::pi * prefilterBinWidth *
                             (gain * gain + (1.0 - gain) * (1.0 - gain));
        curve[i] = {gain, slope};
    }
    return curve;
}();

float prefilterCoefficient(double step) noexcept {
    if (step >= .4999) return prefilterMaximum;
    const double position = step / prefilterBinWidth;
    const auto index = static_cast<unsigned>(position);
    const double t = position - index;
    const auto& a = prefilterCurve[index];
    const auto& b = prefilterCurve[index + 1];
    const double delta = b.gain - a.gain;
    return static_cast<float>(a.gain + t * (a.slope + t *
        (3.0 * delta - 2.0 * a.slope - b.slope + t * (a.slope + b.slope - 2.0 * delta))));
}

float sanitized(float height) noexcept {
    return std::isfinite(height) ? std::clamp(height, 0.f, 1.f) : 1.f;
}
} // namespace

bool AdiShaperCrush::prepare(double sampleRate) noexcept {
    rate_ = std::isfinite(sampleRate) && sampleRate >= 1 ? sampleRate : 0;
    if (rate_ > 0) {
        highpassAmount_ = -std::expm1(-2.0 * std::numbers::pi * 10.0 / rate_);
        highpassGain_ = 1.0 - .5 * highpassAmount_;
    }
    reset();
    return rate_ > 0;
}

void AdiShaperCrush::reset() noexcept {
    bits_ = clock_ = -1;
    phase_ = 0;
    seed_ = 0x297a04b3u;
    previousQuantInput_ = previousHoldInput_ = held_ = low_ = {};
    prefilterMemory_ = {};
    prefilterStep_ = -1;
}

void AdiShaperCrush::updateResolution(float height) noexcept {
    bits_ = height;
    const double u = height;
    const double u2 = u * u, u4 = u2 * u2, u8 = u4 * u4, u16 = u8 * u8;
    levels_ = 1.0 + linearLevelRange * u + (totalLevelRange - linearLevelRange) * (u16 * u16);
    const double remaining = 1.0 - u;
    quantizedGain_ = (1.0 - .25 * remaining * remaining * remaining) / levels_;
}

void AdiShaperCrush::updateClock(float height) noexcept {
    clock_ = height;
    if (height == 1.f) {
        step_ = period_ = 1;
        return;
    }
    const double index = static_cast<double>(height) * clockIntervals;
    const auto slot = static_cast<unsigned>(index);
    const double x = (index - slot) * logStep;
    // A short residual polynomial keeps the fractional sample clock precise
    // while a moving contour avoids a transcendental function at audio rate.
    const double power = exponent[slot] * (1.0 + x * (1.0 + x * (.5 + x / 6.0)));
    const double hz = holdLow + holdScale * (power - 1.0);
    step_ = std::min(hz / rate_, 1.0);
    period_ = 1.0 / step_;
}

double AdiShaperCrush::noise() noexcept {
    // Two uniform draws shared by L/R give triangular dither between +/- half
    // a quantization step. Reset is reproducible for renders and seeks.
    const auto uniform = [this]() noexcept {
        seed_ ^= seed_ << 13;
        seed_ ^= seed_ >> 17;
        seed_ ^= seed_ << 5;
        return static_cast<double>(seed_ >> 8) * 0x1p-24;
    };
    const double first = uniform();
    return .5 * (first - uniform());
}

void AdiShaperCrush::quantize(Stereo& sample) noexcept {
    if (bits_ == 1.f) {
        previousQuantInput_ = sample;
        return;
    }
    const double perturbation = dither_ ? noise() / levels_ : 0.0;
    for (unsigned ch = 0; ch < 2; ++ch) {
        const double value = sample[ch] + perturbation;
        const double before = previousQuantInput_[ch] * levels_;
        const double now = value * levels_;
        const double priorCode = std::floor(before + .5);
        const double code = std::floor(now + .5);
        if (code == priorCode) {
            sample[ch] = quantizedGain_ * code;
        } else {
            // Integrated staircase over the linear transition from before to
            // now. This centered form avoids subtracting two large primitives.
            const double residuals = (now - code) + (before - priorCode);
            const double mean = .5 * ((code + priorCode) + (code - priorCode) * residuals / (now - before));
            sample[ch] = quantizedGain_ * mean;
        }
        previousQuantInput_[ch] = value;
    }
}

void AdiShaperCrush::prefilter(Stereo& sample) noexcept {
    if (step_ != prefilterStep_) {
        prefilterGain_ = prefilterCoefficient(step_);
        prefilterStep_ = step_;
    }
    std::array<float, 2> value{static_cast<float>(sample[0]), static_cast<float>(sample[1])};
    for (auto& memory : prefilterMemory_) {
        for (unsigned ch = 0; ch < 2; ++ch) {
            const float delta = prefilterGain_ * (value[ch] - memory[ch]);
            value[ch] = memory[ch] + delta;
            memory[ch] = value[ch] + delta;
        }
    }
    sample = {value[0], value[1]};
}

void AdiShaperCrush::hold(Stereo& sample) noexcept {
    if (step_ >= 1.0) {
        previousHoldInput_ = held_ = sample;
        phase_ = 0;
        return;
    }
    const auto input = sample;
    const double next = phase_ + step_;
    if (next < 1.0) {
        sample = held_;
        phase_ = next;
    } else {
        const double fraction = std::clamp((1.0 - phase_) * period_, 0.0, 1.0);
        for (unsigned ch = 0; ch < 2; ++ch) {
            const double captured = previousHoldInput_[ch] +
                                    fraction * (input[ch] - previousHoldInput_[ch]);
            sample[ch] = held_[ch] + (1.0 - fraction) * (captured - held_[ch]);
            held_[ch] = captured;
        }
        phase_ = next - 1.0;
    }
    previousHoldInput_ = input;
}

void AdiShaperCrush::processStereo(float& left, float& right, float bitsHeight,
                                   float resampleHeight, bool flatHold) noexcept {
    if (rate_ == 0) return;
    const auto u = sanitized(bitsHeight), r = sanitized(resampleHeight);
    if (u != bits_) updateResolution(u);
    if (r != clock_) updateClock(r);
    Stereo sample{left, right};
    quantize(sample);
    if (prefilterEnabled_ && !(flatHold && r == 1.f)) prefilter(sample);
    hold(sample);
    for (unsigned ch = 0; ch < 2; ++ch) {
        const double difference = sample[ch] - low_[ch];
        low_[ch] += highpassAmount_ * difference;
        if (std::abs(low_[ch]) < 1.0e-28) low_[ch] = 0;
        sample[ch] = highpassGain_ * difference;
    }
    left = static_cast<float>(sample[0]);
    right = static_cast<float>(sample[1]);
}

bool AdiShaperCrush::finishBlock() noexcept {
    bool repaired = false;
    for (std::size_t ch = 0; ch < low_.size(); ++ch) {
        const bool damagedFilter = std::any_of(prefilterMemory_.begin(), prefilterMemory_.end(),
            [ch](const auto& pole) { return !std::isfinite(pole[ch]); });
        if (damagedFilter || !std::isfinite(low_[ch]) || !std::isfinite(held_[ch]) ||
            !std::isfinite(previousHoldInput_[ch]) || !std::isfinite(previousQuantInput_[ch])) {
            low_[ch] = held_[ch] = previousHoldInput_[ch] = previousQuantInput_[ch] = 0.0;
            for (auto& pole : prefilterMemory_) pole[ch] = 0.f;
            repaired = true;
        }
    }
    return repaired;
}
} // namespace adi::dsp
