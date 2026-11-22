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

std::size_t AutoShift::latencyFor(double sampleRate, std::size_t window) noexcept {
    const double longest = std::ceil(sampleRate / kLowestShiftedHz);
    return window / 2 + window / 8 + kChunk + static_cast<std::size_t>(std::ceil(kHighestRatio * longest / 2)) + 8;
}

void AutoShift::prepare(double sampleRate) noexcept {
    rate_ = std::isfinite(sampleRate) && sampleRate > 0 ? sampleRate : 48000;
    ready_ = false;
    size_ = 0;
    detector_.prepare(rate_);
    // A rate past any host's (the detector inert, or past 10 MHz) leaves the
    // device inert: its input, no latency.
    if (detector_.window() == 0 || rate_ > 1e7) {
        return;
    }
    longest_ = std::ceil(rate_ / kLowestShiftedHz);
    latency_ = latencyFor(rate_, detector_.window());
    std::size_t size = 1;
    while (size < 2 * latency_ + 16) {
        size *= 2;
    }
    try {
        for (auto& ring : in_) {
            ring.assign(size, 0.f);
        }
        size_ = size;
        ready_ = true;
    } catch (...) {
        size_ = 0;
    }
    mix_.prepare(rate_);
    mix_.set(p_[DryWet] / 100);
    target_ = current_ = 0;
    period_ = rate_ / 200;
    clock_ = pushed_ = head_ = count_ = fade_ = fadeLength_ = 0;
    tap_ = old_ = -static_cast<double>(latency_);
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

void AutoShift::retune() noexcept { ratio_ = std::clamp(std::exp2(shift() / 12), 1 / kHighestRatio, kHighestRatio); }

double AutoShift::read(const std::vector<float>& ring, double at) const noexcept {
    // Four-point Hermite, at an absolute (fractional) sample position; a
    // negative one wraps on the power-of-two ring.
    const double whole = std::floor(at), t = at - whole;
    const auto i = static_cast<long long>(whole);
    const auto get = [&](long long k) { return static_cast<double>(ring[static_cast<std::size_t>(k) & (size_ - 1)]); };
    const double y0 = get(i - 1), y1 = get(i), y2 = get(i + 1), y3 = get(i + 2);
    const double c1 = 0.5 * (y2 - y0), c2 = y0 - 2.5 * y1 + 2 * y2 - 0.5 * y3, c3 = 0.5 * (y3 - y0) + 1.5 * (y1 - y2);
    return ((c3 * t + c2) * t + c1) * t + y1;
}

void AutoShift::adopt(const Reading& r) noexcept {
    if (!r.clear) {
        return;   // an unclear reading holds the last period and correction
    }
    period_ = std::clamp(rate_ / r.hz, rate_ / PitchDetector::kHighestHz, longest_);
    if (p_[Correction] >= 0.5) {
        const double note = 69 + 12 * std::log2(r.hz / p_[Reference]);
        const double goal = nearest(note, static_cast<int>(p_[Root]), static_cast<int>(p_[Scale]));
        target_ = p_[Strength] / 100 * 100 * (goal - note);
    }
}

void AutoShift::process(const float* inL, const float* inR, float* outL, float* outR, std::size_t frames) noexcept {
    const std::size_t window = detector_.window(), hop = detector_.hop();
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
        // Every 64th sample: the correction glides, the shift is set again.
        if (clock_ % kChunk == 0) {
            if (p_[Correction] < 0.5) {
                target_ = 0;
            }
            const double glide = p_[Smooth] <= 0 ? 1.0 : 1 - std::exp(-static_cast<double>(kChunk) / (p_[Smooth] / 1000 * rate_));
            current_ += (target_ - current_) * glide;
            if (std::abs(target_ - current_) < 1e-9) {
                current_ = target_;
            }
            retune();
        }
        const std::size_t slot = clock_ & (size_ - 1);
        in_[0][slot] = x[0];
        in_[1][slot] = x[1];
        // The detector, a chunk at a time; each new reading is queued with
        // the middle of the window it was read from.
        mono_[clock_ % kChunk] = 0.5f * (x[0] + x[1]);
        if (clock_ % kChunk == kChunk - 1) {
            detector_.push(mono_.data(), kChunk);
            pushed_ += kChunk;
            if (pushed_ >= window && pushed_ % hop == 0) {
                Reading r;
                r.centre = static_cast<double>(clock_) - static_cast<double>(window - 1) / 2;
                double clarity = 0;
                r.clear = detector_.estimate(r.hz, clarity) && clarity >= kClarity;
                if (count_ == kQueue) {   // never at this latency; bounded all the same
                    adopt(queue_[head_]);
                    head_ = (head_ + 1) % kQueue;
                    --count_;
                }
                queue_[(head_ + count_) % kQueue] = r;
                ++count_;
            }
        }
        // The readings the tap has reached.
        while (count_ > 0 && queue_[head_].centre <= tap_ + static_cast<double>(hop) / 2) {
            adopt(queue_[head_]);
            head_ = (head_ + 1) % kQueue;
            --count_;
        }
        // A splice, once the tap has drifted half a period from the centre
        // (and none is under way): a whole number of periods back toward it,
        // far enough that the next is not due before this one's crossfade
        // ends.
        if (fade_ == 0) {
            const double p = period_, centre = static_cast<double>(latency_), delay = static_cast<double>(clock_) - tap_;
            const double length = std::clamp(std::round(p / 2), 16.0, std::ceil(longest_ / 2));
            double by = 0;
            if (ratio_ > 1 && delay < centre - p / 2) {
                by = -std::ceil((centre - p / 2 + (ratio_ - 1) * length - delay) / p) * p;
            } else if (ratio_ < 1 && delay > centre + p / 2) {
                by = std::ceil((delay - (centre + p / 2 - (1 - ratio_) * length)) / p) * p;
            }
            if (by != 0) {
                old_ = tap_;
                tap_ += by;
                fadeLength_ = fade_ = static_cast<std::size_t>(length);
            }
        }
        const double mix = mix_.next();
        const double fadeIn = fade_ == 0 ? 1.0
                                         : 0.5 - 0.5 * std::cos(std::numbers::pi * static_cast<double>(fadeLength_ - fade_ + 1) /
                                                                 static_cast<double>(fadeLength_ + 1));
        for (std::size_t c = 0; c < 2; ++c) {
            double wet = read(in_[c], tap_);
            if (fade_ > 0) {
                wet = fadeIn * wet + (1 - fadeIn) * read(in_[c], old_);
            }
            const double dry = in_[c][(clock_ + size_ - latency_) & (size_ - 1)];
            float* out = c == 0 ? outL : outR;
            if (out) {
                out[i] = static_cast<float>(dry + (wet - dry) * mix);
            }
        }
        tap_ += ratio_;
        if (fade_ > 0) {
            old_ += ratio_;
            --fade_;
        }
        ++clock_;
    }
}

}  // namespace adi::dsp
