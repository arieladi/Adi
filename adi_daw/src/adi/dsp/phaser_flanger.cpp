/*
 * sst-effects - an open source library of audio effects
 * built by Surge Synth Team.
 *
 * Copyright 2018-2023, various authors, as described in the GitHub
 * transaction log.
 *
 * sst-effects is released under the GNU General Public Licence v3
 * or later (GPL-3.0-or-later). The license is found in the "LICENSE"
 * file in the root of this repository, or at
 * https://www.gnu.org/licenses/gpl-3.0.en.html
 *
 * The majority of these effects at initiation were factored from
 * Surge XT, and so git history prior to April 2023 is found in the
 * surge repo, https://github.com/surge-synthesizer/surge
 *
 * All source in sst-effects available at
 * https://github.com/surge-synthesizer/sst-effects
 */
// Scalar ADI adaptation: Surge Phaser's serial APF feedback and Flanger's comb.
#include "adi/dsp/phaser_flanger.hpp"
#include <algorithm>
#include <cmath>
#include <numbers>
namespace adi::dsp {
void PhaserFlanger::prepare(double sr) {
    rate_ = std::isfinite(sr) && sr > 0 ? sr : 48000;
    write_ = 0;
    for (std::size_t c = 0; c < 2; ++c) {
        lane_[c] = Lane{};
        lane_[c].random = 0x12345678u + static_cast<std::uint32_t>(c);
        lane_[c].ring.assign(static_cast<std::size_t>(rate_ * .3) + 4, 0.f);
    }
}
void PhaserFlanger::tempo(double v) noexcept {
    if (std::isfinite(v))
        bpm_ = std::clamp(v, 20., 999.);
}
void PhaserFlanger::set(Param id, double v) noexcept {
    constexpr std::array<double, Count> lo{0, 1, 20,   0,    0, 0.1, 0,    0.01,   0.0625, 0,
                                           0, 0, -50,  -100, 0, 0,   0.01, 0.0625, 0,      0,
                                           0, 0, -100, 0.1,  1, 0,   5,    -60,    0,      0},
        hi{2, 8,  18000, 100, 1,  100, 1, 20,  16,   9,    1, 360,  50, 100, 100,
           1, 20, 16,    100, 95, 1,   1, 100, 1000, 5000, 1, 3000, 12, 100, 100};
    if (id < 0 || id >= Count || !std::isfinite(v))
        return;
    const auto i = static_cast<std::size_t>(id);
    p_[i] = std::clamp(v, lo[i], hi[i]);
}
void PhaserFlanger::process(const float *il, const float *ir, float *ol, float *orr,
                            std::size_t n) noexcept {
    if (lane_[0].ring.empty()) {
        std::fill(ol, ol + n, 0.f);
        std::fill(orr, orr + n, 0.f);
        return;
    }
    constexpr double pi = std::numbers::pi;
    const int mode = static_cast<int>(std::round(p_[Mode])),
              stages = static_cast<int>(std::round(p_[Notches])),
              wave = static_cast<int>(std::round(p_[Wave]));
    const double hz = p_[Sync] >= .5 ? bpm_ / (60 * p_[Beats]) : p_[Rate],
                 hz2 = p_[Sync2] >= .5 ? bpm_ / (60 * p_[Beats2]) : p_[Rate2];
    const double hp = 1 - std::exp(-2 * pi * p_[SafeBass] / rate_), mix = p_[Mix] / 100,
                 gain = std::pow(10., p_[Output] / 20), warmth = p_[Warmth] / 100;
    for (std::size_t i = 0; i < n; ++i) {
        const std::array<double, 2> in{il[i], ir[i]};
        std::array<double, 2> out{};
        for (std::size_t c = 0; c < 2; ++c) {
            auto &l = lane_[c];
            l.phase += hz / rate_ * (c == 1 && p_[StereoMode] >= .5 ? 1 + p_[Spin] / 100 : 1);
            if (l.phase >= 1) {
                l.phase -= std::floor(l.phase);
                l.randomFrom = l.randomTo;
                l.random ^= l.random << 13;
                l.random ^= l.random >> 17;
                l.random ^= l.random << 5;
                l.randomTo = 2 * static_cast<double>(l.random) / 4294967295. - 1;
            }
            l.phase2 += hz2 / rate_;
            l.phase2 -= std::floor(l.phase2);
            double phase = l.phase + (c == 1 && p_[StereoMode] < .5 ? p_[Phase] / 360 : 0);
            phase -= std::floor(phase);
            if (wave < 8) {
                const double duty = .5 + p_[Duty] * .0049;
                phase = phase < duty ? phase / (2 * duty) : .5 + (phase - duty) / (2 * (1 - duty));
            }
            const double triangle = 1 - 4 * std::abs(phase - .5);
            double mod = std::sin(2 * pi * phase);
            if (wave == 1)
                mod = triangle;
            if (wave == 2) {
                l.analog +=
                    (phase < .5 ? 1 - l.analog : -1 - l.analog) * (1 - std::exp(-8 * hz / rate_));
                mod = l.analog;
            }
            if (wave == 3 || wave == 4) {
                const double steps = wave == 3 ? 8 : 16;
                mod = std::round(triangle * steps) / steps;
            }
            if (wave == 5)
                mod = 2 * phase - 1;
            if (wave == 6)
                mod = 1 - 2 * phase;
            if (wave == 7)
                mod = phase < .5 ? 1 : -1;
            if (wave == 8)
                mod = l.randomFrom + (l.randomTo - l.randomFrom) * phase;
            if (wave == 9)
                mod = l.randomTo;
            mod += (1 - 4 * std::abs(l.phase2 - .5) - mod) * p_[Lfo2Mix] / 100;
            const double envTarget = std::abs(in[c]),
                         ms = envTarget > l.env ? p_[Attack] : p_[Release];
            l.env += (envTarget - l.env) * (1 - std::exp(-1000 / (rate_ * ms)));
            mod *= p_[Amount] / 100;
            if (p_[EnvOn] >= .5)
                mod += l.env * p_[EnvAmount] / 100;
            mod = std::clamp(mod, -1., 1.);
            l.low += hp * (in[c] - l.low);
            const double bass = p_[SafeBassOn] >= .5 ? l.low : 0, x = in[c] - bass;
            const double fb = p_[Feedback] / 100 * (p_[Invert] >= .5 ? -1 : 1);
            double wet = 0;
            if (mode == 0) {
                wet = std::clamp(x + l.feedback * fb, -32., 32.);
                for (int j = 0; j < stages; ++j) {
                    const double spread = p_[Spread] / 100;
                    const double centered =
                        static_cast<double>(j) - static_cast<double>(stages - 1) / 2;
                    const double freq = std::clamp(
                        p_[Center] * std::exp2(centered * spread + mod * 2 * (1 - p_[Blend])), 5.,
                        rate_ * .45);
                    const double q = std::clamp(.5 + 4 * spread + mod * p_[Blend] * 2, .2, 8.);
                    const double omega = 2 * pi * freq / rate_, alpha = std::sin(omega) / (2 * q),
                                 a0 = 1 + alpha, b0 = (1 - alpha) / a0,
                                 b1 = -2 * std::cos(omega) / a0;
                    auto &z = l.ap[static_cast<std::size_t>(j)];
                    const double y = b0 * wet + z[0];
                    z[0] = b1 * wet - b1 * y + z[1];
                    z[1] = wet - b0 * y;
                    wet = y;
                }
                l.feedback = wet;
            } else {
                const double d = std::clamp(
                    p_[Time] * (mode == 1 ? 1 - .9 * (mod + p_[Amount] / 100) * .5 : 1 + .8 * mod) *
                        rate_ / 1000,
                    1., static_cast<double>(l.ring.size() - 2));
                const auto whole = static_cast<std::size_t>(d);
                const double frac = d - static_cast<double>(whole);
                const auto at = (write_ + l.ring.size() - whole) % l.ring.size();
                wet = l.ring[at] * (1 - frac) +
                      l.ring[(at + l.ring.size() - 1) % l.ring.size()] * frac;
                l.ring[write_] = static_cast<float>(std::clamp(x + wet * fb, -4., 4.));
            }
            if (warmth > 0) {
                const double shaped = std::tanh(wet * (1 + 3 * warmth)) / (1 + 3 * warmth);
                l.warm +=
                    (shaped - l.warm) * (1 - std::exp(-2 * pi * (18000 - 15000 * warmth) / rate_));
                wet = l.warm;
            }
            out[c] = gain * (in[c] * (1 - mix) + (wet + bass) * mix);
        }
        ol[i] = static_cast<float>(out[0]);
        orr[i] = static_cast<float>(out[1]);
        write_ = (write_ + 1) % lane_[0].ring.size();
    }
}
} // namespace adi::dsp
