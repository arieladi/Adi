// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <span>
#include <stop_token>
#include <string_view>

namespace adi::audio {

struct FfprobeAudioStream {
    std::uint32_t index; // absolute stream index, for an explicit FFmpeg map
    std::uint32_t sampleRate;
    std::uint32_t channels;
};

/// Validates the bounded JSON requested by probeFfprobeAudio. Audio layout and
/// length must still be checked against the decoded output.
[[nodiscard]] FfprobeAudioStream parseFfprobeAudio(std::string_view);

/// Probe the first audio stream in a supported local container using an
/// explicitly selected ffprobe executable. Throws DecodeError on failure.
[[nodiscard]] FfprobeAudioStream probeFfprobeAudio(
    const std::filesystem::path& ffprobe, const std::filesystem::path& source,
    std::stop_token stop = {});

using FfmpegAudioSink = std::function<bool(std::span<const float>)>;

/// Delivers interleaved complete frames from a selected stream. Returns its
/// exact decoded frame count after exit zero. Sink spans live through the call.
/// The caller owns staged output and commits it after a successful return.
[[nodiscard]] std::uint64_t streamFfmpegAudio(
    const std::filesystem::path& ffmpeg, const std::filesystem::path& source,
    FfprobeAudioStream stream, std::uint64_t maximumFrames,
    const FfmpegAudioSink& sink, std::stop_token stop = {});

} // namespace adi::audio
