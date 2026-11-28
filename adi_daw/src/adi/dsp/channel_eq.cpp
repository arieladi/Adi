// SPDX-License-Identifier: GPL-3.0-or-later
#include "adi/dsp/channel_eq.hpp"

#include <algorithm>
#include <cmath>

namespace adi::dsp {
namespace {
double settle(double x) noexcept { return std::abs(x) < 1e-30 ? 0 : x; }
}  // namespace

ChannelEq::ChannelEq() noexcept {
    for (std::size_t i = 0; i < Count; ++i) {
        p_[i] = ranges[i].initial;
    }
    prepare(48000);
}

void ChannelEq::prepare(double sampleRate) noexcept {
    rate_ = std::isfinite(sampleRate) && sampleRate > 0 ? sampleRate : 48000;
    state_ = {};
    output_.prepare(rate_);
    update();
}

void ChannelEq::set(Param p, double v) noexcept {
    const int index = static_cast<int>(p);
    if (index < 0 || index >= static_cast<int>(Count) || !std::isfinite(v)) {
        return;
    }
    const auto& r = ranges[static_cast<std::size_t>(index)];
    p_[static_cast<std::size_t>(index)] = std::clamp(v, r.low, r.high);
    update();
}

void ChannelEq::update() noexcept {
    // The bands' corners kept under the Nyquist frequency at any rate.
    const double top = 0.45 * rate_;
    band_[Cut] = p_[LowCut] >= 0.5 ? highPass(rate_, kLowCutHz, 1 / std::sqrt(2.0)) : Biquad{};
    band_[LowShelf] = lowShelf(rate_, kLowHz, p_[Low]);
    band_[Bell] = peaking(rate_, std::min(p_[MidFreq], top), kMidQ, p_[Mid]);
    band_[HighShelf] = highShelf(rate_, std::min(kHighHz, top), p_[High]);
    output_.set(std::pow(10.0, p_[Output] / 20));
}

double ChannelEq::responseDb(double hz) const noexcept {
    double db = p_[Output];
    for (const auto& b : band_) {
        db += magnitudeDb(b, rate_, hz);
    }
    return db;
}

void ChannelEq::process(const float* inL, const float* inR, float* outL, float* outR, std::size_t frames) noexcept {
    const float* in[2] = {inL, inR};
    float* out[2] = {outL, outR};
    for (std::size_t i = 0; i < frames; ++i) {
        const double gain = output_.next();
        // Both inputs are read before either output is written (a host may
        // hand an output plane that is an input plane).
        double x[2];
        for (std::size_t c = 0; c < 2; ++c) {
            x[c] = in[c] && std::isfinite(in[c][i]) ? in[c][i] : 0.0;
        }
        for (std::size_t c = 0; c < 2; ++c) {
            double v = x[c];
            for (std::size_t b = 0; b < Bands; ++b) {
                const auto& k = band_[b];
                auto& s = state_[c][b];
                const double y = k.b0 * v + k.b1 * s.x1 + k.b2 * s.x2 - k.a1 * s.y1 - k.a2 * s.y2;
                s.x2 = s.x1;
                s.x1 = v;
                s.y2 = settle(s.y1);
                s.y1 = settle(y);
                v = y;
            }
            if (out[c]) {
                out[c][i] = static_cast<float>(v * gain);
            }
        }
    }
}

}  // namespace adi::dsp
