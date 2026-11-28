// SPDX-License-Identifier: GPL-3.0-or-later
#include "adi/dsp/lfo_modulator.hpp"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace adi::dsp {
namespace {
constexpr double kTwoPi = 2 * std::numbers::pi;
constexpr std::uint64_t kValueSalt = 0x9E3779B97F4A7C15ull, kJitterSalt = 0xC2B2AE3D27D4EB4Full;
}  // namespace

LfoModulator::LfoModulator() noexcept {
    for (std::size_t i = 0; i < Count; ++i) {
        p_[i] = ranges[i].initial;
    }
    prepare(48000);
}

void LfoModulator::prepare(double sampleRate) noexcept {
    rate_ = std::isfinite(sampleRate) && sampleRate > 0 ? sampleRate : 48000;
    clock_.prepare(rate_);
    smoothed_ = 0;
    primed_ = false;
    update();
}

void LfoModulator::set(Param p, double v) noexcept {
    const int index = static_cast<int>(p);
    if (index < 0 || index >= static_cast<int>(Count) || !std::isfinite(v)) {
        return;
    }
    const auto& r = ranges[static_cast<std::size_t>(index)];
    p_[static_cast<std::size_t>(index)] = std::clamp(v, r.low, r.high);
    update();
}

void LfoModulator::update() noexcept {
    waveform_ = static_cast<int>(std::lround(p_[Shape]));
    // Smooth up to 50 ms; at 0 the waveform passes as it is.
    const double seconds = 0.05 * p_[Smooth];
    smoothing_ = seconds > 0 ? 1 - std::exp(-1 / (seconds * rate_)) : 1;
}

void LfoModulator::retrigger() noexcept { clock_.retrigger(); }

double LfoModulator::shape(double phase, std::int64_t cycle) const noexcept {
    switch (waveform_) {
    case Triangle:
        return phase < 0.25 ? 4 * phase : phase < 0.75 ? 2 - 4 * phase : 4 * phase - 4;
    case SawUp:
        return 2 * phase - 1;
    case SawDown:
        return 1 - 2 * phase;
    case Square:
        return phase < 0.5 ? 1.0 : -1.0;
    case Random:
        return ModulatorClock::hash(cycle, kValueSalt);
    case Wander: {
        const double from = ModulatorClock::hash(cycle, kValueSalt), to = ModulatorClock::hash(cycle + 1, kValueSalt);
        return from + (to - from) * 0.5 * (1 - std::cos(std::numbers::pi * phase));
    }
    default:
        return std::sin(kTwoPi * phase);
    }
}

void LfoModulator::render(double* out, std::size_t frames, const Position& position) noexcept {
    if (out == nullptr) {
        return;
    }
    const bool synced = p_[Sync] >= 0.5;
    const double offset = p_[Phase] / 360;
    const auto choice = static_cast<std::size_t>(std::clamp<long>(std::lround(p_[Division]), 0, 11));
    const double quarters = kDivisionQuarters[choice];
    for (std::size_t i = 0; i < frames; ++i) {
        // The position in cycles, then Phase, then this cycle's jitter.
        double cycles = clock_.cycles(i, synced, quarters, position) + offset;
        std::int64_t cycle = static_cast<std::int64_t>(std::floor(cycles));
        if (p_[Jitter] > 0) {
            cycles += 0.5 * p_[Jitter] * ModulatorClock::hash(cycle, kJitterSalt);
            cycle = static_cast<std::int64_t>(std::floor(cycles));
        }
        const double phase = cycles - static_cast<double>(cycle);
        const double raw = shape(phase, cycle);
        if (!primed_) {
            smoothed_ = raw;
            primed_ = true;
        }
        smoothed_ += (raw - smoothed_) * smoothing_;
        if (std::abs(raw - smoothed_) < 1e-12) {
            smoothed_ = raw;   // settles exactly on its target, never into subnormals
        }
        out[i] = p_[Offset] + p_[Depth] * smoothed_;
        clock_.step(synced, p_[Rate]);
    }
}

}  // namespace adi::dsp
