// SPDX-License-Identifier: GPL-3.0-or-later
#include "adi/dsp/corpus.hpp"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace adi::dsp {
namespace {
double settle(double x) noexcept { return std::abs(x) < 1e-30 ? 0 : x; }
}  // namespace

std::array<double, Corpus::kModes> Corpus::ratios(Model m) noexcept {
    std::array<double, kModes> r{};
    // A free-free beam: (beta_n / beta_1)^2, beta_n L = 4.7300, 7.8532,
    // 10.9956, 14.1372, then (2n + 1) pi / 2.
    std::array<double, kModes> beam{};
    constexpr double first[4] = {4.730040745, 7.853204624, 10.99560784, 14.13716549};
    for (std::size_t k = 0; k < kModes; ++k) {
        const double b = k < 4 ? first[k] : (2.0 * static_cast<double>(k + 1) + 1) * std::numbers::pi / 2;
        beam[k] = (b / first[0]) * (b / first[0]);
    }
    // A circular membrane: the zeros of the Bessel functions, j_mn / j_01.
    constexpr double membrane[kModes] = {1, 1.5933, 2.1355, 2.2954, 2.6531, 2.9173, 3.1555, 3.5001,
                                         3.5985, 3.6475, 4.0589, 4.1317, 4.2304, 4.6010, 4.6101, 4.8319};
    // A square simply supported plate: (m^2 + n^2) / 2, in order.
    constexpr double plate[kModes] = {1, 2.5, 2.5, 4, 5, 5, 6.5, 6.5, 8.5, 8.5, 9, 10, 10, 12.5, 12.5, 13};
    for (std::size_t k = 0; k < kModes; ++k) {
        const auto n = static_cast<double>(k + 1);
        switch (m) {
        case Beam:
            r[k] = beam[k];
            break;
        case Marimba:
            // The bar's first three modes tuned 1 : 4 : 10, the rest the
            // beam's above them in the same proportion as the third.
            r[k] = k == 0 ? 1 : k == 1 ? 4 : k == 2 ? 10 : beam[k] * 10 / beam[2];
            break;
        case Membrane:
            r[k] = membrane[k];
            break;
        case Plate:
            r[k] = plate[k];
            break;
        case Pipe:
            r[k] = 2 * n - 1;
            break;
        default:   // String, Tube
            r[k] = n;
            break;
        }
    }
    return r;
}

Corpus::Corpus() noexcept {
    for (std::size_t i = 0; i < Count; ++i) {
        p_[i] = ranges[i].initial;
    }
    prepare(48000);
}

void Corpus::prepare(double sampleRate) noexcept {
    rate_ = std::isfinite(sampleRate) && sampleRate > 0 ? sampleRate : 48000;
    for (auto& m : mode_) {
        m.y1 = m.y2 = 0;
    }
    for (auto* g : {&gain_, &bleed_, &wet_}) {
        g->prepare(rate_);
    }
    update();
}

void Corpus::set(Param p, double v) noexcept {
    const int index = static_cast<int>(p);
    if (index < 0 || index >= static_cast<int>(Count) || !std::isfinite(v)) {
        return;
    }
    const auto& r = ranges[static_cast<std::size_t>(index)];
    p_[static_cast<std::size_t>(index)] = std::clamp(index == Type ? std::round(v) : v, r.low, r.high);
    update();
}

void Corpus::update() noexcept {
    const auto r = ratios(static_cast<Model>(static_cast<int>(p_[Type])));
    const double f1 = p_[Tune] * std::exp2(p_[Fine] / 1200), t1 = t60(p_[Decay]);
    const double m = (1 + p_[Material] / 100) / 2, stretch = 1 + p_[Inharmonics] / 1000;
    const double hit = p_[Hit] / 100, posL = p_[ListenL] / 100, posR = p_[ListenR] / 100;
    for (std::size_t k = 0; k < kModes; ++k) {
        auto& mode = mode_[k];
        const double ratio = std::pow(r[k], stretch), hz = f1 * ratio;
        if (!(hz < 0.45 * rate_)) {
            mode.hz = mode.a1 = mode.a2 = mode.in = mode.left = mode.right = 0;
            continue;
        }
        const double w = 2 * std::numbers::pi * hz / rate_;
        const double t = t1 * std::pow(1 / ratio, m), radius = std::pow(10.0, -3 / (t * rate_));
        mode.hz = hz;
        mode.a1 = 2 * radius * std::cos(w);
        mode.a2 = -radius * radius;
        const auto n = static_cast<double>(k + 1);
        // sin(w) gives each mode a unit impulse envelope; 48 kHz / rate keeps
        // a hit of a given length ringing at the same level at every rate
        // (the hit has rate / 48 kHz times the samples).
        mode.in = std::sin(w) * (48000 / rate_) * std::abs(std::sin(std::numbers::pi * n * hit)) * std::pow(ratio, p_[Brightness] / 200);
        mode.left = std::sin(std::numbers::pi * n * posL);
        mode.right = std::sin(std::numbers::pi * n * posR);
    }
    gain_.set(0.25 * std::pow(10.0, p_[Gain] / 20));
    bleed_.set(p_[Bleed] / 100);
    wet_.set(p_[DryWet] / 100);
}

void Corpus::process(const float* inL, const float* inR, float* outL, float* outR, std::size_t frames) noexcept {
    for (std::size_t i = 0; i < frames; ++i) {
        // Both inputs are read before either output is written (a host may
        // hand an output plane that is an input plane).
        const double l = inL && std::isfinite(inL[i]) ? inL[i] : 0.0, r = inR && std::isfinite(inR[i]) ? inR[i] : 0.0;
        const double x = 0.5 * (l + r);
        double yl = 0, yr = 0;
        for (auto& m : mode_) {
            if (m.hz == 0) {
                continue;
            }
            const double y = m.a1 * m.y1 + m.a2 * m.y2 + m.in * x;
            m.y2 = settle(m.y1);
            m.y1 = settle(y);
            yl += m.left * y;
            yr += m.right * y;
        }
        // The resonance passes a tanh: a tone held at a mode builds up by the
        // mode's Q (thousands at a long Decay), and is held under full scale.
        const double g = gain_.next(), b = bleed_.next(), mix = wet_.next();
        const double wetL = std::tanh(g * yl) + b * x, wetR = std::tanh(g * yr) + b * x;
        if (outL) {
            outL[i] = static_cast<float>(l + (wetL - l) * mix);
        }
        if (outR) {
            outR[i] = static_cast<float>(r + (wetR - r) * mix);
        }
    }
}

}  // namespace adi::dsp
