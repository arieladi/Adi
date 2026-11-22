// SPDX-License-Identifier: GPL-3.0-or-later
#include "adi/dsp/spectrum_device.hpp"

#include <algorithm>
#include <cmath>

namespace adi::dsp {

SpectrumDevice::SpectrumDevice() noexcept {
    for (std::size_t i = 0; i < Count; ++i) {
        p_[i] = ranges[i].initial;
    }
    prepare(48000);
}

void SpectrumDevice::prepare(double sampleRate) noexcept {
    rate_ = std::isfinite(sampleRate) && sampleRate > 0 ? sampleRate : 48000;
    update();
}

void SpectrumDevice::set(Param p, double v) noexcept {
    const int index = static_cast<int>(p);
    if (index < 0 || index >= static_cast<int>(Count) || !std::isfinite(v)) {
        return;
    }
    const auto& r = ranges[static_cast<std::size_t>(index)];
    const bool whole = index == Block || index == Channel || index == Graph || index == ScaleX || index == Auto;
    p_[static_cast<std::size_t>(index)] = std::clamp(whole ? std::round(v) : v, r.low, r.high);
    update();
}

float SpectrumDevice::pick(ChannelKind c, float left, float right) noexcept {
    switch (c) {
    case Left:
        return left;
    case Right:
        return right;
    default:
        return 0.5f * (left + right);
    }
}

void SpectrumDevice::update() noexcept {
    Analysis a;
    a.fftSize = 1024 << static_cast<int>(p_[Block]);
    a.channel = static_cast<ChannelKind>(static_cast<int>(p_[Channel]));
    a.hopSamples = std::clamp(static_cast<std::int32_t>(std::lround(p_[Refresh] / 1000 * rate_)), std::int32_t{1},
                              LinearMagnitudeAverage::kMaximumHop);
    // Each analysis keeps 1 - 1/Avg of the average: the averager's decay is
    // the time that takes to fall 20 dB, 0.1 = (1 - 1/Avg)^(analyses), so
    // decay = hop / rate x ln 0.1 / ln(1 - 1/Avg). Avg 1 keeps a vanishing
    // remainder (1e-9), so each analysis all but replaces the last.
    const double keep = std::max(1 - 1 / p_[Avg], 1e-9);
    a.average.bins = static_cast<std::size_t>(a.fftSize / 2 + 1);
    a.average.sampleRate = rate_;
    a.average.hopSamples = a.hopSamples;
    a.average.decay20dBSeconds = static_cast<double>(a.hopSamples) / rate_ * std::log(0.1) / std::log(keep);
    analysis_ = a;
}

void SpectrumDevice::process(const float* inL, const float* inR, float* outL, float* outR, std::size_t frames) noexcept {
    for (std::size_t i = 0; i < frames; ++i) {
        const float l = inL ? inL[i] : 0.f, r = inR ? inR[i] : 0.f;
        if (outL) {
            outL[i] = l;
        }
        if (outR) {
            outR[i] = r;
        }
    }
}

}  // namespace adi::dsp
