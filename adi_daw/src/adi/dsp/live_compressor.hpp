// SPDX-License-Identifier: GPL-3.0-or-later
//
// Live 12's Compressor, the core only (DEV-22: the queue's first new effect).
//
// A feed-forward compressor in the log domain, stereo-linked on the louder
// channel: a level detector (Peak, RMS, or Expand's downward expansion), a
// static gain computer with a quadratic soft knee, and a ballistics stage that
// smooths either the gain reduction in dB (Log) or the detected level (Lin).
// Lookahead delays the audio, never the detector, by 0, 1 or 10 ms, rounded to
// whole samples: 480 at 48 kHz for 10 ms, the delay Live 12.4 reports (the
// research's latency table). The dry path waits the same, so Dry/Wet stays
// coherent.
//
// WHAT IS NOT LIVE'S YET, and says so: the defaults, Makeup's law (here the
// static curve's reduction at 0 dBFS, made up in full), the RMS window (10 ms),
// the Dry/Wet law (linear) and Auto Release (not here) wait on the director's
// reading of Live's Compressor (reading 17 in the devices log). Until then this
// core claims no parity and is in no catalogue.
//
// Allocation-free: the lookahead lines are fixed arrays sized for 10 ms at
// 384 kHz.

#pragma once

#include <array>
#include <cstddef>

namespace adi::dsp {

class LiveCompressor {
public:
    enum Param { Threshold, Ratio, Attack, Release, Knee, Lookahead, Mode, Envelope, Makeup, Output, DryWet, Count };
    enum DetectorMode { Peak, Rms, Expand };
    enum EnvelopeMode { Lin, Log };
    struct Range {
        double low, high, initial;
    };
    // Threshold dB; Ratio n:1 (100 is infinite); Attack and Release ms; Knee dB;
    // Lookahead 0/1/10 ms as 0/1/2; Mode; Envelope; Makeup on/off; Output dB;
    // Dry/Wet %. Initial values are ADI's until reading 17.
    static constexpr Range ranges[Count] = {
        {-70, 6, -20}, {1, 100, 4}, {0.01, 1000, 2}, {1, 3000, 50}, {0, 18, 6}, {0, 2, 0},
        {0, 2, Peak},  {0, 1, Log}, {0, 1, 0},       {-36, 36, 0},  {0, 100, 100}};
    static constexpr int kMaxLookahead = 4096;  // 10 ms at 384 kHz is 3,840

    LiveCompressor() noexcept;
    void prepare(double sampleRate) noexcept;
    void set(Param, double) noexcept;
    [[nodiscard]] double value(Param p) const noexcept { return p_[static_cast<std::size_t>(p)]; }
    /// The audio's delay: the lookahead in whole samples at the prepared rate.
    [[nodiscard]] int latency() const noexcept { return lookahead_; }
    /// The static curve: gain change in dB (never positive for Peak and RMS)
    /// for a detected level in dBFS, before makeup.
    [[nodiscard]] double staticGainDb(double levelDb) const noexcept;
    /// The current gain reduction in dB (0 or negative), for a meter.
    [[nodiscard]] double gainReductionDb() const noexcept { return reductionDb_; }
    void process(const float* inL, const float* inR, float* outL, float* outR, std::size_t frames) noexcept;

private:
    void update() noexcept;
    std::array<double, Count> p_{};
    std::array<std::array<double, kMaxLookahead>, 2> line_{};
    std::size_t write_ = 0;
    int lookahead_ = 0;
    double rate_ = 48000, attack_ = 0, release_ = 0, rms_ = 0, power_ = 0, level_ = 0;
    double reductionDb_ = 0, makeupDb_ = 0, output_ = 1, outputTarget_ = 1, wet_ = 1, wetTarget_ = 1, smooth_ = 0;
};

}  // namespace adi::dsp
