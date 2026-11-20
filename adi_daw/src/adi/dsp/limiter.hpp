// SPDX-License-Identifier: GPL-3.0-or-later
//
// A lookahead brickwall limiter on SAMPLE PEAK — ADR-0096.
//
// The reference implementation behind the Pd limiter module (ADR-0093 goal 3),
// and the correction to it: the goal named Pd's `env~` for detection, and `env~`
// is an RMS follower -- it outputs power in dB over a window. RMS is the thing a
// brickwall limiter must not use. A single-sample spike has almost no RMS over a
// 1.5 ms window, so an RMS limiter never reacts and the spike leaves at full
// level; the test drives exactly that spike through.
//
// THE GUARANTEE, and why it holds rather than usually holding. With a lookahead
// of L samples the audio is delayed by L, and for every input sample n the
// target gain is t[n] = min(1, ceiling / peak[n]). Then:
//
//   hold[k] = min of t over the window [k-L, k]           (a sliding minimum)
//   rel[k]  = hold[k] when it falls, and rises from rel[k-1] exponentially --
//             so rel[k] <= hold[k] always
//   g[n]    = the average of rel over [n-L, n]            (a boxcar of L+1)
//
// The output is y[n] = x[n-L] * g[n]. Every rel[k] with k in [n-L, n] covers a
// window containing n-L, so every one is <= t[n-L], and so is their average.
// Hence |y[n]| <= |x[n-L]| * t[n-L] <= ceiling, for every sample, exactly. The
// boxcar is also what makes the attack a smooth ramp across the lookahead
// instead of a step.
//
// Channels are LINKED: one gain for all of them, so a peak on the left does not
// move the stereo image by ducking only the left.
//
// Allocation-free after `prepare`. Latency is exactly the lookahead, and the
// device wrapping this must report it -- which is what ADR-0095 exists for.
//
// TRUE PEAK, OFF BY DEFAULT (DEV-29 c, EX-05 LD5). A sample-peak brickwall
// lets a sine at a quarter of the rate, sampled 45 degrees off its crest,
// leave 3 dB over the ceiling between the samples. With setTruePeak(true) the
// detector is the meter's own interpolator (TruePeakMeter::design, BS.1770
// Annex 2's rate rule) -- upsample, then hold the largest value -- and the
// target for audio sample a is ceiling / the largest interpolated value within
// kTruePeakSpread samples of it, so every sample that builds an over between
// the samples is turned down by at least what that over needs. The detector
// reads 8 samples behind the input and the spread adds kTruePeakSpread, so
// the latency is the lookahead plus truePeakDelay() -- whole samples, and
// reported. The sample-peak guarantee above still holds exactly (a true peak
// is never below the sample peak); between the samples, where the gain moves
// inside the interpolator's span, a residue is left: under 0.001 dB with a
// lookahead of 64 samples or more and under 0.05 dB with none, on the tests'
// noise, bursts and doublets. The encoded check after dither stays the final
// gate (the refine loop in NormalizationCapture is the engine's). Below the ceiling the output is the
// input, delayed, bit for bit. From 176.4 kHz the Annex oversamples by 1 and
// the mode is the sample-peak limiter with no added delay.
//
// float or double audio; the delay line is double either way.

#pragma once

#include <cstdint>
#include <vector>

namespace adi::dsp {

class LookaheadLimiter {
public:
    /// Message thread. Allocates for the largest lookahead that will be asked.
    void prepare(double sampleRate, int channels, int maxLookaheadSamples);

    /// Samples of lookahead, clamped to what `prepare` allowed. Resets the
    /// state, because the delay line changes length: changing it is a latency
    /// change and the caller reports it.
    void setLookahead(int samples);
    /// Lookahead from milliseconds at the prepared rate, rounded to a WHOLE
    /// number of samples -- the unit ADR-0095 reports in.
    void setLookaheadMs(double ms);

    /// Linear, not dB. 0.966 is -0.3 dBFS.
    void setCeiling(double linear) noexcept { ceiling_ = linear > 0.0 ? linear : 1.0e-6; }
    /// Retained in seconds across prepare(), including a sample-rate change.
    void setReleaseSeconds(double s) noexcept;

    /// DEV-29 c: detect TRUE peak rather than sample peak. Off by default.
    /// Resets the state and changes the latency: the caller reports it.
    void setTruePeak(bool on) noexcept;
    [[nodiscard]] bool truePeak() const noexcept { return truePeak_; }
    /// The samples true-peak detection adds to the lookahead at the prepared
    /// rate: TruePeakMeter::kDelay + kTruePeakSpread when the rate is
    /// oversampled, else 0.
    [[nodiscard]] int truePeakDelay() const noexcept { return tpDelay_; }
    static constexpr int kTruePeakSpread = 2;

    [[nodiscard]] int latencySamples() const noexcept {
        return lookahead_ + (truePeak_ ? tpDelay_ : 0);
    }
    [[nodiscard]] double ceiling() const noexcept { return ceiling_; }
    /// The gain applied to the last sample produced, for metering and tests.
    [[nodiscard]] double lastGain() const noexcept { return lastGain_; }

    /// Audio thread. `in` and `out` may be the same buffers.
    void process(const float* const* in, float* const* out, int frames) noexcept;
    void process(const double* const* in, double* const* out, int frames) noexcept;

private:
    void reset() noexcept;
    template <class Sample>
    void run(const Sample* const* in, Sample* const* out, int frames) noexcept;
    /// True-peak mode: takes input frame i, returns the target gain for the
    /// sample truePeakDelay() behind it.
    template <class Sample>
    double truePeakTarget(const Sample* const* in, int i) noexcept;

    double sr_ = 48000.0;
    int channels_ = 0;
    int maxLookahead_ = 0;
    int lookahead_ = 0;
    double ceiling_ = 1.0;
    double releaseSeconds_ = 0.05;
    double releaseCoef_ = 0.0;

    // True peak: the interpolator, each channel's last kTapsPerPhase inputs,
    // and the last 2 * spread + 1 segment peaks.
    bool truePeak_ = false;
    int factor_ = 1;
    int tpDelay_ = 0;
    std::vector<double> taps_;
    std::vector<double> history_;
    int historyHead_ = 0;
    double segments_[2 * kTruePeakSpread + 1] = {};
    int segmentHead_ = 0;

    // The delayed audio: one ring per channel, maxLookahead + truePeakDelay
    // + 1 long.
    std::vector<double> delay_;
    int ring_ = 1;
    int writeIdx_ = 0;

    // The sliding minimum of the target gain: a monotonic deque over a ring.
    std::vector<double> dqValue_;
    std::vector<std::int64_t> dqIndex_;
    int dqHead_ = 0, dqSize_ = 0, dqCap_ = 1;

    // The boxcar over the released gain.
    std::vector<double> box_;
    int boxIdx_ = 0;
    double boxSum_ = 0.0;

    double rel_ = 1.0;
    double lastGain_ = 1.0;
    std::int64_t n_ = 0;
};

}  // namespace adi::dsp
