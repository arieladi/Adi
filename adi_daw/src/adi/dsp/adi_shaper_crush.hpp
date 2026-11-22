// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <array>
#include <cstdint>

namespace adi::dsp {

// One stereo band, normalized bit-resolution and hold-rate contours.
// A fully wet neutral band retains its measured 10 Hz DC response. The rack
// owns the dry/wet blend. Processing is allocation-free and sample-continuous.
class AdiShaperCrush {
public:
    bool prepare(double sampleRate) noexcept;
    void reset() noexcept;
    void setDither(bool enabled) noexcept { dither_ = enabled; }
    void processStereo(float& left, float& right, float bitsHeight,
                       float resampleHeight) noexcept;
    bool finishBlock() noexcept;

private:
    using Stereo = std::array<double, 2>;
    void updateResolution(float height) noexcept;
    void updateClock(float height) noexcept;
    double noise() noexcept;
    void quantize(Stereo& sample) noexcept;
    void hold(Stereo& sample) noexcept;

    double rate_ = 0, highpassAmount_ = 0, highpassGain_ = 1;
    double levels_ = 1, quantizedGain_ = 1, step_ = 1, period_ = 1, phase_ = 0;
    float bits_ = -1, clock_ = -1;
    Stereo previousQuantInput_{}, previousHoldInput_{}, held_{}, low_{};
    std::uint32_t seed_ = 0x297a04b3u;
    bool dither_ = false;
};

} // namespace adi::dsp
