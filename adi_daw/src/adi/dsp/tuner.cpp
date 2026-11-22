// SPDX-License-Identifier: GPL-3.0-or-later
#include "adi/dsp/tuner.hpp"

#include <algorithm>
#include <cmath>

namespace adi::dsp {

Tuner::Tuner() {
    for (std::size_t i = 0; i < Count; ++i) {
        p_[i] = ranges[i].initial;
    }
    prepare(48000);
}

void Tuner::prepare(double sampleRate) {
    detector_.prepare(sampleRate);
    hz_.store(0);
    clarity_.store(0);
    valid_.store(false);
}

void Tuner::set(Param p, double v) noexcept {
    const int index = static_cast<int>(p);
    if (index < 0 || index >= static_cast<int>(Count) || !std::isfinite(v)) {
        return;
    }
    const auto& r = ranges[static_cast<std::size_t>(index)];
    p_[static_cast<std::size_t>(index)] = std::clamp(v, r.low, r.high);
    reference_.store(p_[Reference]);
}

double Tuner::noteOf(double hz, double reference) noexcept {
    if (!(hz > 0) || !(reference > 0)) {
        return 0;
    }
    return 69 + 12 * std::log2(hz / reference);
}

Tuner::Reading Tuner::reading() const noexcept {
    Reading r;
    r.valid = valid_.load();
    if (!r.valid) {
        return r;
    }
    r.hz = hz_.load();
    r.clarity = clarity_.load();
    r.note = noteOf(r.hz, reference_.load());
    r.nearest = static_cast<int>(std::lround(r.note));
    r.cents = 100 * (r.note - r.nearest);
    return r;
}

void Tuner::process(const float* inL, const float* inR, float* outL, float* outR, std::size_t frames) noexcept {
    for (std::size_t at = 0; at < frames; at += kChunk) {
        const std::size_t n = std::min(kChunk, frames - at);
        // Both inputs are read into the chunk before either output is
        // written (a host may hand an output plane that is an input plane,
        // even the other channel's). The signal passes untouched.
        std::array<float, kChunk> l{}, r{};
        for (std::size_t i = 0; i < n; ++i) {
            l[i] = inL ? inL[at + i] : 0.f;
            r[i] = inR ? inR[at + i] : 0.f;
            mono_[i] = 0.5f * (l[i] + r[i]);
        }
        if (outL) {
            std::copy(l.begin(), l.begin() + static_cast<std::ptrdiff_t>(n), outL + at);
        }
        if (outR) {
            std::copy(r.begin(), r.begin() + static_cast<std::ptrdiff_t>(n), outR + at);
        }
        detector_.push(mono_.data(), n);
        double hz = 0, clarity = 0;
        if (detector_.estimate(hz, clarity) && clarity >= kClarity) {
            hz_.store(hz);
            clarity_.store(clarity);
            valid_.store(true);
        }
    }
}

}  // namespace adi::dsp
