// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>

namespace adi::library {

inline constexpr std::int32_t sampleMaskingFftBins = 1025;
inline constexpr double sampleMaskingSampleRate = 44100;
inline constexpr std::size_t sampleMaskingBandCount = 40;

struct SampleDescriptors {
    // Shares of audible spectral power. Internal edges, in Hz: 125, 250,
    // 500, 1000, 2000, 4000, 8000. The measured range is 20 Hz to 20 kHz.
    std::array<float, 8> spectralEnergy{};
    // Shares of signal energy in the four equal time ranges of the file.
    std::array<float, 4> envelopeEnergy{};
    // Shares of detected attacks in the eight equal time ranges of the file.
    std::array<float, 8> onsetDensity{};
    // Power shares in the shared MaskingBands layout at the reference rate.
    std::array<float, sampleMaskingBandCount> maskingEnergy{};
    float brightnessHz = 0;
    float hitDensityHz = 0;
    float rmsDbfs = -200;
};

// Worker owns one analyzer, including destruction. Each append consumes a
// whole number of interleaved source frames in any chunk size. Scratch usage
// is bounded independently of the source duration. Existing audio callbacks
// are refused before allocating or changing any state.
class SampleDescriptorAnalyzer {
public:
    SampleDescriptorAnalyzer(std::uint32_t sampleRate, std::uint32_t channels);
    ~SampleDescriptorAnalyzer();
    SampleDescriptorAnalyzer(const SampleDescriptorAnalyzer&) = delete;
    SampleDescriptorAnalyzer& operator=(const SampleDescriptorAnalyzer&) = delete;

    [[nodiscard]] bool append(std::span<const float> interleaved) noexcept;
    // Transient frames are sorted, unique positions from the shared detector.
    // A successful finish consumes the analyzer. Invalid input retains state.
    [[nodiscard]] std::optional<SampleDescriptors> finish(std::span<const std::uint64_t> onsets) noexcept;

private:
    struct State;
    std::unique_ptr<State> state_;
};

} // namespace adi::library
