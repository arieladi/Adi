// SPDX-License-Identifier: GPL-3.0-or-later

#include "adi/audio/ffprobe_audio.hpp"

#include "adi/audio/decode.hpp"
#include "adi/audio/io_audit.hpp"
#include "adi/scan_process.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <charconv>
#include <cstring>
#include <exception>
#include <limits>
#include <string>

namespace adi::audio {
namespace {

constexpr std::size_t kMaximumProbeBytes = 64U * 1024U;

// TM-MD-05. Limit the tool to local media demuxers. Playlist and concat
// inputs must not select additional files for an ordinary media import.
constexpr auto kDemuxers = "asf,asf_o,wv,caf,ape,mov,mp4,m4a,3gp,3g2,mj2,dsf,iff,"
                           "mpc,mpc8,rm,matroska,webm,avi,ac3,dts,aac,mpegts";

std::uint64_t streamInteger(const nlohmann::json& stream, const char* key) {
    const auto found = stream.find(key);
    if (found != stream.end()) {
        if (found->is_number_unsigned()) return found->get<std::uint64_t>();
        if (found->is_number_integer()) {
            const auto value = found->get<std::int64_t>();
            if (value >= 0) return static_cast<std::uint64_t>(value);
        }
    }
    throw DecodeError(DecodeProblem::Corrupt, std::string("Invalid ffprobe field ") + key);
}

std::uint32_t sampleRate(const nlohmann::json& stream) {
    const auto found = stream.find("sample_rate");
    if (found != stream.end() && found->is_string()) {
        const auto& text = found->get_ref<const std::string&>();
        std::uint32_t value = 0;
        const auto end = text.data() + text.size();
        const auto parsed = std::from_chars(text.data(), end, value);
        if (parsed.ec == std::errc{} && parsed.ptr == end) return value;
    }
    throw DecodeError(DecodeProblem::Corrupt, "Invalid ffprobe sample_rate");
}

std::string localInput(const std::filesystem::path& tool, const std::filesystem::path& source,
                       const char* name, std::stop_token stop) {
    if (onAudioThread) throw DecodeError(DecodeProblem::AudioThread, "External decoding requires a worker");
    if (stop.stop_requested()) throw DecodeError(DecodeProblem::Cancelled, "External decoding cancelled");

    std::error_code ec;
    if (!tool.is_absolute() || !std::filesystem::is_regular_file(tool, ec))
        throw DecodeError(DecodeProblem::Unsupported, std::string("A valid ") + name + " executable is required");
    if (source.empty() || source.native().find(std::filesystem::path::value_type{}) !=
                              std::filesystem::path::string_type::npos)
        throw DecodeError(DecodeProblem::Unreadable, "Invalid local audio source path");
    const auto absolute = std::filesystem::absolute(source, ec);
    if (ec || !std::filesystem::is_regular_file(absolute, ec))
        throw DecodeError(DecodeProblem::Unreadable, "Cannot open the local audio source");
    const auto bytes = absolute.generic_u8string();
    return {reinterpret_cast<const char*>(bytes.data()), bytes.size()};
}

} // namespace

FfprobeAudioStream parseFfprobeAudio(std::string_view text) {
    if (text.empty() || text.size() > kMaximumProbeBytes)
        throw DecodeError(DecodeProblem::Corrupt, "Missing or oversized ffprobe JSON");
    if (text.find('\0') != std::string_view::npos)
        throw DecodeError(DecodeProblem::Corrupt, "Null byte in ffprobe JSON");
    const auto keep = [](int depth, nlohmann::json::parse_event_t, nlohmann::json&) {
        if (depth > 8) throw DecodeError(DecodeProblem::Corrupt, "Excessively nested ffprobe JSON");
        return true;
    };
    const auto result = nlohmann::json::parse(text.begin(), text.end(), keep, false);
    if (!result.is_object())
        throw DecodeError(DecodeProblem::Corrupt, "Invalid ffprobe JSON object");
    const auto streams = result.find("streams");
    if (streams == result.end() || !streams->is_array() || streams->size() > 1)
        throw DecodeError(DecodeProblem::Corrupt, "Invalid ffprobe streams array");
    if (streams->empty()) throw DecodeError(DecodeProblem::NoAudio, "Media has no audio stream");

    const auto& stream = streams->front();
    const auto type = stream.find("codec_type");
    if (!stream.is_object() || type == stream.end() || *type != "audio")
        throw DecodeError(DecodeProblem::Corrupt, "Invalid ffprobe audio stream");
    const auto index = streamInteger(stream, "index");
    const auto channels = streamInteger(stream, "channels");
    const auto rate = sampleRate(stream);
    if (index > static_cast<std::uint64_t>(std::numeric_limits<int>::max()))
        throw DecodeError(DecodeProblem::Corrupt, "FFmpeg stream index is out of range");
    if (channels == 0 || channels > kMaxDecodeChannels || rate == 0 || rate > kMaxDecodeRate)
        throw DecodeError(DecodeProblem::Unsupported, "FFmpeg stream rate or channel count is out of range");
    return {static_cast<std::uint32_t>(index), rate, static_cast<std::uint32_t>(channels)};
}

FfprobeAudioStream probeFfprobeAudio(const std::filesystem::path& ffprobe,
                                   const std::filesystem::path& source, std::stop_token stop) {
    const auto input = localInput(ffprobe, source, "ffprobe", stop);

    std::atomic<bool> cancelled{false};
    const std::stop_callback cancellation(stop, [&cancelled] {
        cancelled.store(true, std::memory_order_relaxed);
    });
    scan::ProcessOptions options;
    options.maximumOutput = kMaximumProbeBytes;
    options.cancel = &cancelled;
    const auto result = scan::runProcess(ffprobe,
        {"-v", "error", "-protocol_whitelist", "file", "-format_whitelist", kDemuxers,
         "-select_streams", "a:0", "-show_entries", "stream=index,codec_type,sample_rate,channels",
         "-of", "json", "-i", input}, options);
    if (result.status == scan::ProcessStatus::Cancelled || stop.stop_requested())
        throw DecodeError(DecodeProblem::Cancelled, "Audio probe cancelled");
    if (result.status == scan::ProcessStatus::LaunchFailed)
        throw DecodeError(DecodeProblem::Unsupported, "Could not start ffprobe");
    if (!result.succeeded()) throw DecodeError(DecodeProblem::Corrupt, "ffprobe could not read the audio stream");
    return parseFfprobeAudio(result.output);
}

std::uint64_t streamFfmpegAudio(const std::filesystem::path& ffmpeg, const std::filesystem::path& source,
                                FfprobeAudioStream stream, std::uint64_t maximumFrames,
                                const FfmpegAudioSink& sink, std::stop_token stop) {
    const auto input = localInput(ffmpeg, source, "ffmpeg", stop);
    if (stream.sampleRate == 0 || stream.sampleRate > kMaxDecodeRate ||
        stream.channels == 0 || stream.channels > kMaxDecodeChannels ||
        stream.index > static_cast<std::uint32_t>(std::numeric_limits<int>::max()))
        throw DecodeError(DecodeProblem::Unsupported, "Invalid FFmpeg stream layout");
    if (!sink) throw DecodeError(DecodeProblem::CacheWrite, "A PCM sink is required");

    constexpr std::size_t sampleBytes = 4;
    const auto frameBytes = stream.channels * sampleBytes;
    if (maximumFrames == 0 || maximumFrames > std::numeric_limits<std::uint64_t>::max() / frameBytes)
        throw DecodeError(DecodeProblem::NoSpace, "Invalid FFmpeg decode frame budget");
    constexpr std::size_t bufferBytes = 4096;
    std::array<std::byte, bufferBytes> raw{};
    std::array<float, bufferBytes / sampleBytes> floats{};
    const auto batchBytes = raw.size() / frameBytes * frameBytes;
    const auto deliver = [&](std::size_t bytes) {
        for (std::size_t at = 0; at < bytes; at += sampleBytes) {
            const auto bits = std::to_integer<std::uint32_t>(raw[at]) |
                (std::to_integer<std::uint32_t>(raw[at + 1]) << 8U) |
                (std::to_integer<std::uint32_t>(raw[at + 2]) << 16U) |
                (std::to_integer<std::uint32_t>(raw[at + 3]) << 24U);
            floats[at / sampleBytes] = std::bit_cast<float>(bits);
        }
        return sink(std::span<const float>(floats).first(bytes / sampleBytes));
    };
    std::size_t filled = 0;
    std::exception_ptr sinkFailure;
    const auto consume = [&](std::span<const std::byte> bytes) {
        try {
            while (!bytes.empty()) {
                const auto count = std::min(batchBytes - filled, bytes.size());
                std::memcpy(raw.data() + filled, bytes.data(), count);
                filled += count;
                bytes = bytes.subspan(count);
                if (filled == batchBytes) {
                    if (!deliver(filled)) return false;
                    filled = 0;
                }
            }
            return true;
        } catch (...) {
            sinkFailure = std::current_exception();
            return false;
        }
    };

    std::atomic<bool> cancelled{false};
    const std::stop_callback cancellation(stop, [&cancelled] {
        cancelled.store(true, std::memory_order_relaxed);
    });
    scan::ProcessStreamOptions options;
    options.maximumOutput = maximumFrames * frameBytes;
    options.cancel = &cancelled;
    const auto result = scan::runProcessStream(ffmpeg,
        {"-nostdin", "-v", "error", "-xerror", "-protocol_whitelist", "file",
         "-format_whitelist", kDemuxers, "-i", input, "-map", "0:" + std::to_string(stream.index),
         "-vn", "-sn", "-dn", "-c:a", "pcm_f32le", "-ar", std::to_string(stream.sampleRate),
         "-ac", std::to_string(stream.channels), "-f", "f32le", "pipe:1"}, consume, options);
    if (sinkFailure) std::rethrow_exception(sinkFailure);
    if (result.status == scan::ProcessStatus::Cancelled || stop.stop_requested())
        throw DecodeError(DecodeProblem::Cancelled, "FFmpeg decoding cancelled");
    if (result.status == scan::ProcessStatus::LaunchFailed)
        throw DecodeError(DecodeProblem::Unsupported, "Could not start ffmpeg");
    if (result.status == scan::ProcessStatus::OutputLimit)
        throw DecodeError(DecodeProblem::NoSpace, "FFmpeg output exceeded the decode frame budget");
    if (result.status == scan::ProcessStatus::ConsumerFailed)
        throw DecodeError(DecodeProblem::CacheWrite, "PCM sink refused FFmpeg audio");
    if (!result.succeeded() || result.bytesDelivered == 0 || filled % frameBytes != 0)
        throw DecodeError(DecodeProblem::Corrupt, "FFmpeg output failed or ended with an incomplete PCM frame");
    if (filled != 0 && !deliver(filled))
        throw DecodeError(DecodeProblem::CacheWrite, "PCM sink refused final FFmpeg frames");
    if (stop.stop_requested()) throw DecodeError(DecodeProblem::Cancelled, "FFmpeg decoding cancelled");
    return result.bytesDelivered / frameBytes;
}

} // namespace adi::audio
