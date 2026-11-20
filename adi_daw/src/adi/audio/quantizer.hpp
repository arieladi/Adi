// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace adi::audio {

// Preserve the original three WavDither values for existing callers.
enum class WavDither { None, Triangular, Rectangular, Shaped1, Shaped2, Shaped3 };

[[nodiscard]] bool validDither(WavDither) noexcept;
[[nodiscard]] const char* ditherName(WavDither) noexcept;
// Shaped modes without a supported rate table use ordinary triangular dither.
// Matching follows the pinned SoX table order and inclusive five-percent range.
// Invalid enum values remain invalid; callers can use validDither to reject them.
[[nodiscard]] WavDither effectiveDither(WavDither, std::uint32_t sampleRate) noexcept;

// Stateful PCM16/24 quantization, independent of output container and write size.
// Construct off the audio thread. quantize() allocates nothing and consumes one
// interleaved sample, including a nonfinite sample; this object is not thread-safe.
// None/TPDF/rectangular retain WavWriter's rounding and exact SplitMix64 stream.
// Shaped modes use separate channel histories and reserve conservative headroom
// for finite input in [-1,1]. gain() reports that input attenuation (1 otherwise).
// Nonfinite input consumes its normal random draws, returns zero, and clears only
// that channel's shaping history. Out-of-range finite input is still saturated.
class PcmQuantizer {
public:
    PcmQuantizer(std::uint32_t sampleRate, std::uint16_t channels, unsigned bits,
                 WavDither mode, std::uint64_t seed);

    [[nodiscard]] std::int32_t quantize(double sample) noexcept;
    [[nodiscard]] double gain() const noexcept { return gain_; }
    // Counts pre-clamp integer values outside the PCM range, saturating at UINT64_MAX.
    // This distinguishes clipping from an otherwise valid endpoint code.
    [[nodiscard]] std::uint64_t saturationCount() const noexcept { return saturations_; }

private:
    static constexpr std::size_t maximumTaps = 20;
    struct Channel {
        std::array<double, maximumTaps> errors{};
        std::size_t position = 0;
    };

    [[nodiscard]] double uniform() noexcept;
    [[nodiscard]] double noise() noexcept;
    void nextChannel() noexcept;

    WavDither mode_;
    std::uint16_t channels_;
    std::size_t channel_ = 0;
    std::uint64_t randomState_;
    std::uint64_t saturations_ = 0;
    double scale_ = 0;
    double gain_ = 1;
    std::span<const double> coefficients_{};
    std::vector<Channel> histories_;
};

} // namespace adi::audio
