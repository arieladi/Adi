// SPDX-License-Identifier: GPL-3.0-or-later
#include "adi/dsp/auto_pan.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <numbers>

namespace adi::dsp {
namespace {
constexpr std::uint64_t kRandomSalt = 0xA0761D6478BD642Full;
}  // namespace

AutoPan::AutoPan() noexcept {
    for (std::size_t i = 0; i < Count; ++i) {
        p_[i] = ranges[i].initial;
    }
    prepare(48000);
}

void AutoPan::prepare(double sampleRate) noexcept {
    const double rate = std::isfinite(sampleRate) && sampleRate > 0 ? sampleRate : 48000;
    clock_.prepare(rate);
    gain_ = {1, 1};
    amount_.prepare(rate);
    amount_.set(p_[Amount] / 100);
}

void AutoPan::set(Param p, double v) noexcept {
    const int index = static_cast<int>(p);
    if (index < 0 || index >= static_cast<int>(Count) || !std::isfinite(v)) {
        return;
    }
    const auto& r = ranges[static_cast<std::size_t>(index)];
    p_[static_cast<std::size_t>(index)] = std::clamp(v, r.low, r.high);
}

void AutoPan::tempo(double bpm) noexcept {
    if (std::isfinite(bpm) && bpm > 0) {
        bpm_ = std::clamp(bpm, 20.0, 999.0);
    }
}

double AutoPan::wave(double cycles) const noexcept {
    const auto cycle = static_cast<std::int64_t>(std::floor(cycles));
    const double phase = cycles - static_cast<double>(cycle);
    double w = 0;
    switch (static_cast<int>(std::lround(p_[Wave]))) {
    case Triangle:
        w = phase < 0.25 ? 4 * phase : phase < 0.75 ? 2 - 4 * phase : 4 * phase - 4;
        break;
    case SawDown:
        w = 1 - 2 * phase;
        break;
    case Random:
        w = ModulatorClock::hash(cycle, kRandomSalt);
        break;
    default:
        w = std::sin(2 * std::numbers::pi * phase);
        break;
    }
    // Shape hardens the wave toward a square: up to 5 times, clipped.
    w = std::clamp(w * (1 + 4 * p_[Shape] / 100), -1.0, 1.0);
    return p_[Invert] >= 0.5 ? -w : w;
}

void AutoPan::process(const float* inL, const float* inR, float* outL, float* outR, std::size_t frames) noexcept {
    const bool tremolo = std::lround(p_[Mode]) == Tremolo;
    const auto choice = static_cast<std::size_t>(std::clamp<long>(std::lround(p_[Division]), 0, 11));
    const double hz = p_[Sync] >= 0.5 ? bpm_ / 60 / kDivisionQuarters[choice] : p_[Rate];
    const double start = p_[Offset] / 360, apart = p_[Phase] / 360;
    amount_.set(p_[Amount] / 100);   // moved since the last block: it glides, not steps
    const ModulatorClock::Position unused;
    for (std::size_t i = 0; i < frames; ++i) {
        // Both inputs are read before either output is written (a host may
        // hand an output plane that is an input plane).
        const double l = inL && std::isfinite(inL[i]) ? inL[i] : 0.0;
        const double r = inR && std::isfinite(inR[i]) ? inR[i] : 0.0;
        const double cycles = clock_.cycles(i, false, 1, unused) + start;
        const double left = wave(cycles), right = tremolo ? left : wave(cycles + apart), amount = amount_.next();
        gain_[0] = 1 - amount * (1 - (1 + left) / 2);
        gain_[1] = 1 - amount * (1 - (1 + right) / 2);
        if (outL) {
            outL[i] = static_cast<float>(l * gain_[0]);
        }
        if (outR) {
            outR[i] = static_cast<float>(r * gain_[1]);
        }
        clock_.step(false, hz);
    }
}

}  // namespace adi::dsp
