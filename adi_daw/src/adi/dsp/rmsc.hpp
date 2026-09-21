// SPDX-License-Identifier: GPL-3.0-or-later
//
// Ring-modulation sidechain ducking ("RMSC") — ADR-0096.
//
// The reference behind the Pd module (ADR-0093 goal 6). The main signal is
// multiplied by (1 - depth * env), where env is the rectified sidechain:
// sample-accurate, zero latency, no attack/release ballistics.
//
// TWO CORRECTIONS to the goal as written:
//
//   1. **The envelope is CLAMPED to [0, 1].** A sidechain above 0 dBFS -- a hot
//      kick, easily -- makes |sc| exceed 1, the gain goes negative, and the main
//      signal comes out polarity-inverted. That is the instability; the clamp
//      removes it, so the gain always lies in [1 - depth, 1].
//
//   2. **The sidebands are what RMSC IS, and they are optional here.** |sc| is
//      rectification, not an envelope: a 60 Hz sidechain gives a 120 Hz
//      ripple, and multiplying by it is audio-rate amplitude modulation, which
//      puts sidebands either side of every partial in the main signal. That is
//      the character. `setSmoothingHz` low-passes the rectified sidechain
//      first, which removes the ripple -- and with it the sidebands -- at the
//      price of the smoothing's own lag. At 0 Hz it is classic RMSC.
//
// The goal also said "multiply/subtract". Subtracting |sc| from the main signal
// is not ducking at all -- it adds a rectified copy of the kick into the mix --
// so this only multiplies.

#pragma once

namespace adi::dsp {

class RingModSidechain {
public:
    void prepare(double sampleRate) noexcept;

    /// 0 leaves the main signal untouched; 1 ducks it fully at a full-scale key.
    void setDepth(double d) noexcept { depth_ = d < 0.0 ? 0.0 : (d > 1.0 ? 1.0 : d); }

    /// Cutoff of a one-pole low-pass on the rectified sidechain. 0 disables it:
    /// raw rectification, classic RMSC, sidebands and all.
    void setSmoothingHz(double hz) noexcept;

    [[nodiscard]] double depth() const noexcept { return depth_; }

    /// Audio thread. `mainIn` and `mainOut` may be the same buffers; the
    /// sidechain is mono. Zero latency: sample n of the key shapes sample n of
    /// the output.
    void process(const float* const* mainIn, float* const* mainOut, int channels,
                 const float* sidechain, int frames) noexcept;

private:
    double sr_ = 48000.0;
    double depth_ = 1.0;
    double smoothHz_ = 0.0;
    double smoothK_ = 1.0;     // 1 = no smoothing
    double env_ = 0.0;
};

}  // namespace adi::dsp
