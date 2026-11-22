// SPDX-License-Identifier: GPL-3.0-or-later
#include "adi/dsp/live_pedal.hpp"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace adi::dsp {
namespace {
double settle(double x) noexcept { return std::abs(x) < 1e-30 ? 0 : x; }
}  // namespace

double LivePedal::curve(int voice, double x) noexcept {
    switch (voice) {
    case Overdrive:
        return std::tanh(x);
    case Distortion:
        return x / (1 + std::abs(x));
    default:
        // Fuzz: tanh(x + x^3), steep past its knee.
        return std::tanh(x + x * x * x);
    }
}

double LivePedal::slope(int voice, double x) noexcept {
    switch (voice) {
    case Overdrive: {
        const double t = std::tanh(x);
        return 1 - t * t;
    }
    case Distortion: {
        const double d = 1 + std::abs(x);
        return 1 / (d * d);
    }
    default: {
        const double t = std::tanh(x + x * x * x);
        return (1 - t * t) * (1 + 3 * x * x);
    }
    }
}

LivePedal::LivePedal() noexcept {
    for (std::size_t i = 0; i < Count; ++i) {
        p_[i] = ranges[i].initial;
    }
    prepare(48000);
}

void LivePedal::prepare(double sampleRate) noexcept {
    rate_ = std::isfinite(sampleRate) && sampleRate > 0 ? sampleRate : 48000;
    state_ = {};
    dcIn_ = dcOut_ = {};
    dcPole_ = std::exp(-2 * std::numbers::pi * 5 / rate_);
    update();
}

void LivePedal::set(Param p, double v) noexcept {
    const int index = static_cast<int>(p);
    if (index < 0 || index >= static_cast<int>(Count) || !std::isfinite(v)) {
        return;
    }
    const auto& r = ranges[static_cast<std::size_t>(index)];
    const bool whole = index == Type || index == Sub || index == MidFreq;
    p_[static_cast<std::size_t>(index)] = std::clamp(whole ? std::round(v) : v, r.low, r.high);
    update();
}

void LivePedal::update() noexcept {
    const auto voice = static_cast<int>(p_[Type]);
    const double top = 0.45 * rate_;
    design_[0] = highPass(rate_, p_[Sub] >= 0.5 ? 30.0 : 100.0, 1 / std::sqrt(2.0));
    design_[1] = lowShelf(rate_, 100, p_[Bass] / 100 * 15);
    design_[2] = peaking(rate_, std::min(kMidHz[static_cast<std::size_t>(p_[MidFreq])], top), 1, p_[Mid] / 100 * 15);
    design_[3] = highShelf(rate_, std::min(3500.0, top), p_[Treble] / 100 * 15);
    const auto& spec = kVoices[static_cast<std::size_t>(voice)];
    gain_ = std::pow(10.0, (spec.baseDb + 30 * p_[Gain] / 100) / 20);
    bias_ = spec.bias;
    offset_ = curve(voice, bias_);
    output_ = std::pow(10.0, p_[Output] / 20);
    wet_ = p_[DryWet] / 100;
}

double LivePedal::run(std::size_t f, std::size_t c, double x) noexcept {
    const auto& k = design_[f];
    auto& s = state_[c][f];
    const double y = k.b0 * x + k.b1 * s.x1 + k.b2 * s.x2 - k.a1 * s.y1 - k.a2 * s.y2;
    s.x2 = s.x1;
    s.x1 = x;
    s.y2 = settle(s.y1);
    s.y1 = settle(y);
    return y;
}

void LivePedal::process(const float* inL, const float* inR, float* outL, float* outR, std::size_t frames) noexcept {
    const auto voice = static_cast<int>(p_[Type]);
    float* out[2] = {outL, outR};
    for (std::size_t i = 0; i < frames; ++i) {
        // Both inputs are read before either output is written (a host may
        // hand an output plane that is an input plane).
        const double dry[2] = {inL && std::isfinite(inL[i]) ? inL[i] : 0.0, inR && std::isfinite(inR[i]) ? inR[i] : 0.0};
        for (std::size_t c = 0; c < 2; ++c) {
            const double x = run(0, c, dry[c]);
            const double e = curve(voice, gain_ * x + bias_) - offset_;
            const double blocked = e - dcIn_[c] + dcPole_ * dcOut_[c];
            dcIn_[c] = e;
            dcOut_[c] = settle(blocked);
            const double y = run(3, c, run(2, c, run(1, c, blocked)));
            const double wet = output_ * y;
            if (out[c]) {
                out[c][i] = static_cast<float>(dry[c] + (wet - dry[c]) * wet_);
            }
        }
    }
}

}  // namespace adi::dsp
