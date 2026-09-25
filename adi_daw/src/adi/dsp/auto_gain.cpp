// SPDX-License-Identifier: GPL-3.0-or-later

#include "adi/dsp/auto_gain.hpp"

#include <cmath>

namespace adi::dsp {

namespace {

constexpr double kPi = 3.14159265358979323846;

double dbToGain(double db) noexcept { return std::pow(10.0, db / 20.0); }

// Tiny states are flushed at the end of each block: K-weighting a silent tail
// would otherwise decay into denormals, and a host that does not set
// flush-to-zero pays for every one.
void flush(double& v) noexcept {
    if (std::fabs(v) < 1e-30) {
        v = 0.0;
    }
}

}  // namespace

// ITU-R BS.1770-4's filters are given as coefficients at 48 kHz only. These
// are the analog prototypes they come from (the stage-1 high shelf and the
// stage-2 "RLB" high-pass), through the bilinear transform, so every sample
// rate gets the same curve; at 48 kHz they reproduce the table (the test).
Biquad AutoGain::kShelf(double fs) noexcept {
    const double f0 = 1681.974450955533;
    const double g = 3.999843853973347;
    const double q = 0.7071752369554196;
    const double k = std::tan(kPi * f0 / fs);
    const double vh = std::pow(10.0, g / 20.0);
    const double vb = std::pow(vh, 0.4996667741545416);
    const double a0 = 1.0 + k / q + k * k;
    Biquad c;
    c.b0 = (vh + vb * k / q + k * k) / a0;
    c.b1 = 2.0 * (k * k - vh) / a0;
    c.b2 = (vh - vb * k / q + k * k) / a0;
    c.a1 = 2.0 * (k * k - 1.0) / a0;
    c.a2 = (1.0 - k / q + k * k) / a0;
    return c;
}

Biquad AutoGain::kHighPass(double fs) noexcept {
    const double f0 = 38.13547087602444;
    const double q = 0.5003270373238773;
    const double k = std::tan(kPi * f0 / fs);
    const double a0 = 1.0 + k / q + k * k;
    Biquad c;
    c.b0 = 1.0;
    c.b1 = -2.0;
    c.b2 = 1.0;
    c.a1 = 2.0 * (k * k - 1.0) / a0;
    c.a2 = (1.0 - k / q + k * k) / a0;
    return c;
}

void AutoGain::prepare(double sampleRate) noexcept {
    sr_ = sampleRate > 0.0 ? sampleRate : 48000.0;
    shelf_ = kShelf(sr_);
    hp_ = kHighPass(sr_);
    k_ = 1.0 - std::exp(-1.0 / (kWindowSeconds * sr_));
    reset();
}

void AutoGain::reset() noexcept {
    dry_ = Side{};
    wet_ = Side{};
    gain_ = 1.0;
}

double AutoGain::weigh(Side& side, const float* const* x, int channels, int from,
                       int count) noexcept {
    const int measured = channels < kMaxChannels ? channels : kMaxChannels;
    double total = 0.0;
    for (int i = from; i < from + count; ++i) {
        double energy = 0.0;
        for (int c = 0; c < measured; ++c) {
            const double in = static_cast<double>(x[c][i]);
            Stage& s = side.shelf[c];
            const double y = shelf_.b0 * in + shelf_.b1 * s.x1 + shelf_.b2 * s.x2 -
                             shelf_.a1 * s.y1 - shelf_.a2 * s.y2;
            s.x2 = s.x1;
            s.x1 = in;
            s.y2 = s.y1;
            s.y1 = y;
            Stage& h = side.hp[c];
            const double z = hp_.b0 * y + hp_.b1 * h.x1 + hp_.b2 * h.x2 - hp_.a1 * h.y1 -
                             hp_.a2 * h.y2;
            h.x2 = h.x1;
            h.x1 = y;
            h.y2 = h.y1;
            h.y1 = z;
            energy += z * z;
        }
        side.energy[i - from] = energy;
        total += energy;
    }
    for (int c = 0; c < measured; ++c) {
        flush(side.shelf[c].y1);
        flush(side.shelf[c].y2);
        flush(side.hp[c].y1);
        flush(side.hp[c].y2);
    }
    return total / static_cast<double>(count);
}

void AutoGain::feed(Side& side, int count) const noexcept {
    for (int i = 0; i < count; ++i) {
        side.meanSquare += (side.energy[i] - side.meanSquare) * k_;
    }
}

void AutoGain::process(const float* const* dry, float* const* wet, int channels,
                       int frames) noexcept {
    if (channels <= 0) {
        return;
    }
    const double gate = std::pow(10.0, kGateDb / 10.0);
    const double lo = dbToGain(-kMaxDb);
    const double hi = dbToGain(kMaxDb);
    const double span = std::pow(10.0, kLiveSpanDb / 10.0);   // as energy
    for (int start = 0; start < frames; start += kControlFrames) {
        const int n = frames - start < kControlFrames ? frames - start : kControlFrames;
        // Both sides are measured BEFORE the gain touches the output: the
        // effect's own level change is what is being matched.
        const double cd = weigh(dry_, dry, channels, start, n);
        const double cw = weigh(wet_, wet, channels, start, n);
        const bool live = cd > gate && cw > gate && cd < cw * span && cw < cd * span;
        if (live) {
            feed(dry_, n);
            feed(wet_, n);
        }
        double target = gain_;
        const double d = dry_.meanSquare;
        const double w = wet_.meanSquare;
        if (!enabled_) {
            target = 1.0;
        } else if (live && d > gate && w > gate) {
            target = std::sqrt(d / w);
            target = target < lo ? lo : (target > hi ? hi : target);
        }
        const double step = (target - gain_) / static_cast<double>(n);
        for (int c = 0; c < channels; ++c) {
            double g = gain_;
            float* out = wet[c] + start;
            for (int i = 0; i < n; ++i) {
                g += step;
                out[i] = static_cast<float>(static_cast<double>(out[i]) * g);
            }
        }
        gain_ = target;
    }
}

double AutoGain::gainDb() const noexcept { return 20.0 * std::log10(gain_); }

}  // namespace adi::dsp
