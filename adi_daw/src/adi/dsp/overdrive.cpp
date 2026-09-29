// SPDX-License-Identifier: GPL-3.0-or-later
#include "overdrive.hpp"
#include "byod_ojd.hpp"
#include <algorithm>
#include <cmath>
#include <numbers>
namespace adi::dsp {
void Overdrive::prepare(double rate) noexcept {
    rate_ = std::isfinite(rate) ? std::clamp(rate, 8000., 384000.) : 48000.;
    lane_ = {};
    p_ = target_;
    smooth_ = std::exp(-1. / (.005 * rate_));
    envelope_ = std::exp(-1. / (.02 * rate_));
    dc_ = std::exp(-2 * std::numbers::pi * 20 / rate_);
}
void Overdrive::set(Param id, double value) noexcept {
    if (id < 0 || id >= Count || !std::isfinite(value))
        return;
    constexpr double lo[] = {50, .1, 0, 0, 0, 0}, hi[] = {12000, 6, 100, 100, 100, 100};
    target_[static_cast<std::size_t>(id)] = std::clamp(value, lo[id], hi[id]);
}
void Overdrive::process(const float *l, const float *r, float *ol, float *orr,
                        std::size_t n) noexcept {
    for (std::size_t i = 0; i < n; ++i) {
        for (std::size_t p = 0; p < p_.size(); ++p)
            p_[p] = target_[p] + smooth_ * (p_[p] - target_[p]);
        const double omega = 2 * std::numbers::pi * std::min(p_[Frequency], rate_ * .45) / rate_;
        const double alpha = std::sin(omega) * std::sinh(std::log(2.) * .5 * p_[Bandwidth]);
        const double b0 = alpha / (1 + alpha), a1 = -2 * std::cos(omega) / (1 + alpha),
                     a2 = (1 - alpha) / (1 + alpha);
        const double tonePole =
            std::exp(-2 * std::numbers::pi *
                     std::min(400. * std::pow(40., p_[Tone] / 100), rate_ * .45) / rate_);
        const float drive = static_cast<float>(2 * std::pow(16., p_[Drive] / 100));
        const double dynamics = p_[Dynamics] / 100, mix = p_[Mix] / 100;
        const float input[] = {l ? l[i] : 0, r ? r[i] : 0};
        float *output[] = {ol, orr};
        for (std::size_t c = 0; c < 2; ++c) {
            auto &s = lane_[c];
            const double dry = std::isfinite(input[c]) ? input[c] : 0;
            const double band = b0 * dry + s.z1;
            s.z1 = -a1 * band + s.z2;
            s.z2 = -b0 * dry - a2 * band;
            const double shaped = byod::ojd(static_cast<float>(band), drive);
            s.tone = (1 - tonePole) * shaped + tonePole * s.tone;
            const double wet = s.tone - s.dcIn + dc_ * s.dcOut;
            s.dcIn = s.tone;
            s.dcOut = wet;
            s.inputPower = (1 - envelope_) * band * band + envelope_ * s.inputPower;
            s.outputPower = (1 - envelope_) * wet * wet + envelope_ * s.outputPower;
            const double preserving =
                std::clamp(std::sqrt((s.inputPower + 1e-12) / (s.outputPower + 1e-12)), 0., 4.);
            const double gain = 1 + dynamics * (preserving - 1);
            output[c][i] = static_cast<float>(dry + mix * (wet * gain - dry));
        }
    }
}
} // namespace adi::dsp
