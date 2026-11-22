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
    fastA_ = coefficient(0.0005, rate_);
    slowA_ = coefficient(0.010, rate_);
    releaseA_ = coefficient(0.300, rate_);
    smoothA_ = coefficient(0.0005, rate_);
    crunchPole_ = std::exp(-2 * std::numbers::pi * 20 / rate_);
    crunchDcIn_ = crunchDcOut_ = {};
    shape_ = {1, 1};
    for (auto* g : {&trim_, &drive_, &match_, &crunch_, &transients_, &boomGain_, &output_, &wet_}) {
        g->prepare(rate_);
    }
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
    trim_.set(level(p_[Trim]));
    const double drive = level(24 * p_[Drive] / 100);
    drive_.set(drive);
    match_.set(curve(static_cast<int>(p_[Type]), 0.5) / curve(static_cast<int>(p_[Type]), 0.5 * drive));
    crunch_.set(p_[Crunch] / 100);
    transients_.set(p_[Transients] / 100);
    boomGain_.set(4 * p_[Boom] / 100);
    output_.set(level(p_[Output]));
    wet_.set(p_[DryWet] / 100);
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
        const double trim = trim_.next(), drive = drive_.next(), match = match_.next(), crunch = crunch_.next();
        const double transients = transients_.next(), boom = boomGain_.next(), gainOut = output_.next(), mix = wet_.next();
        double x[2] = {trim * dry[0], trim * dry[1]};
        double gain = 1;
        if (comp) {
            const double peak = std::max(std::abs(x[0]), std::abs(x[1]));
            compEnv_ = settle(peak + (peak > compEnv_ ? compAttack_ : compRelease_) * (compEnv_ - peak));
            gain = compEnv_ > 0 ? level(compressorGainDb(20 * std::log10(compEnv_))) : 1.0;
        }
        for (std::size_t c = 0; c < 2; ++c) {
            const double in = gain * x[c];
            // Drive, matched at -6 dBFS: a peak of 0.5 keeps its level, a
            // quieter signal gains (the curve compresses), a louder one is held.
            double y = curve(type, drive * in) * match;
            // Crunch: the high band's square, its DC removed, an octave up.
            // A sine's square holds no fundamental, so Crunch brightens
            // without lifting the band, and being of the second order it
            // leaves a quiet signal untouched.
            const double band = std::clamp(run(crunchBand_, crunchState_[c], y), -1.0, 1.0), sq = band * band;
            const double even = sq - crunchDcIn_[c] + crunchPole_ * crunchDcOut_[c];
            crunchDcIn_[c] = sq;
            crunchDcOut_[c] = settle(even);
            y += 2 * crunch * even;
            y = run(damp_, dampState_[c], y);
            // Transients: a peak envelope (0.5 ms attack, 300 ms release)
            // against itself lagged by a 10 ms one-pole. A held tone keeps
            // the two together; an onset parts them for about the lag. Their
            // ratio, smoothed over 0.5 ms, is the gain.
            const double a = std::abs(in);
            fast_[c] = settle(a + (a > fast_[c] ? fastA_ : releaseA_) * (fast_[c] - a));
            slow_[c] = settle(fast_[c] + slowA_ * (slow_[c] - fast_[c]));
            const double target = slow_[c] > 1e-9 ? std::clamp(1 + transients * (fast_[c] - slow_[c]) / slow_[c], 0.25, 4.0) : 1.0;
            shape_[c] = target + smoothA_ * (shape_[c] - target);
            if (transients != 0) {
                y *= shape_[c];
            }
            y += boom * run(boom_, boomState_[c], in);
            const double wet = gainOut * y;
            if (out[c]) {
                out[c][i] = static_cast<float>(dry[c] + (wet - dry[c]) * mix);
            }
        }
    }
}

}  // namespace adi::dsp
