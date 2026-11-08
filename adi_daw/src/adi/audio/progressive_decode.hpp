// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "adi/audio/pcm_stream.hpp"
#include <functional>
namespace adi::audio {
// Header/codec initialization only. Exact cheap length stays optional. AAC
// transport bounds are those of readAacAudioMovie/readAdts; no PCM predecode.
[[nodiscard]] PcmStreamInfo inspectPcm(const std::filesystem::path&,std::stop_token={});
enum class ProgressiveStatus { Pending, Complete, Cancelled, Failed };
enum class ProgressiveRead { Frames, Pending, Cancelled, Failed, InvalidBuffer, AudioThread };
struct ProgressiveSnapshot {
    ProgressiveStatus status=ProgressiveStatus::Pending;
    std::uint64_t readyFrames=0;
};
class ProgressiveDecoder;
class ProgressiveFile {
public:
    ~ProgressiveFile();
    ProgressiveFile(const ProgressiveFile&)=delete;
    ProgressiveFile& operator=(const ProgressiveFile&)=delete;
    [[nodiscard]] const PcmStreamInfo& info() const noexcept;
    [[nodiscard]] ProgressiveSnapshot snapshot() const noexcept;
    // Disk/page worker only. Reads a whole range or leaves the buffer unchanged
    // when Pending. No reader survives a call, including final publication.
    [[nodiscard]] ProgressiveRead read(std::uint64_t first,std::span<float>);
    [[nodiscard]] std::string error() const;
    // Off-callback; explicitly cancels this shared job for every lease. The
    // final lease dropping cancels automatically. A complete file stays usable.
    void cancel() noexcept;
private:
    struct State;
    explicit ProgressiveFile(std::shared_ptr<State>);
    std::shared_ptr<State> state_;
    friend class ProgressiveDecoder;
};
struct ProgressiveHooks {
    // Worker service point after each published PCM prefix, outside all locks.
    // May throw; must return promptly on cancellation (the token is the job's).
    std::function<void(std::uint64_t,std::stop_token)> checkpoint;
};
class ProgressiveDecoder {
public:
    // Cache must outlive the decoder AND every returned file. All destruction
    // and open/read/error calls are off-callback. Fixed 1..8 workers, at most
    // 32 admitted jobs (running plus queued); equal hashes share one live job.
    explicit ProgressiveDecoder(DecodeCache&,ProgressiveHooks={},unsigned workers=2);
    ~ProgressiveDecoder();
    ProgressiveDecoder(const ProgressiveDecoder&)=delete;
    ProgressiveDecoder& operator=(const ProgressiveDecoder&)=delete;
    // stop governs admission/hash/probe only; use the returned file for job cancellation.
    [[nodiscard]] std::shared_ptr<ProgressiveFile> open(const std::filesystem::path&,std::stop_token={});
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
[[nodiscard]] ProgressiveDecoder& defaultProgressiveDecoder();
} // namespace adi::audio
