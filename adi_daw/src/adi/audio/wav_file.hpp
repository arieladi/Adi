// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>

namespace adi::audio {
enum class WavFormat { Pcm16, Pcm24, Float32 };
struct WavMetadata {
    std::string description;
    std::string originator;
    std::uint64_t timeReference = 0; // samples since midnight
    std::string ixml;              // optional XML bytes, no implicit terminator
};

// Off-callback streaming I/O. Throws on invalid arguments, unsupported formats,
// corrupt input or I/O failure. Not thread-safe. PCM input is rounded to nearest,
// clipped to [-1, 1), with non-finite input mapped to zero. Float32 preserves bits.
// A non-null metadata pointer emits a version-0 bext, plus iXML when nonempty.
// close() reports finalization errors; destructor is best effort, never throws.
class WavWriter {
public:
    WavWriter(const std::filesystem::path&, std::uint32_t sampleRate,
              std::uint16_t channels, WavFormat,
              const WavMetadata* = nullptr,
              std::uint64_t promoteThresholdBytes = (std::uint64_t{1} << 32));
    ~WavWriter();
    WavWriter(const WavWriter&) = delete;
    WavWriter& operator=(const WavWriter&) = delete;
    void write(const float* interleaved, std::uint32_t frames);
    void close();
private:
    std::fstream file_;
    WavFormat format_;
    std::uint16_t channels_, bytes_;
    std::uint64_t threshold_, dataOffset_ = 0, factOffset_ = 0, dataBytes_ = 0;
    bool promoted_ = false;
    void header();
};

// Reads little-endian PCM16/24 and IEEE float32, including extensible variants,
// RF64 and BW64. Unknown chunks are skipped with bounds checks. Unsupported
// encodings (including reduced valid-bit PCM) are refused, never misdecoded.
class WavReader {
public:
    explicit WavReader(const std::filesystem::path&);
    WavReader(const WavReader&) = delete;
    WavReader& operator=(const WavReader&) = delete;
    [[nodiscard]] std::uint32_t sampleRate() const noexcept { return rate_; }
    [[nodiscard]] std::uint16_t channels() const noexcept { return channels_; }
    [[nodiscard]] std::uint64_t frames() const noexcept { return frames_; }
    [[nodiscard]] WavFormat format() const noexcept { return format_; }
    std::uint32_t read(float* interleaved, std::uint32_t frames);
    void seek(std::uint64_t frame); // frame==frames() is EOF; beyond EOF throws
private:
    std::ifstream file_;
    WavFormat format_ = WavFormat::Pcm16;
    std::uint32_t rate_ = 0;
    std::uint16_t channels_ = 0, bytes_ = 0, align_ = 0;
    std::uint64_t frames_ = 0, position_ = 0, dataOffset_ = 0;
};
} // namespace adi::audio
