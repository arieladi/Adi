// SPDX-License-Identifier: GPL-3.0-or-later
// Original ADI partitioned convolution. Algorithm stage reuses the GPL-3 Surge
// Reverb1 scalar adaptation in live_reverb.cpp, retaining that source's headers.
#include "hybrid_reverb.hpp"
#include <algorithm>
#include <cmath>
#include <numbers>
#include <stdexcept>
namespace adi::dsp {
namespace {
void fft(HybridImpulse::Spectrum &x, bool inverse) noexcept {
    constexpr auto n = HybridImpulse::fftSize;
    for (std::size_t i = 1, j = 0; i < n; ++i) {
        std::size_t bit = n >> 1;
        for (; j & bit; bit >>= 1)
            j ^= bit;
        j ^= bit;
        if (i < j)
            std::swap(x[i], x[j]);
    }
    for (std::size_t size = 2; size <= n; size *= 2) {
        const auto step =
            std::polar(1., (inverse ? 2 : -2) * std::numbers::pi / static_cast<double>(size));
        for (std::size_t at = 0; at < n; at += size) {
            std::complex<double> w = 1;
            for (std::size_t k = 0; k < size / 2; ++k) {
                const auto a = x[at + k], b = w * x[at + k + size / 2];
                x[at + k] = a + b;
                x[at + k + size / 2] = a - b;
                w *= step;
            }
        }
    }
    if (inverse)
        for (auto &v : x)
            v /= static_cast<double>(n);
}
} // namespace
HybridImpulse prepareHybridImpulse(std::span<const float> samples, int channels, double rate,
                                   HybridImpulseOptions options) {
    if (channels < 1 || channels > 2 || samples.empty() ||
        samples.size() % static_cast<std::size_t>(channels) || !std::isfinite(rate) ||
        rate < 1000 || rate > 768000 || !std::isfinite(options.size) || options.size < .25 ||
        options.size > 4 || !std::isfinite(options.attackSeconds) || options.attackSeconds < 0 ||
        options.attackSeconds > 20 || !std::isfinite(options.decaySeconds) ||
        options.decaySeconds < 0 || options.decaySeconds > 60)
        throw std::invalid_argument("invalid Hybrid Reverb impulse");
    for (float v : samples)
        if (!std::isfinite(v))
            throw std::invalid_argument("nonfinite impulse");
    const auto sourceFrames = samples.size() / static_cast<std::size_t>(channels);
    const double length = static_cast<double>(sourceFrames) * options.size;
    if (length > HybridImpulse::maxFrames)
        throw std::invalid_argument("Hybrid Reverb impulse exceeds 262144 frames");
    HybridImpulse ir;
    ir.sampleRate = rate;
    ir.frames = std::max<std::size_t>(1, static_cast<std::size_t>(std::ceil(length)));
    for (std::size_t c = 0; c < 2; ++c) {
        auto &spectra = ir.spectra[c];
        spectra.resize((ir.frames + 255) / 256);
        for (std::size_t i = 0; i < ir.frames; ++i) {
            const double pos = static_cast<double>(i) / options.size;
            const auto a = std::min(sourceFrames - 1, static_cast<std::size_t>(pos)),
                       b = std::min(a + 1, sourceFrames - 1);
            const auto channel = std::min(c, static_cast<std::size_t>(channels - 1));
            const auto stride = static_cast<std::size_t>(channels);
            double value = samples[a * stride + channel] +
                           (samples[b * stride + channel] - samples[a * stride + channel]) *
                               (pos - static_cast<double>(a));
            if (options.attackSeconds > 0)
                value *= std::min(1., static_cast<double>(i) / (options.attackSeconds * rate));
            if (options.decaySeconds > 0)
                value *= std::exp(-std::log(1000.) * static_cast<double>(i) /
                                  (options.decaySeconds * rate));
            spectra[i / 256][i % 256] = value;
        }
        for (auto &spectrum : spectra)
            fft(spectrum, false);
    }
    return ir;
}
HybridReverb::HybridReverb() {
    for (int i = 0; i < Count; ++i)
        p_[static_cast<std::size_t>(i)] = ranges[i].initial;
}
void HybridReverb::set(Param p, double v) noexcept {
    if (p >= 0 && p < Count && std::isfinite(v))
        p_[static_cast<std::size_t>(p)] = std::clamp(v, ranges[p].low, ranges[p].high);
}
void HybridReverb::tempo(double v) noexcept {
    if (std::isfinite(v))
        bpm_ = std::clamp(v, 20., 999.);
}
void HybridReverb::prepare(double rate) {
    rate_ = std::isfinite(rate) && rate >= 1000 && rate <= 768000 ? rate : 48000;
    algorithm_.prepare(rate_);
    algorithm_.set(LiveReverb::Mix, 100);
    algorithm_.set(LiveReverb::Predelay, 0);
    for (auto &h : history_)
        h.resize(HybridImpulse::maxFrames / 256);
    for (auto &p : predelay_)
        p.assign(static_cast<std::size_t>(std::ceil(rate_ * 12)) + 2, 0);
    input_ = {};
    output_ = {};
    overlap_ = {};
    dry_ = {};
    aligned_ = {};
    cursor_ = head_ = used_ = delayAt_ = 0;
    mix_ = p_[Mix] / 100;
    blend_ = p_[Blend] / 100;
}
void HybridReverb::partition(const HybridImpulse *ir) noexcept {
    const auto capacity = history_[0].size();
    used_ = std::min(used_ + 1, capacity);
    for (std::size_t c = 0; c < 2; ++c) {
        auto &newest = history_[c][head_];
        newest = {};
        for (std::size_t i = 0; i < 256; ++i)
            newest[i] = input_[c][i];
        fft(newest, false);
        HybridImpulse::Spectrum sum{};
        if (ir && ir->sampleRate == rate_) {
            const auto parts = std::min(used_, ir->spectra[c].size());
            for (std::size_t p = 0; p < parts; ++p) {
                const auto &prior = history_[c][(head_ + capacity - p) % capacity];
                const auto &kernel = ir->spectra[c][p];
                for (std::size_t k = 0; k < sum.size(); ++k)
                    sum[k] += prior[k] * kernel[k];
            }
        }
        fft(sum, true);
        for (std::size_t i = 0; i < 256; ++i) {
            output_[c][i] = sum[i].real() + overlap_[c][i];
            overlap_[c][i] = sum[i + 256].real();
        }
    }
    head_ = (head_ + 1) % capacity;
}
void HybridReverb::process(const float *left, const float *right, float *outL, float *outR,
                           std::size_t frames, const HybridImpulse *ir) noexcept {
    if (history_[0].empty()) {
        std::fill_n(outL, frames, 0.f);
        std::fill_n(outR, frames, 0.f);
        return;
    }
    algorithm_.set(LiveReverb::Decay, p_[Decay]);
    algorithm_.set(LiveReverb::Size, p_[Size]);
    algorithm_.set(LiveReverb::Predelay, p_[AlgoDelay]);
    algorithm_.set(LiveReverb::Freeze, p_[Freeze]);
    algorithm_.set(LiveReverb::Cut, p_[FreezeIn] > .5 ? 0 : 1);
    const auto route = static_cast<int>(std::round(p_[Route]));
    const double send = std::pow(10., p_[Send] / 20), smoothing = 1 - std::exp(-1 / (.005 * rate_));
    const auto delay =
        std::min(predelay_[0].size() - 1,
                 static_cast<std::size_t>(std::llround(
                     (p_[Sync] > .5 ? 60 / bpm_ * p_[Beats] : p_[Predelay] * .001) * rate_)));
    for (std::size_t i = 0; i < frames; ++i) {
        const double in[2]{left ? left[i] : 0, right ? right[i] : 0};
        float algoIn[2]{}, algoOut[2]{};
        double convolution[2]{}, dry[2]{};
        for (std::size_t c = 0; c < 2; ++c) {
            dry[c] = dry_[c][cursor_];
            dry_[c][cursor_] = in[c];
            const auto position = (delayAt_ + predelay_[c].size() - delay) % predelay_[c].size();
            const double old = predelay_[c][position];
            const double drive = in[c] * send + (delay ? old * p_[Feedback] : 0);
            predelay_[c][delayAt_] = static_cast<float>(drive);
            const double wet = delay ? old : drive;
            convolution[c] = output_[c][cursor_];
            input_[c][cursor_] = wet;
            algoIn[c] = static_cast<float>(route == 0 ? convolution[c] : aligned_[c][cursor_]);
            aligned_[c][cursor_] = wet;
        }
        if (p_[Freeze] > .5 && p_[FreezeIn] < .5)
            algoIn[0] = algoIn[1] = 0;
        algorithm_.process(&algoIn[0], &algoIn[1], &algoOut[0], &algoOut[1], 1);
        mix_ += (p_[Mix] / 100 - mix_) * smoothing;
        blend_ += (p_[Blend] / 100 - blend_) * smoothing;
        double wet[2]{};
        for (std::size_t c = 0; c < 2; ++c)
            wet[c] = route == 2   ? algoOut[c]
                     : route == 3 ? convolution[c]
                                  : (1 - blend_) * convolution[c] + blend_ * algoOut[c];
        const double mid = (wet[0] + wet[1]) * .5, side = (wet[0] - wet[1]) * .5 * p_[Width] / 100;
        outL[i] = static_cast<float>(dry[0] * (1 - mix_) + (mid + side) * mix_);
        outR[i] = static_cast<float>(dry[1] * (1 - mix_) + (mid - side) * mix_);
        delayAt_ = (delayAt_ + 1) % predelay_[0].size();
        if (++cursor_ == 256) {
            partition(ir);
            cursor_ = 0;
        }
    }
}
} // namespace adi::dsp
