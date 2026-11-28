// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <numbers>

namespace adi::dsp {

// Stereo detector input. Each active edge is a pair of trapezoidal one-poles.
class AdiShaperFollowerFilter {
  public:
    void prepare(double sampleRate) noexcept {
        if (!std::isfinite(sampleRate) || sampleRate <= 0.) return;
        rate_ = sampleRate;
        update(true);
        reset();
    }

    void reset() noexcept {
        high_.channels = {};
        low_.channels = {};
    }

    void setHighPass(float percent) noexcept {
        if (!std::isfinite(percent)) return;
        percent = std::clamp(percent, 0.f, 95.f);
        if (percent != hp_) {
            hp_ = percent;
            update(false);
        }
    }

    void setLowPass(float percent) noexcept {
        if (!std::isfinite(percent)) return;
        percent = std::clamp(percent, 0.f, 100.f);
        if (percent != lp_) {
            lp_ = percent;
            update(false);
        }
    }

    void process(float& left, float& right) noexcept {
        if (!high_.active && !low_.active) return;
        double l = left, r = right;
        if (high_.active) {
            l = high_.tick<true>(l, 0);
            r = high_.tick<true>(r, 1);
        }
        if (low_.active) {
            l = low_.tick<false>(l, 0);
            r = low_.tick<false>(r, 1);
        }
        left = static_cast<float>(l);
        right = static_cast<float>(r);
    }

  private:
    struct Section {
        std::array<std::array<double, 2>, 2> channels{};
        double gain = 0.;
        float cutoff = -1.f;
        bool active = false;

        void tune(float position, bool enabled, double rate, bool force) noexcept {
            active = enabled;
            if (active && (force || position != cutoff)) {
                constexpr double e0Hz = 20.6017223070544;
                const double hz = e0Hz * std::exp2(static_cast<double>(position) * .1);
                const double g = std::tan(std::numbers::pi * std::min(.4999, hz / rate));
                gain = g / (1. + g);
            }
            cutoff = position;
        }

        template<bool high>
        double tick(double sample, std::size_t channel) noexcept {
            for (double& state : channels[channel]) {
                const double change = gain * (sample - state);
                const double filtered = state + change;
                state = filtered + change;
                if constexpr (high) sample -= filtered;
                else sample = filtered;
            }
            return sample;
        }
    };

    void update(bool force) noexcept {
        // Keep both requested positions. Moving the lower edge can push the
        // upper edge open. An open edge freezes its own two integrators.
        const float effectiveLp = std::max(lp_, hp_ + 5.f);
        high_.tune(hp_, hp_ > 0.f, rate_, force);
        low_.tune(effectiveLp, effectiveLp < 100.f, rate_, force);
    }

    double rate_ = 48000.;
    float hp_ = 0.f, lp_ = 100.f;
    Section high_{}, low_{};
};

} // namespace adi::dsp
