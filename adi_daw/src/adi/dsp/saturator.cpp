/*
 * sst-waveshaper - an open source library of waveshaper algorithms
 * by the Surge Synth Team
 *
 * Copyright 2018-2025, various authors, as described in the GitHub
 * transaction log.
 *
 * sst-jucegui is released under the GNU General Public License 3 or later
 * as found in LICENSE.md in this repository.
 *
 * All source in sst-waveshapers available at
 * https://github.com/surge-synthesizer/sst-waveshapers
 */
// Scalar CLIP/TANH adaptation and Distortion-style pre/post shaper architecture.
// Other curves and oversampling are ADI implementations, not Live's private DSP.
#include "adi/dsp/saturator.hpp"
#include <algorithm>
#include <cmath>
#include <numbers>
namespace adi::dsp {
namespace {
double analog(double x, double threshold = .5) {
    const double a = std::abs(x);
    if (a <= threshold)
        return x;
    if (threshold >= 1)
        return std::clamp(x, -1., 1.);
    return std::copysign(
        threshold + (1 - threshold) * (1 - std::exp(-(a - threshold) / (1 - threshold))), x);
}
} // namespace
void Saturator::prepare(double sr) noexcept {
    rate_ = std::isfinite(sr) && sr > 0 ? sr : 48000;
    lane_ = {};
    double sum = 0;
    for (std::size_t i = 0; i < 65; ++i) {
        const double x = static_cast<double>(i) - 32;
        const double sinc =
            x == 0 ? .225 : std::sin(std::numbers::pi * .225 * x) / (std::numbers::pi * x);
        fir_[i] = sinc * (.5 - .5 * std::cos(2 * std::numbers::pi * static_cast<double>(i) / 64));
        sum += fir_[i];
    }
    for (auto &c : fir_)
        c /= sum;
}
void Saturator::set(Param id, double v) noexcept {
    constexpr std::array<double, Count> lo{0, -36, -50,  0, 0, -24, -24, 20, 0.1, -60,
                                           0, 0,   -100, 0, 0, 0,   0.1, 0,  0},
        hi{7, 36, 0, 2, 1, 24, 24, 18000, 4, 0, 100, 100, 100, 100, 100, 100, 20, 1, 1};
    if (id < 0 || id >= Count || !std::isfinite(v))
        return;
    const auto i = static_cast<std::size_t>(id);
    p_[i] = std::clamp(v, lo[i], hi[i]);
}
double Saturator::shape(double x) const noexcept {
    const int curve = static_cast<int>(std::round(p_[CurveType]));
    switch (curve) {
    case 0:
        return analog(x);
    case 1:
        return std::sin(std::clamp(x, -std::numbers::pi / 2, std::numbers::pi / 2));
    case 2:
        return analog(x, std::pow(10., p_[BassThreshold] / 20));
    case 3: {
        const double xx = x * x;
        return std::clamp(x * (27 + xx) / (27 + 9 * xx), -1., 1.);
    }
    case 4: {
        const double z = std::clamp(x, -1., 1.);
        return 1.5 * z - .5 * z * z * z;
    }
    case 5:
        return std::sin(std::numbers::pi * x);
    case 6:
        return std::clamp(x, -1., 1.);
    default: {
        const double damp = p_[Damp] / 100;
        const double z = std::copysign(std::max(0., std::abs(x) - damp), x);
        const double shaped = z * (.5 + p_[Linear] / 100) +
                              p_[Curve] / 100 * z * z * z / (1 + z * z) +
                              p_[Depth] / 100 * std::sin(std::numbers::pi * p_[Period] * z);
        return x + (shaped - x) * p_[ShaperDrive] / 100;
    }
    }
}
double Saturator::filter(double x, const Filter &c, std::array<double, 2> &z) noexcept {
    const double y = c.b0 * x + z[0];
    z[0] = c.b1 * x - c.a1 * y + z[1];
    z[1] = c.b2 * x - c.a2 * y;
    return y;
}
void Saturator::process(const float *il, const float *ir, float *ol, float *orr,
                        std::size_t n) noexcept {
    constexpr double pi = std::numbers::pi;
    const bool hq = p_[HiQuality] >= .5;
    const int factor = hq ? 4 : 1;
    const double sr = rate_ * factor, drive = std::pow(10., p_[Drive] / 20),
                 gain = std::pow(10., p_[Output] / 20), mix = p_[Mix] / 100;
    std::array<Filter, 4> f{};
    const double pole = std::exp(-2 * pi * 200 / sr), g = std::pow(10., p_[AmtLo] / 20);
    f[0] = {1 + (g - 1) * (1 - pole), -pole, 0, -pole, 0};
    const double a = std::pow(10., p_[AmtHi] / 40),
                 w = 2 * pi * std::min(p_[Frequency], sr * .45) / sr,
                 alpha = std::sin(w) *
                         std::sinh(std::log(2.) * .5 * p_[Width] * w / std::max(1e-9, std::sin(w))),
                 den = 1 + alpha / a;
    f[1] = {(1 + alpha * a) / den, -2 * std::cos(w) / den, (1 - alpha * a) / den,
            -2 * std::cos(w) / den, (1 - alpha / a) / den};
    const auto inverse = [](Filter c) {
        return Filter{1 / c.b0, c.a1 / c.b0, c.a2 / c.b0, c.b1 / c.b0, c.b2 / c.b0};
    };
    f[2] = inverse(f[1]);
    f[3] = inverse(f[0]);
    const double dc = std::exp(-2 * pi * 5 / rate_);
    for (std::size_t i = 0; i < n; ++i) {
        const std::array<double, 2> in{il[i], ir[i]};
        std::array<double, 2> out{};
        for (std::size_t c = 0; c < 2; ++c) {
            auto &l = lane_[c];
            double input = in[c];
            const double hp = input - l.dcIn + dc * l.dcOut;
            l.dcIn = input;
            l.dcOut = hp;
            if (p_[PreDC] >= .5)
                input = hp;
            l.dry[l.dryPos] = in[c];
            const double dry = hq ? l.dry[(l.dryPos + 1) % 17] : in[c];
            l.dryPos = (l.dryPos + 1) % 17;
            double wet = 0;
            for (int k = 0; k < factor; ++k) {
                double x = input;
                if (hq) {
                    l.up[l.pos] = k == 0 ? input * 4 : 0;
                    x = 0;
                    for (std::size_t j = 0; j < 65; ++j)
                        x += fir_[j] * l.up[(l.pos + 65 - j) % 65];
                }
                if (p_[ColorOn] >= .5) {
                    x = filter(x, f[0], l.z[0]);
                    x = filter(x, f[1], l.z[1]);
                }
                x = shape(x * drive);
                if (p_[ColorOn] >= .5) {
                    x = filter(x, f[2], l.z[2]);
                    x = filter(x, f[3], l.z[3]);
                }
                if (hq) {
                    l.down[l.pos] = x;
                    double y = 0;
                    for (std::size_t j = 0; j < 65; ++j)
                        y += fir_[j] * l.down[(l.pos + 65 - j) % 65];
                    if (k == 0)
                        wet = y;
                    l.pos = (l.pos + 1) % 65;
                } else
                    wet = x;
            }
            double combined = dry + (wet - dry) * mix;
            if (p_[PostClip] >= 1.5)
                combined = std::clamp(combined, -1., 1.);
            else if (p_[PostClip] >= .5)
                combined = analog(combined);
            out[c] = gain * combined;
        }
        ol[i] = static_cast<float>(out[0]);
        orr[i] = static_cast<float>(out[1]);
    }
}
} // namespace adi::dsp
