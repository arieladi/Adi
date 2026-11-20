// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <array>
#include <cstddef>
#include <vector>

namespace adi::dsp {

// One stereo band of the Sweep stage. Both contour heights are normalized.
// Polarity belongs to the effect response and can change at zero feedback.
class AdiShaperSweep {
public:
    enum class Mode { Off, Flange, Phaser4, Phaser8, Phaser12, Phaser16 };

    // The only allocating operation. A failed preparation leaves dry passthrough.
    bool prepare(double sampleRate) noexcept;
    void reset() noexcept;
    void setMode(Mode mode) noexcept;
    void setPolarity(bool negative) noexcept;
    void processStereo(float& left, float& right, float centerHeight,
                       float feedbackHeight) noexcept;

private:
    using Stereo = std::array<float, 2>;
    struct Tuning { float delay = 1.0f, pole = 0.0f; };
    static constexpr std::size_t intervals = 1024;
    void updateCenter(float value) noexcept;
    void updateFeedback(float value) noexcept;
    void flange(float& left, float& right) noexcept;
    void phase(float& left, float& right) noexcept;

    Mode mode_ = Mode::Off;
    bool negative_ = false, ready_ = false;
    unsigned stages_ = 0;
    std::array<Tuning, intervals + 1> tuning_{};
    std::vector<Stereo> delay_;
    std::size_t write_ = 0, valid_ = 0, mask_ = 0, delayFrames_ = 1;
    Stereo low_{};
    std::array<Stereo, 16> states_{};
    float center_ = -1, feedback_ = -1, delayFraction_ = 0, highpassAmount_ = 0;
    float pole_ = 0, cascadeGain_ = 0, inverse_ = 1;
    float dry_ = 1, wet_ = 0, loop_ = 0;
};

} // namespace adi::dsp
