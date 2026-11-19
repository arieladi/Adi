// SPDX-License-Identifier: GPL-3.0-or-later

#include "adi/audio/ffprobe_audio.hpp"

#include "adi/audio/decode.hpp"
#include "adi/audio/io_audit.hpp"
#include "adi/scan_process.hpp"

#include <atomic>
#include <charconv>
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
    if (onAudioThread) throw DecodeError(DecodeProblem::AudioThread, "ffprobe is an owner-worker operation");
    if (stop.stop_requested()) throw DecodeError(DecodeProblem::Cancelled, "Audio probe cancelled");

    std::error_code ec;
    if (!ffprobe.is_absolute() || !std::filesystem::is_regular_file(ffprobe, ec))
        throw DecodeError(DecodeProblem::Unsupported, "A valid ffprobe executable is required for this format");
    if (source.empty() || source.native().find(std::filesystem::path::value_type{}) !=
                              std::filesystem::path::string_type::npos)
        throw DecodeError(DecodeProblem::Unreadable, "Invalid local audio source path");
    const auto absolute = std::filesystem::absolute(source, ec);
    if (ec || !std::filesystem::is_regular_file(absolute, ec))
        throw DecodeError(DecodeProblem::Unreadable, "Cannot open the local audio source");
    const auto bytes = absolute.generic_u8string();
    const std::string input(reinterpret_cast<const char*>(bytes.data()), bytes.size());

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

} // namespace adi::audio
