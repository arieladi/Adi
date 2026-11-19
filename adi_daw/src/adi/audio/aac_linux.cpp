// SPDX-License-Identifier: GPL-3.0-or-later
#include "adi/audio/aac_codec.hpp"
#include "adi/audio/wav_file.hpp"
#include <stdexcept>
#include <utility>
namespace adi::audio {
bool ffmpeg60Available() noexcept;
bool ffmpeg61Available() noexcept;
AacTrack ffmpeg60Encode(const std::filesystem::path &, const std::filesystem::path &, std::uint32_t,
                        CodecCheckpoint);
AacTrack ffmpeg61Encode(const std::filesystem::path &, const std::filesystem::path &, std::uint32_t,
                        CodecCheckpoint);
void ffmpeg60DecodePcm(const AacTrack &, const AacPcmSink &, CodecCheckpoint);
void ffmpeg61DecodePcm(const AacTrack &, const AacPcmSink &, CodecCheckpoint);
bool aacCodecAvailable() noexcept {
    return ffmpeg61Available() || ffmpeg60Available();
}
AacTrack encodeAac(const std::filesystem::path &wav, const std::filesystem::path &packets, std::uint32_t rate,
                   CodecCheckpoint checkpoint) {
    if (ffmpeg61Available())
        return ffmpeg61Encode(wav, packets, rate, std::move(checkpoint));
    if (ffmpeg60Available())
        return ffmpeg60Encode(wav, packets, rate, std::move(checkpoint));
    throw std::runtime_error("AAC codec unavailable: requires libavcodec 60/61 and matching libavutil 58/59");
}
void decodeAacPcm(const AacTrack &track, const std::filesystem::path &, const AacPcmSink &sink, CodecCheckpoint checkpoint) {
    if (ffmpeg61Available()) { ffmpeg61DecodePcm(track, sink, std::move(checkpoint)); return; }
    if (ffmpeg60Available()) { ffmpeg60DecodePcm(track, sink, std::move(checkpoint)); return; }
    throw std::runtime_error("AAC codec unavailable: requires libavcodec 60/61 and matching libavutil 58/59");
}
void decodeAac(const AacTrack &track, const std::filesystem::path &wav, CodecCheckpoint checkpoint) {
    const auto channels=aacConfig(track.config).channels;
    WavWriter writer(wav,track.sampleRate,channels,WavFormat::Float32);
    decodeAacPcm(track,wav,[&](std::span<const float> pcm) {
        writer.write(pcm.data(),static_cast<std::uint32_t>(pcm.size()/channels));
    },std::move(checkpoint));
    writer.close();
}
} // namespace adi::audio
