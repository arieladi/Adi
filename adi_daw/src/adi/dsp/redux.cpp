// SPDX-License-Identifier: MIT
// Original ADI sample-and-hold decimator and companding quantizer.
#include "adi/dsp/redux.hpp"
#include <algorithm>
#include <cmath>
#include <numbers>

namespace adi::dsp {
void Redux::prepare(double sr) noexcept {
    sampleRate_ = std::isfinite(sr) && sr > 0 ? sr : 48000;
    channels_ = {};
    processing_ = false;
    rampSamples_ = static_cast<std::size_t>(std::max(1., std::round(sampleRate_ * 0.005)));
    bitsCurrent_ = bits_;
    mixCurrent_ = mix_;
    bitsRemaining_ = mixRemaining_ = 0;
    channels_[0].random = 0x12345678u;
    channels_[1].random = 0x87654321u;
    coefficients();
}
void Redux::coefficients() noexcept {
    const double nyquist = std::min(rate_, sampleRate_) * 0.5;
    const auto coefficient = [this](double hz) {
        return 1 - std::exp(-2 * std::numbers::pi * std::clamp(hz, 0.01, sampleRate_ * 0.49) /
                            sampleRate_);
    };
    preCoefficient_ = coefficient(nyquist);
    postCoefficient_ = coefficient(nyquist * std::exp2(octave_));
}
void Redux::setRate(double v) noexcept {
    if (std::isfinite(v)) {
        rate_ = std::clamp(v, 20., 48000.);
        coefficients();
    }
}
void Redux::setJitter(double v) noexcept {
    if (std::isfinite(v))
        jitter_ = std::clamp(v, 0., 100.) / 100;
}
void Redux::setBits(double v) noexcept {
    if (!std::isfinite(v))
        return;
    const double target = std::clamp(v, 1., 24.);
    if (target == bits_)
        return;
    bits_ = target;
    if (!processing_) {
        bitsCurrent_ = bits_;
        return;
    }
    bitsRemaining_ = rampSamples_;
    bitsStep_ = (bits_ - bitsCurrent_) / static_cast<double>(rampSamples_);
}
void Redux::setShape(double v) noexcept {
    if (std::isfinite(v))
        shape_ = std::clamp(v, 0., 100.) / 100;
}
void Redux::setPre(double v) noexcept {
    if (std::isfinite(v))
        pre_ = v >= 0.5;
}
void Redux::setPost(double v) noexcept {
    if (std::isfinite(v))
        post_ = v >= 0.5;
}
void Redux::setOctave(double v) noexcept {
    if (std::isfinite(v)) {
        octave_ = std::clamp(v, -4., 4.);
        coefficients();
    }
}
void Redux::setDcShift(double v) noexcept {
    if (std::isfinite(v))
        dc_ = v >= 0.5;
}
void Redux::setMix(double v) noexcept {
    if (!std::isfinite(v))
        return;
    const double target = std::clamp(v, 0., 100.) / 100;
    if (target == mix_)
        return;
    mix_ = target;
    if (!processing_) {
        mixCurrent_ = mix_;
        return;
    }
    mixRemaining_ = rampSamples_;
    mixStep_ = (mix_ - mixCurrent_) / static_cast<double>(rampSamples_);
}
void Redux::advanceRamps() noexcept {
    if (bitsRemaining_ && --bitsRemaining_ == 0)
        bitsCurrent_ = bits_;
    else if (bitsRemaining_)
        bitsCurrent_ += bitsStep_;
    if (mixRemaining_ && --mixRemaining_ == 0)
        mixCurrent_ = mix_;
    else if (mixRemaining_)
        mixCurrent_ += mixStep_;
}
double Redux::sample(double input, Channel &c) noexcept {
    const double dry = input;
    c.pre1 += preCoefficient_ * (input - c.pre1);
    c.pre2 += preCoefficient_ * (c.pre1 - c.pre2);
    if (pre_)
        input = c.pre2;
    // Carry fractional time across every sample and every host block. No
    // anti-aliasing here: the deliberately folded spectrum is the effect.
    if (c.remaining <= 0) {
        c.held = input;
        c.random ^= c.random << 13;
        c.random ^= c.random >> 17;
        c.random ^= c.random << 5;
        const double noise = 2 * static_cast<double>(c.random) / 4294967295. - 1;
        const double period = sampleRate_ / std::min(rate_, sampleRate_);
        c.remaining += std::max(1., period * (1 + 0.95 * jitter_ * noise));
    }
    c.remaining -= 1;
    const double exponent = 1 / (1 + 3 * shape_);
    const double compressed =
        std::copysign(std::pow(std::min(1., std::abs(c.held)), exponent), c.held);
    const auto quantize = [&](double bits) {
        const double step = std::exp2(1 - bits);
        const double shifted = compressed + (dc_ ? step * 0.5 : 0);
        // Preserve the existing integer-depth mid-rise quantizer, including DC Shift.
        double q =
            shifted == 0 ? 0 : (std::floor(std::clamp(shifted, -1., 1.) / step) + 0.5) * step;
        q = std::clamp(q, -1 + step * 0.5, 1 - step * 0.5);
        return std::copysign(std::pow(std::abs(q), 1 / exponent), q);
    };
    const double lower = std::floor(bitsCurrent_);
    const double fraction = bitsCurrent_ - lower;
    const double a = quantize(lower);
    // Continuous parameter motion without moving quantizer thresholds through
    // a stationary input. Integer depths retain their original response.
    double wet = a;
    if (fraction > 0)
        wet += fraction * (quantize(std::min(24., lower + 1)) - a);
    c.post1 += postCoefficient_ * (wet - c.post1);
    c.post2 += postCoefficient_ * (c.post1 - c.post2);
    if (post_)
        wet = c.post2;
    return dry + (wet - dry) * mixCurrent_;
}
void Redux::process(const float *left, const float *right, float *outLeft, float *outRight,
                    std::size_t n) noexcept {
    if (n)
        processing_ = true;
    for (std::size_t i = 0; i < n; ++i) {
        advanceRamps();
        outLeft[i] = static_cast<float>(sample(left[i], channels_[0]));
        outRight[i] = static_cast<float>(sample(right[i], channels_[1]));
    }
}
} // namespace adi::dsp
