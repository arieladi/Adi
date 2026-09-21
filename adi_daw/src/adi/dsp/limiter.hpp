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
    void setReleaseSeconds(double s) noexcept;

    [[nodiscard]] int latencySamples() const noexcept { return lookahead_; }
    [[nodiscard]] double ceiling() const noexcept { return ceiling_; }
    /// The gain applied to the last sample produced, for metering and tests.
    [[nodiscard]] double lastGain() const noexcept { return lastGain_; }

    /// Audio thread. `in` and `out` may be the same buffers.
    void process(const float* const* in, float* const* out, int frames) noexcept;

private:
    void reset() noexcept;

    double sr_ = 48000.0;
    int channels_ = 0;
    int maxLookahead_ = 0;
    int lookahead_ = 0;
    double ceiling_ = 1.0;
    double releaseCoef_ = 0.0;

    // The delayed audio: one ring per channel, maxLookahead + 1 long.
    std::vector<float> delay_;
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
