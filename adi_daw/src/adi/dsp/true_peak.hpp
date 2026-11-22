// SPDX-License-Identifier: GPL-3.0-or-later
//
// True peak, by ITU-R BS.1770-4 Annex 2's method -- ADR-0167 d5.
//
// A sample peak misses what the DAC's reconstruction puts between samples: a
// sine at a quarter of the rate, sampled 45 degrees off its crest, has sample
// peaks 3 dB below the wave. The Annex's method is to oversample and take the
// largest absolute value. The factor is the smallest whole number that brings
// the rate to at least 176.4 kHz, up to 8: 4x at 44.1 and 48 kHz, 2x at 88.2
// and 96 kHz, none from 176.4 kHz, 6x at 32 kHz, 8x at 22.05 kHz -- so the
// oversampled rate is at least 176.4 kHz from 22.05 kHz up (DF-E20 (d): it was
// 128 kHz at 32 kHz).
//
// THE FILTER IS OURS. The Annex gives an example interpolator, not a required
// one. This is a Kaiser-windowed sinc, 16 taps per phase, each phase scaled to
// unity gain at DC so a constant reads as itself, and verified by the tests
// against sines whose crest is known analytically. The original samples count
// too, so a true peak is never below the sample peak.
//
// Any number of channels (prepare's count); float or double input.
// prepare() allocates and designs; process() allocates nothing.

#pragma once

#include <cstdint>
#include <vector>

namespace adi::dsp {

class TruePeakMeter {
public:
    static constexpr int kTapsPerPhase = 16;
    static constexpr int kMaxFactor = 8;
    /// The channels prepare() makes room for when it is not told.
    static constexpr int kDefaultChannels = 8;

    /// Message thread: the rate decides the oversampling; room for `channels`.
    void prepare(double sampleRate, int channels = kDefaultChannels);

    /// Audio or analysis thread. Raises the held peak with this block. Channels
    /// beyond the prepared count are not read.
    void process(const float* const* x, int channels, int frames) noexcept;
    void process(const double* const* x, int channels, int frames) noexcept;

    /// The largest absolute value since the last reset, linear (1.0 = 0 dBTP).
    [[nodiscard]] double peak() const noexcept { return peak_; }
    [[nodiscard]] double peakDb() const noexcept;
    /// Clears the held peak AND the filter's history: a new signal.
    void reset() noexcept;
    /// Clears only the held peak: a meter's hold released on a running signal.
    void resetPeak() noexcept { peak_ = 0.0; }

    /// 1 to kMaxFactor.
    [[nodiscard]] int oversampling() const noexcept { return factor_; }

    /// The oversampling the Annex asks for at `sampleRate` (see the header).
    [[nodiscard]] static int factorFor(double sampleRate) noexcept;
    /// The interpolator, `factor` phases of kTapsPerPhase: tap p + k*factor
    /// weighs x[n - k] for phase p. Phase p at input sample n reads the signal
    /// at n - 8 + (2p + 1) / (2 * factor) -- strictly between n - 8 and n - 7
    /// (kDelay). One tap of 1 when factor is 1. The limiter's true-peak
    /// detector (DEV-29 c) uses the same one, so a limited signal measures
    /// at the ceiling on this meter.
    static void design(int factor, std::vector<double>& taps);
    static constexpr int kDelay = kTapsPerPhase / 2;
    [[nodiscard]] int channels() const noexcept { return channels_; }

private:
    template <class Sample>
    void run(const Sample* const* x, int channels, int frames) noexcept;
    int factor_ = 4;
    int channels_ = 0;
    std::vector<double> taps_;                         // factor_ phases x kTapsPerPhase
    std::vector<double> history_;                      // channels_ x kTapsPerPhase
    int head_ = 0;
    double peak_ = 0.0;
};

}  // namespace adi::dsp
