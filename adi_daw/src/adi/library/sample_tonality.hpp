// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include "adi/dsp/spectrum.hpp"

#include <array>
#include <complex>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <stop_token>
#include <string>
#include <vector>

namespace adi::library {

struct SampleKey {
    std::uint8_t tonic = 0; // C = 0, B = 11
    bool minor = false;
    friend bool operator==(SampleKey, SampleKey) = default;
};

struct DetectedSampleKey {
    SampleKey key;
    float strength = 0; // separation from the next best major/minor profile, in [0, 1]
};

// Worker-only streaming detector. Input is interleaved decoder output; append
// boundaries have no effect. Silence, single notes and ambiguous keys return no key.
class SampleKeyAnalyzer {
public:
    SampleKeyAnalyzer(std::uint32_t sampleRate, std::uint32_t channels);
    [[nodiscard]] bool append(std::span<const float> interleaved);
    [[nodiscard]] std::optional<DetectedSampleKey> finish();

private:
    static constexpr std::size_t windowSize = 4096;
    static constexpr std::size_t hopSize = windowSize / 2;
    void addFrame(float mid, float side);
    void accumulate();

    std::uint32_t channels_;
    std::uint32_t reduction_;
    double analysisRate_;
    dsp::RealFft fft_;
    std::array<float, windowSize> window_{};
    std::array<float, windowSize> frame_{};
    std::array<float, windowSize> sideFrame_{};
    std::array<float, windowSize> input_{};
    std::array<std::complex<float>, windowSize / 2 + 1> spectrum_{};
    std::array<double, 12> chroma_{};
    double reductionSum_ = 0;
    double reductionSideSum_ = 0;
    std::uint32_t reductionFrames_ = 0;
    std::size_t position_ = 0;
    bool hasCompleteWindow_ = false;
    bool finished_ = false;
    std::optional<DetectedSampleKey> result_;
};

struct SampleFileTags {
    std::optional<double> tagBpm, acidBpm;
    std::optional<SampleKey> tagKey;
    std::optional<std::uint8_t> rootNote; // MIDI note, without an inferred mode
    std::optional<bool> tagLoop, acidLoop;
    std::vector<std::string> tags;
};

// Reads recognized source metadata without decoding audio or changing the file.
// Unsupported and malformed tags are ignored. Work is bounded and off callback.
[[nodiscard]] SampleFileTags readSampleFileTags(const std::filesystem::path&, std::stop_token = {});

} // namespace adi::library
