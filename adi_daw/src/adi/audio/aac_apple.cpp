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
AudioStreamBasicDescription pcm(std::uint32_t rate,std::uint32_t channels=2) {
    AudioStreamBasicDescription f{};
    f.mSampleRate = rate;
    f.mFormatID = kAudioFormatLinearPCM;
    f.mFormatFlags = kAudioFormatFlagsNativeFloatPacked;
    f.mBytesPerPacket = channels*4;
    f.mFramesPerPacket = 1;
    f.mBytesPerFrame = channels*4;
    f.mChannelsPerFrame = channels;
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
    auto movie = readAacMovie(path);
    if (movie.tracks.size() != 1)
        throw std::runtime_error("CoreAudio returned multiple AAC tracks");
    auto track = std::move(movie.tracks[0]);
    if (track.priming != prime.leadingFrames)
        throw std::runtime_error("CoreAudio container/encoder priming mismatch");
    if (track.frames != reader.frames())
        throw std::runtime_error("CoreAudio AAC container lost sample-exact duration");
    return track;
}
void decodeAac(const AacTrack &track, const std::filesystem::path &wav, CodecCheckpoint call) {
    const auto info=aacConfig(track.config);
    if(info.sampleRate!=track.sampleRate)throw std::invalid_argument("AAC decode config mismatch");
    const auto channels=info.channels;
    auto source = wav;
    source += ".m4a";
    struct Cleanup {
        std::filesystem::path path;
        ~Cleanup() {
            std::error_code ec;
            std::filesystem::remove(path, ec);
        }
    } cleanup{source};
    writeAacAudioMovie(source,track,call);
    Url url(source);
    File file;
    check(ExtAudioFileOpenURL(url.value, &file.value), "open");
    auto client = pcm(track.sampleRate,channels);
    check(
        ExtAudioFileSetProperty(file.value, kExtAudioFileProperty_ClientDataFormat, sizeof(client), &client),
        "decode format");
    SInt64 frames = 0;
    UInt32 size = sizeof(frames);
    check(ExtAudioFileGetProperty(file.value, kExtAudioFileProperty_FileLengthFrames, &size, &frames),
          "decoded length");
    if (frames < 0 || static_cast<std::uint64_t>(frames) != track.frames)
        throw std::runtime_error("CoreAudio did not honor AAC edit-list duration");
    WavWriter writer(wav, track.sampleRate, channels, WavFormat::Float32);
    std::array<float, 8192> samples{};
    std::uint64_t done = 0;
    while (done < track.frames) {
        tick(call);
        UInt32 count = static_cast<UInt32>((std::min)(track.frames - done, std::uint64_t{4096}));
        AudioBufferList buffers{};
        buffers.mNumberBuffers = 1;
        buffers.mBuffers[0] = {channels, count * channels * 4, samples.data()};
        check(ExtAudioFileRead(file.value, &count, &buffers), "decode");
        if (!count)
            throw std::runtime_error("CoreAudio AAC decode ended early");
        for (std::size_t i = 0; i < static_cast<std::size_t>(count) * channels; ++i)
            if (!std::isfinite(samples[i]))
                throw std::runtime_error("nonfinite native AAC decode");
        writer.write(samples.data(), count);
        done += count;
    }
    file.close();
    writer.close();
}
} // namespace adi::audio
