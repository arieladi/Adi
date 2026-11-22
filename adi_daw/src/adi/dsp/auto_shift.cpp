// SPDX-License-Identifier: GPL-3.0-or-later
#include "adi/dsp/auto_shift.hpp"

#include <algorithm>
#include <cmath>

namespace adi::dsp {

AutoShift::AutoShift() {
    for (std::size_t i = 0; i < Count; ++i) {
        p_[i] = ranges[i].initial;
    }
    shifter_.set(Shifter::Mode, 0);
    shifter_.set(Shifter::Window, kWindowMs);
    prepare(48000);
}

void AutoShift::prepare(double sampleRate) {
    rate_ = std::isfinite(sampleRate) && sampleRate > 0 ? sampleRate : 48000;
    detector_.prepare(rate_);
    shifter_.prepare(rate_);
    shifter_.set(Shifter::Mix, 100);
    mix_.prepare(rate_);
    mix_.set(p_[DryWet] / 100);
    target_ = current_ = 0;
    window_ = kWindowMs;
    periods_ = 0;
    clock_ = 0;
    retune();
}

void AutoShift::set(Param p, double v) noexcept {
    const int index = static_cast<int>(p);
    if (index < 0 || index >= static_cast<int>(Count) || !std::isfinite(v)) {
        return;
    }
    const auto& r = ranges[static_cast<std::size_t>(index)];
    const bool whole = index == Correction || index == Root || index == Scale;
    p_[static_cast<std::size_t>(index)] = std::clamp(whole ? std::round(v) : v, r.low, r.high);
    if (index == DryWet) {
        mix_.set(p_[DryWet] / 100);
    }
    retune();
}

double AutoShift::nearest(double note, int root, int scale) noexcept {
    static constexpr std::array<std::array<bool, 12>, 3> kIn = {{
        {true, true, true, true, true, true, true, true, true, true, true, true},
        {true, false, true, false, true, true, false, true, false, true, false, true},
        {true, false, true, true, false, true, false, true, true, false, true, false},
    }};
    const auto& in = kIn[static_cast<std::size_t>(std::clamp(scale, 0, 2))];
    const double centre = std::round(note);
    double best = centre, distance = 1e9;
    for (int k = -6; k <= 6; ++k) {
        const double candidate = centre + k;
        const auto degree = static_cast<std::size_t>(((static_cast<long long>(candidate) - root) % 12 + 12) % 12);
        const double d = std::abs(candidate - note);
        if (in[degree] && d < distance) {   // the lower of two equally near notes wins
            best = candidate;
            distance = d;
        }
    }
    return best;
}

double AutoShift::shift() const noexcept { return p_[Shift] + (p_[Detune] + current_) / 100; }

void AutoShift::retune() noexcept {
    // The whole shift goes through Coarse (a double the Shifter does not
    // round), so Detune and a whole-tone correction are never clamped.
    shifter_.set(Shifter::Coarse, shift());
    shifter_.set(Shifter::Fine, 0);
    shifter_.set(Shifter::Window, window_);
}

double AutoShift::windowFor(double hz, double& periods) noexcept {
    // Half the window spans a whole number of the input's periods, the
    // number nearest kWindowMs / 2: the Shifter's two taps, half a window
    // apart, then read the same phase of the tone and its level holds
    // through every crossfade. (A fixed 50 ms put them in antiphase for
    // tones near (k + 1/2) / 25 ms, 420 or 460 Hz, and the tone fell 40 dB
    // at each crossfade's middle.)
    //
    // The count of periods keeps its value until the reading is more than
    // three quarters of a period from it: a tone near half way between two
    // counts (220 Hz is 5.5 periods in 25 ms) would otherwise flip between
    // them with the reading's last decimals, and each flip moves the window,
    // and with it the Shifter's taps, by a whole period.
    if (!(hz > 0) || !std::isfinite(hz)) {
        return kWindowMs;
    }
    const double period = 1000 / hz, ideal = kWindowMs / 2 / period;
    if (periods < 1 || std::abs(ideal - periods) > 0.75) {
        periods = std::max(1.0, std::round(ideal));
    }
    return std::clamp(2 * periods * period, Shifter::ranges[Shifter::Window].low, Shifter::ranges[Shifter::Window].high);
}

void AutoShift::process(const float* inL, const float* inR, float* outL, float* outR, std::size_t frames) noexcept {
    for (std::size_t at = 0; at < frames;) {
        // Retuned at every 64th sample of the device's own clock, not of the
        // call, so every block size gives the same samples.
        if (clock_ % kChunk == 0) {
            double hz = 0, clarity = 0;
            const bool clear = detector_.estimate(hz, clarity) && clarity >= kClarity;
            if (clear) {
                window_ = windowFor(hz, periods_);   // with the correction off too: Shift needs it
            }
            if (p_[Correction] < 0.5) {
                target_ = 0;
            } else if (clear) {
                const double note = 69 + 12 * std::log2(hz / p_[Reference]);
                const double goal = nearest(note, static_cast<int>(p_[Root]), static_cast<int>(p_[Scale]));
                target_ = p_[Strength] / 100 * 100 * (goal - note);
            }
            const double glide = p_[Smooth] <= 0 ? 1.0 : 1 - std::exp(-static_cast<double>(kChunk) / (p_[Smooth] / 1000 * rate_));
            current_ += (target_ - current_) * glide;
            if (std::abs(target_ - current_) < 1e-9) {
                current_ = target_;
            }
            retune();
        }
        const std::size_t n = std::min(kChunk - clock_ % kChunk, frames - at);
        // Both inputs are read, and made finite, before anything is written
        // (a host may hand an output plane that is an input plane; the
        // Shifter's lines must never hold a NaN).
        for (std::size_t i = 0; i < n; ++i) {
            const float l = inL ? inL[at + i] : 0.f, r = inR ? inR[at + i] : 0.f;
            left_[i] = std::isfinite(l) ? l : 0.f;
            right_[i] = std::isfinite(r) ? r : 0.f;
            mono_[i] = 0.5f * (left_[i] + right_[i]);
        }
        detector_.push(mono_.data(), n);
        shifter_.process(left_.data(), right_.data(), wetL_.data(), wetR_.data(), n);
        for (std::size_t i = 0; i < n; ++i) {
            const double mix = mix_.next();
            if (outL) {
                outL[at + i] = static_cast<float>(left_[i] + (wetL_[i] - left_[i]) * mix);
            }
            if (outR) {
                outR[at + i] = static_cast<float>(right_[i] + (wetR_[i] - right_[i]) * mix);
            }
        }
        at += n;
        clock_ += n;
    }
}

}  // namespace adi::dsp
