// SPDX-License-Identifier: GPL-3.0-or-later
#include "adi/dsp/align_delay.hpp"

#include <algorithm>
#include <cmath>

namespace adi::dsp {

AlignDelay::AlignDelay() {
    for (std::size_t i = 0; i < Count; ++i) {
        p_[i] = ranges[i].initial;
    }
    prepare(48000);
}

void AlignDelay::prepare(double sampleRate) {
    rate_ = std::isfinite(sampleRate) && sampleRate > 0 ? sampleRate : 48000;
    // The longest of the three modes' delays: 1 s, 48,000 samples, 300 m at
    // the slowest sound (-30 degrees: 296.5 m/s), plus one for the current sample.
    const double slowest = 331.3 * std::sqrt(1 + ranges[Temperature].low / 273.15);
    const auto longest = std::max({static_cast<std::size_t>(std::ceil(rate_ * ranges[LeftMs].high / 1000)),
                                   static_cast<std::size_t>(ranges[LeftSamples].high),
                                   static_cast<std::size_t>(std::ceil(rate_ * ranges[LeftDistance].high / slowest))});
    for (auto& line : line_) {
        line.assign(longest + 1, 0.f);
    }
    write_ = 0;
    update();
}

void AlignDelay::set(Param p, double v) noexcept {
    const int index = static_cast<int>(p);
    if (index < 0 || index >= static_cast<int>(Count) || !std::isfinite(v)) {
        return;
    }
    const auto& r = ranges[static_cast<std::size_t>(index)];
    p_[static_cast<std::size_t>(index)] = std::clamp(v, r.low, r.high);
    update();
}

double AlignDelay::speedOfSound() const noexcept { return 331.3 * std::sqrt(1 + p_[Temperature] / 273.15); }

void AlignDelay::update() noexcept {
    const auto mode = static_cast<int>(std::lround(p_[Mode]));
    const bool link = p_[Link] >= 0.5;
    const double metres = p_[Units] >= 0.5 ? 0.3048 : 1.0;
    const auto capacity = line_[0].empty() ? 0 : line_[0].size() - 1;
    for (std::size_t c = 0; c < 2; ++c) {
        const std::size_t source = link ? 0 : c;
        double samples = 0;
        if (mode == Samples) {
            samples = p_[source == 0 ? LeftSamples : RightSamples];
        } else if (mode == Distance) {
            samples = p_[source == 0 ? LeftDistance : RightDistance] * metres / speedOfSound() * rate_;
        } else {
            samples = p_[source == 0 ? LeftMs : RightMs] * rate_ / 1000;
        }
        delay_[c] = std::min(capacity, static_cast<std::size_t>(std::llround(samples)));
    }
}

void AlignDelay::process(const float* inL, const float* inR, float* outL, float* outR, std::size_t frames) noexcept {
    const float* in[2] = {inL, inR};
    float* out[2] = {outL, outR};
    const auto size = line_[0].size();
    if (size == 0) {
        return;
    }
    for (std::size_t i = 0; i < frames; ++i) {
        // Both inputs are read before either output is written: a host (Pd)
        // may hand an output plane that is an input plane (a found defect:
        // the left output overwrote the right input before it was read).
        for (std::size_t c = 0; c < 2; ++c) {
            line_[c][write_] = in[c] && std::isfinite(in[c][i]) ? in[c][i] : 0.f;
        }
        for (std::size_t c = 0; c < 2; ++c) {
            if (out[c]) {
                out[c][i] = line_[c][(write_ + size - delay_[c]) % size];
            }
        }
        write_ = (write_ + 1) % size;
    }
}

}  // namespace adi::dsp
