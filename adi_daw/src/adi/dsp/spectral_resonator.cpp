// SPDX-License-Identifier: GPL-3.0-or-later
#include "adi/dsp/spectral_resonator.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <numbers>

namespace adi::dsp {
namespace {
constexpr double kTwoPi = 2 * std::numbers::pi;
double wrapPhase(double x) noexcept { return x - kTwoPi * std::round(x / kTwoPi); }
}  // namespace

SpectralResonator::SpectralResonator() {
    for (std::size_t i = 0; i < Count; ++i) {
        p_[i] = ranges[i].initial;
    }
    prepare(48000);
}

void SpectralResonator::prepare(double sampleRate) noexcept {
    rate_ = std::isfinite(sampleRate) && sampleRate > 0 ? sampleRate : 48000;
    ready_ = false;
    try {
        if (!fft_) {
            fft_ = std::make_unique<RealFft>(static_cast<std::int32_t>(kSize));
        }
        window_.assign(kSize, 0.f);
        for (std::size_t i = 0; i < kSize; ++i) {
            window_[i] = static_cast<float>(0.5 - 0.5 * std::cos(kTwoPi * static_cast<double>(i) / static_cast<double>(kSize)));
        }
        scratch_.assign(kSize, 0.f);
        weights_.assign(kBins, 0.f);
        advance_.assign(kBins, 0.f);
        bins_.assign(kBins, {});
        out_.assign(kBins, {});
        for (auto& lane : lanes_) {
            lane.in.assign(kSize, 0.f);
            lane.acc.assign(2 * kSize, 0.f);
            lane.dry.assign(kSize, 0.f);
            lane.magnitude.assign(kBins, 0.f);
            lane.phase.assign(kBins, 0.f);
            lane.lastPhase.assign(kBins, 0.f);
        }
        ready_ = fft_->valid();
    } catch (...) {
        ready_ = false;
    }
    written_ = 0;
    update();
}

void SpectralResonator::set(Param p, double v) noexcept {
    const int index = static_cast<int>(p);
    if (index < 0 || index >= static_cast<int>(Count) || !std::isfinite(v)) {
        return;
    }
    const auto& r = ranges[static_cast<std::size_t>(index)];
    p_[static_cast<std::size_t>(index)] = std::clamp(index == Harmonics ? std::round(v) : v, r.low, r.high);
    update();
}

double SpectralResonator::partial(int n) const noexcept {
    return p_[Frequency] * std::pow(static_cast<double>(n), 1 + 0.1 * p_[Stretch] / 100);
}

void SpectralResonator::update() noexcept {
    wet_ = p_[DryWet] / 100;
    // The fall per hop that takes a ring 60 dB down in Decay.
    fall_ = std::pow(10.0, -3.0 * static_cast<double>(kHop) / (p_[Decay] / 1000 * rate_));
    if (!ready_) {
        return;
    }
    std::fill(weights_.begin(), weights_.end(), 0.f);
    const double binHz = rate_ / static_cast<double>(kSize);
    const int count = static_cast<int>(p_[Harmonics]);
    for (int n = 1; n <= count; ++n) {
        const double hz = partial(n);
        if (!(hz < 0.45 * rate_)) {
            break;
        }
        const auto low = static_cast<std::size_t>(std::max(0.0, std::ceil(hz / binHz - 1)));
        const auto high = std::min(kBins - 1, static_cast<std::size_t>(std::floor(hz / binHz + 1)));
        const auto level = static_cast<float>(std::pow(static_cast<double>(n), p_[Brightness] / 100));
        for (std::size_t k = low; k <= high; ++k) {
            if (level > weights_[k]) {
                weights_[k] = level;
                advance_[k] = static_cast<float>(wrapPhase(kTwoPi * hz * static_cast<double>(kHop) / rate_));
            }
        }
    }
}

void SpectralResonator::frame(Lane& lane) noexcept {
    for (std::size_t i = 0; i < kSize; ++i) {
        scratch_[i] = lane.in[(written_ + i) % kSize] * window_[i];
    }
    fft_->forward(scratch_, bins_);
    for (std::size_t k = 0; k < kBins; ++k) {
        const std::complex<float> x = bins_[k];
        const double phase = std::arg(x);
        lane.lastPhase[k] = static_cast<float>(phase);
        if (weights_[k] == 0) {
            lane.magnitude[k] = 0;
            out_[k] = {};
            continue;
        }
        // The ring: the input's magnitude, or the last frame's fallen.
        const auto fallen = static_cast<float>(lane.magnitude[k] * fall_);
        const float now = std::abs(x);
        if (now >= fallen) {
            lane.magnitude[k] = now;
            lane.phase[k] = static_cast<float>(phase);
        } else {
            lane.magnitude[k] = fallen < 1e-20f ? 0.f : fallen;
            lane.phase[k] = static_cast<float>(wrapPhase(lane.phase[k] + advance_[k]));
        }
        out_[k] = std::polar(lane.magnitude[k] * weights_[k], lane.phase[k]);
    }
    fft_->inverse(out_, scratch_);
    for (std::size_t i = 0; i < kSize; ++i) {
        lane.acc[(written_ + i) % (2 * kSize)] += static_cast<float>(scratch_[i] * window_[i] / 1.5);
    }
}

void SpectralResonator::process(const float* inL, const float* inR, float* outL, float* outR, std::size_t frames) noexcept {
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
            for (auto& lane : lanes_) {
                frame(lane);
            }
        }
        for (std::size_t c = 0; c < 2; ++c) {
            if (out[c]) {
                out[c][i] = y[c];
            }
        }
    }
}

}  // namespace adi::dsp
