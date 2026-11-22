// SPDX-License-Identifier: GPL-3.0-or-later
#include "adi/dsp/dynamic_tube.hpp"

#include <algorithm>
#include <cmath>

namespace adi::dsp {
namespace {
double settle(double x) noexcept { return std::abs(x) < 1e-30 ? 0 : x; }
double level(double db) noexcept { return std::pow(10.0, db / 20); }
}  // namespace

double DynamicTube::curve(int tube, double x) noexcept {
    switch (tube) {
    case A:
        return std::tanh(x);
    case B:
        return x / std::sqrt(1 + 4 * x * x);
    default: {
        // A clip at +-1 softened at its corners: x / (1 + |x|^4)^(1/4).
        const double x2 = x * x;
        return x / std::sqrt(std::sqrt(1 + x2 * x2));
    }
    }
}

double DynamicTube::slope(int tube, double x) noexcept {
    switch (tube) {
    case A: {
        const double t = std::tanh(x);
        return 1 - t * t;
    }
    case B: {
        const double d = 1 + 4 * x * x;
        return 1 / (d * std::sqrt(d));
    }
    default: {
        const double x4 = x * x * x * x;
        return 1 / std::pow(1 + x4, 1.25);
    }
    }
}

DynamicTube::DynamicTube() noexcept {
    for (std::size_t i = 0; i < Count; ++i) {
        p_[i] = ranges[i].initial;
    }
    prepare(48000);
}

void DynamicTube::prepare(double sampleRate) noexcept {
    rate_ = std::isfinite(sampleRate) && sampleRate > 0 ? sampleRate : 48000;
    preState_ = postState_ = {};
    env_ = {};
    update();
}

void DynamicTube::set(Param p, double v) noexcept {
    const int index = static_cast<int>(p);
    if (index < 0 || index >= static_cast<int>(Count) || !std::isfinite(v)) {
        return;
    }
    const auto& r = ranges[static_cast<std::size_t>(index)];
    p_[static_cast<std::size_t>(index)] = std::clamp(index == Tube ? std::round(v) : v, r.low, r.high);
    update();
}

void DynamicTube::update() noexcept {
    // Tone: a 1 kHz high shelf of Tone x 12 dB before, its inverse after.
    const double tilt = p_[Tone] / 100 * 12;
    pre_ = highShelf(rate_, std::min(1000.0, 0.45 * rate_), tilt);
    post_ = highShelf(rate_, std::min(1000.0, 0.45 * rate_), -tilt);
    drive_ = level(p_[Drive]);
    bias_ = 0.5 * p_[Bias] / 100;
    envelope_ = p_[Envelope] / 100;
    attack_ = std::exp(-1 / (p_[Attack] / 1000 * rate_));
    release_ = std::exp(-1 / (p_[Release] / 1000 * rate_));
    output_ = level(p_[Output]);
    wet_ = p_[DryWet] / 100;
}

double DynamicTube::run(const Biquad& b, State& s, double x) noexcept {
    const double y = b.b0 * x + b.b1 * s.x1 + b.b2 * s.x2 - b.a1 * s.y1 - b.a2 * s.y2;
    s.x2 = s.x1;
    s.x1 = x;
    s.y2 = settle(s.y1);
    s.y1 = settle(y);
    return y;
}

void DynamicTube::process(const float* inL, const float* inR, float* outL, float* outR, std::size_t frames) noexcept {
    const int tube = static_cast<int>(p_[Tube]);
    float* out[2] = {outL, outR};
    for (std::size_t i = 0; i < frames; ++i) {
        // Both inputs are read before either output is written (a host may
        // hand an output plane that is an input plane).
        const double dry[2] = {inL && std::isfinite(inL[i]) ? inL[i] : 0.0, inR && std::isfinite(inR[i]) ? inR[i] : 0.0};
        for (std::size_t c = 0; c < 2; ++c) {
            const double x = run(pre_, preState_[c], dry[c]);
            const double a = std::abs(drive_ * x);
            env_[c] = settle(a + (a > env_[c] ? attack_ : release_) * (env_[c] - a));
            // The operating point: Bias, moved by the envelope, kept on the
            // curve's useful span.
            const double b = std::clamp(bias_ + envelope_ * 0.5 * std::min(env_[c], 1.0), -0.9, 0.9);
            const double y = (curve(tube, drive_ * x + b) - curve(tube, b)) / (drive_ * slope(tube, b));
            const double wet = output_ * run(post_, postState_[c], y);
            if (out[c]) {
                out[c][i] = static_cast<float>(dry[c] + (wet - dry[c]) * wet_);
            }
        }
    }
}

}  // namespace adi::dsp
