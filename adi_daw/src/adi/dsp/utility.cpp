// SPDX-License-Identifier: MIT
#include "adi/dsp/utility.hpp"
#include <algorithm>
#include <cmath>
#include <numbers>
namespace adi::dsp {
void Utility::prepare(double rate) noexcept {
    rate_ = std::isfinite(rate) && rate > 0 ? rate : 48000;
    low1_ = {};
    low2_ = {};
    dcIn_ = {};
    dcOut_ = {};
    started_ = false;
    dc_ = std::exp(-2 * std::numbers::pi * 5 / rate_);
    set(BassFreq, p_[BassFreq]);
    gain_ = p_[Gain] <= -120 ? 0 : std::pow(10., p_[Gain] / 20);
    balance_ = p_[Balance] / 100;
}
void Utility::set(Param id, double value) noexcept {
    constexpr std::array<double, Count> lo{0, 0, 0, 0, 0, -100, 0, 0, 50, 0, -120, -100, 0, 0};
    constexpr std::array<double, Count> hi{1, 1, 3, 400, 1, 100, 1, 1, 500, 1, 35, 100, 1, 1};
    if (id < 0 || id >= Count || !std::isfinite(value))
        return;
    const auto i = static_cast<std::size_t>(id);
    p_[i] = std::clamp(value, lo[i], hi[i]);
    if (id == BassFreq)
        low_ = 1 - std::exp(-2 * std::numbers::pi * p_[BassFreq] / rate_);
}
void Utility::process(const float *il, const float *ir, float *ol, float *orr,
                      std::size_t n) noexcept {
    const double target = p_[Gain] <= -120 ? 0 : std::pow(10., p_[Gain] / 20),
                 pan = p_[Balance] / 100;
    if (!started_) {
        gain_ = target;
        balance_ = pan;
        started_ = true;
    }
    const double ramp = 1 - std::exp(-1 / (rate_ * 0.005));
    for (std::size_t i = 0; i < n; ++i) {
        // Cache both inputs before writing: Pd may reuse signal buffers.
        double l = il[i] * (p_[PhaseL] >= 0.5 ? -1 : 1), r = ir[i] * (p_[PhaseR] >= 0.5 ? -1 : 1);
        const int mode = static_cast<int>(std::round(p_[Channel]));
        if (mode == 0)
            r = l;
        else if (mode == 1)
            l = r;
        else if (mode == 2)
            std::swap(l, r);
        double mid = (l + r) * 0.5, side = (l - r) * 0.5;
        if (mode >= 2) {
            if (p_[MidSideMode] >= 0.5) {
                mid *= 1 - std::max(0., p_[MidSide] / 100);
                side *= 1 + std::min(0., p_[MidSide] / 100);
            } else
                side *= p_[Width] / 100;
        }
        if (p_[Mono] >= 0.5)
            side = 0;
        l = mid + side;
        r = mid - side;
        std::array<double, 2> values{l, r}, bass{};
        for (std::size_t c = 0; c < 2; ++c) {
            low1_[c] += low_ * (values[c] - low1_[c]);
            low2_[c] += low_ * (low1_[c] - low2_[c]);
            bass[c] = low2_[c];
        }
        if (p_[BassMono] >= 0.5) {
            const double m = (bass[0] + bass[1]) * 0.5;
            values[0] += m - bass[0];
            values[1] += m - bass[1];
            if (p_[BassAudition] >= 0.5)
                values = {m, m};
        }
        gain_ += (target - gain_) * ramp;
        balance_ += (pan - balance_) * ramp;
        if (std::abs(target - gain_) < 1e-12)
            gain_ = target;
        for (std::size_t c = 0; c < 2; ++c) {
            const double x = values[c];
            const double filtered = x - dcIn_[c] + dc_ * dcOut_[c];
            dcIn_[c] = x;
            dcOut_[c] = filtered;
            if (p_[DC] >= 0.5)
                values[c] = filtered;
        }
        const double g = p_[Mute] >= 0.5 ? 0 : gain_;
        ol[i] = static_cast<float>(values[0] * g * (1 - std::max(0., balance_)));
        orr[i] = static_cast<float>(values[1] * g * (1 + std::min(0., balance_)));
    }
}
} // namespace adi::dsp
