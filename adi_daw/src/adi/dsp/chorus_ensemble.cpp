/*
 * Surge XT - a free and open source hybrid synthesizer,
 * built by Surge Synth Team
 *
 * Learn more at https://surge-synthesizer.github.io/
 *
 * Copyright 2018-2024, various authors, as described in the GitHub
 * transaction log.
 *
 * Surge XT is released under the GNU General Public Licence v3
 * or later (GPL-3.0-or-later). The license is found in the "LICENSE"
 * file in the root of this repository, or at
 * https://www.gnu.org/licenses/gpl-3.0.en.html
 *
 * Surge was a commercial product from 2004-2018, copyright and ownership
 * held by Claes Johanson at Vember Audio during that period.
 * Claes made Surge open source in September 2018.
 *
 * All source for Surge XT is available at
 * https://github.com/surge-synthesizer/surge
 */
// Scalar adaptation of ChorusEffect and BBDEnsembleEffect's modulated tap network.
// ADI uses Live's control topology and cubic digital reads, not the BBD circuit.
#include "adi/dsp/chorus_ensemble.hpp"
#include <algorithm>
#include <cmath>
#include <numbers>
namespace adi::dsp {
void ChorusEnsemble::prepare(double sr) {
    rate_ = std::isfinite(sr) && sr > 0 ? sr : 48000;
    for (auto &b : ring_)
        b.assign(static_cast<std::size_t>(rate_ * .1) + 8, 0);
    low_ = {};
    warm_ = {};
    feedback_ = {};
    phase_ = 0;
    write_ = 0;
}
void ChorusEnsemble::set(Param id, double v) noexcept {
    constexpr std::array<double, Count> lo{0, 0, 20, 0, 0, 1, 1, 0, 0, 0.01, 0, 0, 0, -60, 0, 0},
        hi{2, 1, 2000, 200, 1, 40, 2, 180, 100, 20, 100, 95, 1, 12, 100, 100};
    if (id < 0 || id >= Count || !std::isfinite(v))
        return;
    const auto i = static_cast<std::size_t>(id);
    p_[i] = std::clamp(v, lo[i], hi[i]);
}
double ChorusEnsemble::read(std::size_t c, double delay) const noexcept {
    const auto &b = ring_[c];
    const double d = std::clamp(delay, 2., static_cast<double>(b.size() - 3));
    const auto whole = static_cast<std::size_t>(d);
    const double f = d - static_cast<double>(whole);
    const auto at = (write_ + b.size() - whole) % b.size();
    const double a = b[(at + 1) % b.size()], v = b[at], w = b[(at + b.size() - 1) % b.size()],
                 z = b[(at + b.size() - 2) % b.size()];
    return v + .5 * f * (w - a + f * (2 * a - 5 * v + 4 * w - z + f * (3 * (v - w) + z - a)));
}
void ChorusEnsemble::process(const float *il, const float *ir, float *ol, float *orr,
                             std::size_t n) noexcept {
    if (ring_[0].empty()) {
        std::fill(ol, ol + n, 0.f);
        std::fill(orr, orr + n, 0.f);
        return;
    }
    const int mode = static_cast<int>(std::round(p_[Mode]));
    const int taps = mode == 1 ? 3 : (mode == 2 ? 1 : static_cast<int>(std::round(p_[Taps])));
    const double amount = p_[Amount] / 100;
    const double base = (p_[DelayAuto] >= .5 ? 2 + 18 * amount : p_[Time]);
    const double depth = std::min(base * .9, amount * (mode == 2 ? 8 : 4));
    const double hp = 1 - std::exp(-2 * std::numbers::pi * p_[HighPass] / rate_);
    const double warmth = p_[Warmth] / 100;
    const double lp = 1 - std::exp(-2 * std::numbers::pi * (18000 - 15000 * warmth) / rate_);
    const double gain = std::pow(10., p_[Output] / 20), mix = mode == 2 ? 1 : p_[Mix] / 100;
    for (std::size_t i = 0; i < n; ++i) {
        const std::array<double, 2> in{il[i], ir[i]};
        std::array<double, 2> wet{}, bass{};
        for (std::size_t c = 0; c < 2; ++c) {
            low_[c] += hp * (in[c] - low_[c]);
            bass[c] = p_[HighPassOn] >= .5 ? low_[c] : 0;
            for (int t = 0; t < taps; ++t) {
                double ph = phase_ + static_cast<double>(t) / taps +
                            (c == 1 ? (mode == 2 ? p_[Offset] / 360 : .25) : 0);
                ph -= std::floor(ph);
                const double sine = std::sin(2 * std::numbers::pi * ph),
                             triangle = 1 - 4 * std::abs(ph - .5);
                const double wave = mode == 2 ? sine + (triangle - sine) * p_[Shape] / 100 : sine;
                wet[c] += read(c, (base + depth * wave) * rate_ / 1000) / taps;
            }
            feedback_[c] =
                mode == 2 ? 0 : wet[c] * p_[Feedback] / 100 * (p_[Invert] >= .5 ? -1 : 1);
            const double x = in[c] - bass[c] + feedback_[c];
            ring_[c][write_] = static_cast<float>(std::clamp(x, -4., 4.));
            const double shaped =
                warmth > 0 ? std::tanh(wet[c] * (1 + 3 * warmth)) / (1 + 3 * warmth) : wet[c];
            warm_[c] += lp * (shaped - warm_[c]);
            if (warmth > 0)
                wet[c] = warm_[c];
        }
        const double mid = (wet[0] + wet[1]) * .5,
                     side = (wet[0] - wet[1]) * .5 * (mode == 2 ? 1 : p_[Width] / 100);
        ol[i] = static_cast<float>(gain * (in[0] * (1 - mix) + (mid + side + bass[0]) * mix));
        orr[i] = static_cast<float>(gain * (in[1] * (1 - mix) + (mid - side + bass[1]) * mix));
        write_ = (write_ + 1) % ring_[0].size();
        phase_ += p_[Rate] / rate_;
        phase_ -= std::floor(phase_);
    }
}
} // namespace adi::dsp
