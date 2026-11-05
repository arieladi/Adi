// SPDX-License-Identifier: GPL-3.0-or-later
#include "adi/audio/aac_codec.hpp"
#include "adi/audio/wav_file.hpp"
#include <AudioToolbox/AudioToolbox.h>
#include <CoreFoundation/CoreFoundation.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <stdexcept>
#include <string>
#include <utility>
namespace adi::audio {
namespace {
void check(OSStatus status, const char *action) {
    if (status != noErr)
        throw std::runtime_error(std::string("CoreAudio AAC ") + action + " failed (" +
                                 std::to_string(status) + ")");
}
struct Url {
    CFURLRef value = nullptr;
    explicit Url(const std::filesystem::path &path) {
        const auto bytes = path.native();
        value = CFURLCreateFromFileSystemRepresentation(kCFAllocatorDefault,
                                                        reinterpret_cast<const UInt8 *>(bytes.data()),
                                                        static_cast<CFIndex>(bytes.size()), false);
        if (!value)
            throw std::runtime_error("AAC file URL failed");
    }
    ~Url() {
        CFRelease(value);
    }
};
struct File {
    ExtAudioFileRef value = nullptr;
    ~File() {
        if (value)
            ExtAudioFileDispose(value);
    }
    void close() {
        auto f = value;
        value = nullptr;
        check(ExtAudioFileDispose(f), "close");
    }
};
AudioStreamBasicDescription pcm(std::uint32_t rate) {
    AudioStreamBasicDescription f{};
    f.mSampleRate = rate;
    f.mFormatID = kAudioFormatLinearPCM;
    f.mFormatFlags = kAudioFormatFlagsNativeFloatPacked;
    f.mBytesPerPacket = 8;
    f.mFramesPerPacket = 1;
    f.mBytesPerFrame = 8;
    f.mChannelsPerFrame = 2;
    f.mBitsPerChannel = 32;
    return f;
}
void tick(const CodecCheckpoint &call) {
    if (call)
        call();
}
} // namespace
bool aacCodecAvailable() noexcept {
    return true;
}
AacTrack encodeAac(const std::filesystem::path &wav, const std::filesystem::path &packets,
                   std::uint32_t bitRate, CodecCheckpoint call) {
    WavReader reader(wav);
    if (reader.channels() != 2 || (reader.sampleRate() != 44100 && reader.sampleRate() != 48000) ||
        !reader.frames() || reader.frames() > 999990ULL * 1024 || bitRate < 64000 || bitRate > 320000)
        throw std::invalid_argument("AAC export requires bounded stereo 44.1/48 kHz and 64000..320000 bit/s");
    auto path = packets;
    path += ".m4a";
    Url url(path);
    File file;
    AudioStreamBasicDescription format{};
    format.mSampleRate = reader.sampleRate();
    format.mFormatID = kAudioFormatMPEG4AAC;
    format.mFormatFlags = kMPEG4Object_AAC_LC;
    format.mChannelsPerFrame = 2;
    format.mFramesPerPacket = 1024;
    check(ExtAudioFileCreateWithURL(url.value, kAudioFileM4AType, &format, nullptr, 0, &file.value),
          "create");
    auto client = pcm(reader.sampleRate());
    check(
        ExtAudioFileSetProperty(file.value, kExtAudioFileProperty_ClientDataFormat, sizeof(client), &client),
        "input format");
    AudioConverterRef converter = nullptr;
    UInt32 size = sizeof(converter);
    check(ExtAudioFileGetProperty(file.value, kExtAudioFileProperty_AudioConverter, &size, &converter),
          "converter");
    UInt32 rate = bitRate;
    check(AudioConverterSetProperty(converter, kAudioConverterEncodeBitRate, sizeof(rate), &rate),
          "bit rate");
    AudioConverterPrimeInfo prime{};

    std::array<float, 8192> samples{};
    for (;;) {
        tick(call);
        auto count = reader.read(samples.data(), 4096);
        if (!count)
            break;
        for (std::size_t i = 0; i < static_cast<std::size_t>(count) * 2; ++i)
            if (!std::isfinite(samples[i]) || std::abs(samples[i]) > 1)
                throw std::runtime_error("AAC input exceeds finite [-1,1]; lower render gain");
        AudioBufferList buffers{};
        buffers.mNumberBuffers = 1;
        buffers.mBuffers[0] = {2, count * 8, samples.data()};
        check(ExtAudioFileWrite(file.value, count, &buffers), "encode");
    }
    size = sizeof(prime);
    check(AudioConverterGetProperty(converter, kAudioConverterPrimeInfo, &size, &prime), "priming");
    file.close();
    tick(call);
    // CoreAudio's M4A writer records priming, valid and remainder frames as iTunSMPB
    // metadata, not as an edit list (measured on macOS 26), and reports them as the
    // file's packet table. Packets and config still come from the container.
    AudioFilePacketTableInfo table{};
    {
        File written;
        check(ExtAudioFileOpenURL(url.value, &written.value), "reopen");
        size = sizeof(table);
        check(ExtAudioFileGetProperty(written.value, kExtAudioFileProperty_PacketTable, &size, &table),
              "packet table");
    }
    auto movie = readAacMovie(path);
    if (movie.tracks.size() != 1)
        throw std::runtime_error("CoreAudio returned multiple AAC tracks");
    auto track = std::move(movie.tracks[0]);
    const auto priming = static_cast<std::uint64_t>(table.mPrimingFrames);
    const auto valid = static_cast<std::uint64_t>(table.mNumberValidFrames);
    if (table.mPrimingFrames < 0 || table.mRemainderFrames < 0 || table.mNumberValidFrames < 0 ||
        priming + valid + static_cast<std::uint64_t>(table.mRemainderFrames) != track.packets.size() * 1024)
        throw std::runtime_error("CoreAudio AAC packet table does not match its packets");
    if (priming != prime.leadingFrames)
        throw std::runtime_error("CoreAudio container/encoder priming mismatch");
    if (valid != reader.frames())
        throw std::runtime_error("CoreAudio AAC container lost sample-exact duration");
    track.priming = priming;
    track.frames = valid;
    return track;
}
void decodeAac(const AacTrack &track, const std::filesystem::path &wav, CodecCheckpoint call) {
    auto source = wav;
    source += ".m4a";
    struct Cleanup {
        std::filesystem::path path;
        ~Cleanup() {
            std::error_code ec;
            std::filesystem::remove(path, ec);
        }
    } cleanup{source};
    writeAacMovie(source, std::span<const AacTrack>(&track, 1));
    Url url(source);
    File file;
    check(ExtAudioFileOpenURL(url.value, &file.value), "open");
    // CoreAudio's MP4 reader ignores edit lists, version 0 or 1 (measured on macOS 26:
    // without iTunSMPB it assumes its own encoder's 2112-frame priming and no
    // remainder), so apply the track's priming and duration explicitly.
    // writeAacMovie bounded both by the packets and left a remainder below one packet.
    AudioFilePacketTableInfo table{};
    table.mNumberValidFrames = static_cast<SInt64>(track.frames);
    table.mPrimingFrames = static_cast<SInt32>(track.priming);
    table.mRemainderFrames = static_cast<SInt32>(track.packets.size() * 1024 - track.priming - track.frames);
    check(ExtAudioFileSetProperty(file.value, kExtAudioFileProperty_PacketTable, sizeof(table), &table),
          "packet table");
    auto client = pcm(track.sampleRate);
    check(
        ExtAudioFileSetProperty(file.value, kExtAudioFileProperty_ClientDataFormat, sizeof(client), &client),
        "decode format");
    AudioFilePacketTableInfo applied{};
    UInt32 size = sizeof(applied);
    check(ExtAudioFileGetProperty(file.value, kExtAudioFileProperty_PacketTable, &size, &applied),
          "decoded length");
    if (applied.mNumberValidFrames != table.mNumberValidFrames ||
        applied.mPrimingFrames != table.mPrimingFrames || applied.mRemainderFrames != table.mRemainderFrames)
        throw std::runtime_error("CoreAudio did not honor AAC edit-list duration");
    WavWriter writer(wav, track.sampleRate, 2, WavFormat::Float32);
    std::array<float, 8192> samples{};
    std::uint64_t done = 0;
    while (done < track.frames) {
        tick(call);
        UInt32 count = static_cast<UInt32>((std::min)(track.frames - done, std::uint64_t{4096}));
        AudioBufferList buffers{};
        buffers.mNumberBuffers = 1;
        buffers.mBuffers[0] = {2, count * 8, samples.data()};
        check(ExtAudioFileRead(file.value, &count, &buffers), "decode");
        if (!count)
            throw std::runtime_error("CoreAudio AAC decode ended early");
        for (std::size_t i = 0; i < static_cast<std::size_t>(count) * 2; ++i)
            if (!std::isfinite(samples[i]))
                throw std::runtime_error("nonfinite native AAC decode");
        writer.write(samples.data(), count);
        done += count;
    }
    // The decode must also end exactly at the track's duration.
    UInt32 extra = 4096;
    AudioBufferList buffers{};
    buffers.mNumberBuffers = 1;
    buffers.mBuffers[0] = {2, extra * 8, samples.data()};
    check(ExtAudioFileRead(file.value, &extra, &buffers), "decode");
    if (extra)
        throw std::runtime_error("CoreAudio did not honor AAC edit-list duration");
    file.close();
    writer.close();
}
} // namespace adi::audio
