// SPDX-License-Identifier: GPL-3.0-or-later
#include "adi/dsp/dj_filter.hpp"

#include <algorithm>
#include <cmath>

namespace adi::dsp {

DjFilter::DjFilter() {
    core_.set(AutoFilter::Type, AutoFilter::DJ);
    for (std::size_t i = 0; i < Count; ++i) {
        p_[i] = ranges[i].initial;
        core_.set(kTarget[i], p_[i]);
    }
    prepare(48000);
}

void DjFilter::prepare(double sampleRate) { core_.prepare(std::isfinite(sampleRate) && sampleRate > 0 ? sampleRate : 48000); }

void DjFilter::set(Param p, double v) noexcept {
    const int index = static_cast<int>(p);
    if (index < 0 || index >= static_cast<int>(Count) || !std::isfinite(v)) {
        return;
    }
    const auto& r = ranges[static_cast<std::size_t>(index)];
    p_[static_cast<std::size_t>(index)] = std::clamp(v, r.low, r.high);
    core_.set(kTarget[static_cast<std::size_t>(index)], p_[static_cast<std::size_t>(index)]);
}

void DjFilter::process(const float* inL, const float* inR, float* outL, float* outR, std::size_t frames) noexcept {
    core_.process(inL, inR, outL, outR, frames);
}

}  // namespace adi::dsp
