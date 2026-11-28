// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "adi/audio/mp4_aac.hpp"
#include <functional>
#include <string>
namespace adi::audio {
// Native platform AAC on Windows/macOS; dynamically loaded libavcodec on Linux.
// All calls are worker-only. checkpoint may throw to cancel, including during
// codec draining; the caller owns the private working directory and its cleanup.
using CodecCheckpoint = std::function<void()>;
// Availability may probe the real native codec with private temporary files.
// DECODING (an M4A or AAC file played or imported): proven by a decode whose length and alignment are
// measured. SC-376: on Windows 10 it was tied to the encoder's export checks, and every file was refused.
[[nodiscard]] bool aacCodecAvailable() noexcept;
// ENCODING with sample-exact priming metadata (the NI Stems export). On MF versions without verifiable
// gapless metadata it is false.
[[nodiscard]] bool aacEncoderAvailable() noexcept;
// Why decoding is unavailable, in a sentence, for a suite's line; empty when it is available.
[[nodiscard]] std::string aacUnavailableReason();
// Whether the platform has an AAC decoder installed at all (Windows: Media Foundation registers one;
// macOS: always). A suite FAILS when it is present and aacCodecAvailable is still false (SC-376).
[[nodiscard]] bool aacPlatformDecoderPresent() noexcept;
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
