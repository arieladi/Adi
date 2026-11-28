// SPDX-License-Identifier: GPL-3.0-or-later
#include "adi/dsp/live_gate.hpp"

#include <algorithm>
#include <cmath>

namespace adi::dsp {
namespace {
double fromDb(double db) noexcept { return std::pow(10.0, db / 20.0); }
/// A one-pole's coefficient for a time constant of `ms` at `rate`.
double coefficient(double ms, double rate) noexcept {
    const double samples = ms * rate / 1000.0;
    return samples > 0 ? std::exp(-1.0 / samples) : 0.0;
}
}  // namespace

LiveGate::LiveGate() noexcept {
    for (std::size_t i = 0; i < Count; ++i) {
        p_[i] = ranges[i].initial;
    }
    prepare(48000);
}

void LiveGate::prepare(double sampleRate) noexcept {
    rate_ = std::isfinite(sampleRate) && sampleRate > 0 ? sampleRate : 48000;
    for (auto& line : line_) {
        line.fill(0.0);
    }
    write_ = 0;
    update();
    open_ = false;
    holdLeft_ = 0;
    opening_ = 0;
    applied_ = flip_ ? 1.0 : floor_;
}

void LiveGate::set(Param p, double v) noexcept {
    const int index = static_cast<int>(p);
    if (index < 0 || index >= static_cast<int>(Count) || !std::isfinite(v)) {
        return;
    }
    const auto& r = ranges[static_cast<std::size_t>(index)];
    const double before = p_[static_cast<std::size_t>(p)];
    p_[static_cast<std::size_t>(p)] = std::clamp(v, r.low, r.high);
    if (p == Lookahead && std::lround(before) != std::lround(p_[static_cast<std::size_t>(p)])) {
        // A new delay length: the line restarts, as a latency change does.
        for (auto& line : line_) {
            line.fill(0.0);
        }
        write_ = 0;
    }
    update();
}

void LiveGate::update() noexcept {
    openLevel_ = fromDb(p_[Threshold]);
    closeLevel_ = fromDb(p_[Threshold] - p_[Return]);
    attack_ = coefficient(p_[Attack], rate_);
    release_ = coefficient(p_[Release], rate_);
    floor_ = p_[Floor] <= ranges[Floor].low ? 0.0 : fromDb(p_[Floor]);
    holdSamples_ = std::lround(p_[Hold] * rate_ / 1000.0);
    flip_ = p_[Flip] >= 0.5;
    const auto choice = std::lround(p_[Lookahead]);
    const double ms = choice == 2 ? 10.0 : choice == 1 ? 1.0 : 0.0;
    lookahead_ = std::min(static_cast<int>(std::lround(ms * rate_ / 1000.0)), kMaxLookahead - 1);
}

void LiveGate::process(const float* inL, const float* inR, float* outL, float* outR, std::size_t frames) noexcept {
    const float* in[2] = {inL, inR};
    float* out[2] = {outL, outR};
    for (std::size_t i = 0; i < frames; ++i) {
        double x[2];
        for (int c = 0; c < 2; ++c) {
            const double v = in[c] ? static_cast<double>(in[c][i]) : 0.0;
            x[c] = std::isfinite(v) ? v : 0.0;
        }
        // The detector reads the input now; the audio leaves lookahead later.
        const double level = std::max(std::abs(x[0]), std::abs(x[1]));
        if (level >= openLevel_) {
            open_ = true;
            holdLeft_ = holdSamples_;
        } else if (open_) {
            if (level >= closeLevel_) {
                holdLeft_ = holdSamples_;   // still above the close level: Hold starts again
            } else if (holdLeft_ > 0) {
                --holdLeft_;
            } else {
                open_ = false;
            }
        }
        // The one-pole on the opening; it settles exactly, so it never decays
        // into denormals whatever the host's floating-point mode.
        const double target = open_ ? 1.0 : 0.0;
        opening_ = target + (opening_ - target) * (target > opening_ ? attack_ : release_);
        if (std::abs(opening_ - target) < 1e-12) {
            opening_ = target;
        }
        applied_ = floor_ + (1.0 - floor_) * (flip_ ? 1.0 - opening_ : opening_);
        const std::size_t read = (write_ + kMaxLookahead - static_cast<std::size_t>(lookahead_)) % kMaxLookahead;
        for (int c = 0; c < 2; ++c) {
            line_[static_cast<std::size_t>(c)][write_] = x[c];
            const double delayed = line_[static_cast<std::size_t>(c)][read];
            if (out[c]) {
                out[c][i] = static_cast<float>(delayed * applied_);
            }
        }
        write_ = (write_ + 1) % kMaxLookahead;
    }
}

}  // namespace adi::dsp
