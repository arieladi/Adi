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
// ADI scalar adaptation of Delay and FloatyDelay read/feedback/write and correlated warp.
#include "adi/dsp/echo.hpp"
#include <algorithm>
#include <cmath>
#include <numbers>
namespace adi::dsp {
void Echo::prepare(double sr) {
    rate_ = std::isfinite(sr) && sr > 0 ? sr : 48000;
    write_ = 0;
    started_ = false;
    phase_ = wobble_ = env_ = gate_ = 0;
    duck_ = 1;
    constexpr std::array<double, 4> ms{17, 23, 31, 43};
    for (std::size_t c = 0; c < 2; ++c) {
        auto &l = lanes_[c];
        l = Lane{};
        l.random = 0x12345678u + static_cast<std::uint32_t>(c);
        l.noiseTo = random(l);
        l.lfoTo = random(l);
        l.ring.assign(static_cast<std::size_t>(rate_ * 26) + 8, 0.f);
        for (std::size_t j = 0; j < 4; ++j)
            l.room[j].assign(static_cast<std::size_t>(rate_ * ms[j] / 1000) + 1, 0.f);
    }
}
void Echo::tempo(double v) noexcept {
    if (std::isfinite(v))
        bpm_ = std::clamp(v, 20., 999.);
}
void Echo::set(Param id, double v) noexcept {
    constexpr std::array<double, Count> lo{
        0,   1, 1,   0, 0, 0.0625, 0.0625, 0,      0, 0, -33, -33, -24, 0,   0,   0, 0,
        20,  0, 100, 0, 0, 0,      0.01,   0.0625, 0, 0, 0,   0,   0,   0,   -80, 1, 0,
        -80, 1, 0,   0, 0, 0,      0,      0,      0, 0, 0,   0.1, 0,   -60, 0,   0},
        hi{2,     8000, 8000,  1,   1,   4, 4,   3,   3,   1,   33, 33,  24,  1,  95,  1,    1,
           10000, 100,  20000, 100, 5,   1, 20,  16,  360, 100, 1,  100, 100, 1,  0,   5000, 1,
           0,     5000, 1,     100, 100, 1, 100, 100, 1,   100, 2,  20,  200, 12, 100, 1};
    if (id < 0 || id >= Count || !std::isfinite(v))
        return;
    const auto i = static_cast<std::size_t>(id);
    p_[i] = std::clamp(v, lo[i], hi[i]);
    if (p_[Link] >= .5) {
        if (id == TimeL)
            p_[TimeR] = p_[TimeL];
        if (id == TimeR)
            p_[TimeL] = p_[TimeR];
        if (id == SyncL)
            p_[SyncR] = p_[SyncL];
        if (id == SyncR)
            p_[SyncL] = p_[SyncR];
        if (id == BeatsL)
            p_[BeatsR] = p_[BeatsL];
        if (id == BeatsR)
            p_[BeatsL] = p_[BeatsR];
        if (id == SyncModeL)
            p_[SyncModeR] = p_[SyncModeL];
        if (id == SyncModeR)
            p_[SyncModeL] = p_[SyncModeR];
        if (id == Link) {
            p_[TimeR] = p_[TimeL];
            p_[SyncR] = p_[SyncL];
            p_[BeatsR] = p_[BeatsL];
            p_[SyncModeR] = p_[SyncModeL];
        }
    }
}
double Echo::delay(std::size_t c) const noexcept {
    const bool right = c == 1;
    double sec = p_[right ? TimeR : TimeL] / 1000;
    if (p_[right ? SyncR : SyncL] >= .5) {
        constexpr std::array<double, 4> scales{1, 2. / 3, 1.5, .25};
        const auto mode = static_cast<std::size_t>(std::round(p_[right ? SyncModeR : SyncModeL]));
        sec = 60 / bpm_ * p_[right ? BeatsR : BeatsL] * scales[mode];
    }
    return sec * (1 + p_[right ? OffsetR : OffsetL] / 100) * rate_;
}
double Echo::random(Lane &l) noexcept {
    l.random ^= l.random << 13;
    l.random ^= l.random >> 17;
    l.random ^= l.random << 5;
    return 2 * static_cast<double>(l.random) / 4294967295. - 1;
}
double Echo::read(const Lane &l, double d) const noexcept {
    d = std::clamp(d, 2., static_cast<double>(l.ring.size() - 3));
    const auto whole = static_cast<std::size_t>(d);
    const double f = d - static_cast<double>(whole);
    const auto at = (write_ + l.ring.size() - whole) % l.ring.size();
    const auto size = l.ring.size();
    const double a = l.ring[(at + 1) % size], b = l.ring[at], c = l.ring[(at + size - 1) % size],
                 e = l.ring[(at + size - 2) % size];
    return b + .5 * f * (c - a + f * (2 * a - 5 * b + 4 * c - e + f * (3 * (b - c) + e - a)));
}
double Echo::room(Lane &l, double input) noexcept {
    std::array<double, 4> y{};
    double sum = 0;
    for (std::size_t j = 0; j < 4; ++j) {
        y[j] = l.room[j][l.roomPos[j]];
        sum += y[j];
    }
    for (std::size_t j = 0; j < 4; ++j) {
        const double gain =
            std::pow(.001, static_cast<double>(l.room[j].size()) / (rate_ * p_[Decay]));
        l.room[j][l.roomPos[j]] = static_cast<float>((y[j] - .5 * sum + input * .5) * gain);
        l.roomPos[j] = (l.roomPos[j] + 1) % l.room[j].size();
    }
    return sum * .25;
}
void Echo::process(const float *il, const float *ir, float *ol, float *orr,
                   std::size_t n) noexcept {
    if (lanes_[0].ring.empty()) {
        std::fill(ol, ol + n, 0.f);
        std::fill(orr, orr + n, 0.f);
        return;
    }
    constexpr double pi = std::numbers::pi;
    const int mode = static_cast<int>(std::round(p_[Mode])),
              wave = static_cast<int>(std::round(p_[Wave])),
              location = static_cast<int>(std::round(p_[ReverbLocation]));
    const double inputGain = std::pow(10., p_[Input] / 20), output = std::pow(10., p_[Output] / 20),
                 mix = p_[Mix] / 100, reverb = p_[Reverb] / 100,
                 fb = p_[Feedback] / 100 * (p_[Invert] >= .5 ? -1 : 1);
    const double gateThreshold = std::pow(10., p_[GateThreshold] / 20),
                 duckThreshold = std::pow(10., p_[DuckThreshold] / 20),
                 gateRelease = std::exp(-1000 / (rate_ * p_[GateRelease])),
                 duckRelease = std::exp(-1000 / (rate_ * p_[DuckRelease]));
    const double lfoHz = p_[LfoSync] >= .5 ? bpm_ / (60 * p_[LfoBeats]) : p_[LfoRate];
    for (std::size_t c = 0; c < 2; ++c) {
        auto &l = lanes_[c];
        const double target = delay(c);
        if (!started_) {
            l.time = l.oldTime = l.target = target;
            l.fade = 1;
        } else if (target != l.target) {
            l.oldTime = l.time;
            l.target = target;
            l.fade = 0;
        }
    }
    started_ = true;
    for (std::size_t i = 0; i < n; ++i) {
        std::array<double, 2> dry{il[i] * inputGain, ir[i] * inputGain};
        if (p_[Distort] >= .5)
            for (auto &v : dry)
                v = std::tanh(v);
        const double level = std::max(std::abs(dry[0]), std::abs(dry[1]));
        env_ += (level - env_) * (1 - std::exp(-1 / (rate_ * (level > env_ ? .01 : .1))));
        gate_ = level >= gateThreshold ? 1 : gate_ * gateRelease;
        const double duckTarget =
            level > duckThreshold ? duckThreshold / std::max(level, 1e-12) : 1;
        duck_ = duckTarget < duck_ ? duckTarget : 1 - (1 - duck_) * duckRelease;
        std::array<double, 2> input = dry;
        if (p_[GateOn] >= .5)
            for (auto &v : input)
                v *= gate_;
        if (mode == 2)
            input = {(input[0] + input[1]) * .5, (input[0] - input[1]) * .5};
        phase_ += lfoHz / rate_;
        if (phase_ >= 1) {
            phase_ -= std::floor(phase_);
            for (auto &l : lanes_) {
                l.lfoFrom = l.lfoTo;
                l.lfoTo = random(l);
            }
        }
        wobble_ += .5 / rate_;
        if (wobble_ >= 1) {
            wobble_ -= 1;
            for (auto &l : lanes_) {
                l.noiseFrom = l.noiseTo;
                l.noiseTo = random(l);
            }
        }
        const double commonNoise =
            lanes_[0].noiseFrom + (lanes_[0].noiseTo - lanes_[0].noiseFrom) * wobble_;
        std::array<double, 2> wet{}, roomFeedback{};
        for (std::size_t c = 0; c < 2; ++c) {
            auto &l = lanes_[c];
            double ph = phase_ + (c == 1 ? p_[Phase] / 360 : 0);
            ph -= std::floor(ph);
            double mod = std::sin(2 * pi * ph);
            if (wave == 1)
                mod = 1 - 4 * std::abs(ph - .5);
            if (wave == 2)
                mod = 2 * ph - 1;
            if (wave == 3)
                mod = 1 - 2 * ph;
            if (wave == 4)
                mod = ph < .5 ? 1 : -1;
            if (wave == 5)
                mod = l.lfoFrom + (l.lfoTo - l.lfoFrom) * ph;
            mod += (env_ - mod) * p_[EnvMix] / 100;
            mod = std::clamp(mod, -1., 1.);
            const double localNoise = l.noiseFrom + (l.noiseTo - l.noiseFrom) * wobble_;
            const double warp = std::sin(2 * pi * wobble_) * (1 - p_[WobbleMorph] / 100) +
                                (commonNoise + localNoise) * .5 * p_[WobbleMorph] / 100;
            const double modulation =
                mod * p_[ModDelay] / 100 * (p_[ModX4] >= .5 ? 4 : 1) * .01 * l.target +
                (p_[WobbleOn] >= .5 ? warp * p_[WobbleAmount] / 100 * .01 * l.target : 0);
            if (p_[Repitch] >= .5) {
                l.time += (l.target - l.time) * (1 - std::exp(-1 / (rate_ * .005)));
                wet[c] = read(l, l.time + modulation);
                l.fade = 1;
            } else {
                l.fade = std::min(1., l.fade + 1 / (rate_ * .01));
                wet[c] = read(l, l.oldTime + modulation) * (1 - l.fade) +
                         read(l, l.target + modulation) * l.fade;
                l.time = l.oldTime + (l.target - l.oldTime) * l.fade;
            }
            if (p_[FilterOn] >= .5) {
                for (std::size_t j = 0; j < 2; ++j) {
                    const double cutoff = std::clamp(p_[j == 0 ? HP : LP] *
                                                         std::exp2(mod * p_[ModFilter] / 100 * 2),
                                                     5., rate_ * .45),
                                 q = .5 + p_[j == 0 ? HPRes : LPRes] * .095,
                                 w = 2 * pi * cutoff / rate_, co = std::cos(w),
                                 alpha = std::sin(w) / (2 * q), a0 = 1 + alpha, a1 = -2 * co / a0,
                                 a2 = (1 - alpha) / a0,
                                 b0 = (j == 0 ? (1 + co) : (1 - co)) * .5 / a0,
                                 b1 = (j == 0 ? -2 : 2) * b0;
                    auto &z = l.filter[j];
                    const double x = wet[c], y = b0 * x + z[0];
                    z[0] = b1 * x - a1 * y + z[1];
                    z[1] = b0 * x - a2 * y;
                    wet[c] = y;
                }
            }
            if (reverb > 0) {
                const double roomIn = location == 0 ? input[c] : wet[c], rev = room(l, roomIn);
                if (location == 0)
                    input[c] += reverb * (rev - input[c]);
                if (location == 1)
                    wet[c] += reverb * (rev - wet[c]);
                if (location == 2)
                    roomFeedback[c] = reverb * (rev - wet[c]);
            }
        }
        for (std::size_t c = 0; c < 2; ++c) {
            auto &l = lanes_[c];
            const double white = random(l);
            l.brown += .01 * (white - l.brown);
            const double noise = p_[NoiseOn] >= .5
                                     ? p_[NoiseAmount] / 100 * .02 *
                                           (white + (l.brown * 10 - white) * p_[NoiseMorph] / 100)
                                     : 0;
            const auto source = mode == 1 ? 1 - c : c;
            const double x = input[c] + fb * (wet[source] + roomFeedback[source]) + noise;
            l.ring[write_] = static_cast<float>(std::clamp(x, -8., 8.));
        }
        if (mode == 2)
            wet = {wet[0] + wet[1], wet[0] - wet[1]};
        const double mid = (wet[0] + wet[1]) * .5, side = (wet[0] - wet[1]) * .5 * p_[Width] / 100;
        wet = {mid + side, mid - side};
        const double dw = p_[EqualLoudness] >= .5 ? std::cos(mix * pi / 2) : 1 - mix,
                     ww = p_[EqualLoudness] >= .5 ? std::sin(mix * pi / 2) : mix,
                     d = p_[DuckOn] >= .5 ? duck_ : 1;
        ol[i] = static_cast<float>(output * (dry[0] * dw + wet[0] * ww * d));
        orr[i] = static_cast<float>(output * (dry[1] * dw + wet[1] * ww * d));
        write_ = (write_ + 1) % lanes_[0].ring.size();
    }
}
} // namespace adi::dsp
