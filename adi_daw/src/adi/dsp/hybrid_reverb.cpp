// SPDX-License-Identifier: GPL-3.0-or-later
// Original ADI partitioned convolution. Algorithm stage reuses the GPL-3 Surge
// Reverb1 scalar adaptation in live_reverb.cpp, retaining that source's headers.
#include "hybrid_reverb.hpp"
#include <algorithm>
#include <cmath>
#include <new>
#include <numbers>
#include <stdexcept>
namespace adi::dsp {
namespace {
// Recursive states settle on exact 0 instead of decaying into subnormals.
double clean(double x) noexcept { return std::abs(x) < 1e-30 ? 0 : x; }
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
    RealFft transform(static_cast<std::int32_t>(HybridImpulse::fftSize));
    if (!transform.valid()) throw std::bad_alloc();
    std::array<float, HybridImpulse::fftSize> frame{};
    // The shaped impulse, each channel, before it is partitioned.
    std::array<std::vector<float>, 2> taps;
    for (std::size_t c = 0; c < 2; ++c) {
        taps[c].assign(ir.frames, 0.f);
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
            taps[c][i] = static_cast<float>(value);
        }
        // The head: up to headParts partitions of 256. The second half of
        // frame stays zero for overlap-add padding.
        auto &spectra = ir.spectra[c];
        spectra.resize(std::min(HybridImpulse::headParts, (ir.frames + 255) / 256));
        for (std::size_t k = 0; k < spectra.size(); ++k) {
            for (std::size_t i = 0; i < HybridImpulse::partition; ++i) {
                const auto at = k * HybridImpulse::partition + i;
                frame[i] = at < ir.frames ? taps[c][at] : 0.f;
            }
            transform.forward(frame, spectra[k]);
        }
    }
    // The longer stages: each part's block of taps, zero-padded to twice the
    // block, transformed once here.
    for (std::size_t s = 0; s < HybridImpulse::stages; ++s) {
        const auto block = HybridImpulse::stageBlock[s], offset = HybridImpulse::stageOffset[s];
        const auto end = std::min(ir.frames, HybridImpulse::stageEnd[s]);
        auto &stage = ir.stage[s];
        stage.parts = end > offset ? (end - offset + block - 1) / block : 0;
        if (stage.parts == 0) {
            continue;
        }
        RealFft longer(static_cast<std::int32_t>(2 * block));
        if (!longer.valid()) throw std::bad_alloc();
        std::vector<float> padded(2 * block);
        for (std::size_t c = 0; c < 2; ++c) {
            stage.spectra[c].assign(stage.parts * (block + 1), {});
            for (std::size_t k = 0; k < stage.parts; ++k) {
                std::fill(padded.begin(), padded.end(), 0.f);
                for (std::size_t i = 0; i < block; ++i) {
                    const auto at = offset + k * block + i;
                    if (at < end) {
                        padded[i] = taps[c][at];
                    }
                }
                longer.forward(padded, std::span(stage.spectra[c]).subspan(k * (block + 1), block + 1));
            }
        }
    }
    return ir;
}
HybridReverb::HybridReverb() : fft_(static_cast<std::int32_t>(HybridImpulse::fftSize)) {
    if (!fft_.valid()) throw std::bad_alloc();
    for (std::size_t s = 0; s < HybridImpulse::stages; ++s) {
        stages_[s].fft = std::make_unique<RealFft>(static_cast<std::int32_t>(2 * HybridImpulse::stageBlock[s]));
        if (!stages_[s].fft->valid()) throw std::bad_alloc();
    }
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
        h.resize(HybridImpulse::headParts);
    for (std::size_t s = 0; s < HybridImpulse::stages; ++s) {
        auto &stage = stages_[s];
        const auto block = HybridImpulse::stageBlock[s];
        stage.capacity = (HybridImpulse::stageEnd[s] - HybridImpulse::stageOffset[s] + block - 1) / block;
        for (std::size_t c = 0; c < 2; ++c) {
            stage.input[c].assign(block, 0.f);
            stage.history[c].assign(stage.capacity * (block + 1), {});
            stage.sum[c].assign(block + 1, {});
        }
        stage.frame.assign(2 * block, 0.f);
        stage.bins.assign(block + 1, {});
        stage.head = stage.capacity - 1;
        stage.used = stage.done = stage.total = 0;
        stage.start = 0;
        stage.ir = nullptr;
        stage.live = false;
    }
    for (auto &t : tail_)
        t.assign(tailSize, 0.);
    samples_ = 0;
    for (auto &p : predelay_)
        p.assign(static_cast<std::size_t>(std::ceil(rate_ * 12)) + 2, 0);
    input_ = {};
    output_ = {};
    overlap_ = {};
    dry_ = {};
    aligned_ = {};
    cursor_ = head_ = used_ = delayAt_ = 0;
    pending_ = {};
    pendingDone_ = pendingParts_ = 0;
    pendingIr_ = nullptr;
    mix_ = p_[Mix] / 100;
    blend_ = p_[Blend] / 100;
}
void HybridReverb::stageAccumulate(std::size_t s, std::size_t target) noexcept {
    auto &stage = stages_[s];
    const auto bins = HybridImpulse::stageBlock[s] + 1;
    while (stage.done < target) {
        const auto k = stage.done++;
        // Part k pairs the impulse's k-th block of this stage with the input
        // block k before the newest.
        const auto slot = (stage.head + stage.capacity - k) % stage.capacity;
        for (std::size_t c = 0; c < 2; ++c) {
            const auto *prior = stage.history[c].data() + slot * bins;
            const auto *kernel = stage.ir->stage[s].spectra[c].data() + k * bins;
            auto *sum = stage.sum[c].data();
            for (std::size_t b = 0; b < bins; ++b) {
                const double ar = prior[b].real(), ai = prior[b].imag();
                const double br = kernel[b].real(), bi = kernel[b].imag();
                sum[b] = {sum[b].real() + (ar * br - ai * bi), sum[b].imag() + (ar * bi + ai * br)};
            }
        }
    }
}

void HybridReverb::stageFinish(std::size_t s, const HybridImpulse *ir) noexcept {
    auto &stage = stages_[s];
    if (!stage.live) {
        return;
    }
    const auto block = HybridImpulse::stageBlock[s];
    if (ir != stage.ir) {
        // The impulse changed during the period: the parts are summed again
        // from the new one, here, once (as the head does).
        for (auto &sum : stage.sum) {
            std::fill(sum.begin(), sum.end(), std::complex<double>{});
        }
        const bool usable = ir && ir->sampleRate == rate_ && ir->stage[s].parts > 0;
        stage.ir = usable ? ir : nullptr;
        stage.done = 0;
        stage.total = stage.ir ? std::min(stage.used, stage.ir->stage[s].parts) : 0;
    }
    if (stage.ir) {
        stageAccumulate(s, stage.total);
        for (std::size_t c = 0; c < 2; ++c) {
            // Summed in double, rounded once before the float inverse, as the head.
            for (std::size_t b = 0; b <= block; ++b) {
                stage.bins[b] = static_cast<std::complex<float>>(stage.sum[c][b]);
            }
            stage.fft->inverse(stage.bins, stage.frame);
            for (std::size_t i = 0; i < 2 * block; ++i) {
                const auto at = static_cast<std::size_t>(stage.start + static_cast<std::int64_t>(i)) % tailSize;
                tail_[c][at] += static_cast<double>(stage.frame[i]);
            }
        }
    }
    for (auto &sum : stage.sum) {
        std::fill(sum.begin(), sum.end(), std::complex<double>{});
    }
    stage.live = false;
}

void HybridReverb::stageStart(std::size_t s, const HybridImpulse *ir) noexcept {
    auto &stage = stages_[s];
    const auto block = HybridImpulse::stageBlock[s];
    // The block just completed is the newest in the history, whatever the
    // impulse: one may arrive later and needs the input that came before it.
    stage.head = (stage.head + 1) % stage.capacity;
    for (std::size_t c = 0; c < 2; ++c) {
        std::copy(stage.input[c].begin(), stage.input[c].end(), stage.frame.begin());
        std::fill(stage.frame.begin() + static_cast<std::ptrdiff_t>(block), stage.frame.end(), 0.f);
        stage.fft->forward(stage.frame, std::span(stage.history[c]).subspan(stage.head * (block + 1), block + 1));
    }
    stage.used = std::min(stage.used + 1, stage.capacity);
    const bool usable = ir && ir->sampleRate == rate_ && ir->stage[s].parts > 0;
    stage.ir = usable ? ir : nullptr;
    stage.total = stage.ir ? std::min(stage.used, stage.ir->stage[s].parts) : 0;
    stage.done = 0;
    stage.live = stage.ir != nullptr;
    // That block began `block` samples ago; its sum is heard `offset` later.
    stage.start = samples_ - static_cast<std::int64_t>(block) + static_cast<std::int64_t>(HybridImpulse::stageOffset[s]);
}

void HybridReverb::stageSample(std::size_t s, const HybridImpulse *ir) noexcept {
    auto &stage = stages_[s];
    const auto block = HybridImpulse::stageBlock[s];
    const auto position = static_cast<std::size_t>(samples_ % static_cast<std::int64_t>(block));
    if (position != 0) {
        // By sample `position` of the block, that share of the parts is done:
        // a schedule by position alone, so any blocking renders the same bits.
        if (stage.live && ir == stage.ir) {
            stageAccumulate(s, (stage.total * position + block - 1) / block);
        }
        return;
    }
    // A block boundary: the period's sum is heard, the block just completed
    // starts the next. A stage of block B starting at offset S >= 2B - 256
    // has its whole period: the first time it adds to is still to be heard.
    stageFinish(s, ir);
    stageStart(s, ir);
}

void HybridReverb::accumulatePartition(std::size_t p) noexcept {
    // Partition p (from 1) of the NEXT block: the block p blocks before the
    // one that block will write at head_.
    const auto capacity = history_[0].size();
    for (std::size_t c = 0; c < 2; ++c) {
        if (p >= pendingIr_->spectra[c].size())
            continue;
        const auto &prior = history_[c][(head_ + capacity - p) % capacity];
        const auto &kernel = pendingIr_->spectra[c][p];
        auto &sum = pending_[c];
        // The complex product written out: std::complex's operator* recovers
        // infinities (Annex G) at a cost paid on every bin of every partition,
        // and neither factor can hold one (a finite FFT of finite samples).
        for (std::size_t k = 0; k < sum.size(); ++k) {
            const double ar = prior[k].real(), ai = prior[k].imag();
            const double br = kernel[k].real(), bi = kernel[k].imag();
            sum[k] = {sum[k].real() + (ar * br - ai * bi), sum[k].imag() + (ar * bi + ai * br)};
        }
    }
}

void HybridReverb::accumulate(std::size_t position, const HybridImpulse *ir) noexcept {
    // Only the impulse this call was given is alive for it (the sample slot's
    // rule); a new one is summed afresh at the boundary instead.
    if (pendingIr_ == nullptr || pendingParts_ == 0 || ir != pendingIr_)
        return;
    // By sample `position` of 256, that share of the partitions is done. The
    // schedule depends on the position alone, never on the callback's size,
    // so any blocking renders the same bits.
    const auto target = (pendingParts_ * position + 255) / 256;
    while (pendingDone_ < target)
        accumulatePartition(1 + pendingDone_++);
}

void HybridReverb::partition(const HybridImpulse *ir) noexcept {
    const auto capacity = history_[0].size();
    used_ = std::min(used_ + 1, capacity);
    const bool usable = ir && ir->sampleRate == rate_;
    if (pendingIr_ != (usable ? ir : nullptr)) {
        // The impulse changed during the block (a new sample published): the
        // older partitions are summed again from the new one, here, once.
        for (auto &sum : pending_)
            sum.fill({});
        pendingIr_ = usable ? ir : nullptr;
        pendingDone_ = 0;
        pendingParts_ = 0;
        if (pendingIr_) {
            const auto parts = std::min(used_, std::max(ir->spectra[0].size(), ir->spectra[1].size()));
            pendingParts_ = parts > 0 ? parts - 1 : 0;
        }
    }
    accumulate(256, pendingIr_);   // anything left undone, from the impulse this call was given
    for (std::size_t c = 0; c < 2; ++c) {
        auto &newest = history_[c][head_];
        std::array<float, HybridImpulse::fftSize> frame{};
        for (std::size_t i = 0; i < 256; ++i)
            frame[i] = static_cast<float>(input_[c][i]);
        fft_.forward(frame, newest);
        // Accumulated in double (the older partitions, then the newest), then
        // rounded once before the float inverse: a long IR's error does not
        // grow with float additions.
        auto &accumulated = pending_[c];
        if (pendingIr_ && !pendingIr_->spectra[c].empty()) {
            const auto &kernel = pendingIr_->spectra[c][0];
            for (std::size_t k = 0; k < accumulated.size(); ++k)
                accumulated[k] += std::complex<double>(newest[k]) * std::complex<double>(kernel[k]);
        }
        HybridImpulse::Spectrum sum{};
        for (std::size_t k = 0; k < sum.size(); ++k)
            sum[k] = static_cast<std::complex<float>>(accumulated[k]);
        fft_.inverse(sum, frame);
        for (std::size_t i = 0; i < 256; ++i) {
            output_[c][i] = static_cast<double>(frame[i]) + overlap_[c][i];
            overlap_[c][i] = frame[i + 256];
        }
        accumulated.fill({});
    }
    head_ = (head_ + 1) % capacity;
    // The next block: its older partitions are p = 1 to parts - 1, summed
    // over the 256 samples to come from blocks already transformed.
    pendingIr_ = usable ? ir : nullptr;
    pendingDone_ = 0;
    pendingParts_ = 0;
    if (pendingIr_) {
        const auto next = std::min(used_ + 1, capacity);
        const auto parts = std::min(next, std::max(ir->spectra[0].size(), ir->spectra[1].size()));
        pendingParts_ = parts > 0 ? parts - 1 : 0;
    }
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
        // The later stages' share of the convolution sample heard now (it
        // trails the input by the head's latency); its ring cell is cleared.
        const std::int64_t heard = samples_ - static_cast<std::int64_t>(HybridImpulse::partition);
        const std::size_t tailAt = heard >= 0 ? static_cast<std::size_t>(heard) % tailSize : 0;
        for (std::size_t c = 0; c < 2; ++c) {
            dry[c] = dry_[c][cursor_];
            dry_[c][cursor_] = in[c];
            const auto position = (delayAt_ + predelay_[c].size() - delay) % predelay_[c].size();
            const double old = predelay_[c][position];
            const double drive = in[c] * send + (delay ? old * p_[Feedback] : 0);
            predelay_[c][delayAt_] = static_cast<float>(clean(drive));
            const double wet = delay ? old : drive;
            convolution[c] = output_[c][cursor_];
            if (heard >= 0) {
                convolution[c] += tail_[c][tailAt];
                tail_[c][tailAt] = 0;
            }
            input_[c][cursor_] = wet;
            for (std::size_t s = 0; s < HybridImpulse::stages; ++s) {
                const auto block = static_cast<std::int64_t>(HybridImpulse::stageBlock[s]);
                stages_[s].input[c][static_cast<std::size_t>(samples_ % block)] = static_cast<float>(wet);
            }
            algoIn[c] = static_cast<float>(route == 0 ? convolution[c] : aligned_[c][cursor_]);
            aligned_[c][cursor_] = wet;
        }
        if (p_[Freeze] > .5 && p_[FreezeIn] < .5)
            algoIn[0] = algoIn[1] = 0;
        algorithm_.process(&algoIn[0], &algoIn[1], &algoOut[0], &algoOut[1], 1);
        mix_ += (p_[Mix] / 100 - mix_) * smoothing;
        {
            const double target = p_[Mix] / 100;
            if (std::abs(target - mix_) < 1e-12) {
                mix_ = target;
            }
        }
        blend_ += (p_[Blend] / 100 - blend_) * smoothing;
        {
            const double target = p_[Blend] / 100;
            if (std::abs(target - blend_) < 1e-12) {
                blend_ = target;
            }
        }
        double wet[2]{};
        for (std::size_t c = 0; c < 2; ++c)
            wet[c] = route == 2   ? algoOut[c]
                     : route == 3 ? convolution[c]
                                  : (1 - blend_) * convolution[c] + blend_ * algoOut[c];
        const double mid = (wet[0] + wet[1]) * .5, side = (wet[0] - wet[1]) * .5 * p_[Width] / 100;
        // DEV-22: sin/cos, as Live 12.4 measures (dry 0.7071 at its 50 % default).
        const double dryGain = mix_ <= 0 ? 1 : mix_ >= 1 ? 0 : std::cos(mix_ * std::numbers::pi / 2);
        const double wetGain = mix_ <= 0 ? 0 : mix_ >= 1 ? 1 : std::sin(mix_ * std::numbers::pi / 2);
        outL[i] = static_cast<float>(dry[0] * dryGain + (mid + side) * wetGain);
        outR[i] = static_cast<float>(dry[1] * dryGain + (mid - side) * wetGain);
        delayAt_ = (delayAt_ + 1) % predelay_[0].size();
        accumulate(++cursor_, ir);
        if (cursor_ == 256) {
            partition(ir);
            cursor_ = 0;
        }
        ++samples_;
        for (std::size_t s = 0; s < HybridImpulse::stages; ++s) {
            stageSample(s, ir);
        }
    }
}
} // namespace adi::dsp
