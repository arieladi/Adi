// SPDX-License-Identifier: GPL-3.0-or-later
#include "adi/dsp/drum_buss.hpp"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace adi::dsp {
namespace {
double settle(double x) noexcept { return std::abs(x) < 1e-30 ? 0 : x; }
double level(double db) noexcept { return std::pow(10.0, db / 20); }
double coefficient(double seconds, double rate) noexcept { return std::exp(-1 / (seconds * rate)); }
}  // namespace

double DrumBuss::curve(int type, double x) noexcept {
    switch (type) {
    case Soft:
        return std::tanh(x);
    case Medium:
        return x / (1 + std::abs(x));
    default: {
        // x - x^3 / 3 has slope 1 at 0 and peaks at +-2/3 for |x| = 1.
        const double c = std::clamp(x, -1.0, 1.0);
        return c - c * c * c / 3;
    }
    }
}

double DrumBuss::compressorGainDb(double levelDb) noexcept {
    return levelDb > kThresholdDb ? (kThresholdDb - levelDb) * (1 - 1 / kRatio) : 0.0;
}

DrumBuss::DrumBuss() noexcept {
    for (std::size_t i = 0; i < Count; ++i) {
        p_[i] = ranges[i].initial;
    }
    prepare(48000);
}

void DrumBuss::prepare(double sampleRate) noexcept {
    rate_ = std::isfinite(sampleRate) && sampleRate > 0 ? sampleRate : 48000;
    dampState_ = crunchState_ = boomState_ = {};
    fast_ = slow_ = {};
    compEnv_ = 0;
    compAttack_ = coefficient(0.001, rate_);
    compRelease_ = coefficient(0.100, rate_);
    fastA_ = coefficient(0.001, rate_);
    slowA_ = coefficient(0.050, rate_);
    update();
}

void DrumBuss::set(Param p, double v) noexcept {
    const int index = static_cast<int>(p);
    if (index < 0 || index >= static_cast<int>(Count) || !std::isfinite(v)) {
        return;
    }
    const auto& r = ranges[static_cast<std::size_t>(index)];
    const bool whole = index == Comp || index == Type;
    p_[static_cast<std::size_t>(index)] = std::clamp(whole ? std::round(v) : v, r.low, r.high);
    update();
}

void DrumBuss::update() noexcept {
    const double top = 0.45 * rate_;
    damp_ = lowPass(rate_, std::min(p_[Damp], top), 1 / std::sqrt(2.0));
    crunchBand_ = highPass(rate_, std::min(1000.0, top), 1 / std::sqrt(2.0));
    // A constant-peak band pass (RBJ) at Boom Freq: 0 dB at its centre.
    const double q = 2 + 28 * p_[BoomDecay] / 100, w = 2 * std::numbers::pi * p_[BoomFreq] / rate_;
    const double alpha = std::sin(w) / (2 * q), a0 = 1 + alpha;
    boom_ = {alpha / a0, 0, -alpha / a0, -2 * std::cos(w) / a0, (1 - alpha) / a0};
    trim_ = level(p_[Trim]);
    drive_ = level(24 * p_[Drive] / 100);
    crunch_ = p_[Crunch] / 100;
    transients_ = p_[Transients] / 100;
    boomGain_ = 4 * p_[Boom] / 100;
    output_ = level(p_[Output]);
    wet_ = p_[DryWet] / 100;
}

double DrumBuss::run(const Biquad& b, State& s, double x) noexcept {
    const double y = b.b0 * x + b.b1 * s.x1 + b.b2 * s.x2 - b.a1 * s.y1 - b.a2 * s.y2;
    s.x2 = s.x1;
    s.x1 = x;
    s.y2 = settle(s.y1);
    s.y1 = settle(y);
    return y;
}

void DrumBuss::process(const float* inL, const float* inR, float* outL, float* outR, std::size_t frames) noexcept {
    const bool comp = p_[Comp] >= 0.5;
    const int type = static_cast<int>(p_[Type]);
    float* out[2] = {outL, outR};
    for (std::size_t i = 0; i < frames; ++i) {
        // Both inputs are read before either output is written (a host may
        // hand an output plane that is an input plane).
        const double dry[2] = {inL && std::isfinite(inL[i]) ? inL[i] : 0.0, inR && std::isfinite(inR[i]) ? inR[i] : 0.0};
        double x[2] = {trim_ * dry[0], trim_ * dry[1]};
        double gain = 1;
        if (comp) {
            const double peak = std::max(std::abs(x[0]), std::abs(x[1]));
            compEnv_ = settle(peak + (peak > compEnv_ ? compAttack_ : compRelease_) * (compEnv_ - peak));
            gain = compEnv_ > 0 ? level(compressorGainDb(20 * std::log10(compEnv_))) : 1.0;
        }
        for (std::size_t c = 0; c < 2; ++c) {
            const double in = gain * x[c];
            double y = curve(type, drive_ * in) / drive_;
            const double band = run(crunchBand_, crunchState_[c], y);
            // Crunch adds only the distortion of the high band (no slope at
            // 0, so a quiet signal is untouched): band - tanh(4 band) / 4.
            y += 4 * crunch_ * (band - std::tanh(4 * band) / 4);
            y = run(damp_, dampState_[c], y);
            // Transients: the fast envelope over the slow, as a gain.
            const double a = std::abs(in);
            fast_[c] = settle(a + fastA_ * (fast_[c] - a));
            slow_[c] = settle(a + slowA_ * (slow_[c] - a));
            if (transients_ != 0 && slow_[c] > 1e-9) {
                y *= std::clamp(1 + transients_ * (fast_[c] - slow_[c]) / slow_[c], 0.25, 4.0);
            }
            y += boomGain_ * run(boom_, boomState_[c], in);
            const double wet = output_ * y;
            if (out[c]) {
                out[c][i] = static_cast<float>(dry[c] + (wet - dry[c]) * wet_);
            }
        }
    }
}

}  // namespace adi::dsp
