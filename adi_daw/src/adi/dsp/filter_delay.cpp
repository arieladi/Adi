// SPDX-License-Identifier: GPL-3.0-or-later
#include "adi/dsp/filter_delay.hpp"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace adi::dsp {
namespace {
double settle(double x) noexcept { return std::abs(x) < 1e-30 ? 0 : x; }
double level(double db) noexcept { return db <= -70 ? 0.0 : std::pow(10.0, db / 20); }
constexpr FilterDelay::Range kField[FilterDelay::Fields] = {
    {0, 1, 1}, {0, 1, 1}, {50, 18000, 1000}, {0.5, 9, 3}, {0, 1, 1}, {1, 16, 3}, {1, 3000, 250}, {0, 95, 50}, {-50, 50, 0}, {-70, 6, 0}};
// The three lines' own defaults: the left line panned left, the right right,
// their bands and times apart.
constexpr double kFreq[FilterDelay::kLines] = {400, 1000, 2500};
constexpr double kSixteenths[FilterDelay::kLines] = {3, 4, 6};
constexpr double kPan[FilterDelay::kLines] = {-50, 0, 50};
}  // namespace

FilterDelay::Range FilterDelay::range(Param p) noexcept {
    const auto index = static_cast<std::size_t>(p);
    if (index == static_cast<std::size_t>(Dry)) {
        return {-70, 6, 0};
    }
    const std::size_t line = index / Fields, field = index % Fields;
    Range r = kField[field];
    if (line < kLines) {
        if (field == Freq) {
            r.initial = kFreq[line];
        } else if (field == Sixteenths) {
            r.initial = kSixteenths[line];
        } else if (field == Pan) {
            r.initial = kPan[line];
        }
    }
    return r;
}

FilterDelay::FilterDelay() {
    for (std::size_t i = 0; i < Count; ++i) {
        p_[i] = range(static_cast<Param>(i)).initial;
    }
    prepare(48000);
}

void FilterDelay::prepare(double sampleRate) noexcept {
    rate_ = std::isfinite(sampleRate) && sampleRate > 0 ? sampleRate : 48000;
    // The lines are sized before size_ says so, and memory that cannot be had
    // leaves the device inert (the dry signal only): never a throw out of a
    // host's DSP callback, which is where a Pd external prepares, and never a
    // size_ larger than a line.
    const auto size = static_cast<std::size_t>(std::ceil(kMaxSeconds * rate_)) + 4;
    size_ = 0;
    try {
        for (auto& line : lines_) {
            line.ring.assign(size, 0.f);
        }
        size_ = size;
    } catch (...) {
        for (auto& line : lines_) {
            line.ring.clear();
        }
    }
    for (auto& line : lines_) {
        line.s1 = line.s2 = line.last = 0;
    }
    write_ = 0;
    glide_ = 1 - std::exp(-1 / (0.050 * rate_ / 5));   // about 50 ms to arrive
    for (auto& line : lines_) {
        for (auto* g : {&line.gainL, &line.gainR, &line.feedback, &line.on}) {
            g->prepare(rate_);
        }
    }
    dry_.prepare(rate_);
    for (std::size_t l = 0; l < kLines; ++l) {
        update(l);
        lines_[l].current = lines_[l].target;
    }
    dry_.set(level(p_[Dry]));
}

void FilterDelay::set(Param p, double v) noexcept {
    const auto index = static_cast<std::size_t>(p);
    if (index >= static_cast<std::size_t>(Count) || !std::isfinite(v)) {
        return;
    }
    const Range r = range(p);
    const std::size_t field = index % Fields;
    const bool whole = index != static_cast<std::size_t>(Dry) && (field == On || field == Filter || field == Sync || field == Sixteenths);
    p_[index] = std::clamp(whole ? std::round(v) : v, r.low, r.high);
    if (index == static_cast<std::size_t>(Dry)) {
        dry_.set(level(p_[Dry]));
    } else {
        update(index / Fields);
    }
}

void FilterDelay::tempo(double bpm) noexcept {
    if (!std::isfinite(bpm) || bpm <= 0) {
        return;
    }
    bpm_ = std::clamp(bpm, 20.0, 999.0);
    for (std::size_t l = 0; l < kLines; ++l) {
        update(l);
    }
}

void FilterDelay::update(std::size_t l) noexcept {
    auto& line = lines_[l];
    const auto at = [&](Field f) { return p_[l * Fields + static_cast<std::size_t>(f)]; };
    line.on.set(at(On) >= 0.5 ? 1.0 : 0.0);
    line.filter = at(Filter) >= 0.5;
    // A constant-peak band pass, its width in octaves (RBJ).
    const double f = std::min(at(Freq), 0.45 * rate_), w = 2 * std::numbers::pi * f / rate_;
    const double alpha = std::sin(w) * std::sinh(std::log(2.0) / 2 * at(Width) * w / std::sin(w)), a0 = 1 + alpha;
    line.b0 = alpha / a0;
    line.a1 = -2 * std::cos(w) / a0;
    line.a2 = (1 - alpha) / a0;
    const double seconds = at(Sync) >= 0.5 ? at(Sixteenths) * 15 / bpm_ : at(Time) / 1000;
    line.target = std::clamp(std::round(seconds * rate_), 1.0, kMaxSeconds * rate_);
    line.feedback.set(at(Feedback) / 100);
    const double theta = (at(Pan) + 50) / 100 * std::numbers::pi / 2, gain = level(at(Volume));
    line.gainL.set(gain * std::cos(theta));
    line.gainR.set(gain * std::sin(theta));
}

void FilterDelay::process(const float* inL, const float* inR, float* outL, float* outR, std::size_t frames) noexcept {
    if (size_ == 0) {
        // Inert (prepare could not allocate): the dry signal alone.
        for (std::size_t i = 0; i < frames; ++i) {
            const double l = inL && std::isfinite(inL[i]) ? inL[i] : 0.0, r = inR && std::isfinite(inR[i]) ? inR[i] : 0.0;
            if (outL) {
                outL[i] = static_cast<float>(dry_.value * l);
            }
            if (outR) {
                outR[i] = static_cast<float>(dry_.value * r);
            }
        }
        return;
    }
    for (std::size_t i = 0; i < frames; ++i) {
        // Both inputs are read before either output is written (a host may
        // hand an output plane that is an input plane).
        const double l = inL && std::isfinite(inL[i]) ? inL[i] : 0.0, r = inR && std::isfinite(inR[i]) ? inR[i] : 0.0;
        const double feeds[kLines] = {l, 0.5 * (l + r), r};
        const double dry = dry_.next();
        double yl = dry * l, yr = dry * r;
        for (std::size_t k = 0; k < kLines; ++k) {
            auto& line = lines_[k];
            line.current += (line.target - line.current) * glide_;
            if (std::abs(line.target - line.current) < 1e-6) {
                line.current = line.target;
            }
            // The tap: whole samples once arrived, linear while gliding.
            const double back = line.current, whole = std::floor(back), frac = back - whole;
            const auto index = [&](double d) { return (write_ + size_ - static_cast<std::size_t>(d)) % size_; };
            double y = line.ring[index(whole)];
            if (frac > 0) {
                y += frac * (static_cast<double>(line.ring[index(whole + 1)]) - y);
            }
            const double into = line.on.next() * feeds[k] + line.feedback.next() * y;
            double v = into;
            if (line.filter) {
                v = line.b0 * into + line.s1;
                line.s1 = settle(line.s2 - line.a1 * v);
                line.s2 = settle(-line.b0 * into - line.a2 * v);
            }
            line.ring[write_] = static_cast<float>(settle(v));
            yl += line.gainL.next() * y;
            yr += line.gainR.next() * y;
        }
        if (outL) {
            outL[i] = static_cast<float>(yl);
        }
        if (outR) {
            outR[i] = static_cast<float>(yr);
        }
        write_ = (write_ + 1) % size_;
    }
}

}  // namespace adi::dsp
