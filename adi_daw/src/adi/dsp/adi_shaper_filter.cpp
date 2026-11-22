// SPDX-License-Identifier: GPL-3.0-or-later
#include "adi_shaper_filter.hpp"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace adi::dsp {
namespace {
// The second section uses the same normalized resonance law as the first.
const double fixedDamping = 2.0 - 1.995 * std::cbrt(static_cast<double>(0.2f));
} // namespace

AdiShaperFilter::AdiShaperFilter() noexcept { prepare(48000.0); }

void AdiShaperFilter::prepare(double sampleRate) noexcept {
    if (!(sampleRate > 0.0 && std::isfinite(sampleRate))) return;
    if (sampleRate != sampleRate_) {
        sampleRate_ = sampleRate;
        // Ten octaves from E0. Store the value and derivative per interval
        // so moving a cutoff costs a cubic interpolation on the audio thread.
        for (int i = 0; i <= intervals; ++i) {
            const double freq = 20.6017223070544 * std::exp2(10.0 * i / intervals);
            const double angle = std::numbers::pi * std::min(0.49, 0.5 * freq / sampleRate);
            const double g = std::tan(angle);
            const double slope = freq < 0.98 * sampleRate
                ? angle * (1.0 + g * g) * (10.0 * std::numbers::ln2 / intervals) : 0.0;
            warp_[static_cast<std::size_t>(i)] = {g, slope};
        }
    }
    lastCutoff_ = -1.0f;
    lastResonance_ = -1.0f;
    reset();
}

void AdiShaperFilter::reset() noexcept {
    channels_ = {};
    second_ = {};
}

void AdiShaperFilter::setMode(Mode mode) noexcept {
    const bool cascade = mode == Mode::LowPass24 || mode == Mode::HighPass24;
    if (mode == Mode::LowPass24) mode = Mode::LowPass12;
    if (mode == Mode::HighPass24) mode = Mode::HighPass12;
    if (mode_ != mode || cascade_ != cascade) {
        mode_ = mode;
        cascade_ = cascade;
        if (cascade && lastCutoff_ >= 0.0f) fixed_ = response(fixedDamping);
        reset();
    }
}

double AdiShaperFilter::warpAt(float cutoff) const noexcept {
    const double position = static_cast<double>(cutoff) * intervals;
    const auto index = std::min(static_cast<int>(position), intervals - 1);
    const double t = position - index;
    const auto& left = warp_[static_cast<std::size_t>(index)];
    const auto& right = warp_[static_cast<std::size_t>(index + 1)];
    const double delta = right.value - left.value;
    return left.value + t * (left.slope + t * (3.0 * delta - 2.0 * left.slope - right.slope
                                             + t * (left.slope + right.slope - 2.0 * delta)));
}

AdiShaperFilter::Response AdiShaperFilter::response(double damping) const noexcept {
    const double a1 = 1.0 / (1.0 + g_ * (g_ + damping));
    return {a1, 1.0 / 12.0 + (4.0 / 27.0) * g_ * a1};
}

void AdiShaperFilter::tune(float cutoff, float resonance) noexcept {
    if (cutoff != lastCutoff_) {
        lastCutoff_ = cutoff;
        g_ = warpAt(cutoff);
        inverseG_ = 1.0 / g_;
        poleGain_ = g_ / (1.0 + g_);
        if (cascade_) fixed_ = response(fixedDamping);
    }
    if (resonance != lastResonance_) {
        lastResonance_ = resonance;
        damping_ = 2.0 - 1.995 * std::cbrt(static_cast<double>(resonance));
        gentleResonance_ = (2.0 - damping_) * (2.0 / 3.0);
    }
    variable_ = response(damping_);
}

float AdiShaperFilter::tick(float input, Channel& state, const Response& coefficients) const noexcept {
    const double x = input;
    if (x == 0.0 && std::abs(state.band) + std::abs(state.low) + std::abs(state.pole) < 1.0e-28) {
        state = {};
        return 0.0f;
    }
    // Two trapezoidal steps consume the same input. The reference's gentle
    // slopes also include a single-pole branch. The steep high-pass reads
    // the mean of the two high-pass outputs.
    const bool gentle = mode_ == Mode::LowPass6 || mode_ == Mode::HighPass6;
    double band = 0.0, low = 0.0, pole = 0.0, highSum = 0.0;
    for (int step = 0; step < 2; ++step) {
        double excitation = x;
        if (gentle) {
            const double difference = poleGain_ * (x - state.pole);
            pole = state.pole + difference;
            state.pole = pole + difference;
            excitation = pole;
        }
        // Impulse amplitude sweeps show additional damping proportional to
        // the preceding band output squared, saturating at large amplitudes.
        const double squared = state.previousBand * state.previousBand;
        const double gain = coefficients.a1 * (1.0 + squared / 12.0) / (1.0 + coefficients.resonanceScale * squared);
        band = gain * (state.band + g_ * (excitation - state.low));
        low = state.low + g_ * band;
        if (mode_ == Mode::HighPass12) highSum += (band - state.band) * inverseG_;
        state.band = 2.0 * band - state.band;
        state.low = 2.0 * low - state.low;
        state.previousBand = band;
    }
    if (gentle) {
        const double output = pole + gentleResonance_ * band;
        return static_cast<float>(mode_ == Mode::LowPass6 ? output : x - output);
    }
    if (mode_ == Mode::HighPass12) return static_cast<float>(0.5 * highSum);
    if (mode_ == Mode::BandPass12) return static_cast<float>(band);
    return static_cast<float>(low);
}

void AdiShaperFilter::process(float& left, float& right, float cutoff, float resonance) noexcept {
    if (cutoff != lastCutoff_ || resonance != lastResonance_) {
        // A malformed automation sample retains the last valid control.
        if (!std::isfinite(cutoff)) cutoff = lastCutoff_ >= 0.0f ? lastCutoff_ : 1.0f;
        if (!std::isfinite(resonance)) resonance = std::max(0.0f, lastResonance_);
        tune(std::clamp(cutoff, 0.0f, 1.0f), std::clamp(resonance, 0.0f, 1.0f));
    }
    left = tick(left, channels_[0], variable_);
    right = tick(right, channels_[1], variable_);
    if (cascade_) {
        left = tick(left, second_[0], fixed_);
        right = tick(right, second_[1], fixed_);
    }
}

bool AdiShaperFilter::finishBlock() noexcept {
    bool repaired = false;
    const auto verify = [&](Channel& state) {
        if (!std::isfinite(state.pole) || !std::isfinite(state.band) ||
            !std::isfinite(state.low) || !std::isfinite(state.previousBand)) {
            state = {};
            repaired = true;
        }
    };
    for (auto& state : channels_) verify(state);
    if (cascade_) {
        for (auto& state : second_) verify(state);
    }
    return repaired;
}

} // namespace adi::dsp
