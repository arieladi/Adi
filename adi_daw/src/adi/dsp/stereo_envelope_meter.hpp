// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include "adi/dsp/biquad.hpp"

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace adi::dsp {

// Prepared coefficients can be reused across channels with independent histories.
class StereoMeterWeighting {
  public:
    enum class Weighting { Standard, A, C, K };
    enum class Purpose { Audition, Measurement };
    struct State { double first = 0, second = 0; };
    using History = std::array<State, 4>;

    bool prepare(double sampleRate, Weighting, Purpose = Purpose::Audition) noexcept;
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
        double peakDecaySeconds;       // seconds for peak amplitude to fall 20 dB in silence
        double meanSquareSeconds;      // sliding RMS window duration
        double coincidenceSeconds;     // time constant of the correlation powers
        double coincidenceFloor;       // linear magnitude floor for correlation
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
        std::array<std::uint64_t, 2> clipCount{};
        double balanceDb = 0, maxCrestFactorDb = 0, linkedSineReferencedRms = 0;
        bool operator==(const Reading &) const = default;
    };

    // Starts a fresh stream. Invalid settings preserve the current stream.
    // Allocates the sample-power history for both RMS windows.
    bool prepare(const Settings &) noexcept;
    // Clear history after a source discontinuity, keeping the configuration.
    void reset() noexcept;
    // Empty spans are a no-op. Invalid sizes or non-finite samples leave history intact.
    bool process(std::span<const float> left, std::span<const float> right) noexcept;
    Reading reading() const noexcept;

  private:
    template <bool weighted>
    void update(std::span<const float>, std::span<const float>) noexcept;
    struct Lane {
        double peak = 0, maximum = 0, heldPeak = 0, heldPower = 0, floatingPeak = 0;
        double rmsSum = 0, crestSum = 0;
        std::size_t peakWait = 0, rmsWait = 0, silentSamples = 0;
    };
    bool prepared_ = false, midSide_ = false, linkedDynamic_ = false;
    double peakReleaseStep_ = 0, coincidenceStep_ = 0, balanceStep_ = 0, floorPower_ = 0, floatStep_ = 0;
    std::size_t rmsSamples_ = 0, crestSamples_ = 0, holdSamples_ = 0;
    double rmsScale_ = 0, crestScale_ = 0;
    std::vector<std::array<double, 2>> powerHistory_;
    std::size_t historyNext_ = 0, historyFilled_ = 0;
    std::array<Lane, 2> lanes_{};
    StereoMeterWeighting weighting_;
    std::array<StereoMeterWeighting::History, 2> filterState_{};
    std::array<double, 2> correlationPower_{}, balancePower_{};
    double crossPower_ = 0, maxCrestRatio_ = 1;
    std::array<std::uint64_t, 2> clipCount_{};
    std::array<bool, 2> clipping_{};
};

} // namespace adi::dsp
