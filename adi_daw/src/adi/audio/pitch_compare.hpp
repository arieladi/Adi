// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <span>
#include <stop_token>
#include <vector>

namespace adi::audio {

enum class PitchCompareSide { Original, Corrected };
inline constexpr std::uint64_t kPitchCompareMaxResidentSamples = 64u * 1024 * 1024;
inline constexpr std::uint32_t kPitchCompareMaxCrossfadeFrames = 4096;

/// One pair on the same interleaved clip clock. Acquire and release ownership
/// off the callback. The consumers may share this immutable PCM.
class PitchComparePcm {
public:
    // Worker-only. Values carry the same channel order and clip-clock origin.
    // Empty, mismatched or oversized payloads return no prepared pair.
    // Non-finite samples become zero before publishing the pair.
    [[nodiscard]] static std::shared_ptr<const PitchComparePcm> prepare(
        std::uint32_t rate, std::uint16_t channels, std::vector<float>&& original,
        std::vector<float>&& corrected, std::stop_token = {});
    [[nodiscard]] std::uint32_t rate() const noexcept { return rate_; }
    [[nodiscard]] std::uint16_t channels() const noexcept { return channels_; }
    [[nodiscard]] std::uint64_t frames() const noexcept { return original_.size() / channels_; }

private:
    PitchComparePcm(std::uint32_t, std::uint16_t, std::vector<float>&&, std::vector<float>&&);
    std::uint32_t rate_;
    std::uint16_t channels_;
    std::vector<float> original_, corrected_;
    friend class PitchComparePlayback;
};

/// Single driver uses readAt with its existing absolute frame. Selection is a
/// lock-free request from any thread. There is no transport or reader cursor to
/// advance. Construct and retire off the callback. Zero fade frames gives an
/// exact switch. A nonzero fade mixes the same absolute frame from both sides
/// and is capped at kPitchCompareMaxCrossfadeFrames.
class PitchComparePlayback {
public:
    explicit PitchComparePlayback(std::shared_ptr<const PitchComparePcm>, std::uint32_t crossfadeFrames = 0) noexcept;
    [[nodiscard]] std::uint32_t rate() const noexcept;
    [[nodiscard]] std::uint16_t channels() const noexcept;
    [[nodiscard]] std::uint64_t frames() const noexcept;
    void select(PitchCompareSide) noexcept;
    // On failure the destination and crossfade state are unchanged.
    [[nodiscard]] bool readAt(std::uint64_t firstFrame, std::span<float> interleaved) noexcept;

    // A streaming processor can read the two native-rate inputs and apply the
    // same selection ramp after rendering one frame of each side.
    [[nodiscard]] std::span<const float> source(PitchCompareSide) const noexcept;
    [[nodiscard]] float nextMix() noexcept;

private:
    [[nodiscard]] bool beginCrossfade() noexcept;
    [[nodiscard]] float advanceCrossfade(bool corrected) noexcept;
    const std::shared_ptr<const PitchComparePcm> pcm_;
    const std::uint32_t crossfadeFrames_;
    std::atomic<bool> requestedCorrected_{true};
    bool latchedCorrected_ = true;
    bool started_ = false;
    float mix_ = 1, step_ = 0;
    std::uint32_t remaining_ = 0;
};

} // namespace adi::audio
