// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <stop_token>
#include <string>
#include <vector>
namespace adi {class Store;namespace audio {class DecodeCache;}}
namespace adi::media {
inline constexpr std::size_t kMaxPeakValues=1024*1024;
struct PeakBin {
    float minimum=0,maximum=0;
    friend bool operator==(const PeakBin&,const PeakBin&)=default;
};
struct MediaPeaks {
    std::string contentHash;
    std::uint32_t sampleRate=0,channels=0;
    std::uint64_t frames=0,framesPerBin=1;
    // Bin-major, then channel. Last bin may contain fewer frames.
    std::vector<PeakBin> bins;
    friend bool operator==(const MediaPeaks&,const MediaPeaks&)=default;
};
enum class PeakProblem {none,audioThread,cancelled,unknownMedia,noCache,unsupportedCache,
    invalidCache,invalidSource,missingMedia,database,read,resource,invalidRequest};
struct PeakResult {
    PeakProblem problem=PeakProblem::none;
    std::string detail;
    std::optional<MediaPeaks> data;
    explicit operator bool()const noexcept{return problem==PeakProblem::none && data.has_value();}
};
// Optional APKS v1 cache, never authoritative audio. Bounded explicit LE codec.
// All entry points refuse the audio thread before allocation/SQL/filesystem work.
// Encode returns empty on callback misuse, throws for invalid data off callback.
std::vector<std::byte> encodePeaks(const MediaPeaks&);
PeakResult decodePeaks(std::span<const std::byte>);
// Cached pool BLOB only, no filesystem access or project writes.
PeakResult readMediaPeaks(const Store&,std::int64_t mediaId);
// A worker-local read-only Store is suitable. Reuses a valid pool cache, otherwise
// hashes/resolves/decodes/streams the source. No pool/op/history writes; the window
// may memoize by contentHash and must discard stale document/generation results.
// maxBinsPerChannel applies to newly built data. DecodeCache decoding itself is
// not interruptible; cancellation is checked before/after it and per hash/read.
PeakResult buildMediaPeaks(const Store&,std::int64_t mediaId,std::uint32_t maxBinsPerChannel=4096,
    std::stop_token={},audio::DecodeCache* = nullptr);
}
