// SPDX-License-Identifier: GPL-3.0-or-later

#include "adi/dsp/limiter.hpp"

#include "adi/dsp/true_peak.hpp"

#include <algorithm>
#include <cmath>

namespace adi::dsp {

void LookaheadLimiter::prepare(double sampleRate, int channels, int maxLookaheadSamples) {
    sr_ = sampleRate > 0.0 ? sampleRate : 48000.0;
    channels_ = channels > 0 ? channels : 1;
    maxLookahead_ = maxLookaheadSamples > 0 ? maxLookaheadSamples : 0;
    factor_ = TruePeakMeter::factorFor(sr_);
    TruePeakMeter::design(factor_, taps_);
    tpDelay_ = factor_ > 1 ? TruePeakMeter::kDelay + kTruePeakSpread : 0;
    history_.assign(static_cast<std::size_t>(channels_) * TruePeakMeter::kTapsPerPhase, 0.0);
    ring_ = maxLookahead_ + tpDelay_ + 1;
    delay_.assign(static_cast<std::size_t>(channels_) * static_cast<std::size_t>(ring_), 0.0);
    dqCap_ = maxLookahead_ + 2;
    dqValue_.assign(static_cast<std::size_t>(dqCap_), 1.0);
    dqIndex_.assign(static_cast<std::size_t>(dqCap_), 0);
    box_.assign(static_cast<std::size_t>(ring_), 1.0);
    setReleaseSeconds(releaseSeconds_);
    lookahead_ = std::min(lookahead_, maxLookahead_);
    reset();
}

void LookaheadLimiter::setLookahead(int samples) {
    lookahead_ = std::clamp(samples, 0, maxLookahead_);
    reset();
}

void LookaheadLimiter::setTruePeak(bool on) noexcept {
    truePeak_ = on;
    reset();
}

void LookaheadLimiter::setLookaheadMs(double ms) {
    setLookahead(static_cast<int>(std::lround(ms * sr_ / 1000.0)));
}

void LookaheadLimiter::setReleaseSeconds(double s) noexcept {
    releaseSeconds_ = s > 1.0e-5 ? s : 1.0e-5;
    releaseCoef_ = std::exp(-1.0 / (releaseSeconds_ * sr_));
}

void LookaheadLimiter::reset() noexcept {
    std::fill(delay_.begin(), delay_.end(), 0.0);
    std::fill(history_.begin(), history_.end(), 0.0);
    historyHead_ = 0;
    std::fill(std::begin(segments_), std::end(segments_), 0.0);
    segmentHead_ = 0;
    writeIdx_ = 0;
    dqHead_ = 0;
    dqSize_ = 0;
    // The boxcar starts FULL OF UNITY, so the first samples out are transparent
    // rather than faded in from a gain of zero.
    std::fill(box_.begin(), box_.end(), 1.0);
    boxIdx_ = 0;
    boxSum_ = static_cast<double>(lookahead_ + 1);
    rel_ = 1.0;
    lastGain_ = 1.0;
    n_ = 0;
}

void LookaheadLimiter::process(const float* const* in, float* const* out, int frames) noexcept {
    run(in, out, frames);
}

void LookaheadLimiter::process(const double* const* in, double* const* out, int frames) noexcept {
    run(in, out, frames);
}

template <class Sample>
double LookaheadLimiter::truePeakTarget(const Sample* const* in, int i) noexcept {
    constexpr int kTaps = TruePeakMeter::kTapsPerPhase;
    // The segment from sample j = n - 8 to j + 1: |x[j]| and every phase the
    // interpolator puts between j and j + 1 (all of them land there; see
    // TruePeakMeter::design), across all channels.
    const int slot = historyHead_;
    historyHead_ = (historyHead_ + 1) % kTaps;
    double peak = 0.0;
    for (int c = 0; c < channels_; ++c) {
        double* h = history_.data() + static_cast<std::size_t>(c) * kTaps;
        h[slot] = static_cast<double>(in[c][i]);
        peak = std::max(peak, std::fabs(h[(slot - TruePeakMeter::kDelay + kTaps) % kTaps]));
        for (int p = 0; p < factor_; ++p) {
            double y = 0.0;
            int at = slot;
            for (int k = 0; k < kTaps; ++k) {
                y += taps_[static_cast<std::size_t>(p + k * factor_)] * h[at];
                at = at == 0 ? kTaps - 1 : at - 1;
            }
            peak = std::max(peak, std::fabs(y));
        }
    }
    // The sample `spread` segments back takes the largest of the
    // 2 * spread + 1 segments around it.
    segments_[segmentHead_] = peak;
    segmentHead_ = (segmentHead_ + 1) % (2 * kTruePeakSpread + 1);
    double around = 0.0;
    for (double s : segments_) around = std::max(around, s);
    return around > ceiling_ ? ceiling_ / around : 1.0;
}

template <class Sample>
void LookaheadLimiter::run(const Sample* const* in, Sample* const* out, int frames) noexcept {
    const int L = lookahead_;
    const int win = L + 1;
    const bool tp = truePeak_ && factor_ > 1;
    const int audioDelay = L + (tp ? tpDelay_ : 0);

    for (int i = 0; i < frames; ++i) {
        // --- detection: SAMPLE PEAK, or TRUE PEAK, across all channels -----
        double t = 1.0;
        if (tp) {
            t = truePeakTarget(in, i);
        } else {
            double peak = 0.0;
            for (int c = 0; c < channels_; ++c) {
                const double a = std::fabs(static_cast<double>(in[c][i]));
                if (a > peak) peak = a;
            }
            t = peak > ceiling_ ? ceiling_ / peak : 1.0;
        }

        // --- sliding minimum of t over the last L+1 samples ---------------
        while (dqSize_ > 0) {
            const int back = (dqHead_ + dqSize_ - 1) % dqCap_;
            if (dqValue_[static_cast<std::size_t>(back)] >= t) --dqSize_;
            else break;
        }
        {
            const int slot = (dqHead_ + dqSize_) % dqCap_;
            dqValue_[static_cast<std::size_t>(slot)] = t;
            dqIndex_[static_cast<std::size_t>(slot)] = n_;
            ++dqSize_;
        }
        while (dqIndex_[static_cast<std::size_t>(dqHead_)] <= n_ - win) {
            dqHead_ = (dqHead_ + 1) % dqCap_;
            --dqSize_;
        }
        const double hold = dqValue_[static_cast<std::size_t>(dqHead_)];

        // --- release: down at once, up exponentially, never above hold -----
        rel_ = hold < rel_ ? hold : hold + (rel_ - hold) * releaseCoef_;

        // --- boxcar of rel over the last L+1 samples ----------------------
        boxSum_ += rel_ - box_[static_cast<std::size_t>(boxIdx_)];
        box_[static_cast<std::size_t>(boxIdx_)] = rel_;
        if (++boxIdx_ == win) {
            boxIdx_ = 0;
            // Re-summed once per window. A running add-and-subtract drifts,
            // and a drift upward is a gain above the one the guarantee allows.
            double s = 0.0;
            for (int k = 0; k < win; ++k) s += box_[static_cast<std::size_t>(k)];
            boxSum_ = s;
        }
        const double g = boxSum_ / static_cast<double>(win);

        // --- the delayed audio, times the gain ------------------------------
        const int readIdx = (writeIdx_ - audioDelay + ring_) % ring_;
        for (int c = 0; c < channels_; ++c) {
            double* ringC = delay_.data() + static_cast<std::size_t>(c) *
                                            static_cast<std::size_t>(ring_);
            ringC[writeIdx_] = static_cast<double>(in[c][i]);
            out[c][i] = static_cast<Sample>(ringC[readIdx] * g);
        }
        writeIdx_ = (writeIdx_ + 1) % ring_;
        lastGain_ = g;
        ++n_;
    }
}

}  // namespace adi::dsp
