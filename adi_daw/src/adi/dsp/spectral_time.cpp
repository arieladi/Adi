// SPDX-License-Identifier: GPL-3.0-or-later
#include "adi/dsp/spectral_time.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <numbers>

namespace adi::dsp {
namespace {
constexpr double kTwoPi = 2 * std::numbers::pi;
double wrapPhase(double x) noexcept { return x - kTwoPi * std::round(x / kTwoPi); }
}  // namespace

SpectralTime::SpectralTime() {
    for (std::size_t i = 0; i < Count; ++i) {
        p_[i] = ranges[i].initial;
    }
    prepare(48000);
}

void SpectralTime::prepare(double sampleRate) noexcept {
    rate_ = std::isfinite(sampleRate) && sampleRate > 0 ? sampleRate : 48000;
    ready_ = false;
    try {
        if (!fft_) {
            fft_ = std::make_unique<RealFft>(static_cast<std::int32_t>(kSize));
        }
        // The longest delay: 2 s doubled by Tilt and lengthened by Spray.
        frames_ = static_cast<std::size_t>(std::ceil(2.0 * 2 * 2 * rate_ / static_cast<double>(kHop))) + 2;
        window_.assign(kSize, 0.f);
        for (std::size_t i = 0; i < kSize; ++i) {
            window_[i] = static_cast<float>(0.5 - 0.5 * std::cos(kTwoPi * static_cast<double>(i) / static_cast<double>(kSize)));
        }
        scratch_.assign(kSize, 0.f);
        bins_.assign(kBins, {});
        out_.assign(kBins, {});
        delays_.assign(kBins, 1);
        sprayDraw_.assign(kBins, 0.f);
        std::uint32_t seed = 0x51ED27u;
        for (auto& v : sprayDraw_) {
            seed ^= seed << 13;
            seed ^= seed >> 17;
            seed ^= seed << 5;
            v = static_cast<float>(static_cast<double>(seed) / 4294967296.0);
        }
        for (auto& lane : lanes_) {
            lane.in.assign(kSize, 0.f);
            lane.acc.assign(2 * kSize, 0.f);
            lane.dry.assign(kSize, 0.f);
            lane.history.assign(frames_ * kBins, {});
            lane.magnitude.assign(kBins, 0.f);
            lane.phase.assign(kBins, 0.f);
            lane.advance.assign(kBins, 0.f);
            lane.lastPhase.assign(kBins, 0.f);
        }
        ready_ = fft_->valid();
    } catch (...) {
        ready_ = false;
    }
    written_ = frameIndex_ = 0;
    frozen_ = false;
    update();
}

void SpectralTime::set(Param p, double v) noexcept {
    const int index = static_cast<int>(p);
    if (index < 0 || index >= static_cast<int>(Count) || !std::isfinite(v)) {
        return;
    }
    const auto& r = ranges[static_cast<std::size_t>(index)];
    p_[static_cast<std::size_t>(index)] = std::clamp(index == Freeze ? std::round(v) : v, r.low, r.high);
    update();
}

void SpectralTime::update() noexcept {
    feedback_ = p_[Feedback] / 100;
    wet_ = p_[DryWet] / 100;
    if (!ready_) {
        return;
    }
    const double base = p_[Time] / 1000 * rate_ / static_cast<double>(kHop), tilt = p_[Tilt] / 100, spray = p_[Spray] / 100;
    const double mask = p_[Mask] / 100, split = 0.5 * std::abs(mask) * static_cast<double>(kBins - 1);
    for (std::size_t k = 0; k < kBins; ++k) {
        const double u = static_cast<double>(k) / static_cast<double>(kBins - 1);   // 0 at DC, 1 at Nyquist
        double d = base * std::exp2(tilt * (2 * u - 1)) * (1 + spray * sprayDraw_[k]);
        // Outside the mask a bin is not delayed (0 frames: straight through).
        const bool inside = mask > 0 ? static_cast<double>(k) >= split : mask < 0 ? static_cast<double>(kBins - 1 - k) >= split : true;
        delays_[k] = inside ? std::clamp<std::size_t>(static_cast<std::size_t>(std::lround(std::max(d, 1.0))), 1, frames_ - 1) : 0;
    }
}

void SpectralTime::frame(Lane& lane) noexcept {
    // The last window of input, analysed.
    for (std::size_t i = 0; i < kSize; ++i) {
        scratch_[i] = lane.in[(written_ + i) % kSize] * window_[i];
    }
    fft_->forward(scratch_, bins_);
    const std::size_t now = frameIndex_ % frames_;
    for (std::size_t k = 0; k < kBins; ++k) {
        const std::complex<float> x = bins_[k];
        // The phase vocoder's measure, kept up while not frozen.
        const double phase = std::arg(x), expected = kTwoPi * static_cast<double>(k * kHop) / static_cast<double>(kSize);
        if (!frozen_) {
            lane.advance[k] = static_cast<float>(expected + wrapPhase(phase - lane.lastPhase[k] - expected));
            lane.magnitude[k] = std::abs(x);
            lane.phase[k] = static_cast<float>(phase);
        }
        lane.lastPhase[k] = static_cast<float>(phase);
        std::complex<float> y;
        if (frozen_) {
            lane.phase[k] = static_cast<float>(wrapPhase(lane.phase[k] + lane.advance[k]));
            y = std::polar(lane.magnitude[k], lane.phase[k]);
        } else if (delays_[k] == 0) {
            y = x;
        } else {
            const std::size_t then = (frameIndex_ + frames_ - delays_[k]) % frames_;
            const std::complex<float> old = lane.history[then * kBins + k];
            lane.history[now * kBins + k] = x + static_cast<float>(feedback_) * old;
            y = old;
        }
        if (frozen_ || delays_[k] == 0) {
            lane.history[now * kBins + k] = {};
        }
        out_[k] = y;
    }
    fft_->inverse(out_, scratch_);
    // Overlap-add: the frame covers the last window, and is heard one window
    // later (the latency); four overlaps of the squared window sum to 1.5.
    for (std::size_t i = 0; i < kSize; ++i) {
        lane.acc[(written_ + i) % (2 * kSize)] += static_cast<float>(scratch_[i] * window_[i] / 1.5);
    }
}

void SpectralTime::process(const float* inL, const float* inR, float* outL, float* outR, std::size_t frames) noexcept {
    const bool freeze = p_[Freeze] >= 0.5;
    float* out[2] = {outL, outR};
    for (std::size_t i = 0; i < frames; ++i) {
        // Both inputs are read before either output is written (a host may
        // hand an output plane that is an input plane).
        const float x[2] = {inL && std::isfinite(inL[i]) ? inL[i] : 0.f, inR && std::isfinite(inR[i]) ? inR[i] : 0.f};
        if (!ready_) {
            for (std::size_t c = 0; c < 2; ++c) {
                if (out[c]) {
                    out[c][i] = x[c] * static_cast<float>(1 - wet_);
                }
            }
            continue;
        }
        float y[2];
        for (std::size_t c = 0; c < 2; ++c) {
            auto& lane = lanes_[c];
            const std::size_t slot = written_ % kSize;
            const float delayedDry = lane.dry[slot];
            lane.dry[slot] = x[c];
            lane.in[slot] = x[c];
            const std::size_t at = written_ % (2 * kSize);
            const float wet = lane.acc[at];
            lane.acc[at] = 0.f;
            y[c] = static_cast<float>(delayedDry + (wet - delayedDry) * wet_);
        }
        ++written_;
        if (written_ % kHop == 0) {
            frozen_ = freeze;
            for (auto& lane : lanes_) {
                frame(lane);
            }
            ++frameIndex_;
        }
        for (std::size_t c = 0; c < 2; ++c) {
            if (out[c]) {
                out[c][i] = y[c];
            }
        }
    }
}

}  // namespace adi::dsp
