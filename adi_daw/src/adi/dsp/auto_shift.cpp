// SPDX-License-Identifier: GPL-3.0-or-later
#include "adi/dsp/auto_shift.hpp"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace adi::dsp {

AutoShift::AutoShift() {
    for (std::size_t i = 0; i < Count; ++i) {
        p_[i] = ranges[i].initial;
    }
    prepare(48000);
}

void AutoShift::prepare(double sampleRate) noexcept {
    rate_ = std::isfinite(sampleRate) && sampleRate > 0 ? sampleRate : 48000;
    detector_.prepare(rate_);
    longest_ = std::ceil(rate_ / kLowestShiftedHz);
    // A grain reaches 2 H <= 4 periods; its input mark sits within half a
    // period of the output's time one latency back; two samples for the
    // interpolation.
    latency_ = static_cast<std::size_t>(std::ceil(4.5 * longest_)) + 8;
    std::size_t size = 1;
    while (size < 2 * latency_ + 8 * static_cast<std::size_t>(longest_)) {
        size *= 2;
    }
    ready_ = false;
    try {
        for (std::size_t c = 0; c < 2; ++c) {
            in_[c].assign(size, 0.f);
            acc_[c].assign(size, 0.f);
        }
        weight_.assign(size, 0.f);
        size_ = size;
        ready_ = true;
    } catch (...) {
        size_ = 0;
    }
    mix_.prepare(rate_);
    mix_.set(p_[DryWet] / 100);
    target_ = current_ = 0;
    period_ = rate_ / 200;
    ratio_ = 1;
    clock_ = 0;
    started_ = false;
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

void AutoShift::retune() noexcept { ratio_ = std::clamp(std::exp2(shift() / 12), 1.0 / 16, 16.0); }

double AutoShift::read(const std::vector<float>& ring, double at) const noexcept {
    // Four-point Hermite, at an absolute (fractional) sample position.
    const double whole = std::floor(at), t = at - whole;
    const auto i = static_cast<long long>(whole);
    const auto get = [&](long long k) { return static_cast<double>(ring[static_cast<std::size_t>(k) % size_]); };
    const double y0 = get(i - 1), y1 = get(i), y2 = get(i + 1), y3 = get(i + 2);
    const double c1 = 0.5 * (y2 - y0), c2 = y0 - 2.5 * y1 + 2 * y2 - 0.5 * y3, c3 = 0.5 * (y3 - y0) + 1.5 * (y1 - y2);
    return ((c3 * t + c2) * t + c1) * t + y1;
}

void AutoShift::grain() noexcept {
    const double p = period_, spacing = p / ratio_, half = std::clamp(std::max(p, spacing), p, 2 * p);
    // The input mark: on the period's grid, the one nearest the output's
    // time one latency back.
    const double target = outMark_ - static_cast<double>(latency_);
    while (inMark_ + 0.5 * p < target) {
        inMark_ += p;
    }
    while (inMark_ - 0.5 * p > target) {   // a sudden longer period: never ahead of the latency
        inMark_ -= p;
    }
    const auto now = static_cast<double>(clock_);
    const auto first = static_cast<long long>(std::max(std::ceil(outMark_ - half), now));
    const auto last = static_cast<long long>(std::floor(outMark_ + half));
    for (long long k = first; k <= last; ++k) {
        const double tau = static_cast<double>(k) - outMark_;
        const double w = 0.5 + 0.5 * std::cos(std::numbers::pi * tau / half);
        const auto slot = static_cast<std::size_t>(k) % size_;
        for (std::size_t c = 0; c < 2; ++c) {
            acc_[c][slot] += static_cast<float>(w * read(in_[c], inMark_ + tau));
        }
        weight_[slot] += static_cast<float>(w);
    }
    outMark_ += spacing;
}

void AutoShift::process(const float* inL, const float* inR, float* outL, float* outR, std::size_t frames) noexcept {
    for (std::size_t i = 0; i < frames; ++i) {
        // Both inputs are read, and made finite and bounded, before anything
        // is written (a host may hand an output plane that is an input plane).
        const auto bounded = [&](const float* in) { return in && std::isfinite(in[i]) ? std::clamp(in[i], -1e6f, 1e6f) : 0.f; };
        const float x[2] = {bounded(inL), bounded(inR)};
        if (!ready_) {
            if (outL) {
                outL[i] = x[0];
            }
            if (outR) {
                outR[i] = x[1];
            }
            continue;
        }
        // Every 64th sample of the device's own clock: the reading, the
        // correction, the shift.
        if (clock_ % kChunk == 0) {
            double hz = 0, clarity = 0;
            const bool clear = detector_.estimate(hz, clarity) && clarity >= kClarity;
            if (clear) {
                period_ = std::clamp(rate_ / hz, rate_ / PitchDetector::kHighestHz, longest_);
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
        mono_[clock_ % kChunk] = 0.5f * (x[0] + x[1]);
        if (clock_ % kChunk == kChunk - 1) {
            detector_.push(mono_.data(), kChunk);
        }
        const std::size_t slot = clock_ % size_;
        in_[0][slot] = x[0];
        in_[1][slot] = x[1];
        // The grains due by now.
        if (!started_ && clock_ >= latency_) {
            started_ = true;
            outMark_ = static_cast<double>(clock_) + period_;
            inMark_ = outMark_ - static_cast<double>(latency_);
        }
        while (started_ && static_cast<double>(clock_) >= outMark_ - std::clamp(std::max(period_, period_ / ratio_), period_, 2 * period_)) {
            grain();
        }
        const double mix = mix_.next(), w = weight_[slot];
        for (std::size_t c = 0; c < 2; ++c) {
            const double wet = w > 1e-6 ? acc_[c][slot] / w : 0.0;
            const double dry = in_[c][(clock_ + size_ - latency_) % size_];
            acc_[c][slot] = 0.f;
            float* out = c == 0 ? outL : outR;
            if (out) {
                out[i] = static_cast<float>(dry + (wet - dry) * mix);
            }
        }
        weight_[slot] = 0.f;
        ++clock_;
    }
}

}  // namespace adi::dsp
