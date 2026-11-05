// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "adi/audio/mp4_aac.hpp"
#include <functional>
namespace adi::audio {
// Native platform AAC on Windows/macOS; dynamically loaded libavcodec on Linux.
// All calls are worker-only. checkpoint may throw to cancel, including during
// codec draining; the caller owns the private working directory and its cleanup.
using CodecCheckpoint = std::function<void()>;
// Availability may probe the real native codec with private temporary files.
// On MF versions without verifiable gapless metadata it is false.
[[nodiscard]] bool aacCodecAvailable() noexcept;
[[nodiscard]] AacTrack encodeAac(const std::filesystem::path &stereoWav,
                                 const std::filesystem::path &privatePacketFile,
                                 std::uint32_t bitRate = 256000, CodecCheckpoint = {});
// Synchronous owner-worker chunks, interleaved and valid only during sink().
// The caller owns a private scratch pathname (native rewrap on Windows/macOS).
// A throwing sink/checkpoint cancels delivery; no whole decoded PCM is retained.
using AacPcmSink = std::function<void(std::span<const float>)>;
void decodeAacPcm(const AacTrack &, const std::filesystem::path &privateScratch,
                  const AacPcmSink &, CodecCheckpoint = {});
void decodeAac(const AacTrack &, const std::filesystem::path &privateWav, CodecCheckpoint = {});
} // namespace adi::audio
