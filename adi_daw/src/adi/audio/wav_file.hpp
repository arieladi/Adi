// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <array>
#include "adi/audio/quantizer.hpp"
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>

namespace adi::audio {
enum class WavFormat { Pcm16, Pcm24, Float32 };
// WavDither lives in quantizer.hpp (its first three values unchanged).
// The sample read back after encoding at this depth without dither.
[[nodiscard]] float wavSampleAtDepth(float value, WavFormat) noexcept;
struct BwfV2Metadata {
    // Caller-supplied ASCII, never generated from a clock, identity or job.
    // Empty text is zero-filled. Nonempty date/time use YYYY?MM?DD / hh?mm?ss;
    // EBU permits any non-NUL ASCII separator (normally '-' and ':').
    std::string originatorReference; // <=32 bytes; allocation/uniqueness is the caller's
    std::string originationDate;     // exactly 10 bytes when supplied
    std::string originationTime;     // exactly 8 bytes when supplied
    std::array<std::uint8_t,64> umid{}; // opaque caller-owned SMPTE UMID, zero if absent
    std::string codingHistory;       // ASCII CR/LF-terminated rows, no implicit terminator
    // Physical units: LUFS, LU, dBTP, LUFS, LUFS. Missing encodes 0x7fff.
    // Supplied values must be finite, within [-99.99,99.99] (range [0,99.99]);
    // multiply by 100 and round to nearest, ties away from zero.
    std::optional<double> loudnessValue, loudnessRange, maxTruePeakLevel;
    std::optional<double> maxMomentaryLoudness, maxShortTermLoudness;
};

// Zero keeps direct channel order unspecified. An explicit mask assigns
// every channel a speaker, in order of increasing bit significance.
[[nodiscard]] bool validWavSpeakerMask(std::uint16_t channels, std::uint32_t mask) noexcept;
struct WavMetadata {
    std::string description;
    std::string originator;
    std::uint64_t timeReference = 0; // samples since midnight
    std::string ixml;              // optional XML bytes, no implicit terminator
    std::optional<BwfV2Metadata> bwfV2{}; // absent retains the exact legacy v0 layout/validation
};

// Off-callback streaming I/O. Throws on invalid arguments, unsupported formats,
// corrupt input or I/O failure. Not thread-safe. PCM input is rounded to nearest,
// clipped to [-1, 1), with non-finite input mapped to zero. Float32 preserves bits.
// Optional dither is in destination LSBs, applied once before PCM rounding.
// Its seeded stream advances in interleaved sample order, independent of write
// boundaries. Float32 never dithers. Existing writers default to no dither.
// A non-null metadata pointer emits bext v0 unless bwfV2 is supplied, plus raw
// iXML when nonempty. V2 validates ASCII/length/date/time/history and loudness
// before opening the file; fields are fixed at construction, not patched later.
// close() reports finalization errors; destructor is best effort, never throws.
class WavWriter {
public:
    WavWriter(const std::filesystem::path&, std::uint32_t sampleRate,
              std::uint16_t channels, WavFormat,
              const WavMetadata* = nullptr,
              std::uint64_t promoteThresholdBytes = (std::uint64_t{1} << 32),
              WavDither = WavDither::None, std::uint64_t ditherSeed = 0, std::uint32_t speakerMask = 0);
    ~WavWriter();
    WavWriter(const WavWriter&) = delete;
    WavWriter& operator=(const WavWriter&) = delete;
    void write(const float* interleaved, std::uint32_t frames);
    void write(const double* interleaved, std::uint32_t frames);
    void close();
    [[nodiscard]] double quantizationGain() const noexcept {
        return quantizer_ ? quantizer_->gain() : 1.0;
    }
private:
    std::fstream file_;
    WavFormat format_;
    std::uint16_t channels_, bytes_;
    std::uint64_t threshold_, dataOffset_ = 0, factOffset_ = 0, dataBytes_ = 0;
    bool promoted_ = false;
    std::optional<PcmQuantizer> quantizer_;
    template<class Sample> void writeSamples(const Sample*, std::uint32_t);
    void header();
};

// Reads little-endian PCM16/24 and IEEE float32, including extensible variants,
// RF64 and BW64. Unknown chunks are skipped with bounds checks. Unsupported
// encodings (including reduced valid-bit PCM) are refused, never misdecoded.
class WavReader {
public:
    explicit WavReader(const std::filesystem::path&);
    ~WavReader();
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
