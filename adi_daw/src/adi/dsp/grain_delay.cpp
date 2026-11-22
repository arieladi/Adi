// SPDX-License-Identifier: GPL-3.0-or-later
#include "adi/dsp/grain_delay.hpp"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace adi::dsp {
namespace {
// The longest the line must hold: the delay, the spray, and a grain's
// excursion: two periods at 1 Hz times |ratio - 1|, at most 3 (Pitch +12 with
// Random Pitch's +12 reads at four times the speed).
constexpr double kLineSeconds = 1.0 + 0.5 + 6.0 + 0.1;
}  // namespace

GrainDelay::GrainDelay() {
    for (std::size_t i = 0; i < Count; ++i) {
        p_[i] = ranges[i].initial;
    }
    prepare(48000);
}

void GrainDelay::prepare(double sampleRate) noexcept {
    rate_ = std::isfinite(sampleRate) && sampleRate > 0 ? sampleRate : 48000;
    const auto size = static_cast<std::size_t>(std::ceil(kLineSeconds * rate_)) + 8;
    // Memory that cannot be had leaves the device inert (the dry signal),
    // never a throw out of a host's DSP callback.
    size_ = 0;
    try {
        for (auto& lane : lanes_) {
            lane.ring.assign(size, 0.f);
        }
        size_ = size;
    } catch (...) {
        for (auto& lane : lanes_) {
            lane.ring.clear();
        }
    }
    lanes_[0].seed = 0x6D2B79F5u;
    lanes_[1].seed = 0x1B873593u;
    for (auto& lane : lanes_) {
        lane.grains = {};
        lane.last = 0;
    }
    written_ = 0;
    countdown_ = 0;
    next_ = 0;
    lowest_ = 1e300;
    update();
}

void GrainDelay::set(Param p, double v) noexcept {
    const int index = static_cast<int>(p);
    if (index < 0 || index >= static_cast<int>(Count) || !std::isfinite(v)) {
        return;
    }
    const auto& r = ranges[static_cast<std::size_t>(index)];
    const bool whole = index == Sync || index == Sixteenths;
    p_[static_cast<std::size_t>(index)] = std::clamp(whole ? std::round(v) : v, r.low, r.high);
    update();
}

void GrainDelay::tempo(double bpm) noexcept {
    if (std::isfinite(bpm) && bpm > 0) {
        bpm_ = std::clamp(bpm, 20.0, 999.0);
        update();
    }
}

void GrainDelay::update() noexcept {
    const double seconds = p_[Sync] >= 0.5 ? std::min(p_[Sixteenths] * 15 / bpm_, 1.0) : p_[Time] / 1000;
    delay_ = std::max(1.0, std::round(seconds * rate_));
    // A grain lasts two periods (an even number of samples, so two overlap
    // exactly); the next starts every period.
    length_ = 2 * std::max<std::size_t>(1, static_cast<std::size_t>(std::lround(rate_ / p_[Frequency])));
    ratio_ = std::exp2(p_[Pitch] / 12);
    spray_ = p_[Spray] / 1000 * rate_;
    feedback_ = p_[Feedback] / 100;
    wet_ = p_[DryWet] / 100;
}

double GrainDelay::random(Lane& lane) noexcept {
    lane.seed ^= lane.seed << 13;
    lane.seed ^= lane.seed >> 17;
    lane.seed ^= lane.seed << 5;
    return static_cast<double>(lane.seed) / 4294967296.0;   // [0, 1)
}

void GrainDelay::start(Lane& lane, Grain& g, std::size_t rise) noexcept {
    // Both draws are made whether or not they are used, so moving Spray or
    // Random Pitch does not shift the random sequence.
    const double spray = random(lane), pitch = random(lane);
    g.ratio = ratio_ * std::exp2(p_[RandomPitch] / 100 * 12 * (2 * pitch - 1) / 12);
    g.rise = std::max<std::size_t>(1, rise);
    g.fall = std::max<std::size_t>(1, length_ / 2);
    const double length = static_cast<double>(g.rise + g.fall), excursion = std::abs(g.ratio - 1) * length;
    // The delay at the grain's start: the set delay and its spray, and for a
    // grain faster than the line the distance it will gain over its life. It
    // is kept where the whole grain stays inside the line and behind the
    // write head (the line is sized so that this never binds in range).
    const double gain = g.ratio > 1 ? excursion : 0.0, slack = g.ratio < 1 ? excursion : 0.0;
    double d = delay_ + std::round(spray * spray_) + gain;
    d = std::clamp(d, 1 + gain, std::max(static_cast<double>(size_) - 4 - slack, 1 + gain));
    g.read = static_cast<double>(written_) - d;
    g.age = 0;
    g.live = true;
}

double GrainDelay::read(const Lane& lane, double position) const noexcept {
    const double whole = std::floor(position), frac = position - whole;
    const auto at = [&](double p) {
        const auto i = static_cast<long long>(p) % static_cast<long long>(size_);
        return static_cast<double>(lane.ring[static_cast<std::size_t>(i < 0 ? i + static_cast<long long>(size_) : i)]);
    };
    const double a = at(whole);
    return frac > 0 ? a + frac * (at(whole + 1) - a) : a;
}

void GrainDelay::process(const float* inL, const float* inR, float* outL, float* outR, std::size_t frames) noexcept {
    float* out[2] = {outL, outR};
    for (std::size_t i = 0; i < frames; ++i) {
        // Both inputs are read before either output is written (a host may
        // hand an output plane that is an input plane).
        const double dry[2] = {inL && std::isfinite(inL[i]) ? inL[i] : 0.0, inR && std::isfinite(inR[i]) ? inR[i] : 0.0};
        if (size_ == 0) {
            for (std::size_t c = 0; c < 2; ++c) {
                if (out[c]) {
                    out[c][i] = static_cast<float>(dry[c] * (1 - wet_));
                }
            }
            continue;
        }
        // A grain starts as its predecessor reaches its peak, alternating
        // between the two slots; it rises over the time the predecessor
        // falls, so the two windows sum to exactly 1.
        if (countdown_ == 0) {
            const auto& previous = lanes_[0].grains[next_ ^ 1u];
            const std::size_t rise = previous.live ? previous.fall : length_ / 2;
            for (auto& lane : lanes_) {
                start(lane, lane.grains[next_], rise);
            }
            countdown_ = lanes_[0].grains[next_].rise;
            next_ ^= 1u;
        }
        --countdown_;
        for (std::size_t c = 0; c < 2; ++c) {
            auto& lane = lanes_[c];
            // The grains read first (every read is at least a sample old),
            // then the line takes the input and the feedback of this very
            // output: an echo returns after exactly the delay.
            double y = 0;
            for (auto& g : lane.grains) {
                if (!g.live) {
                    continue;
                }
                const double u = g.age < g.rise ? static_cast<double>(g.age) / static_cast<double>(g.rise)
                                                : static_cast<double>(g.age - g.rise) / static_cast<double>(g.fall);
                const double w = g.age < g.rise ? 0.5 - 0.5 * std::cos(std::numbers::pi * u) : 0.5 + 0.5 * std::cos(std::numbers::pi * u);
                lowest_ = std::min(lowest_, static_cast<double>(written_) - g.read);
                y += w * read(lane, g.read);
                g.read += g.ratio;
                if (++g.age >= g.rise + g.fall) {
                    g.live = false;
                }
            }
            lane.last = std::abs(y) < 1e-30 ? 0 : y;
            lane.ring[written_ % size_] = static_cast<float>(dry[c] + feedback_ * lane.last);
            if (out[c]) {
                out[c][i] = static_cast<float>(dry[c] + (y - dry[c]) * wet_);
            }
        }
        ++written_;
    }
}

}  // namespace adi::dsp
