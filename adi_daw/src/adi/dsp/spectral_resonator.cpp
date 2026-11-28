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
    // The window scales with the rate, so a bin spans the same bandwidth (23
    // Hz) at every rate: 2,048 points to 66 kHz, 4,096 to 132 kHz, 8,192 above.
    const std::size_t scale = rate_ <= 66000 ? 1 : rate_ <= 132000 ? 2 : 4, size = kSize * scale;
    try {
        if (!fft_ || size != size_) {
            fft_.reset();
            size_ = size;
            hop_ = size / 4;
            binCount_ = size / 2 + 1;
            fft_ = std::make_unique<RealFft>(static_cast<std::int32_t>(size_));
        }
        window_.assign(size_, 0.f);
        for (std::size_t i = 0; i < size_; ++i) {
            window_[i] = static_cast<float>(0.5 - 0.5 * std::cos(kTwoPi * static_cast<double>(i) / static_cast<double>(size_)));
        }
        scratch_.assign(size_, 0.f);
        weights_.assign(binCount_, 0.f);
        advance_.assign(binCount_, 0.f);
        bins_.assign(binCount_, {});
        out_.assign(binCount_, {});
        for (auto& lane : lanes_) {
            lane.in.assign(size_, 0.f);
            lane.acc.assign(2 * size_, 0.f);
            lane.dry.assign(size_, 0.f);
            lane.magnitude.assign(binCount_, 0.f);
            lane.phase.assign(binCount_, 0.f);
            lane.lastPhase.assign(binCount_, 0.f);
        }
        ready_ = fft_->valid();
    } catch (...) {
        ready_ = false;
    }
    written_ = 0;
    wet_.prepare(rate_);
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
    wet_.set(p_[DryWet] / 100);
    // The fall per hop that takes a ring 60 dB down in Decay.
    fall_ = std::pow(10.0, -3.0 * static_cast<double>(hop_) / (p_[Decay] / 1000 * rate_));
    if (!ready_) {
        return;
    }
    std::fill(weights_.begin(), weights_.end(), 0.f);
    const double binHz = rate_ / static_cast<double>(size_);
    const int count = static_cast<int>(p_[Harmonics]);
    for (int n = 1; n <= count; ++n) {
        const double hz = partial(n);
        if (!(hz < 0.45 * rate_)) {
            break;
        }
        const auto low = static_cast<std::size_t>(std::max(0.0, std::ceil(hz / binHz - 1)));
        const auto high = std::min(binCount_ - 1, static_cast<std::size_t>(std::floor(hz / binHz + 1)));
        const auto level = static_cast<float>(std::pow(static_cast<double>(n), p_[Brightness] / 100));
        for (std::size_t k = low; k <= high; ++k) {
            if (level > weights_[k]) {
                weights_[k] = level;
                advance_[k] = static_cast<float>(wrapPhase(kTwoPi * hz * static_cast<double>(hop_) / rate_));
            }
        }
    }
}

void SpectralResonator::frame(Lane& lane) noexcept {
    for (std::size_t i = 0; i < size_; ++i) {
        scratch_[i] = lane.in[(written_ + i) % size_] * window_[i];
    }
    fft_->forward(scratch_, bins_);
    for (std::size_t k = 0; k < binCount_; ++k) {
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
    for (std::size_t i = 0; i < size_; ++i) {
        lane.acc[(written_ + i) % (2 * size_)] += static_cast<float>(scratch_[i] * window_[i] / 1.5);
    }
}

void SpectralResonator::process(const float* inL, const float* inR, float* outL, float* outR, std::size_t frames) noexcept {
    float* out[2] = {outL, outR};
    for (std::size_t i = 0; i < frames; ++i) {
        // Both inputs are read before either output is written (a host may
        // hand an output plane that is an input plane).
        const auto bounded = [](const float* in, std::size_t at) {
            return in && std::isfinite(in[at]) ? std::clamp(in[at], -1e6f, 1e6f) : 0.f;
        };
        const float x[2] = {bounded(inL, i), bounded(inR, i)};
        const double mix = wet_.next();
        if (!ready_) {
            for (std::size_t c = 0; c < 2; ++c) {
                if (out[c]) {
                    out[c][i] = x[c];   // inert: the input, no latency
                }
            }
            continue;
        }
        float y[2];
        for (std::size_t c = 0; c < 2; ++c) {
            auto& lane = lanes_[c];
            const std::size_t slot = written_ % size_;
            const float delayedDry = lane.dry[slot];
            lane.dry[slot] = x[c];
            lane.in[slot] = x[c];
            const std::size_t at = written_ % (2 * size_);
            const float wet = lane.acc[at];
            lane.acc[at] = 0.f;
            y[c] = static_cast<float>(delayedDry + (wet - delayedDry) * mix);
        }
        ++written_;
        if (written_ % hop_ == 0) {
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
