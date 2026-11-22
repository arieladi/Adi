// SPDX-License-Identifier: GPL-3.0-or-later
#include "adi/dsp/live_cabinet.hpp"

#include <algorithm>
#include <cmath>

namespace adi::dsp {
namespace {
double settle(double x) noexcept { return std::abs(x) < 1e-30 ? 0 : x; }
}  // namespace

LiveCabinet::LiveCabinet() noexcept {
    for (std::size_t i = 0; i < Count; ++i) {
        p_[i] = ranges[i].initial;
    }
    prepare(48000);
}

void LiveCabinet::prepare(double sampleRate) noexcept {
    rate_ = std::isfinite(sampleRate) && sampleRate > 0 ? sampleRate : 48000;
    state_ = {};
    output_.prepare(rate_);
    wet_.prepare(rate_);
    update();
}

void LiveCabinet::set(Param p, double v) noexcept {
    const int index = static_cast<int>(p);
    if (index < 0 || index >= static_cast<int>(Count) || !std::isfinite(v)) {
        return;
    }
    const auto& r = ranges[static_cast<std::size_t>(index)];
    const bool whole = index == Cabinet || index == Microphone || index == MicType || index == Dual;
    const bool wasDual = p_[Dual] >= 0.5;
    p_[static_cast<std::size_t>(index)] = std::clamp(whole ? std::round(v) : v, r.low, r.high);
    if (index == Dual && !wasDual && p_[Dual] >= 0.5) {
        // The right cabinet sat still while Mono ran the left alone: it
        // starts from the left's state, not its own from before.
        state_[1] = state_[0];
    }
    update();
}

void LiveCabinet::update() noexcept {
    const auto& box = kCabinets[static_cast<std::size_t>(p_[Cabinet])];
    const auto mic = static_cast<int>(p_[Microphone]);
    const double top = 0.45 * rate_, q = 1 / std::sqrt(2.0);
    const double cut = std::min(mic == Far ? box.highCutHz / 2 : box.highCutHz, top);
    design_[0] = highPass(rate_, std::min(box.lowCutHz, top), q);
    design_[1] = peaking(rate_, std::min(box.resonanceHz, top), 2, box.resonanceDb);
    design_[2] = peaking(rate_, std::min(box.coneHz, top), 1.5, box.coneDb);
    design_[3] = lowPass(rate_, cut, q);
    design_[4] = lowPass(rate_, cut, q);
    design_[5] = highShelf(rate_, std::min(3000.0, top), mic == NearOff ? -6 : 0);
    design_[6] = lowShelf(rate_, 200, mic == Far ? -3 : 0);
    design_[7] = highShelf(rate_, std::min(8000.0, top), p_[MicType] < 0.5 ? 2 : 0);
    output_.set(std::pow(10.0, p_[Output] / 20));
    wet_.set(p_[DryWet] / 100);
}

void LiveCabinet::process(const float* inL, const float* inR, float* outL, float* outR, std::size_t frames) noexcept {
    const bool dual = p_[Dual] >= 0.5;
    float* out[2] = {outL, outR};
    for (std::size_t i = 0; i < frames; ++i) {
        // Both inputs are read before either output is written (a host may
        // hand an output plane that is an input plane).
        const double l = inL && std::isfinite(inL[i]) ? inL[i] : 0.0, r = inR && std::isfinite(inR[i]) ? inR[i] : 0.0;
        const double dry[2] = {l, r};
        const double level = output_.next(), mix = wet_.next();
        double wet[2] = {dual ? l : 0.5 * (l + r), r};
        for (std::size_t c = 0; c < (dual ? 2u : 1u); ++c) {
            double x = wet[c];
            for (std::size_t k = 0; k < kSections; ++k) {
                const auto& b = design_[k];
                auto& s = state_[c][k];
                const double y = b.b0 * x + b.b1 * s.x1 + b.b2 * s.x2 - b.a1 * s.y1 - b.a2 * s.y2;
                s.x2 = s.x1;
                s.x1 = x;
                s.y2 = settle(s.y1);
                s.y1 = settle(y);
                x = y;
            }
            wet[c] = level * x;
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
