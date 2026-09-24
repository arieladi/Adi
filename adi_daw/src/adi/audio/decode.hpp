// SPDX-License-Identifier: GPL-3.0-or-later
//
// One decoder for imported audio, into the decoding cache (ADR-0132 d3-d4,
// ADR-0156).
//
// The timeline plays PCM with no real-time decoding. A file the WavReader
// already plays (RIFF, RF64 or BW64; 16- or 24-bit PCM, 32-bit float) is used
// as it is. Anything else -- FLAC, AIFF/AIFC, MP3, Ogg Vorbis, and the WAVs the
// WavReader refuses (8-bit, 32-bit integer, 64-bit float, A-law, mu-law...) --
// is decoded once, off the audio thread, to a 32-bit float WAV:
//
//   <appdata cache>/decoded/<BLAKE3 of the source>.wav
//
// Keyed by the SOURCE's hash, never its path: a second project using the same
// file, wherever it lies, decodes nothing. Any rate up to 768 kHz, lower
// rates included (ADR-0157); the file keeps the source's rate and the clip
// path converts.
//
// The cache obeys the Decoding Cache settings: a maximum size and a minimum
// free space on its disk. The least recently used file goes first, and never
// one a live session is reading (a pinned file).

#pragma once

#include <nlohmann/json.hpp>

#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <mutex>
#include <stdexcept>
#include <string>
#include <vector>

namespace adi::audio {

/// Why a file cannot be made playable. toString gives the name a problem list
/// shows: "decode.corrupt", ...
enum class DecodeProblem {
    AudioThread,    // asked on the audio thread; refused before any file I/O
    Unreadable,     // the source cannot be opened or read
    UnknownFormat,  // not a format the decoder reads
    Corrupt,        // a known format, but its data does not decode (truncated, damaged)
    Unsupported,    // decodes, but outside the bounds: over 768 kHz, 0 or over 64 channels
    NoSpace,        // the minimum free space cannot be kept, even after eviction
    CacheWrite,     // the cache folder cannot be written
};
[[nodiscard]] const char* toString(DecodeProblem);

class DecodeError : public std::runtime_error {
public:
    DecodeError(DecodeProblem problem, const std::string& detail);
    [[nodiscard]] DecodeProblem problem() const noexcept { return problem_; }
private:
    DecodeProblem problem_;
};

/// What the first bytes of a file say it is. The extension is never read.
enum class SourceFormat { Riff, Aiff, Flac, Mp3, Ogg, Unknown };
[[nodiscard]] SourceFormat sniff(const std::filesystem::path&);

inline constexpr std::uint32_t kMaxDecodeRate = 768000;   // ADR-0157
inline constexpr std::uint32_t kMaxDecodeChannels = 64;

// --- the Decoding Cache settings ---------------------------------------------------------

/// The registry keys (Settings > File & Folder > Decoding Cache), in MB.
inline constexpr const char* kCacheMaxSizeKey = "cache.maxSizeMb";
inline constexpr const char* kCacheMinFreeKey = "cache.minFreeSpaceMb";
inline constexpr std::uint64_t kCacheMaxSizeMbDefault = 10240;   // 10 GB
inline constexpr std::uint64_t kCacheMinFreeMbDefault = 2048;    // 2 GB

struct DecodeCacheLimits {
    std::uint64_t maxBytes = kCacheMaxSizeMbDefault << 20;
    std::uint64_t minFreeBytes = kCacheMinFreeMbDefault << 20;
};

/// The limits from a settings object (key -> MB). A missing or non-numeric
/// key keeps its default.
[[nodiscard]] DecodeCacheLimits limitsFromSettings(const nlohmann::json& values);

/// Test seams. Both default to the real thing.
struct DecodeCacheHooks {
    /// A use stamp; later calls return larger values. Default: UTC microseconds.
    std::function<std::int64_t()> clock;
    /// Bytes free on the cache's disk. Default: std::filesystem::space.
    std::function<std::uint64_t(const std::filesystem::path&)> freeSpace;
};

class DecodeCache {
public:
    explicit DecodeCache(std::filesystem::path folder, DecodeCacheLimits = {}, DecodeCacheHooks = {});

    /// A file the WavReader plays: the source itself when the WavReader
    /// accepts it, otherwise the decoded copy, decoded now if the cache lacks
    /// it. The copy is pinned for this process (see release). Throws
    /// DecodeError. Refused on the audio thread, before any file I/O.
    [[nodiscard]] std::filesystem::path playable(const std::filesystem::path& source);

    /// A live session stopped reading a file playable() returned. Pins count:
    /// each playable() needs its own release before the file can be evicted.
    void release(const std::filesystem::path& cached);

    void setLimits(DecodeCacheLimits);
    [[nodiscard]] DecodeCacheLimits limits() const;
    /// Evicts until the limits hold, or only pinned files are left.
    void trim();

    struct Entry {
        std::string hash;
        std::uint64_t bytes = 0;
        std::int64_t lastUse = 0;
        int pins = 0;
    };
    /// Least recently used first.
    [[nodiscard]] std::vector<Entry> entries() const;
    [[nodiscard]] std::uint64_t totalBytes() const;
    [[nodiscard]] const std::filesystem::path& folder() const { return folder_; }

    struct Stats {
        std::uint64_t decodes = 0, hits = 0, passthrough = 0, evictions = 0;
    };
    [[nodiscard]] Stats stats() const;
    /// Things the cache could not do, e.g. keep its size limit because every
    /// file in it is pinned. Newest last.
    [[nodiscard]] std::vector<std::string> problems() const;

private:
    struct Item {
        std::uint64_t bytes = 0;
        std::int64_t lastUse = 0;
        int pins = 0;
    };
    [[nodiscard]] std::filesystem::path fileFor(const std::string& hash) const;
    std::int64_t stamp();
    void loadIndex();
    void saveIndex() const;
    void evict(std::uint64_t incoming, const std::string& keep);
    [[nodiscard]] std::uint64_t freeBytes() const;

    std::filesystem::path folder_;
    DecodeCacheLimits limits_;
    DecodeCacheHooks hooks_;
    mutable std::mutex mutex_;
    std::map<std::string, Item> items_;
    std::int64_t lastStamp_ = 0;
    Stats stats_;
    std::vector<std::string> problems_;
};

/// The process's cache: appdata::pathsFor(App::Daw).cache / "decoded" -- the
/// cache folder all three applications share (ADR-0149). Default limits
/// until the application applies its settings with setLimits.
[[nodiscard]] DecodeCache& defaultDecodeCache();

/// defaultDecodeCache().playable(source), except that a WAV the reader plays
/// is returned without constructing the cache: the clip worker's one call.
[[nodiscard]] std::filesystem::path playableFile(const std::filesystem::path& source);

}  // namespace adi::audio
