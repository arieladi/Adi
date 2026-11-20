// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include "adi/audio/wav_file.hpp"
#include "adi/media/publish_file.hpp"
#include <cstdint>
#include <filesystem>

namespace adi::engine::recording_recovery {
struct WaveFormat {
    std::uint32_t sampleRate = 0;
    std::uint16_t channels = 0;
    audio::WavFormat encoding = audio::WavFormat::Float32;
};
struct WavePrefix {
    std::uint64_t frames = 0;
    std::uint64_t dataOffset = 0;
    std::uint64_t physicalBytes = 0;
    // Bytes after the last whole frame: possibly a RIFF pad, possibly an
    // incomplete sample. Their meaning cannot be established after a crash.
    std::uint64_t trailingBytes = 0;
    bool rf64 = false;
};

// Owner/off-callback only; throws on refusal or I/O failure. The caller must
// first validate the recording plan, project/recording id and record-folder
// membership, and establish that the recorder has stopped. Only the default
// WaveSink's no-metadata layout is accepted, not arbitrary imported WAV files.
// Absolute native paths only (Windows: ordinary drive-rooted paths, no ADS).
// Every component is opened without following symlinks/reparse points; source
// hard links are refused. The supplied identity is checked on the opened file.
// Windows denies source write/delete sharing for each operation. POSIX source
// locks are advisory; callers must exclude noncooperating writers. File identity
// is not a content hash and does not prove unchanged content between calls.
[[nodiscard]] WavePrefix inspectWave(const std::filesystem::path& source,
    const media::FileIdentity& expectedIdentity, WaveFormat expectedFormat);

// Reopens/revalidates the source and copies exactly `frames` whole frames,
// refusing a shorter source or different identity. Never modifies the source.
// Destination must be a nonexistent child of an exclusively reserved private
// directory owned by the caller; no directories are created here. Exclusive
// creation never overwrites. Failure may leave that newly created private file
// for the caller's owned-directory cleanup. The caller must not publish it until
// this function succeeds. No publication or project/journal mutation occurs.
// Payload bytes are copied verbatim; only size/count fields and the final pad
// are repaired. RF64 is retained when present, including small forced fixtures.
// The flushed staged result is checked with the ordinary strict WavReader.
void copyRepaired(const std::filesystem::path& source,
    const media::FileIdentity& expectedIdentity, WaveFormat expectedFormat,
    const std::filesystem::path& destination, std::uint64_t frames);
} // namespace adi::engine::recording_recovery
