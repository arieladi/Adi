// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include "adi/engine/process.hpp"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <numbers>

namespace adi::ui {
// A diagnostic signal added only at the device output, outside recording and export.
class AudioTestTone {
  public:
    void set(bool enabled, float levelDb, int frequencyHz) noexcept {
        frequency_.store(frequencyHz, std::memory_order_relaxed);
        gain_.store(enabled ? std::pow(10.f, levelDb / 20.f) : 0.f, std::memory_order_relaxed);
    }

    // Audio thread. Ramp the level at the switch to avoid clicks in the monitors.
    void mix(const engine::AudioIo &io, double sampleRate) noexcept {
        const auto target = gain_.load(std::memory_order_relaxed);
        if ((target == 0.f && currentGain_ == 0.f) || io.out == nullptr || sampleRate <= 0.)
            return;
        constexpr double turn = 2. * std::numbers::pi;
        const auto frequency = (std::min)(static_cast<double>(frequency_.load(std::memory_order_relaxed)),
                                          sampleRate * 0.49);
        const auto step = turn * frequency / sampleRate;
        const float ramp = 1.f / (static_cast<float>(sampleRate) * 0.01f);
        for (int frame = 0; frame < io.frames; ++frame) {
            currentGain_ += std::clamp(target - currentGain_, -ramp, ramp);
            if (currentGain_ == 0.f) {
                phase_ = 0.;
                break;
            }
            const auto signal = currentGain_ * static_cast<float>(std::sin(phase_));
            phase_ += step;
            if (phase_ >= turn)
                phase_ -= turn;
            for (int channel = 0; channel < io.numOut; ++channel)
                if (auto *out = io.out[channel])
                    out[frame] = std::clamp(out[frame] + signal, -1.f, 1.f);
        }
    }

  private:
    std::atomic<float> gain_{0.f};
    std::atomic<int> frequency_{440};
    float currentGain_ = 0.f;
    double phase_ = 0.;
};
} // namespace adi::ui
