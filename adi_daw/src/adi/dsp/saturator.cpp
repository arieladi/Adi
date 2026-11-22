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
// High Quality's 4x resampling filter (DEV-22). Live 12.4 reports 3 samples
// for Saturator's default preset, which has High Quality on; the linear-phase
// 65-tap windowed sinc ADI used (cutoff 0.1125 of the 4x rate, Hann window,
// unity DC gain) delays 16 samples. This is that filter's minimum-phase
// version (real cepstrum, 65 536 points, truncated to 65 taps, renormalised
// to unity DC gain): the same magnitude, a low-frequency group delay of 7.12
// taps per pass, so 3.56 samples for the two passes at the base rate.
// Saturator reports Live's 3; the 0.56-sample remainder is stated, not hidden.
// test_saturator.cpp checks the table's properties.
const std::array<double, 65> &Saturator::resamplingFilter() noexcept {
    static constexpr std::array<double, 65> kMinimumPhase{
    0.0003293405348762169, 0.002176673440947512, 0.007911771045729179,
    0.020780631086244287, 0.0437213033832084, 0.07751952759266591,
    0.11905104515739558, 0.16063017245122582, 0.1912555678892358,
    0.19980219867352123, 0.17923421109096663, 0.13017873534186808,
    0.062140243222746895, -0.008595934842104575, -0.06397484847983334,
    -0.09035610338629377, -0.08343334258186878, -0.04980036303461106,
    -0.0043892673418461856, 0.035264070545582595, 0.055604149026011775,
    0.05179887286997291, 0.02876747200506284, -0.0017473371726911655,
    -0.026603853966077846, -0.03673099126302919, -0.03033284307825303,
    -0.012678575836145962, 0.0070783195151236334, 0.02033909447007628,
    0.022661868537916945, 0.0150388927342886, 0.002587498975775022,
    -0.008395448717446352, -0.013438791293596476, -0.011575026636660497,
    -0.005124455605031215, 0.0020025173153397713, 0.006431769127551455,
    0.006791690704173281, 0.003933381382309, 3.211050588102245e-05,
    -0.002770060573948034, -0.0034113391674449353, -0.0021807502805149407,
    -0.0002367458547505455, 0.001212951985304903, 0.001560688949378694,
    0.0009653382023900397, 5.5264163424560754e-05, -0.0005566934738829543,
    -0.0006223185518011642, -0.0002979883152396614, 7.324316724193997e-05,
    0.00023742769969847656, 0.0001697409881901787, 1.7135364022133357e-05,
    -6.759826665203861e-05, -4.913766292709773e-05, 9.392767549838501e-07,
    1.3169777278025054e-05, 7.742155280719074e-07, 1.2179299303154722e-08,
    6.618539050511837e-10, 1.2658897518109293e-10,
    };
    return kMinimumPhase;
}
void Saturator::prepare(double sr) noexcept {
    rate_ = std::isfinite(sr) && sr > 0 ? sr : 48000;
    lane_ = {};
    fir_ = resamplingFilter();
}
void Saturator::set(Param id, double v) noexcept {
    // Live 12.4's ranges where ADI's control means the same (its shipped
    // default preset): Amt Lo +-36 dB, Frequency 30..18500 Hz, Output -36..0 dB.
    constexpr std::array<double, Count> lo{0, -36, -50,  0, 0, -36, -24, 30, 0.1, -36,
                                           0, 0,   -100, 0, 0, 0,   0.1, 0,  0},
        hi{7, 36, 0, 2, 1, 36, 24, 18500, 4, 0, 100, 100, 100, 100, 100, 100, 20, 1, 1};
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
    if (std::abs(z[0]) < 1e-30) {
        z[0] = 0;
    }
    if (std::abs(z[1]) < 1e-30) {
        z[1] = 0;
    }
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
            if (std::abs(l.dcOut) < 1e-30) {
                l.dcOut = 0;
            }
            if (p_[PreDC] >= .5)
                input = hp;
            l.dry[l.dryPos] = in[c];
            const double dry = hq ? l.dry[(l.dryPos + 1) % 4] : in[c];   // 3 samples, the reported latency
            l.dryPos = (l.dryPos + 1) % 4;
            // DF-E37 (a): at a partial Dry/Wet with High Quality on, the dry is
            // upsampled beside the wet and mixed BEFORE the down-sampling filter,
            // so both pass the same two minimum-phase filters and share one phase
            // response. A plain 3-sample dry against them comb-filtered (-13.9 dB
            // at 18 kHz at 50 %). At 0 % the dry stays the exact 3-sample delay;
            // at 100 % there is no dry.
            const bool inLoop = hq && mix > 0 && mix < 1;
            double wet = 0;
            for (int k = 0; k < factor; ++k) {
                double x = input, xd = 0;
                if (hq) {
                    l.up[l.pos] = k == 0 ? input * 4 : 0;
                    l.upDry[l.pos] = k == 0 ? in[c] * 4 : 0;
                    x = 0;
                    for (std::size_t j = 0; j < 65; ++j)
                        x += fir_[j] * l.up[(l.pos + 65 - j) % 65];
                    if (inLoop)
                        for (std::size_t j = 0; j < 65; ++j)
                            xd += fir_[j] * l.upDry[(l.pos + 65 - j) % 65];
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
                    l.down[l.pos] = inLoop ? xd + (x - xd) * mix : x;
                    double y = 0;
                    for (std::size_t j = 0; j < 65; ++j)
                        y += fir_[j] * l.down[(l.pos + 65 - j) % 65];
                    if (k == 0)
                        wet = y;
                    l.pos = (l.pos + 1) % 65;
                } else
                    wet = x;
            }
            double combined = inLoop ? wet : dry + (wet - dry) * mix;
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
