// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include <array>
#include <optional>
#include <span>
#include <cstdint>

namespace adi::dsp {

// Prepared coefficients can be reused across channels with independent histories.
class StereoMeterWeighting {
  public:
    enum class Weighting { Standard, A, C, K };
    struct State { double first = 0, second = 0; };
    using History = std::array<State, 4>;

    bool prepare(double sampleRate, Weighting) noexcept;
    bool flat() const noexcept { return sectionCount_ == 0; }
    double process(double sample, History &history) const noexcept {
        for (std::size_t section = 0; section < sectionCount_; ++section) {
            const auto &c = coefficients_[section];
            auto &state = history[section];
            const auto result = c.b0 * sample + state.first;
            state.first = c.b1 * sample - c.a1 * result + state.second;
            state.second = c.b2 * sample - c.a2 * result;
            sample = result;
        }
        return sample;
    }

  private:
    struct Biquad {
        double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0;
    };
    std::array<Biquad, 4> coefficients_{};
    std::size_t sectionCount_ = 0;
};

// Passive analysis of contiguous stereo samples. One owner supplies new
// samples exactly once and may read the most recent result at any display cadence.
class StereoEnvelopeMeter {
  public:
    using Weighting = StereoMeterWeighting::Weighting;
    struct Settings {
        double sampleRate;
        double peakDecaySeconds;       // amplitude time constant after a peak
        double meanSquareSeconds;      // power integration time constant
        double coincidenceSeconds;     // time constant of signed coincidence
        double coincidenceFloor;       // linear magnitude, applied to both channels
        bool midSide = false;
        Weighting weighting = Weighting::Standard;
        bool linkedDynamic = false;
    };

    struct Reading {
        std::array<double, 2> peak{};
        std::array<double, 2> sineReferencedRms{};
        double signCoincidence = 0, balance = 0;
        std::array<double, 2> maximumPeak{}, peakHold{}, rmsHold{}, floatingPeak{};
        std::array<double, 2> dynamicDb{};
        std::optional<double> linkedDynamicDb;
        bool operator==(const Reading &) const = default;
    };

    // Starts a fresh stream. Invalid settings preserve the current stream.
    bool prepare(const Settings &) noexcept;
    // Clear history after a source discontinuity, keeping the configuration.
    void reset() noexcept;
    // Empty spans are a no-op. Invalid sizes or non-finite samples leave history intact.
    bool process(std::span<const float> left, std::span<const float> right) noexcept;
    Reading reading() const noexcept;

  private:
    template <bool weighted>
    bool update(std::span<const float>, std::span<const float>) noexcept;
    struct Lane {
        double peak = 0, power = 0, maximum = 0, heldPeak = 0, heldPower = 0, floatingPeak = 0, ratio = .5;
        std::int64_t peakWait = 0, rmsWait = 0;
    };
    bool prepared_ = false, midSide_ = false;
    double peakDecay_ = 0, powerStep_ = 0, coincidenceStep_ = 0, floor_ = 0, drStep_ = 0, floatStep_ = 0;
    std::int64_t holdSamples_ = 0;
    std::array<Lane, 2> lanes_{};
    StereoMeterWeighting weighting_;
    std::array<StereoMeterWeighting::History, 2> filterState_{};
    double coincidence_ = 0, balance_ = 0;
};

} // namespace adi::dsp
