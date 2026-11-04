// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstdint>
#include <filesystem>
#include <functional>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>
namespace adi::audio {
enum class AacTransportProblem { Corrupt, Unsupported, NoAudio };
class AacTransportError : public std::runtime_error {
public:
    AacTransportError(AacTransportProblem problem,const char* message)
        :std::runtime_error(std::string("AAC transport: ")+message),problem_(problem){}
    [[nodiscard]] AacTransportProblem problem() const noexcept{return problem_;}
private:
    AacTransportProblem problem_;
};
struct AacConfig {std::uint32_t sampleRate=0;std::uint16_t channels=0;};
// General AAC-LC mono/stereo config; NI's aacSampleRate retains its stereo contract.
[[nodiscard]] AacConfig aacConfig(std::span<const std::uint8_t>);
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
// General audio import: mono/stereo LC, first audio track is the returned default.
// Sibling audio tracks need not share a config/rate/duration. Other bounds and
// edit/encryption/fragmentation refusals remain the same as the NI reader.
[[nodiscard]] AacMovie readAacAudioMovie(const std::filesystem::path &);
// ADTS, one LC mono/stereo access unit per frame, fixed config, no inferred
// priming/tail trim. The raw transport carries no authored gapless duration.
[[nodiscard]] AacTrack readAdts(const std::filesystem::path &);
// New private destination only (the caller owns no-clobber publication).
// All tracks must be stereo AAC-LC at one sample rate and have equal duration.
// Generic players select the first track; NI metadata lives in moov/udta/stem.
void writeAacMovie(const std::filesystem::path &, std::span<const AacTrack>,
                   const std::string &stemManifest = {}, const std::function<void()> &checkpoint = {});
// Private mono/stereo single-track rewrap for native decoders. No NI manifest.
void writeAacAudioMovie(const std::filesystem::path &,const AacTrack &,
                       const std::function<void()> &checkpoint = {});
// Checks AudioSpecificConfig and returns the stereo AAC-LC sample rate.
[[nodiscard]] std::uint32_t aacSampleRate(std::span<const std::uint8_t>);
} // namespace adi::audio
