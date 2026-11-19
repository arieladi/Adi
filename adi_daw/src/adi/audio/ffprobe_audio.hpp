// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>
#include <filesystem>
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

} // namespace adi::audio
