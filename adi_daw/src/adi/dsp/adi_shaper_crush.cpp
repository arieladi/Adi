// SPDX-License-Identifier: GPL-3.0-or-later
#include "adi/dsp/adi_shaper_crush.hpp"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace adi::dsp {
namespace {
constexpr double totalLevelRange = 32767.0;
constexpr double linearLevelRange = 14.0 * totalLevelRange / 32768.0;

// Logarithmic endpoints at 8.82 and 44100 Hz with the measured 225.96 Hz offset.
constexpr float logMinimum = 5.45864868f;
constexpr float logSpan = 5.24067688f;
constexpr float frequencyOffset = 225.96f;
constexpr unsigned clockPartBits = 7;
constexpr unsigned clockHighShift = 2 * clockPartBits;
constexpr unsigned clockPartMask = (1u << clockPartBits) - 1;
constexpr unsigned firstCell = static_cast<unsigned>(logMinimum * 0x1p21f) >> clockHighShift;
constexpr unsigned lastCell = static_cast<unsigned>((logMinimum + logSpan) * 0x1p21f) >> clockHighShift;

template <std::size_t N>
std::array<double, N> clockFactors(unsigned first, double unit) noexcept {
    std::array<double, N> table{};
    for (std::size_t i = 0; i < N; ++i)
        table[i] = std::exp((static_cast<double>(i) + first) * unit);
    return table;
}
const auto clockHigh = clockFactors<lastCell - firstCell + 1>(firstCell, 0x1p-7);
const auto clockMiddle = clockFactors<clockPartMask + 1>(0, 0x1p-14);
const auto clockLow = clockFactors<clockPartMask + 1>(0, 0x1p-21);

float clockExponential(float logFrequency) noexcept {
    // The calibrated binary32 log frequencies are exact multiples of 2^-21.
    // Splitting that integer into three parts keeps the shared tables below 8 KiB.
    const auto ticks = static_cast<unsigned>(logFrequency * 0x1p21f);
    const auto cell = (ticks >> clockHighShift) - firstCell;
    return static_cast<float>(clockHigh[cell] * clockMiddle[(ticks >> clockPartBits) & clockPartMask] *
                              clockLow[ticks & clockPartMask]);
}

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
    if (height >= 0.f && height <= 1.f) return height;
    return std::isfinite(height) ? std::clamp(height, 0.f, 1.f) : 1.f;
}

double integratedCode(double before, double now) noexcept {
    const double priorCode = std::floor(before + .5);
    const double code = std::floor(now + .5);
    if (code == priorCode) return code;
    // Average the staircase over the signed interval without subtracting
    // large antiderivatives at its two ends.
    const double residuals = (now - code) + (before - priorCode);
    return .5 * ((code + priorCode) + (code - priorCode) * residuals / (now - before));
}
} // namespace

bool AdiShaperCrush::prepare(double sampleRate) noexcept {
    rate_ = std::isfinite(sampleRate) && sampleRate >= 1 ? sampleRate : 0;
    if (rate_ > 0) {
        highpassAmount_ = -std::expm1(-2.0 * std::numbers::pi * 10.0 / rate_);
        highpassGain_ = 1.0 - .5 * highpassAmount_;
        const double g = std::tan(std::numbers::pi * std::min(350.0 / rate_, .49));
        treblePole_ = g / (1.0 + g);
    }
    reset();
    return rate_ > 0;
}

void AdiShaperCrush::reset() noexcept {
    bits_ = clock_ = -1;
    phase_ = 0;
    seed_ = jitterSeed_ = initialSeed;
    previousPerturbation_ = 0.;
    previousQuantInput_ = previousCode_ = previousHoldInput_ = held_ = low_ = trebleState_ = {};
    prefilterMemory_ = {};
    prefilterStep_ = -1;
}

void AdiShaperCrush::setTreble(float normalized) noexcept {
    if (std::isfinite(normalized)) treble_ = std::clamp(normalized, 0.f, 1.f);
}

void AdiShaperCrush::setJitter(float normalized) noexcept {
    const float amount = std::isfinite(normalized) ? std::clamp(normalized, 0.f, 1.f) : 0.f;
    if (amount == jitter_) return;
    jitter_ = amount;
    const double square = static_cast<double>(amount) * amount;
    // A triangular draw of radius 1/2 gives the measured timing distribution.
    jitterScale_ = 20.0 * square;
    jitterOffset_ = 1.0 - std::numbers::e * square * square;
}

void AdiShaperCrush::setBias(float normalized) noexcept {
    if (!std::isfinite(normalized)) return;
    const float bias = std::clamp(normalized, 0.f, 1.f);
    if (bias == bias_) return;
    bias_ = bias;
    if (bits_ >= 0.f) updateQuantizerGain();
}

void AdiShaperCrush::updateResolution(float height) noexcept {
    bits_ = height;
    const double u = height;
    const double u2 = u * u, u4 = u2 * u2, u8 = u4 * u4, u16 = u8 * u8;
    levels_ = 1.0 + linearLevelRange * u + (totalLevelRange - linearLevelRange) * (u16 * u16);
    updateQuantizerGain();
}

void AdiShaperCrush::updateQuantizerGain() noexcept {
    const double remaining = 1.0 - bits_;
    double numerator = 1.0 - .25 * remaining * remaining * remaining;
    if (bias_ == 0.f) {
        biasInput_ = 0.;
    } else {
        const double remainder3 = remaining * remaining * remaining;
        const double skew = static_cast<double>(bias_) * remainder3;
        numerator -= .25 * skew * skew * skew;
        biasInput_ = .499 * static_cast<double>(bias_) / levels_;
    }
    quantizedGain_ = numerator / levels_;
}

void AdiShaperCrush::updateClock(float height) noexcept {
    clock_ = height;
    if (height == 1.f) {
        step_ = 1;
        period_ = 1;
        uncappedStep_ = 1;
        return;
    }
    // The multiply rounds to float before the add. Contraction shifts the
    // capture position on sustained contours, so force that rounding here.
    const volatile float scaled = logSpan * height;
    const float hz = clockExponential(logMinimum + scaled) - frequencyOffset;
    uncappedStep_ = static_cast<double>(hz) / rate_;
    step_ = static_cast<float>(std::min(uncappedStep_, 1.0));
    period_ = 0.0; // Recompute on the first capture at this rate.
}

double AdiShaperCrush::noise(std::uint32_t& seed) noexcept {
    // Two uniform draws shared by L/R give triangular dither between +/- half
    // a quantization step. Reset is reproducible for renders and seeks.
    // Dither and timing each advance this same sequence at their own
    // position, so pausing one leaves the other untouched.
    const auto uniform = [&seed]() noexcept {
        seed ^= seed << 13;
        seed ^= seed >> 17;
        seed ^= seed << 5;
        return static_cast<double>(seed >> 8) * 0x1p-24;
    };
    const double first = uniform();
    return .5 * (first - uniform());
}

void AdiShaperCrush::quantize(Stereo& sample, double previousLevels, double codeScale) noexcept {
    if (bits_ == 1.f) {
        previousQuantInput_ = sample;
        previousPerturbation_ = 0.;
        return;
    }
    // Bias is specified in codes. Keep it with each sample so a change at the
    // next frame integrates from the previous input and its original bias.
    const double perturbation = dither_ ? biasInput_ + noise(seed_) / levels_ : biasInput_;
    // With biased dither, subtract the same staircase driven only by the
    // perturbation. Both channels share this reference and silence stays quiet.
    // Like the signal's own earlier sample, the earlier perturbation is read in
    // the step it was quantized with.
    const double zeroCode = dither_ && bias_ != 0.f ?
        integratedCode(previousPerturbation_ * previousLevels, perturbation * levels_) : 0.;
    for (unsigned ch = 0; ch < 2; ++ch) {
        const double value = sample[ch] + perturbation;
        const double before = previousQuantInput_[ch] * previousLevels;
        const double now = value * levels_;
        const double priorCode = previousCode_[ch];
        const double code = std::floor(now + .5);
        double mean = code;
        if (code != priorCode) {
            // Find the crossing in code space. The previous output step keeps
            // its original amplitude when expressed in units of the new step.
            const double priorOutput = priorCode * codeScale;
            const double residuals = (now - code) + (before - priorCode);
            mean = .5 * ((code + priorOutput) + (code - priorOutput) * residuals / (now - before));
        }
        // zeroCode is exactly zero without bias, so an unbiased band keeps its samples.
        sample[ch] = quantizedGain_ * (mean - zeroCode);
        previousQuantInput_[ch] = value;
        previousCode_[ch] = code;
    }
    previousPerturbation_ = perturbation;
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

void AdiShaperCrush::jitteredHold(Stereo& sample, bool flatHold) noexcept {
    // A flat full-height contour pauses the phase and the timing draws.
    if (flatHold && clock_ == 1.f) {
        previousHoldInput_ = held_ = sample;
        return;
    }
    const double speed = std::max(0.0, jitterOffset_ + jitterScale_ * noise(jitterSeed_));
    const double increment = uncappedStep_ * speed;
    const double phase = phase_;
    if (increment >= 1.0) {
        // Pass this input directly while the capped clock advances one
        // whole cycle. Its fractional remainder sets the next held value.
        const double fraction = 1.0 - phase;
        for (unsigned ch = 0; ch < 2; ++ch)
            held_[ch] = previousHoldInput_[ch] + fraction * (sample[ch] - previousHoldInput_[ch]);
        previousHoldInput_ = sample;
        return;
    }
    const auto input = sample;
    const double next = phase + increment;
    if (next < 1.0) {
        sample = held_;
        phase_ = static_cast<float>(next);
    } else {
        const double fraction = std::clamp((1.0 - phase) / increment, 0.0, 1.0);
        for (unsigned ch = 0; ch < 2; ++ch) {
            const double captured = previousHoldInput_[ch] +
                                    fraction * (input[ch] - previousHoldInput_[ch]);
            sample[ch] = held_[ch] + (1.0 - fraction) * (captured - held_[ch]);
            held_[ch] = captured;
        }
        phase_ = static_cast<float>(next - 1.0);
    }
    previousHoldInput_ = input;
}

void AdiShaperCrush::hold(Stereo& sample, bool flatHold) noexcept {
    // Without jitter the clock is the landed one, sample for sample.
    if (jitter_ > 0.f) {
        jitteredHold(sample, flatHold);
        return;
    }
    if (step_ >= 1.0) {
        if (flatHold && clock_ == 1.f) {
            held_ = sample;
        } else {
            // The direct output spans one whole clock cycle. Keep its
            // remainder and interpolate the capture for the next slower frame.
            const double fraction = 1.0 - phase_;
            for (unsigned ch = 0; ch < 2; ++ch)
                held_[ch] = previousHoldInput_[ch] + fraction * (sample[ch] - previousHoldInput_[ch]);
        }
        previousHoldInput_ = sample;
        return;
    }
    const auto input = sample;
    // The sum rounds before the wrap. Higher precision changes the rate
    // enough to capture different source frames during sustained playback.
    const float next = phase_ + step_;
    if (next < 1.f) {
        sample = held_;
        phase_ = next;
    } else {
        if (period_ == 0.0) period_ = 1.0 / step_;
        const double fraction = std::clamp((1.0 - phase_) * period_, 0.0, 1.0);
        for (unsigned ch = 0; ch < 2; ++ch) {
            const double captured = previousHoldInput_[ch] +
                                    fraction * (input[ch] - previousHoldInput_[ch]);
            sample[ch] = held_[ch] + (1.0 - fraction) * (captured - held_[ch]);
            held_[ch] = captured;
        }
        phase_ = next - 1.f;
    }
    previousHoldInput_ = input;
}

void AdiShaperCrush::processStereo(float& left, float& right, float bitsHeight,
                                   float resampleHeight, bool flatHold) noexcept {
    if (rate_ == 0) return;
    const auto u = sanitized(bitsHeight), r = sanitized(resampleHeight);
    const double previousLevels = levels_;
    double codeScale = 1.0;
    if (u != bits_) {
        // Bypass records its input without quantizing. Recover that final
        // sample's code only when leaving bypass.
        if (bits_ == 1.f) {
            for (unsigned ch = 0; ch < 2; ++ch)
                previousCode_[ch] = std::floor(previousQuantInput_[ch] * previousLevels + .5);
        }
        updateResolution(u);
        codeScale = levels_ / previousLevels;
    }
    if (r != clock_) updateClock(r);
    Stereo sample{left, right};
    quantize(sample, previousLevels, codeScale);
    if (prefilterEnabled_ && !(flatHold && r == 1.f)) prefilter(sample);
    hold(sample, flatHold);
    // The treble shelf follows the product of the bit and hold contours.
    const double highGain = treble_ + (1.0 - treble_) * u * r;
    for (unsigned ch = 0; ch < 2; ++ch) {
        // Keep the split warm at unity so a live edit uses the preceding audio.
        const double delta = treblePole_ * (sample[ch] - trebleState_[ch]);
        const double low = trebleState_[ch] + delta;
        trebleState_[ch] += 2.0 * delta;
        if (std::abs(trebleState_[ch]) < 1.e-28) trebleState_[ch] = 0.;
        if (highGain != 1.) sample[ch] = low + highGain * (sample[ch] - low);
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
        if (damagedFilter || !std::isfinite(trebleState_[ch]) || !std::isfinite(low_[ch]) || !std::isfinite(held_[ch]) ||
            !std::isfinite(previousHoldInput_[ch]) || !std::isfinite(previousQuantInput_[ch])) {
            trebleState_[ch] = low_[ch] = held_[ch] = previousHoldInput_[ch] = previousQuantInput_[ch] =
                previousCode_[ch] = 0.0;
            for (auto& pole : prefilterMemory_) pole[ch] = 0.f;
            repaired = true;
        }
    }
    return repaired;
}
} // namespace adi::dsp
