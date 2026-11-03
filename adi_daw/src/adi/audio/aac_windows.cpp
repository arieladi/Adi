// SPDX-License-Identifier: GPL-3.0-or-later
#include "adi/audio/aac_codec.hpp"
#include "adi/audio/wav_file.hpp"
#include <windows.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <stdexcept>
#include <string>
#include <utility>
#include <wrl/client.h>
namespace adi::audio {
namespace {
using Microsoft::WRL::ComPtr;
void check(HRESULT result, const char *action) {
    if (FAILED(result))
        throw std::runtime_error(std::string("Media Foundation AAC ") + action + " failed (" +
                                 std::to_string(result) + ")");
}
struct Runtime {
    bool com = false, started = false;
    Runtime() {
        auto result = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        com = SUCCEEDED(result);
        if (FAILED(result) && result != RPC_E_CHANGED_MODE)
            check(result, "COM init");
        result = MFStartup(MF_VERSION);
        if (FAILED(result)) {
            if (com)
                CoUninitialize();
            check(result, "startup");
        }
        started = true;
    }
    ~Runtime() {
        if (started)
            MFShutdown();
        if (com)
            CoUninitialize();
    }
};
void tick(const CodecCheckpoint &call) {
    if (call)
        call();
}
ComPtr<IMFMediaType> mediaType(const GUID &subtype, std::uint32_t rate, std::uint32_t bits) {
    ComPtr<IMFMediaType> type;
    check(MFCreateMediaType(&type), "media type");
    check(type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio), "major type");
    check(type->SetGUID(MF_MT_SUBTYPE, subtype), "subtype");
    check(type->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, 2), "channels");
    check(type->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, rate), "sample rate");
    check(type->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, bits), "sample width");
    return type;
}
LONGLONG clock(std::uint64_t frame, std::uint32_t rate) {
    return static_cast<LONGLONG>((frame / rate) * 10000000 + ((frame % rate) * 10000000) / rate);
}
} // namespace
static void decodeNative(const AacTrack &, const std::filesystem::path &, CodecCheckpoint);
bool aacCodecAvailable() noexcept {
    // MF codec versions differ in gapless metadata. Probe the real native
    // path once, rather than advertise sample alignment without proof.
    static const bool available = [] {
        std::filesystem::path folder;
        try {
            for (unsigned attempt = 0; attempt < 128; ++attempt) {
                const auto candidate = std::filesystem::temp_directory_path() /
                                       (L"adi-aac-probe-" + std::to_wstring(GetCurrentProcessId()) + L"-" +
                                        std::to_wstring(GetTickCount64()) + L"-" + std::to_wstring(attempt));
                std::error_code error;
                if (std::filesystem::create_directory(candidate, error)) {
                    folder = candidate;
                    break;
                }
                if (error)
                    return false;
            }
            if (folder.empty())
                return false;
            struct Cleanup {
                std::filesystem::path path;
                ~Cleanup() {
                    std::error_code error;
                    std::filesystem::remove_all(path, error);
                }
            } cleanup{folder};
            std::array<float, 8192> samples{};
            for (std::size_t i = 0; i < 4096; ++i)
                for (std::size_t channel = 0; channel < 2; ++channel)
                    samples[i * 2 + channel] = static_cast<float>(
                        0.2 * std::sin(static_cast<double>(i) * (channel ? 0.037 : 0.071)));
            const auto input = folder / L"input.wav", decoded = folder / L"decoded.wav";
            {
                WavWriter writer(input, 48000, 2, WavFormat::Float32);
                writer.write(samples.data(), 4096);
                writer.close();
            }
            const auto encoded = encodeAac(input, folder / L"encoded", 256000);
            decodeNative(encoded, decoded, {});
            WavReader reader(decoded);
            std::array<float, 8192> actual{};
            if (reader.frames() != 4096 || reader.read(actual.data(), 4096) != 4096)
                return false;
            std::array<double, 17> error{};
            for (int lag = -8; lag <= 8; ++lag)
                for (int i = 1024; i < 3072; ++i)
                    for (int channel = 0; channel < 2; ++channel) {
                        const double delta = actual[static_cast<std::size_t>((i + lag) * 2 + channel)] -
                                             samples[static_cast<std::size_t>(i * 2 + channel)];
                        error[static_cast<std::size_t>(lag + 8)] += delta * delta;
                    }
            return std::min_element(error.begin(), error.end()) - error.begin() == 8;
        } catch (...) {
            if (!folder.empty()) {
                std::error_code ignored;
                std::filesystem::remove_all(folder, ignored);
            }
            return false;
        }
    }();
    return available;
}

AacTrack encodeAac(const std::filesystem::path &wav, const std::filesystem::path &packets,
                   std::uint32_t bitRate, CodecCheckpoint call) {
    WavReader reader(wav);
    if (reader.channels() != 2 || (reader.sampleRate() != 44100 && reader.sampleRate() != 48000) ||
        !reader.frames() || reader.frames() > 999990ULL * 1024 || bitRate < 64000 || bitRate > 320000)
        throw std::invalid_argument("AAC export requires bounded stereo 44.1/48 kHz and 64000..320000 bit/s");
    Runtime runtime;
    auto path = packets;
    path += L".m4a";
    ComPtr<IMFSinkWriter> sink;
    check(MFCreateSinkWriterFromURL(path.c_str(), nullptr, nullptr, &sink), "sink writer");
    auto compressed = mediaType(MFAudioFormat_AAC, reader.sampleRate(), 16);
    check(compressed->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, bitRate / 8), "bit rate");
    check(compressed->SetUINT32(MF_MT_AAC_PAYLOAD_TYPE, 0), "raw payload");
    DWORD stream = 0;
    check(sink->AddStream(compressed.Get(), &stream), "output stream");
    auto input = mediaType(MFAudioFormat_PCM, reader.sampleRate(), 16);
    check(input->SetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT, 4), "input alignment");
    check(input->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, reader.sampleRate() * 4), "input bytes");
    check(sink->SetInputMediaType(stream, input.Get(), nullptr), "input type");
    check(sink->BeginWriting(), "begin");
    std::array<float, 8192> values{};
    std::array<std::int16_t, 8192> pcm{};
    std::uint64_t done = 0;
    for (;;) {
        tick(call);
        const auto count = reader.read(values.data(), 4096);
        if (!count)
            break;
        for (std::size_t i = 0; i < static_cast<std::size_t>(count) * 2; ++i) {
            const auto v = values[i];
            if (!std::isfinite(v) || std::abs(v) > 1)
                throw std::runtime_error("AAC input exceeds finite [-1,1]; lower render gain");
            pcm[i] = static_cast<std::int16_t>(
                (std::clamp)(std::lround(static_cast<double>(v) * 32768), -32768L, 32767L));
        }
        ComPtr<IMFMediaBuffer> buffer;
        check(MFCreateMemoryBuffer(count * 4, &buffer), "input buffer");
        BYTE *bytes = nullptr;
        check(buffer->Lock(&bytes, nullptr, nullptr), "input lock");
        std::memcpy(bytes, pcm.data(), count * 4);
        check(buffer->Unlock(), "input unlock");
        check(buffer->SetCurrentLength(count * 4), "input length");
        ComPtr<IMFSample> sample;
        check(MFCreateSample(&sample), "input sample");
        check(sample->AddBuffer(buffer.Get()), "sample buffer");
        check(sample->SetSampleTime(clock(done, reader.sampleRate())), "sample time");
        check(sample->SetSampleDuration(clock(done + count, reader.sampleRate()) -
                                        clock(done, reader.sampleRate())),
              "sample duration");
        check(sink->WriteSample(stream, sample.Get()), "encode");
        done += count;
    }
    check(sink->Finalize(), "finalize");
    sink.Reset();
    tick(call);
    auto movie = readAacMovie(path);
    if (movie.tracks.size() != 1)
        throw std::runtime_error("Media Foundation returned multiple AAC tracks");
    auto track = std::move(movie.tracks[0]);
    // MF versions without an explicit priming edit cannot establish sample
    // alignment. Refuse that export rather than guess a fixed encoder delay.
    if (!track.priming || track.frames != reader.frames())
        throw std::runtime_error("Media Foundation did not expose sample-exact AAC priming/duration; NI "
                                 "Stems export unavailable on this codec version");
    return track;
}
static void decodeNative(const AacTrack &track, const std::filesystem::path &wav, CodecCheckpoint call) {
    Runtime runtime;
    auto path = wav;
    path += L".m4a";
    struct Cleanup {
        std::filesystem::path path;
        ~Cleanup() {
            std::error_code ec;
            std::filesystem::remove(path, ec);
        }
    } cleanup{path};
    writeAacMovie(path, std::span<const AacTrack>(&track, 1));
    ComPtr<IMFSourceReader> reader;
    check(MFCreateSourceReaderFromURL(path.c_str(), nullptr, &reader), "source reader");
    // MSVC /WX: the MF_SOURCE_READER_* stream ids are an enum, and passing one as the DWORD
    // parameter is C4245 (signed/unsigned) on MSVC 14.51; the cast is what the SDK samples do.
    constexpr DWORD allStreams = static_cast<DWORD>(MF_SOURCE_READER_ALL_STREAMS);
    constexpr DWORD audioStream = static_cast<DWORD>(MF_SOURCE_READER_FIRST_AUDIO_STREAM);
    check(reader->SetStreamSelection(allStreams, FALSE), "deselect");
    check(reader->SetStreamSelection(audioStream, TRUE), "select");
    auto type = mediaType(MFAudioFormat_Float, track.sampleRate, 32);
    check(reader->SetCurrentMediaType(audioStream, nullptr, type.Get()),
          "decode format");
    WavWriter writer(wav, track.sampleRate, 2, WavFormat::Float32);
    std::uint64_t done = 0;
    for (;;) {
        tick(call);
        DWORD flags = 0;
        LONGLONG time = 0;
        ComPtr<IMFSample> sample;
        check(reader->ReadSample(audioStream, 0, nullptr, &flags, &time, &sample),
              "decode");
        if (flags & MF_SOURCE_READERF_ERROR)
            throw std::runtime_error("native AAC reader reported an error");
        if (!sample && !(flags & (MF_SOURCE_READERF_ENDOFSTREAM | MF_SOURCE_READERF_STREAMTICK)))
            throw std::runtime_error("native AAC reader made no progress");
        if (flags & MF_SOURCE_READERF_CURRENTMEDIATYPECHANGED)
            throw std::runtime_error("AAC decode format changed");
        if (sample) {
            ComPtr<IMFMediaBuffer> buffer;
            check(sample->ConvertToContiguousBuffer(&buffer), "PCM buffer");
            BYTE *bytes = nullptr;
            DWORD length = 0;
            check(buffer->Lock(&bytes, nullptr, &length), "PCM lock");
            if (length % 8) {
                buffer->Unlock();
                throw std::runtime_error("unaligned AAC PCM");
            }
            const auto count = static_cast<std::uint32_t>(
                (std::min)(static_cast<std::uint64_t>(length / 8), track.frames - done));
            try {
                if (count) {
                    if (count > 65536)
                        throw std::runtime_error("AAC PCM block exceeds bound");
                    std::vector<float> aligned(static_cast<std::size_t>(count) * 2);
                    std::memcpy(aligned.data(), bytes, static_cast<std::size_t>(count) * 8);
                    for (const auto value : aligned)
                        if (!std::isfinite(value))
                            throw std::runtime_error("nonfinite native AAC decode");
                    writer.write(aligned.data(), count);
                }
            } catch (...) {
                buffer->Unlock();
                throw;
            }
            check(buffer->Unlock(), "PCM unlock");
            done += count;
        }
        if (flags & MF_SOURCE_READERF_ENDOFSTREAM)
            break;
    }
    if (done != track.frames)
        throw std::runtime_error("Media Foundation did not honor AAC edit-list duration");
    writer.close();
}
void decodeAac(const AacTrack &track, const std::filesystem::path &wav, CodecCheckpoint call) {
    if (!aacCodecAvailable())
        throw std::runtime_error(
            "Media Foundation AAC cannot verify sample-exact priming/duration on this codec version");
    decodeNative(track, wav, std::move(call));
}
} // namespace adi::audio
