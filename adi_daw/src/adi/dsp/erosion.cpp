// SPDX-License-Identifier: GPL-3.0-or-later
#include "adi/dsp/erosion.hpp"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace adi::dsp {
namespace {
double settle(double x) noexcept { return std::abs(x) < 1e-30 ? 0 : x; }
}  // namespace

double Erosion::Band::run(double x) noexcept {
    const double y = b0 * x + s1;
    s1 = settle(s2 - a1 * y);
    s2 = settle(-b0 * x - a2 * y);
    return y;
}

Erosion::Erosion() noexcept {
    for (std::size_t i = 0; i < Count; ++i) {
        p_[i] = ranges[i].initial;
    }
    prepare(48000);
}

void Erosion::prepare(double sampleRate) noexcept {
    rate_ = std::isfinite(sampleRate) && sampleRate > 0 ? sampleRate : 48000;
    for (auto& r : ring_) {
        r.fill(0.f);
    }
    for (auto& b : band_) {
        b.s1 = b.s2 = 0;
    }
    seed_ = {0x9E3779B9u, 0x7F4A7C15u};
    write_ = 0;
    phase_ = 0;
    smooth_ = 1 - std::exp(-1 / (0.010 * rate_ / 5));   // about 10 ms to settle
    update();
    depth_ = depthTarget_;
}

void Erosion::set(Param p, double v) noexcept {
    const int index = static_cast<int>(p);
    if (index < 0 || index >= static_cast<int>(Count) || !std::isfinite(v)) {
        return;
    }
    const auto& r = ranges[static_cast<std::size_t>(index)];
    p_[static_cast<std::size_t>(index)] = std::clamp(index == Mode ? std::round(v) : v, r.low, r.high);
    update();
}

void Erosion::update() noexcept {
    const double f = std::min(p_[Frequency], 0.45 * rate_);
    step_ = f / rate_;
    // A constant-peak (0 dB) band pass at f, Q = 1 / Width (RBJ).
    const double q = 1 / p_[Width], w = 2 * std::numbers::pi * f / rate_, alpha = std::sin(w) / (2 * q);
    const double a0 = 1 + alpha;
    for (auto& b : band_) {
        b.b0 = alpha / a0;
        b.a1 = -2 * std::cos(w) / a0;
        b.a2 = (1 - alpha) / a0;
    }
    // White noise of variance 1/3 through it has a variance of (1/3) x its
    // noise gain, the sum of h^2, which for this band pass is exactly
    // alpha / (1 + alpha): scaled to an RMS of 0.5. (The small-angle w / 2Q
    // left the noise up to 12 dB short at a high, wide band.)
    const double rms = std::sqrt(alpha / (3 * (1 + alpha)));
    scale_ = rms > 0 ? 0.5 / rms : 0;
    // The swing stays inside the ring: above about 600 kHz Amount's top is
    // held at kMaxDepth samples rather than reading samples already replaced.
    depthTarget_ = std::min(p_[Amount] / 100 * kDeviationMs / 1000 * rate_, kMaxDepth);
}

double Erosion::noise(std::uint32_t& state) noexcept {
    state ^= state << 13;
    state ^= state >> 17;
    state ^= state << 5;
    return static_cast<double>(state) / 2147483647.5 - 1;
}

double Erosion::read(std::size_t channel, double delay) const noexcept {
    // Four-point Lagrange over the samples k .. k+3 back, k = max(0, floor(d) - 1).
    const auto whole = static_cast<std::size_t>(delay);
    const std::size_t k = whole > 0 ? whole - 1 : 0;
    const double t = delay - static_cast<double>(k);   // in [0, 3)
    const auto& r = ring_[channel];
    const auto at = [&](std::size_t back) { return static_cast<double>(r[(write_ + kRing - back) % kRing]); };
    const double w0 = -(t - 1) * (t - 2) * (t - 3) / 6, w1 = t * (t - 2) * (t - 3) / 2;
    const double w2 = -t * (t - 1) * (t - 3) / 2, w3 = t * (t - 1) * (t - 2) / 6;
    return w0 * at(k) + w1 * at(k + 1) + w2 * at(k + 2) + w3 * at(k + 3);
}

void Erosion::process(const float* inL, const float* inR, float* outL, float* outR, std::size_t frames) noexcept {
    const auto mode = static_cast<int>(p_[Mode]);
    const float* in[2] = {inL, inR};
    float* out[2] = {outL, outR};
    for (std::size_t i = 0; i < frames; ++i) {
        // Both inputs are read before either output is written (a host may
        // hand an output plane that is an input plane).
        for (std::size_t c = 0; c < 2; ++c) {
            const float x = in[c] ? in[c][i] : 0.f;
            ring_[c][write_] = std::isfinite(x) ? x : 0.f;
        }
        depth_ += (depthTarget_ - depth_) * smooth_;
        if (std::abs(depthTarget_ - depth_) < 1e-12) {
            depth_ = depthTarget_;
        }
        std::array<double, 2> m{};
        if (mode == Sine) {
            m[0] = m[1] = std::sin(2 * std::numbers::pi * phase_);
            phase_ += step_;
            phase_ -= std::floor(phase_);
        } else {
            // The generators run in every noise mode, so a switch between
            // them is not a jump in the noise's history.
            for (std::size_t c = 0; c < 2; ++c) {
                m[c] = std::clamp(scale_ * band_[c].run(noise(seed_[c])), -1.0, 1.0);
            }
            if (mode == Noise) {
                m[1] = m[0];
            }
        }
        for (std::size_t c = 0; c < 2; ++c) {
            const double y = depth_ > 0 ? read(c, depth_ * (1 + m[c])) : static_cast<double>(ring_[c][write_]);
            if (out[c]) {
                out[c][i] = static_cast<float>(y);
            }
        }
        write_ = (write_ + 1) % kRing;
    }
}

}  // namespace adi::dsp
