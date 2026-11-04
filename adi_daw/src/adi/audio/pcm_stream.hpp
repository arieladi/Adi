// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include "adi/audio/decode.hpp"
#include <memory>
#include <optional>
#include <span>
#include <stop_token>

namespace adi::audio {
struct PcmStreamInfo {
    SourceFormat format=SourceFormat::Unknown;
    std::uint32_t sampleRate=0,channels=0;
    // Missing means the container has no cheap exact length. Never a guessed
    // duration; MP3 without a length header is not fully scanned on open.
    std::optional<std::uint64_t> frames;
};
enum class PcmReadStatus { Frames, End, Cancelled, InvalidBuffer, AudioThread, WrongOwner, Failed };
struct PcmReadResult {
    PcmReadStatus status=PcmReadStatus::Failed;
    std::uint32_t frames=0;
};
enum class PcmOpenStatus { Ready, Cancelled, AudioThread, Failed };
struct PcmOpenResult;
class PcmStream {
public:
    ~PcmStream();
    PcmStream(const PcmStream&)=delete;
    PcmStream& operator=(const PcmStream&)=delete;
    [[nodiscard]] const PcmStreamInfo& info() const noexcept;
    // Same opening worker only; interleaved buffer must contain 1..65536 whole
    // frames. Invalid/pre-cancelled/callback/wrong-owner calls preserve it and
    // do not advance or poison the decoder. Cancellation is between blocks.
    [[nodiscard]] PcmReadResult read(std::span<float>,std::stop_token={});
    [[nodiscard]] std::uint64_t framesRead() const noexcept; // owner only
    [[nodiscard]] std::optional<DecodeProblem> problem() const noexcept; // owner only
    [[nodiscard]] const std::string& error() const noexcept; // owner only
private:
    struct Impl;
    explicit PcmStream(std::unique_ptr<Impl>);
    std::unique_ptr<Impl> impl_;
    friend PcmOpenResult openPcmStream(const std::filesystem::path&,std::stop_token);
};
struct PcmOpenResult {
    PcmOpenStatus status=PcmOpenStatus::Failed;
    std::unique_ptr<PcmStream> stream;
    std::optional<DecodeProblem> problem;
    std::string error;
    [[nodiscard]] bool ready() const noexcept{return status==PcmOpenStatus::Ready && stream!=nullptr;}
};
// Synchronous owner-worker codec, never an audio callback or asynchronous job.
// Open/read/final destruction belong on the same worker. Metadata is immutable;
// no Store, cache files, background threads or whole decoded PCM allocation.
// Native initialization may read headers/end pages or decode an initial frame.
// It never predecodes the whole file to discover a missing length. Sources
// must remain stable. Info/caller-owned chunks can be copied into job state.
// Read through End to validate a declared total. Reported codec errors and
// length mismatches are terminal. Unknown totals are counted as they stream;
// EOF integrity then has the native decoder's own limits.
[[nodiscard]] PcmOpenResult openPcmStream(const std::filesystem::path&,std::stop_token={});
} // namespace adi::audio
