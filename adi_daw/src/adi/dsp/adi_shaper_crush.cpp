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
        step_ = 1;
        period_ = 1;
        return;
    }
    // The multiply rounds to float before the add. Contraction shifts the
    // capture position on sustained contours, so force that rounding here.
    const volatile float scaled = logSpan * height;
    const float hz = clockExponential(logMinimum + scaled) - frequencyOffset;
    step_ = static_cast<float>(std::min(static_cast<double>(hz) / rate_, 1.0));
    period_ = 0.0; // Recompute on the first capture at this rate.
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

void AdiShaperCrush::hold(Stereo& sample, bool flatHold) noexcept {
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
    if (u != bits_) updateResolution(u);
    if (r != clock_) updateClock(r);
    Stereo sample{left, right};
    quantize(sample);
    hold(sample, flatHold);
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
        if (!std::isfinite(low_[ch]) || !std::isfinite(held_[ch]) ||
            !std::isfinite(previousHoldInput_[ch]) || !std::isfinite(previousQuantInput_[ch])) {
            low_[ch] = held_[ch] = previousHoldInput_[ch] = previousQuantInput_[ch] = 0.0;
            repaired = true;
        }
    }
    return repaired;
}
} // namespace adi::dsp
