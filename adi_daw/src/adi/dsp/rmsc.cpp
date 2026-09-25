// SPDX-License-Identifier: GPL-3.0-or-later

#include "adi/dsp/rmsc.hpp"

#include <cmath>

namespace adi::dsp {

void RingModSidechain::prepare(double sampleRate) noexcept {
    sr_ = sampleRate > 0.0 ? sampleRate : 48000.0;
    setSmoothingHz(smoothHz_);
    setReleaseMs(releaseMs_);
    env_ = 0.0;
    held_ = 0.0;
}

void RingModSidechain::setThresholdDb(double db) noexcept {
    thresholdDb_ = db > 0.0 ? 0.0 : (db < -96.0 ? -96.0 : db);
    invThreshold_ = 1.0 / std::pow(10.0, thresholdDb_ / 20.0);
}

void RingModSidechain::setReleaseMs(double ms) noexcept {
    releaseMs_ = ms > 0.0 ? ms : 0.0;
    // Per-sample decay towards the key: exp(-1 / (tau * fs)), so after tau the
    // envelope has fallen to 1/e of where it started.
    releaseK_ = releaseMs_ > 0.0 ? std::exp(-1000.0 / (releaseMs_ * sr_)) : 0.0;
}

void RingModSidechain::setSmoothingHz(double hz) noexcept {
    smoothHz_ = hz > 0.0 ? hz : 0.0;
    constexpr double kPi = 3.14159265358979323846;
    smoothK_ = smoothHz_ > 0.0 ? 1.0 - std::exp(-2.0 * kPi * smoothHz_ / sr_) : 1.0;
}

void RingModSidechain::process(const float* const* mainIn, float* const* mainOut,
                               int channels, const float* sidechain, int frames,
                               float* gainOut) noexcept {
    for (int i = 0; i < frames; ++i) {
        const double rect = sidechain != nullptr ? std::fabs(static_cast<double>(sidechain[i])) : 0.0;
        env_ += (rect - env_) * smoothK_;
        // CLAMPED, after the threshold scales it. Unclamped, a key above the
        // threshold drives the gain below zero and the main signal comes out
        // upside down.
        const double scaled = env_ * invThreshold_;
        const double e = scaled > 1.0 ? 1.0 : scaled;
        // The release: instant up, exponential down. With releaseK_ = 0 this
        // is `held_ = e`, the classic behaviour, bit for bit.
        held_ = e >= held_ ? e : e + (held_ - e) * releaseK_;
        const double gain = 1.0 - depth_ * held_;
        for (int c = 0; c < channels; ++c)
            mainOut[c][i] = static_cast<float>(static_cast<double>(mainIn[c][i]) * gain);
        if (gainOut != nullptr) gainOut[i] = static_cast<float>(gain);
    }
}

}  // namespace adi::dsp
