// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>

#if defined(_MSC_VER)
#define ADI_SPECTRUM_AVERAGE_NOINLINE __declspec(noinline)
#elif defined(__GNUC__) || defined(__clang__)
#define ADI_SPECTRUM_AVERAGE_NOINLINE __attribute__((noinline))
#else
#define ADI_SPECTRUM_AVERAGE_NOINLINE
#endif

namespace adi::dsp {
// One owner, no clock or source subscription. Each successful process call is
// exactly one real analysis hop, never an output republication or a UI frame.
class LinearMagnitudeAverage {
  public:
    static constexpr std::size_t kMaximumBins = 32769;
    static constexpr std::int32_t kMaximumHop = 65536;
    struct Settings {
        std::size_t bins = 0;
        double sampleRate = 0;
        std::int32_t hopSamples = 0;
        double decay20dBSeconds = 0;
        bool operator==(const Settings &) const = default;
    };
    LinearMagnitudeAverage() = default;
    LinearMagnitudeAverage(const LinearMagnitudeAverage &) = delete;
    LinearMagnitudeAverage &operator=(const LinearMagnitudeAverage &) = delete;
    // Owner-stopped configuration: prepare starts a zero-valued numerical epoch.
    // Invalid settings/allocation failure preserve the complete old configuration.
    bool prepare(const Settings &) noexcept;
    // Changes the coefficient only, retaining all accumulated magnitudes.
    bool setDecay20dBSeconds(double) noexcept;
    void reset() noexcept;
    bool prepared() const noexcept { return static_cast<bool>(values_); }
    const Settings &settings() const noexcept { return settings_; }
    double coefficient() const noexcept { return coefficient_; }
    // Borrowed until the next non-const call/destruction. No publication/lifetime service.
    std::span<const double> magnitudes() const noexcept { return {values_.get(), settings_.bins}; }
    // Checked/untrusted entry: reject malformed/nonfinite/negative input or an
    // unrepresentable next state before mutating any bin. No implicit sanitation.
    ADI_SPECTRUM_AVERAGE_NOINLINE bool processChecked(std::span<const float>) noexcept;
    ADI_SPECTRUM_AVERAGE_NOINLINE bool processChecked(std::span<const double>) noexcept;
    // Internal producer entry: no validation scan. Preconditions: finite,
    // nonnegative magnitudes and resulting states; no concurrent owner mutation.
    // Shape/null/prepared guards still apply. Float input widens once per bin.
    ADI_SPECTRUM_AVERAGE_NOINLINE bool processTrustedFinite(std::span<const float>) noexcept;
    ADI_SPECTRUM_AVERAGE_NOINLINE bool processTrustedFinite(std::span<const double>) noexcept;
  private:
    Settings settings_;
    double coefficient_ = 0;
    std::unique_ptr<double[]> values_;
};
} // namespace adi::dsp
#undef ADI_SPECTRUM_AVERAGE_NOINLINE
