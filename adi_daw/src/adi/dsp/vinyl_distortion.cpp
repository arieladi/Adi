// SPDX-License-Identifier: GPL-3.0-or-later
#include "adi/dsp/vinyl_distortion.hpp"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace adi::dsp {
namespace {
double settle(double x) noexcept { return std::abs(x) < 1e-30 ? 0 : x; }
}  // namespace

void VinylDistortion::Band::design(double rate, double hz, double width) noexcept {
    const double f = std::min(hz, 0.45 * rate), w = 2 * std::numbers::pi * f / rate;
    const double alpha = std::sin(w) * width / 2, a0 = 1 + alpha;   // Q = 1 / width
    b0 = alpha / a0;
    a1 = -2 * std::cos(w) / a0;
    a2 = (1 - alpha) / a0;
}

double VinylDistortion::Band::run(std::size_t c, double x) noexcept {
    const double y = b0 * x + s1[c];
    s1[c] = settle(s2[c] - a1 * y);
    s2[c] = settle(-b0 * x - a2 * y);
    return y;
}

double VinylDistortion::pinch(double b, double drive, bool soft) noexcept {
    const double g = 1 + 20 * drive, x = g * b;
    const double f = soft ? std::tanh(x) : std::clamp(x, -1.0, 1.0);
    return f / g - b;
}

VinylDistortion::VinylDistortion() noexcept {
    for (std::size_t i = 0; i < Count; ++i) {
        p_[i] = ranges[i].initial;
    }
    prepare(48000);
}

void VinylDistortion::prepare(double sampleRate) noexcept {
    rate_ = std::isfinite(sampleRate) && sampleRate > 0 ? sampleRate : 48000;
    tracing_.s1 = tracing_.s2 = pinch_.s1 = pinch_.s2 = {};
    dcIn_ = dcOut_ = {};
    crackle_ = 0;
    seed_ = 0x2545F491u;
    dcPole_ = std::exp(-2 * std::numbers::pi * 5 / rate_);   // a 5 Hz DC blocker
    crackleDecay_ = std::exp(-1 / (kCrackleMs / 1000 * rate_));
    update();
}

void VinylDistortion::set(Param p, double v) noexcept {
    const int index = static_cast<int>(p);
    if (index < 0 || index >= static_cast<int>(Count) || !std::isfinite(v)) {
        return;
    }
    const auto& r = ranges[static_cast<std::size_t>(index)];
    const bool toggle = index == TracingOn || index == PinchOn || index == Soft || index == Stereo;
    p_[static_cast<std::size_t>(index)] = std::clamp(toggle ? std::round(v) : v, r.low, r.high);
    update();
}

void VinylDistortion::update() noexcept {
    tracing_.design(rate_, p_[TracingFreq], p_[TracingWidth]);
    pinch_.design(rate_, p_[PinchFreq], p_[PinchWidth]);
    crackleChance_ = p_[CrackleDensity] / rate_;
    crackleLevel_ = p_[CrackleVolume] <= -70 ? 0 : std::pow(10.0, p_[CrackleVolume] / 20);
}

double VinylDistortion::random() noexcept {
    seed_ ^= seed_ << 13;
    seed_ ^= seed_ >> 17;
    seed_ ^= seed_ << 5;
    return static_cast<double>(seed_) / 4294967296.0;   // [0, 1)
}

void VinylDistortion::process(const float* inL, const float* inR, float* outL, float* outR, std::size_t frames) noexcept {
    const bool tracingOn = p_[TracingOn] >= 0.5 && p_[TracingDrive] > 0, pinchOn = p_[PinchOn] >= 0.5 && p_[PinchDrive] > 0;
    const bool soft = p_[Soft] >= 0.5, stereo = p_[Stereo] >= 0.5;
    const float* in[2] = {inL, inR};
    float* out[2] = {outL, outR};
    for (std::size_t i = 0; i < frames; ++i) {
        // Both inputs are read before either output is written (a host may
        // hand an output plane that is an input plane).
        std::array<double, 2> x{};
        for (std::size_t c = 0; c < 2; ++c) {
            x[c] = in[c] && std::isfinite(in[c][i]) ? in[c][i] : 0.0;
        }
        // The crackle: a click starts with chance Density / rate a sample.
        // The generator runs whatever the volume, so turning it up does not
        // move the clicks.
        const double roll = random(), size = random();
        crackle_ = settle(crackle_ * crackleDecay_);
        if (roll < crackleChance_) {
            crackle_ = (size < 0.5 ? -1 : 1) * (0.25 + 1.5 * std::abs(size - 0.5));
        }
        for (std::size_t c = 0; c < 2; ++c) {
            // The bands and the blocker run whether or not their distortion
            // is on, so switching one on starts from the signal's own state.
            double y = x[c];
            const double b = tracing_.run(c, x[c]), e = 2 * p_[TracingDrive] * b * b;
            const double blocked = e - dcIn_[c] + dcPole_ * dcOut_[c];
            dcIn_[c] = e;
            dcOut_[c] = settle(blocked);
            if (tracingOn) {
                y += blocked;   // the even product, its DC blocked: 2 Drive b^2
            }
            const double band = pinch_.run(c, x[c]);
            if (pinchOn) {
                const double q = VinylDistortion::pinch(band, p_[PinchDrive], soft);
                y += stereo && c == 1 ? -q : q;
            }
            y += crackleLevel_ * crackle_;
            if (out[c]) {
                out[c][i] = static_cast<float>(y);
            }
        }
    }
}

}  // namespace adi::dsp
