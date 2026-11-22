// SPDX-License-Identifier: GPL-3.0-or-later
#include "adi/dsp/eq_three.hpp"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace adi::dsp {
namespace {
double settle(double x) noexcept { return std::abs(x) < 1e-30 ? 0 : x; }

/// A Linkwitz-Riley low or high pass at `hz`: LR4 is a second-order
/// Butterworth twice; LR8 is a fourth-order Butterworth (Q 0.5412, 1.3066)
/// twice.
template <class Design>
void linkwitzRiley(std::array<Biquad, 4>& out, std::size_t& count, bool steep, Design design) {
    if (steep) {
        const double q1 = 1 / (2 * std::cos(std::numbers::pi / 8)), q2 = 1 / (2 * std::cos(3 * std::numbers::pi / 8));
        out = {design(q1), design(q2), design(q1), design(q2)};
        count = 4;
    } else {
        const double q = 1 / std::sqrt(2.0);
        out = {design(q), design(q), Biquad{}, Biquad{}};
        count = 2;
    }
}
}  // namespace

double EqThree::Chain::run(std::size_t channel, double x) noexcept {
    for (std::size_t i = 0; i < count; ++i) {
        const auto& k = sections[i];
        auto& s = state[channel][i];
        const double y = k.b0 * x + k.b1 * s.x1 + k.b2 * s.x2 - k.a1 * s.y1 - k.a2 * s.y2;
        s.x2 = s.x1;
        s.x1 = x;
        s.y2 = settle(s.y1);
        s.y1 = settle(y);
        x = y;
    }
    return x;
}

EqThree::EqThree() noexcept {
    for (std::size_t i = 0; i < Count; ++i) {
        p_[i] = ranges[i].initial;
    }
    prepare(48000);
}

void EqThree::prepare(double sampleRate) noexcept {
    rate_ = std::isfinite(sampleRate) && sampleRate > 0 ? sampleRate : 48000;
    for (auto* chain : {&lowLp_, &lowHp_, &midLp_, &midHp_, &allpassLp_, &allpassHp_}) {
        chain->state = {};
    }
    update();
}

void EqThree::set(Param p, double v) noexcept {
    const int index = static_cast<int>(p);
    if (index < 0 || index >= static_cast<int>(Count) || !std::isfinite(v)) {
        return;
    }
    const auto& r = ranges[static_cast<std::size_t>(index)];
    p_[static_cast<std::size_t>(index)] = std::clamp(v, r.low, r.high);
    update();
}

void EqThree::update() noexcept {
    const bool steep = p_[Slope] >= 0.5;
    const double top = 0.45 * rate_;
    const double low = std::min(p_[FreqLow], top), high = std::min(p_[FreqHigh], top);
    linkwitzRiley(lowLp_.sections, lowLp_.count, steep, [&](double q) { return lowPass(rate_, low, q); });
    linkwitzRiley(lowHp_.sections, lowHp_.count, steep, [&](double q) { return highPass(rate_, low, q); });
    linkwitzRiley(midLp_.sections, midLp_.count, steep, [&](double q) { return lowPass(rate_, high, q); });
    linkwitzRiley(midHp_.sections, midHp_.count, steep, [&](double q) { return highPass(rate_, high, q); });
    linkwitzRiley(allpassLp_.sections, allpassLp_.count, steep, [&](double q) { return lowPass(rate_, high, q); });
    linkwitzRiley(allpassHp_.sections, allpassHp_.count, steep, [&](double q) { return highPass(rate_, high, q); });
    const auto level = [](double db, double on) { return on < 0.5 || db <= -70 ? 0.0 : std::pow(10.0, db / 20); };
    gain_ = {level(p_[Low], p_[LowOn]), level(p_[Mid], p_[MidOn]), level(p_[High], p_[HighOn])};
}

void EqThree::process(const float* inL, const float* inR, float* outL, float* outR, std::size_t frames) noexcept {
    const float* in[2] = {inL, inR};
    float* out[2] = {outL, outR};
    for (std::size_t i = 0; i < frames; ++i) {
        // Both inputs are read before either output is written (a host may
        // hand an output plane that is an input plane).
        double x[2];
        for (std::size_t c = 0; c < 2; ++c) {
            x[c] = in[c] && std::isfinite(in[c][i]) ? in[c][i] : 0.0;
        }
        for (std::size_t c = 0; c < 2; ++c) {
            // The low band, then the second crossover's allpass on it.
            const double lowBand = lowLp_.run(c, x[c]);
            const double aligned = allpassLp_.run(c, lowBand) + allpassHp_.run(c, lowBand);
            const double rest = lowHp_.run(c, x[c]);
            const double mid = midLp_.run(c, rest), highBand = midHp_.run(c, rest);
            const double y = gain_[0] * aligned + gain_[1] * mid + gain_[2] * highBand;
            if (out[c]) {
                out[c][i] = static_cast<float>(y);
            }
        }
    }
}

}  // namespace adi::dsp
