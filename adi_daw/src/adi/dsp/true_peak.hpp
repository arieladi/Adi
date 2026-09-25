// SPDX-License-Identifier: GPL-3.0-or-later
//
// True peak, by ITU-R BS.1770-4 Annex 2's method -- ADR-0167 d5.
//
// A sample peak misses what the DAC's reconstruction puts between samples: a
// sine at a quarter of the rate, sampled 45 degrees off its crest, has sample
// peaks 3 dB below the wave. The Annex's method is to oversample -- 4x at 44.1
// and 48 kHz, 2x at 88.2 and 96 kHz, not at all from 176.4 kHz, so the
// oversampled rate is always at least 176.4 kHz -- and take the largest
// absolute value.
//
// THE FILTER IS OURS. The Annex gives an example interpolator, not a required
// one. This is a Kaiser-windowed sinc, 16 taps per phase, each phase scaled to
// unity gain at DC so a constant reads as itself, and verified by the tests
// against sines whose crest is known analytically. The original samples count
// too, so a true peak is never below the sample peak.
//
// prepare() allocates and designs; process() allocates nothing.

#pragma once

#include <cstdint>
#include <vector>

namespace adi::dsp {

class TruePeakMeter {
public:
    static constexpr int kTapsPerPhase = 16;
    static constexpr int kMaxChannels = 8;

    /// Message thread: the rate decides the oversampling.
    void prepare(double sampleRate);

    /// Audio or analysis thread. Raises the held peak with this block.
    void process(const float* const* x, int channels, int frames) noexcept;

    /// The largest absolute value since the last reset, linear (1.0 = 0 dBTP).
    [[nodiscard]] double peak() const noexcept { return peak_; }
    [[nodiscard]] double peakDb() const noexcept;
    /// Clears the held peak AND the filter's history: a new signal.
    void reset() noexcept;
    /// Clears only the held peak: a meter's hold released on a running signal.
    void resetPeak() noexcept { peak_ = 0.0; }

    /// 4, 2 or 1.
    [[nodiscard]] int oversampling() const noexcept { return factor_; }

private:
    int factor_ = 4;
    std::vector<double> taps_;                         // factor_ phases x kTapsPerPhase
    double history_[kMaxChannels][kTapsPerPhase] = {};
    int head_ = 0;
    double peak_ = 0.0;
};

}  // namespace adi::dsp
