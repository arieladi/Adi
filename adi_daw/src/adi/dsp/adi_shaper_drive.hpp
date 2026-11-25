// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <array>

namespace adi::dsp {

// Fully wet stereo kernel. The rack delays its dry branch before blending.
class AdiShaperDrive {
  public:
    enum class Mode { Saturate, HardClip, Rectify };
    enum class Fold { Off, Sine, Triangle, Saw };
    static constexpr int latencySamples = 16;
    AdiShaperDrive() noexcept;
    void prepare(double sampleRate) noexcept;
    void reset() noexcept;
    void setMode(Mode mode) noexcept;
    void setRoundedClip(bool enabled) noexcept;
    void setPowerSaturation(bool enabled) noexcept;
    void setFold(Fold shape) noexcept;
    void setTrims(float inputNormalized, float outputNormalized) noexcept;
    void process(float& left, float& right, float driveNormalized) noexcept;
    bool finishBlock() noexcept;

  private:
    static constexpr int maxFactor = 4, inputs = 17, maxTaps = 16 * maxFactor + 1;
    using Stereo = std::array<float, 2>;
    using StereoSum = std::array<double, 2>;
    using Phases = std::array<StereoSum, maxFactor>;
    enum class Response { Saturate, HardClip, Rectify, Rounded, Power, Sine, Triangle, Saw };
    struct Filters {
        std::array<double, inputs / 2> oddSum{}, oddDifference{};
        std::array<std::array<double, inputs>, maxFactor> up{};
        std::array<double, maxTaps> down{};
        void preparePhases(int factor, int taps) noexcept;
    };
    template<int Factor> [[nodiscard]] Phases interpolate(const Stereo* history, const Filters& filters) const noexcept;
    template<int Factor, Response Style> void writePhases(const Phases& phases, std::size_t head) noexcept;
    template<int Factor> [[nodiscard]] StereoSum decimate(const Stereo* history, const Filters& filters) const noexcept;
    template<int Factor> [[nodiscard]] StereoSum render(const Stereo* input) noexcept;
    Filters legacyFilters_{}, shapingFilters_{};
    std::array<Stereo, inputs * 2> input_{};
    std::array<Stereo, inputs * maxFactor * 2> wet_{};
    struct Channel {
        double lastZ = 0.0, dc = 0.0;
    };
    std::array<Channel, 2> channels_{};
    int factor_ = 4, taps_ = maxTaps, inputHead_ = 0, wetHead_ = 0;
    Mode mode_ = Mode::Saturate;
    Fold fold_ = Fold::Off;
    bool roundedClip_ = false, powerSaturation_ = false;
    Response response_ = Response::Saturate;
    void updateResponse() noexcept;
    [[nodiscard]] bool usesDelayedControl() const noexcept { return response_ >= Response::Rounded; }
    // The strength contour reaches the transfer at the center of the interpolation filter.
    static constexpr std::size_t controlDelay = 8;
    std::array<float, controlDelay> strengthHistory_{};
    std::size_t strengthHead_ = controlDelay;
    float lastDelayed_ = 0.0f;
    struct Transfer {
        double pre = 1.0, post = 1.0;
        double exponent = 1.0, knee = .85, kneeSquared = .0225, kneeOffset = -.7;
        bool active = false;
    };
    Transfer current_{}, first_{};
    float previousDrive_ = -1.0f, inputTrim_ = 0.5f, outputTrim_ = 0.5f;
    double inputGain_ = 1.0, outputGain_ = 1.0;
    double driveFade_ = 0.0;
    double dcRate_ = 0.0, sampleRate_ = 0.0;
    void tune(float amount) noexcept;
    template<Response Style> [[nodiscard]] double shape(double z, Channel& channel) const noexcept;
};

} // namespace adi::dsp
