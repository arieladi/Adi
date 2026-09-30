// SPDX-License-Identifier: GPL-3.0-or-later
#include "eq_eight.hpp"
#include <algorithm>
#include <cmath>
#include <numbers>
namespace adi::dsp {
EqEight::EqEight() noexcept {
    constexpr double freq[] = {80, 200, 500, 1000, 2000, 4000, 8000, 16000};
    for (std::size_t c = 0; c < 2; ++c)
        for (std::size_t b = 0; b < 8; ++b) {
            const auto n = c * 40 + b * 5;
            p_[n] = b < 4 ? 1 : 0;
            p_[n + 1] = Peak;
            p_[n + 2] = freq[b];
            p_[n + 3] = 0;
            p_[n + 4] = .70710678;
        }
    p_[AdaptiveQ] = 1;
    p_[Scale] = 100;
}
void EqEight::set(Param id, double v) noexcept {
    const auto n = static_cast<std::size_t>(id);
    if (n >= p_.size() || !std::isfinite(v))
        return;
    if (n < 80) {
        constexpr double lo[] = {0, 0, 10, -15, .1}, hi[] = {1, 7, 22000, 15, 18};
        v = std::clamp(v, lo[n % 5], hi[n % 5]);
        if (n % 5 < 2)
            v = std::round(v);
    } else {
        constexpr double lo[] = {0, 0, 0, -200, -24, 0, 0}, hi[] = {2, 1, 1, 200, 24, 8, 1};
        v = std::clamp(v, lo[n - 80], hi[n - 80]);
        if (n != Scale && n != Output)
            v = std::round(v);
    }
    p_[n] = v;
    dirty_ = true;
}
void EqEight::prepare(double rate) noexcept {
    rate_ = std::isfinite(rate) ? std::clamp(rate, 8000., 384000.) : 48000.;
    bands_ = {};
    up_ = {};
    down_ = {};
    firPos_ = 0;
    double sum = 0;
    for (std::size_t i = 0; i < fir_.size(); ++i) {
        const double x = static_cast<double>(i) - 16;
        const double sinc =
            x == 0 ? .5 : std::sin(std::numbers::pi * .5 * x) / (std::numbers::pi * x);
        fir_[i] = sinc * (.42 - .5 * std::cos(2 * std::numbers::pi * static_cast<double>(i) / 32) +
                          .08 * std::cos(4 * std::numbers::pi * static_cast<double>(i) / 32));
        sum += fir_[i];
    }
    for (auto &f : fir_)
        f /= sum;
    update(true);
}
void EqEight::update(bool instant) noexcept {
    const double fs = rate_ * (latency() ? 2 : 1);
    smooth_ = std::exp(-1. / (.005 * fs));
    for (std::size_t c = 0; c < 2; ++c)
        for (std::size_t b = 0; b < 8; ++b) {
            const auto n = (p_[Mode] == 0 ? 0 : c * 40) + b * 5;
            auto &band = bands_[c][b];
            band.targetWet = p_[n];
            const int audition = static_cast<int>(p_[Audition]);
            if (audition > 0 && static_cast<int>(b) + 1 != audition)
                band.targetWet = 0;
            const auto type = static_cast<int>(p_[n + 1]);
            const double gain = p_[n + 3] * p_[Scale] / 100;
            const double q = std::clamp(p_[n + 4] * (p_[AdaptiveQ] >= .5 && type == Peak
                                                         ? std::pow(2., std::abs(gain) / 24)
                                                         : 1),
                                        .1, 36.);
            const double w = 2 * std::numbers::pi * std::min(p_[n + 2], fs * .475) / fs;
            for (std::size_t k = 0; k < 4; ++k) {
                surge_eq::Coeff coeff;
                if (type == LowCut48 || type == HighCut48) {
                    const double butter =
                        1 /
                        (2 * std::cos(std::numbers::pi * (2 * static_cast<double>(k) + 1) / 16));
                    coeff = surge_eq::cut(w, butter * (q / .70710678), type == LowCut48);
                } else if (k == 0) {
                    switch (type) {
                    case LowCut12:
                        coeff = surge_eq::cut(w, q, true);
                        break;
                    case HighCut12:
                        coeff = surge_eq::cut(w, q, false);
                        break;
                    case Notch:
                        coeff = surge_eq::notch(w, q);
                        break;
                    case LowShelf:
                    case HighShelf:
                        coeff = surge_eq::shelf(w, q, gain, type == HighShelf);
                        break;
                    default:
                        coeff = surge_eq::peak(w, 2 * std::asinh(1 / (2 * q)) / std::log(2.),
                                               std::pow(10., gain / 20), std::pow(10., gain / 40));
                        break;
                    }
                }
                band.section[k].target = coeff;
                if (instant)
                    band.section[k].now = coeff;
            }
            if (instant)
                band.wet = band.targetWet;
        }
    if (instant)
        gain_ = std::pow(10., p_[Output] / 20);
    dirty_ = false;
}
double EqEight::channel(double x, std::size_t c) noexcept {
    for (auto &band : bands_[c]) {
        band.wet = band.targetWet + smooth_ * (band.wet - band.targetWet);
        if (band.targetWet == 0 && band.wet < 1e-12) {
            band.wet = 0;
            continue;
        }
        const double dry = x;
        for (auto &s : band.section) {
            auto blend = [&](double &a, double b) { a = b + smooth_ * (a - b); };
            blend(s.now.b0, s.target.b0);
            blend(s.now.b1, s.target.b1);
            blend(s.now.b2, s.target.b2);
            blend(s.now.a1, s.target.a1);
            blend(s.now.a2, s.target.a2);
            const double y = x * s.now.b0 + s.z1;
            s.z1 = x * s.now.b1 - s.now.a1 * y + s.z2;
            s.z2 = x * s.now.b2 - s.now.a2 * y;
            x = y;
        }
        x = dry + band.wet * (x - dry);
    }
    return x;
}
void EqEight::process(const float *l, const float *r, float *ol, float *orr,
                      std::size_t n) noexcept {
    if (dirty_)
        update(false);
    const bool hq = latency() != 0, ms = p_[Mode] == 2;
    const int factor = hq ? 2 : 1;
    const double targetGain = std::pow(10., p_[Output] / 20);
    for (std::size_t i = 0; i < n; ++i) {
        double input[] = {l && std::isfinite(l[i]) ? l[i] : 0, r && std::isfinite(r[i]) ? r[i] : 0},
               output[2]{};
        for (int phase = 0; phase < factor; ++phase) {
            double x[2]{};
            for (std::size_t c = 0; c < 2; ++c) {
                x[c] = input[c];
                if (hq) {
                    up_[c][firPos_] = phase == 0 ? input[c] * 2 : 0;
                    x[c] = 0;
                    for (std::size_t k = 0; k < 33; ++k)
                        x[c] += fir_[k] * up_[c][(firPos_ + 33 - k) % 33];
                }
            }
            if (ms) {
                const double mid = (x[0] + x[1]) * .5;
                x[1] = (x[0] - x[1]) * .5;
                x[0] = mid;
            }
            x[0] = channel(x[0], 0);
            x[1] = channel(x[1], 1);
            if (ms) {
                const double left = x[0] + x[1];
                x[1] = x[0] - x[1];
                x[0] = left;
            }
            gain_ = targetGain + smooth_ * (gain_ - targetGain);
            for (std::size_t c = 0; c < 2; ++c) {
                if (hq) {
                    down_[c][firPos_] = x[c];
                    if (phase == 0) {
                        output[c] = 0;
                        for (std::size_t k = 0; k < 33; ++k)
                            output[c] += fir_[k] * down_[c][(firPos_ + 33 - k) % 33];
                        output[c] *= gain_;
                    }
                } else
                    output[c] = x[c] * gain_;
            }
            if (hq)
                firPos_ = (firPos_ + 1) % 33;
        }
        ol[i] = static_cast<float>(output[0]);
        orr[i] = static_cast<float>(output[1]);
    }
}
} // namespace adi::dsp
