// SPDX-License-Identifier: GPL-3.0-or-later

#include "adi/dsp/rmsc.hpp"

#include <cmath>

namespace adi::dsp {

void RingModSidechain::prepare(double sampleRate) noexcept {
    sr_ = sampleRate > 0.0 ? sampleRate : 48000.0;
    setSmoothingHz(smoothHz_);
    env_ = 0.0;
}

void RingModSidechain::setSmoothingHz(double hz) noexcept {
    smoothHz_ = hz > 0.0 ? hz : 0.0;
    constexpr double kPi = 3.14159265358979323846;
    smoothK_ = smoothHz_ > 0.0 ? 1.0 - std::exp(-2.0 * kPi * smoothHz_ / sr_) : 1.0;
}

void RingModSidechain::process(const float* const* mainIn, float* const* mainOut,
                               int channels, const float* sidechain, int frames) noexcept {
    for (int i = 0; i < frames; ++i) {
        const double rect = sidechain != nullptr ? std::fabs(static_cast<double>(sidechain[i])) : 0.0;
        env_ += (rect - env_) * smoothK_;
        // CLAMPED. Unclamped, a key above full scale drives the gain below zero
        // and the main signal comes out upside down.
        const double e = env_ > 1.0 ? 1.0 : env_;
        const double gain = 1.0 - depth_ * e;
        for (int c = 0; c < channels; ++c)
            mainOut[c][i] = static_cast<float>(static_cast<double>(mainIn[c][i]) * gain);
    }
}

}  // namespace adi::dsp
