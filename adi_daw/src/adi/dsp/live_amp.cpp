// SPDX-License-Identifier: GPL-3.0-or-later
#include "adi/dsp/live_amp.hpp"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace adi::dsp {
namespace {
double settle(double x) noexcept { return std::abs(x) < 1e-30 ? 0 : x; }
double db(double v) noexcept { return std::pow(10.0, v / 20); }
}  // namespace

LiveAmp::LiveAmp() noexcept {
    for (std::size_t i = 0; i < Count; ++i) {
        p_[i] = ranges[i].initial;
    }
    prepare(48000);
}

void LiveAmp::prepare(double sampleRate) noexcept {
    rate_ = std::isfinite(sampleRate) && sampleRate > 0 ? sampleRate : 48000;
    state_ = {};
    dcIn_ = dcOut_ = {};
    dcPole_ = std::exp(-2 * std::numbers::pi * 5 / rate_);
    for (auto* g : {&pre_, &power_, &output_, &wet_}) {
        g->prepare(rate_);
    }
    update();
}

void LiveAmp::set(Param p, double v) noexcept {
    const int index = static_cast<int>(p);
    if (index < 0 || index >= static_cast<int>(Count) || !std::isfinite(v)) {
        return;
    }
    const auto& r = ranges[static_cast<std::size_t>(index)];
    const bool whole = index == Type || index == Dual;
    const bool wasDual = p_[Dual] >= 0.5;
    p_[static_cast<std::size_t>(index)] = std::clamp(whole ? std::round(v) : v, r.low, r.high);
    if (index == Dual && !wasDual && p_[Dual] >= 0.5) {
        // The right amp sat still while Mono ran the left alone: it starts
        // from the left's state, so a switch to Dual does not replay its old
        // state as a thump.
        state_[1] = state_[0];
        dcIn_[1] = dcIn_[0];
        dcOut_[1] = dcOut_[0];
    }
    update();
}

void LiveAmp::update() noexcept {
    const auto& voice = kVoices[static_cast<std::size_t>(p_[Type])];
    const double top = 0.45 * rate_;
    const double q = 1 / std::sqrt(2.0);
    design_[0] = highPass(rate_, std::min(voice.lowCutHz, top), q);
    design_[1] = highPass(rate_, std::min(voice.lowCutHz, top), q);
    design_[2] = peaking(rate_, std::min(voice.peakHz, top), 0.7, voice.peakDb);
    design_[3] = lowShelf(rate_, 100, (p_[Bass] - 5) * 3);
    design_[4] = peaking(rate_, std::min(700.0, top), 0.7, (p_[Middle] - 5) * 3);
    design_[5] = highShelf(rate_, std::min(3000.0, top), (p_[Treble] - 5) * 3);
    design_[6] = highShelf(rate_, std::min(5000.0, top), (p_[Presence] - 5) * 2);
    gains_.pre = db(voice.preGainDb + (p_[Gain] - 5) * 4);
    gains_.bias = voice.bias;
    gains_.power = voice.drive * db((p_[Volume] - 5) * 3);
    gains_.output = db(p_[Output]);
    gains_.wet = p_[DryWet] / 100;
    pre_.set(gains_.pre);
    power_.set(gains_.power);
    output_.set(gains_.output);
    wet_.set(gains_.wet);
}

double LiveAmp::run(std::size_t f, std::size_t c, double x) noexcept {
    const auto& k = design_[f];
    auto& s = state_[c][f];
    const double y = k.b0 * x + k.b1 * s.x1 + k.b2 * s.x2 - k.a1 * s.y1 - k.a2 * s.y2;
    s.x2 = s.x1;
    s.x1 = x;
    s.y2 = settle(s.y1);
    s.y1 = settle(y);
    return y;
}

void LiveAmp::process(const float* inL, const float* inR, float* outL, float* outR, std::size_t frames) noexcept {
    const bool dual = p_[Dual] >= 0.5;
    const double b = gains_.bias, offset = std::tanh(b);
    float* out[2] = {outL, outR};
    for (std::size_t i = 0; i < frames; ++i) {
        // Both inputs are read before either output is written (a host may
        // hand an output plane that is an input plane).
        const double l = inL && std::isfinite(inL[i]) ? inL[i] : 0.0, r = inR && std::isfinite(inR[i]) ? inR[i] : 0.0;
        const double dry[2] = {l, r};
        const double g = pre_.next(), d = power_.next(), level = output_.next(), mix = wet_.next();
        const double feed[2] = {dual ? l : 0.5 * (l + r), r};
        double wet[2] = {0, 0};
        for (std::size_t c = 0; c < (dual ? 2u : 1u); ++c) {
            double y = run(2, c, run(1, c, run(0, c, feed[c])));
            // The preamp, asymmetric, then its DC blocked.
            const double e = std::tanh(g * y + b) - offset;
            const double blocked = e - dcIn_[c] + dcPole_ * dcOut_[c];
            dcIn_[c] = e;
            dcOut_[c] = settle(blocked);
            y = run(5, c, run(4, c, run(3, c, blocked)));
            // The power stage, then presence.
            y = run(6, c, std::tanh(d * y) / d);
            wet[c] = level * y;
        }
        if (!dual) {
            wet[1] = wet[0];
        }
        for (std::size_t c = 0; c < 2; ++c) {
            if (out[c]) {
                out[c][i] = static_cast<float>(dry[c] + (wet[c] - dry[c]) * mix);
            }
        }
    }
}

}  // namespace adi::dsp
