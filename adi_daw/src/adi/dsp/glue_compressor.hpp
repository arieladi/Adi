// SPDX-License-Identifier: GPL-3.0-or-later
//
// Live 12's Glue Compressor, the core (DEV-22, ADR-0249 d7: after the
// Compressor). A bus compressor of discrete choices, stereo-linked on the
// louder channel, feed-forward in the log domain:
//
//   Threshold  dBFS.
//   Ratio      2, 4 or 10 to 1 (choices 0, 1, 2); a hard knee.
//   Attack     0.01, 0.1, 0.3, 1, 3, 10 or 30 ms (choices 0 to 6): the time
//              constant of the reduction growing, in dB.
//   Release    0.1, 0.2, 0.4, 0.6, 0.8 or 1.2 s, or Auto (choices 0 to 6):
//              the time constant of the reduction falling. Auto releases in
//              two stages, 0.1 s from a fast peak and 1.2 s from a held level.
//   Makeup     dB after the reduction.
//   Range      the most reduction it will apply, dB.
//   Dry/Wet    linear, the dry path aligned (there is no lookahead).
//   Soft Clip  tanh toward a -0.5 dBFS ceiling after the makeup.
//
// WHAT IS NOT LIVE'S YET, and says so: the defaults, the knee, the detector
// (here the instantaneous peak), Auto's two-stage law, the Soft Clip curve
// and ceiling, and Dry/Wet's law wait on a Live 12.4 reading of the Glue
// Compressor (the devices log's ONE list). Until then it claims no parity.
//
// Allocation-free and zero-latency; a non-finite sample reads as silence.

#pragma once

#include <array>
#include <cstddef>

namespace adi::dsp {

class GlueCompressor {
public:
    enum Param { Threshold, Ratio, Attack, Release, Makeup, Range, DryWet, SoftClip, Count };
    struct Bounds {
        double low, high, initial;
    };
    static constexpr Bounds ranges[Count] = {{-40, 0, -20}, {0, 2, 1},  {0, 6, 2},     {0, 6, 6},
                                             {0, 20, 0},    {0, 70, 70}, {0, 100, 100}, {0, 1, 0}};
    static constexpr double kRatios[3] = {2, 4, 10};
    static constexpr double kAttackMs[7] = {0.01, 0.1, 0.3, 1, 3, 10, 30};
    static constexpr double kReleaseS[6] = {0.1, 0.2, 0.4, 0.6, 0.8, 1.2};
    static constexpr double kSoftClipDb = -0.5;

    GlueCompressor() noexcept;
    void prepare(double sampleRate) noexcept;
    void set(Param, double) noexcept;
    [[nodiscard]] double value(Param p) const noexcept { return p_[static_cast<std::size_t>(p)]; }
    [[nodiscard]] int latency() const noexcept { return 0; }
    /// The static curve: the reduction in dB (0 or negative) for a level in
    /// dBFS, before Range.
    [[nodiscard]] double staticGainDb(double levelDb) const noexcept;
    /// The reduction now, in dB (0 or negative), for a meter.
    [[nodiscard]] double gainReductionDb() const noexcept { return reductionDb_; }
    void process(const float* inL, const float* inR, float* outL, float* outR, std::size_t frames) noexcept;

private:
    void update() noexcept;
    std::array<double, Count> p_{};
    double rate_ = 48000, threshold_ = -20, slope_ = 0.75, attack_ = 0, release_ = 0, fast_ = 0, slow_ = 0;
    double range_ = 70, makeup_ = 1, wet_ = 1, ceiling_ = 1;
    double reductionDb_ = 0, slowDb_ = 0;
    bool autoRelease_ = true, soft_ = false;
};

}  // namespace adi::dsp
