// SPDX-License-Identifier: GPL-3.0-or-later
#include "adi/dsp/glue_compressor.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace adi::dsp {
namespace {
double fromDb(double db) noexcept { return std::pow(10.0, db / 20.0); }
double coefficient(double seconds, double rate) noexcept {
    const double samples = seconds * rate;
    return samples > 0 ? std::exp(-1.0 / samples) : 0.0;
}
std::size_t choice(double v, std::size_t count) noexcept {
    return static_cast<std::size_t>(std::clamp<long>(std::lround(v), 0, static_cast<long>(count) - 1));
}
}  // namespace

GlueCompressor::GlueCompressor() noexcept {
    for (std::size_t i = 0; i < Count; ++i) {
        p_[i] = ranges[i].initial;
    }
    prepare(48000);
}

void GlueCompressor::prepare(double sampleRate) noexcept {
    rate_ = std::isfinite(sampleRate) && sampleRate > 0 ? sampleRate : 48000;
    reductionDb_ = slowDb_ = 0;
    update();
}

void GlueCompressor::set(Param p, double v) noexcept {
    const int index = static_cast<int>(p);
    if (index < 0 || index >= static_cast<int>(Count) || !std::isfinite(v)) {
        return;
    }
    const auto& r = ranges[static_cast<std::size_t>(index)];
    p_[static_cast<std::size_t>(index)] = std::clamp(v, r.low, r.high);
    update();
}

void GlueCompressor::update() noexcept {
    threshold_ = p_[Threshold];
    slope_ = 1.0 - 1.0 / kRatios[choice(p_[Ratio], 3)];
    attack_ = coefficient(kAttackMs[choice(p_[Attack], 7)] / 1000.0, rate_);
    const auto release = choice(p_[Release], 7);
    const bool wasAuto = autoRelease_;
    autoRelease_ = release == 6;
    if (autoRelease_ && !wasAuto) {
        // The slow follower ran on under a fixed release and may sit deeper
        // than the reduction; Auto starts from the reduction as it is, so its
        // release stays monotone (a found defect: a 3 dB step down).
        slowDb_ = std::max(slowDb_, reductionDb_);
    }
    release_ = coefficient(autoRelease_ ? kReleaseS[0] : kReleaseS[release], rate_);
    fast_ = coefficient(kReleaseS[0], rate_);
    slow_ = coefficient(kReleaseS[5], rate_);
    range_ = p_[Range];
    makeup_ = fromDb(p_[Makeup]);
    wet_ = p_[DryWet] / 100.0;
    soft_ = p_[SoftClip] >= 0.5;
    ceiling_ = fromDb(kSoftClipDb);
}

double GlueCompressor::staticGainDb(double levelDb) const noexcept {
    return levelDb > threshold_ ? -(levelDb - threshold_) * slope_ : 0.0;
}

void GlueCompressor::process(const float* inL, const float* inR, float* outL, float* outR, std::size_t frames) noexcept {
    const float* in[2] = {inL, inR};
    float* out[2] = {outL, outR};
    for (std::size_t i = 0; i < frames; ++i) {
        double x[2];
        for (int c = 0; c < 2; ++c) {
            const double v = in[c] ? static_cast<double>(in[c][i]) : 0.0;
            x[c] = std::isfinite(v) ? v : 0.0;
        }
        const double level = std::max(std::abs(x[0]), std::abs(x[1]));
        const double levelDb = level > 0 ? 20.0 * std::log10(level) : -std::numeric_limits<double>::infinity();
        const double target = std::max(staticGainDb(levelDb), -range_);
        if (target < reductionDb_) {
            reductionDb_ = target + (reductionDb_ - target) * attack_;
        } else if (autoRelease_) {
            // Two stages: a fast release, held back by a slow follower of
            // the reduction that only a sustained level deepens.
            const double fast = target + (reductionDb_ - target) * fast_;
            reductionDb_ = std::min(fast, slowDb_);
        } else {
            reductionDb_ = target + (reductionDb_ - target) * release_;
        }
        if (reductionDb_ < slowDb_) {
            slowDb_ = reductionDb_ + (slowDb_ - reductionDb_) * slow_;
        } else {
            slowDb_ = target + (slowDb_ - target) * slow_;
        }
        // Both states settle exactly on 0 under the threshold instead of
        // decaying into denormals, whatever the host's floating-point mode.
        if (std::abs(reductionDb_) < 1e-9) {
            reductionDb_ = 0.0;
        }
        if (std::abs(slowDb_) < 1e-9) {
            slowDb_ = 0.0;
        }
        const double gain = fromDb(reductionDb_) * makeup_;
        for (int c = 0; c < 2; ++c) {
            double y = x[c] * gain;
            if (soft_) {
                y = ceiling_ * std::tanh(y / ceiling_);
            }
            if (out[c]) {
                out[c][i] = static_cast<float>(x[c] * (1.0 - wet_) + y * wet_);
            }
        }
    }
}

}  // namespace adi::dsp
