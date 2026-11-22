// SPDX-License-Identifier: GPL-3.0-or-later
#include "adi/dsp/live_limiter.hpp"

#include <algorithm>
#include <cmath>

namespace adi::dsp {
namespace {
double fromDb(double db) noexcept { return std::pow(10.0, db / 20.0); }
}  // namespace

LiveLimiter::LiveLimiter() {
    for (std::size_t i = 0; i < Count; ++i) {
        p_[i] = ranges[i].initial;
    }
    prepare(48000);
}

void LiveLimiter::prepare(double sampleRate) {
    rate_ = std::isfinite(sampleRate) && sampleRate > 0 ? sampleRate : 48000;
    // Room for the longest lookahead: 6 ms at the prepared rate.
    const int most = static_cast<int>(std::lround(kLookaheadMs[2] * rate_ / 1000.0));
    limiter_[0].prepare(rate_, 2, most);
    limiter_[1].prepare(rate_, 1, most);
    limiter_[2].prepare(rate_, 1, most);
    update();
}

void LiveLimiter::set(Param p, double v) noexcept {
    const int index = static_cast<int>(p);
    if (index < 0 || index >= static_cast<int>(Count) || !std::isfinite(v)) {
        return;
    }
    const auto& r = ranges[static_cast<std::size_t>(index)];
    p_[static_cast<std::size_t>(index)] = std::clamp(v, r.low, r.high);
    update();
}

void LiveLimiter::update() noexcept {
    gain_ = fromDb(p_[Gain]);
    ceiling_ = fromDb(p_[Ceiling]);
    const auto choice = static_cast<std::size_t>(std::clamp<long>(std::lround(p_[Lookahead]), 0, 2));
    const int lookahead = static_cast<int>(std::lround(kLookaheadMs[choice] * rate_ / 1000.0));
    link_ = static_cast<int>(std::clamp<long>(std::lround(p_[Link]), 0, 2));
    kind_ = static_cast<int>(std::clamp<long>(std::lround(p_[Mode]), 0, 2));
    for (auto& l : limiter_) {
        // The brickwall resets when its lookahead or detector changes (a
        // latency change); unchanged, the calls leave it running.
        if (l.truePeak() != (kind_ == TruePeak)) {
            l.setTruePeak(kind_ == TruePeak);
        }
        if (l.latencySamples() - (l.truePeak() ? l.truePeakDelay() : 0) != lookahead) {
            l.setLookahead(lookahead);
        }
        l.setCeiling(ceiling_);
        l.setReleaseSeconds(p_[Release] / 1000.0);
    }
}

int LiveLimiter::latency() const noexcept {
    // All three run at the same settings; the linked one stands for them.
    return limiter_[0].latencySamples();
}

void LiveLimiter::process(const float* inL, const float* inR, float* outL, float* outR, std::size_t frames) noexcept {
    // In blocks of at most 256: gained (and, for M/S, encoded) into doubles,
    // limited, decoded, softened (Soft Clip), out.
    constexpr std::size_t kBlock = 256;
    std::array<std::array<double, kBlock>, 2> a{}, b{};
    for (std::size_t done = 0; done < frames;) {
        const std::size_t n = std::min(kBlock, frames - done);
        for (std::size_t i = 0; i < n; ++i) {
            double l = inL ? static_cast<double>(inL[done + i]) * gain_ : 0.0;
            double r = inR ? static_cast<double>(inR[done + i]) * gain_ : 0.0;
            if (link_ == MidSide) {
                const double mid = 0.5 * (l + r), side = 0.5 * (l - r);
                l = mid;
                r = side;
            }
            a[0][i] = l;
            a[1][i] = r;
        }
        const double* in2[2] = {a[0].data(), a[1].data()};
        double* out2[2] = {b[0].data(), b[1].data()};
        if (link_ == Stereo) {
            limiter_[0].process(in2, out2, static_cast<int>(n));
        } else {
            limiter_[1].process(&in2[0], &out2[0], static_cast<int>(n));
            limiter_[2].process(&in2[1], &out2[1], static_cast<int>(n));
        }
        for (std::size_t i = 0; i < n; ++i) {
            double l = b[0][i], r = b[1][i];
            if (link_ == MidSide) {
                const double mid = l, side = r;
                l = mid + side;
                r = mid - side;
            }
            if (kind_ == SoftClip) {
                // tanh inside the ceiling: unity slope near 0, so a quiet
                // signal passes as itself, rounding off toward the ceiling and
                // never reaching it.
                const auto soft = [this](double x) { return ceiling_ * std::tanh(x / ceiling_); };
                l = soft(l);
                r = soft(r);
            }
            if (outL) {
                outL[done + i] = static_cast<float>(l);
            }
            if (outR) {
                outR[done + i] = static_cast<float>(r);
            }
        }
        done += n;
    }
}

}  // namespace adi::dsp
