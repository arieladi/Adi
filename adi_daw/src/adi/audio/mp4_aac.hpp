// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstdint>
#include <filesystem>
#include <functional>
#include <span>
#include <string>
#include <vector>
namespace adi::audio {
// Worker-only, bounded ISO-BMFF AAC-LC transport. Packet offsets reference the
// original file; compressed media is never loaded as one unbounded allocation.
struct AacPacket {
    std::uint64_t offset = 0;
    std::uint32_t size = 0;
};
struct AacTrack {
    std::filesystem::path file;
    std::uint32_t sampleRate = 0;
    std::uint64_t frames = 0, priming = 0;
    std::vector<std::uint8_t> config;
    std::vector<AacPacket> packets;
};
struct AacMovie {
    std::vector<AacTrack> tracks;
    std::string stemManifest; // original UTF-8 bytes, including any NUL padding
};
// Throws on corrupt, fragmented/encrypted, non-stereo/non-LC, complex edit,
// or excessive input. Up to five audio tracks, one million packets per track,
// 64 MiB moov, 1 MiB per packet; unknown boxes are skipped within their parent.
[[nodiscard]] AacMovie readAacMovie(const std::filesystem::path &);
// New private destination only (the caller owns no-clobber publication).
// All tracks must be stereo AAC-LC at one sample rate and have equal duration.
// Generic players select the first track; NI metadata lives in moov/udta/stem.
void writeAacMovie(const std::filesystem::path &, std::span<const AacTrack>,
                   const std::string &stemManifest = {}, const std::function<void()> &checkpoint = {});
// Checks AudioSpecificConfig and returns the stereo AAC-LC sample rate.
[[nodiscard]] std::uint32_t aacSampleRate(std::span<const std::uint8_t>);
} // namespace adi::audio
