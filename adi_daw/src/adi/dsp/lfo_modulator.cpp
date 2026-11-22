// SPDX-License-Identifier: GPL-3.0-or-later
#include "adi/dsp/lfo_modulator.hpp"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace adi::dsp {
namespace {
constexpr double kTwoPi = 2 * std::numbers::pi;
constexpr std::uint64_t kValueSalt = 0x9E3779B97F4A7C15ull, kJitterSalt = 0xC2B2AE3D27D4EB4Full;
// Free running, the phase is kept under this many cycles: subtracted at the
// same sample whatever the blocking, so it never changes a value's bits by
// where the host cut.
constexpr double kWrap = 1048576;
}  // namespace

LfoModulator::LfoModulator() noexcept {
    for (std::size_t i = 0; i < Count; ++i) {
        p_[i] = ranges[i].initial;
    }
    prepare(48000);
}

void LfoModulator::prepare(double sampleRate) noexcept {
    rate_ = std::isfinite(sampleRate) && sampleRate > 0 ? sampleRate : 48000;
    freePhase_ = 0;
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

void LfoModulator::retrigger() noexcept { freePhase_ = 0; }

double LfoModulator::hash(std::int64_t cycle, std::uint64_t salt) noexcept {
    // SplitMix64 of the cycle's index: a value in [-1, 1) that is a function
    // of the position alone.
    std::uint64_t z = static_cast<std::uint64_t>(cycle) + salt;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    z ^= z >> 31;
    return static_cast<double>(z >> 11) / 4503599627370496.0 - 1.0;   // 2^52
}

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
        return hash(cycle, kValueSalt);
    case Wander: {
        const double from = hash(cycle, kValueSalt), to = hash(cycle + 1, kValueSalt);
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
    const double free = p_[Rate] / rate_;
    const double perSample = position.bpm > 0 && std::isfinite(position.bpm) ? position.bpm / (60 * rate_) : 0;
    for (std::size_t i = 0; i < frames; ++i) {
        // The position in cycles, then Phase, then this cycle's jitter.
        double cycles = 0;
        if (synced) {
            const auto at = position.sample + static_cast<std::int64_t>(i) - position.originSample;
            cycles = (position.originQuarterNotes + static_cast<double>(at) * perSample) / quarters;
        } else {
            cycles = freePhase_;
        }
        cycles += offset;
        std::int64_t cycle = static_cast<std::int64_t>(std::floor(cycles));
        if (p_[Jitter] > 0) {
            cycles += 0.5 * p_[Jitter] * hash(cycle, kJitterSalt);
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
        if (!synced) {
            freePhase_ += free;
            if (freePhase_ >= kWrap) {
                freePhase_ -= kWrap;
            }
        }
    }
}

}  // namespace adi::dsp
