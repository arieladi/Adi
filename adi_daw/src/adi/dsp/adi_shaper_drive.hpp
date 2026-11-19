// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <array>

namespace adi::dsp {

// Fully wet stereo kernel. The rack delays its dry branch before blending.
class AdiShaperDrive {
  public:
    enum class Mode { Saturate, HardClip, Rectify };
    static constexpr int latencySamples = 16;
    AdiShaperDrive() noexcept;
    void prepare(double sampleRate) noexcept;
    void reset() noexcept;
    void setMode(Mode mode) noexcept;
    void setTrims(float inputNormalized, float outputNormalized) noexcept;
    void process(float& left, float& right, float driveNormalized) noexcept;

  private:
    static constexpr int maxFactor = 4, inputs = 17, maxTaps = 16 * maxFactor + 1;
    using Stereo = std::array<float, 2>;
    using StereoSum = std::array<double, 2>;
    using Phases = std::array<StereoSum, maxFactor>;
    template<int Factor> [[nodiscard]] Phases interpolate(const Stereo* history) const noexcept;
    std::array<double, inputs/2> oddSum_{}, oddDifference_{};
    std::array<Stereo, inputs * 2> input_{};
    std::array<Stereo, maxTaps * 2> wet_{};
    struct Channel {
        double lastZ = 0.0, dc = 0.0;
    };
    std::array<Channel, 2> channels_{};
    std::array<std::array<float, inputs>, maxFactor> interpolate_{};
    std::array<float, maxTaps> decimate_{};
    int factor_ = 4, taps_ = maxTaps, inputHead_ = 0, wetHead_ = 0;
    Mode mode_ = Mode::Saturate;
    float previousDrive_ = -1.0f, inputTrim_ = 0.5f, outputTrim_ = 0.5f;
    double inputGain_ = 1.0, outputGain_ = 1.0, pre_ = 1.0, post_ = 1.0;
    double dcRate_ = 0.0, sampleRate_ = 0.0;
    void tune(float amount) noexcept;
    [[nodiscard]] double transfer(double z) const noexcept;
    [[nodiscard]] double shape(double z, Channel& channel) const noexcept;
};

} // namespace adi::dsp
