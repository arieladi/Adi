// SPDX-License-Identifier: GPL-3.0-or-later
#include "adi/audio/decode.hpp"

#include "adi/appdata.hpp"
#include "adi/audio/io_audit.hpp"
#include "adi/audio/wav_file.hpp"
#include "adi/media/blake3.hpp"

#define DR_FLAC_NO_STDIO
#include "dr_flac.h"
#define DR_MP3_NO_STDIO
#include "dr_mp3.h"
#define DR_WAV_NO_STDIO
#include "dr_wav.h"
#define STB_VORBIS_NO_STDIO
#define STB_VORBIS_NO_PUSHDATA_API
#define STB_VORBIS_HEADER_ONLY
#include "stb_vorbis.c"

#include <algorithm>
#include <array>
#include <chrono>
#include <climits>
#include <fstream>
#include <memory>
#include <random>
#include <system_error>

namespace adi::audio {
namespace fs = std::filesystem;

const char* toString(DecodeProblem p) {
    switch (p) {
    case DecodeProblem::AudioThread: return "decode.audio_thread";
    case DecodeProblem::Unreadable: return "decode.unreadable";
    case DecodeProblem::UnknownFormat: return "decode.unknown_format";
    case DecodeProblem::Corrupt: return "decode.corrupt";
    case DecodeProblem::Unsupported: return "decode.unsupported";
    case DecodeProblem::NoSpace: return "decode.no_space";
    case DecodeProblem::CacheWrite: return "decode.cache_write";
    }
    return "decode.unknown";
}

DecodeError::DecodeError(DecodeProblem problem, const std::string& detail)
    : std::runtime_error(std::string(toString(problem)) + ": " + detail), problem_(problem) {}

namespace {

std::string display(const fs::path& p) {
    const auto u = p.generic_u8string();
    return {reinterpret_cast<const char*>(u.data()), u.size()};
}

// --- the byte source every decoder reads through ---------------------------------------

struct Stream {
    std::ifstream in;
    explicit Stream(const fs::path& p) : in(p, std::ios::binary) {}
};

std::size_t readBytes(void* user, void* out, std::size_t n) {
    auto& s = *static_cast<Stream*>(user);
    fileIoPoint();   // ADR-0151's audit: a read on the audio thread is counted
    s.in.read(static_cast<char*>(out), static_cast<std::streamsize>(n));
    const auto got = s.in.gcount();
    if (!s.in) s.in.clear();   // EOF must not poison a later seek
    return static_cast<std::size_t>(got);
}

// The three libraries' seek origins are distinct enums with the same values:
// 0 set, 1 current, 2 end.
bool seekBytes(void* user, int offset, int origin) {
    auto& s = *static_cast<Stream*>(user);
    const auto dir = origin == 0 ? std::ios::beg : origin == 1 ? std::ios::cur : std::ios::end;
    s.in.seekg(static_cast<std::streamoff>(offset), dir);
    if (!s.in) { s.in.clear(); return false; }
    return true;
}

bool tellBytes(void* user, std::int64_t* at) {
    auto& s = *static_cast<Stream*>(user);
    const auto pos = s.in.tellg();
    if (pos < 0) return false;
    *at = static_cast<std::int64_t>(pos);
    return true;
}

// --- one decoder interface over four libraries ------------------------------------------

class Decoder {
public:
    virtual ~Decoder() = default;
    std::uint32_t channels = 0;
    std::uint32_t rate = 0;
    std::uint64_t frames = 0;   // 0 when the format does not say
    /// Interleaved float frames; fewer than asked only at the end.
    virtual std::uint64_t read(float* out, std::uint64_t n) = 0;
};

class FlacDecoder final : public Decoder {
public:
    explicit FlacDecoder(const fs::path& p) : stream_(p) {
        flac_ = drflac_open(
            [](void* u, void* o, std::size_t n) { return readBytes(u, o, n); },
            [](void* u, int off, drflac_seek_origin org) -> drflac_bool32 { return seekBytes(u, off, static_cast<int>(org)) ? DRFLAC_TRUE : DRFLAC_FALSE; },
            [](void* u, drflac_int64* at) -> drflac_bool32 { std::int64_t v = 0; if (!tellBytes(u, &v)) return DRFLAC_FALSE; *at = v; return DRFLAC_TRUE; },
            &stream_, nullptr);
        if (!flac_) throw DecodeError(DecodeProblem::Corrupt, display(p) + ": not a readable FLAC stream");
        channels = flac_->channels; rate = flac_->sampleRate; frames = flac_->totalPCMFrameCount;
    }
    ~FlacDecoder() override { drflac_close(flac_); }
    std::uint64_t read(float* out, std::uint64_t n) override { return drflac_read_pcm_frames_f32(flac_, n, out); }
private:
    Stream stream_;
    drflac* flac_ = nullptr;
};

class WavDecoder final : public Decoder {   // AIFF, AIFC, and the WAVs WavReader refuses
public:
    explicit WavDecoder(const fs::path& p) : stream_(p) {
        const bool ok = drwav_init(&wav_,
            [](void* u, void* o, std::size_t n) { return readBytes(u, o, n); },
            [](void* u, int off, drwav_seek_origin org) -> drwav_bool32 { return seekBytes(u, off, static_cast<int>(org)) ? DRWAV_TRUE : DRWAV_FALSE; },
            [](void* u, drwav_int64* at) -> drwav_bool32 { std::int64_t v = 0; if (!tellBytes(u, &v)) return DRWAV_FALSE; *at = v; return DRWAV_TRUE; },
            &stream_, nullptr) == DRWAV_TRUE;
        if (!ok) throw DecodeError(DecodeProblem::Corrupt, display(p) + ": not a readable WAV or AIFF file");
        open_ = true;
        channels = wav_.channels; rate = wav_.sampleRate; frames = wav_.totalPCMFrameCount;
    }
    ~WavDecoder() override { if (open_) drwav_uninit(&wav_); }
    std::uint64_t read(float* out, std::uint64_t n) override { return drwav_read_pcm_frames_f32(&wav_, n, out); }
private:
    Stream stream_;
    drwav wav_{};
    bool open_ = false;
};

class Mp3Decoder final : public Decoder {
public:
    explicit Mp3Decoder(const fs::path& p) : stream_(p) {
        const bool ok = drmp3_init(&mp3_,
            [](void* u, void* o, std::size_t n) { return readBytes(u, o, n); },
            [](void* u, int off, drmp3_seek_origin org) -> drmp3_bool32 { return seekBytes(u, off, static_cast<int>(org)) ? DRMP3_TRUE : DRMP3_FALSE; },
            [](void* u, drmp3_int64* at) -> drmp3_bool32 { std::int64_t v = 0; if (!tellBytes(u, &v)) return DRMP3_FALSE; *at = v; return DRMP3_TRUE; },
            nullptr, &stream_, nullptr) == DRMP3_TRUE;
        if (!ok) throw DecodeError(DecodeProblem::Corrupt, display(p) + ": no MPEG audio frame decodes");
        open_ = true;
        channels = mp3_.channels; rate = mp3_.sampleRate;
        frames = drmp3_get_pcm_frame_count(&mp3_);   // scans, then rewinds
    }
    ~Mp3Decoder() override { if (open_) drmp3_uninit(&mp3_); }
    std::uint64_t read(float* out, std::uint64_t n) override { return drmp3_read_pcm_frames_f32(&mp3_, n, out); }
private:
    Stream stream_;
    drmp3 mp3_{};
    bool open_ = false;
};

class VorbisDecoder final : public Decoder {
public:
    // stb_vorbis reads from memory: the compressed file is held while it
    // decodes (about 1 MB a minute at -q 6), never the PCM.
    explicit VorbisDecoder(const fs::path& p) {
        std::error_code ec;
        const auto size = fs::file_size(p, ec);
        if (ec) throw DecodeError(DecodeProblem::Unreadable, display(p) + ": " + ec.message());
        if (size > static_cast<std::uintmax_t>(INT_MAX))
            throw DecodeError(DecodeProblem::Unsupported, display(p) + ": an Ogg file over 2 GiB");
        bytes_.resize(static_cast<std::size_t>(size));
        Stream s(p);
        if (!s.in || readBytes(&s, bytes_.data(), bytes_.size()) != bytes_.size())
            throw DecodeError(DecodeProblem::Unreadable, display(p) + ": short read");
        int error = 0;
        vorbis_ = stb_vorbis_open_memory(bytes_.data(), static_cast<int>(bytes_.size()), &error, nullptr);
        if (!vorbis_) throw DecodeError(DecodeProblem::Corrupt, display(p) + ": not a readable Ogg Vorbis stream (stb_vorbis error " + std::to_string(error) + ")");
        const auto info = stb_vorbis_get_info(vorbis_);
        channels = static_cast<std::uint32_t>(info.channels); rate = info.sample_rate;
        frames = stb_vorbis_stream_length_in_samples(vorbis_);
    }
    ~VorbisDecoder() override { stb_vorbis_close(vorbis_); }
    std::uint64_t read(float* out, std::uint64_t n) override {
        std::uint64_t done = 0;
        while (done < n) {
            const auto want = static_cast<int>(std::min<std::uint64_t>(n - done, 4096) * channels);
            const int got = stb_vorbis_get_samples_float_interleaved(vorbis_, static_cast<int>(channels), out + static_cast<std::size_t>(done) * channels, want);
            if (got <= 0) break;
            done += static_cast<std::uint64_t>(got);
        }
        return done;
    }
private:
    std::vector<unsigned char> bytes_;
    stb_vorbis* vorbis_ = nullptr;
};

/// Ogg holds Vorbis or FLAC; the first packet says which.
bool oggIsFlac(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    std::array<char, 34> b{};
    in.read(b.data(), static_cast<std::streamsize>(b.size()));
    return in.gcount() == static_cast<std::streamsize>(b.size()) && std::string(b.data() + 28, 5) == "\x7F" "FLAC";
}

std::unique_ptr<Decoder> openDecoder(const fs::path& p, SourceFormat f) {
    switch (f) {
    case SourceFormat::Flac: return std::make_unique<FlacDecoder>(p);
    case SourceFormat::Riff:
    case SourceFormat::Aiff: return std::make_unique<WavDecoder>(p);
    case SourceFormat::Mp3: return std::make_unique<Mp3Decoder>(p);
    case SourceFormat::Ogg:
        if (oggIsFlac(p)) return std::make_unique<FlacDecoder>(p);
        return std::make_unique<VorbisDecoder>(p);
    case SourceFormat::Unknown: break;
    }
    throw DecodeError(DecodeProblem::UnknownFormat, display(p));
}

/// Decodes `source` to a float WAV at `out`. `reserve` is told the size
/// before a byte is written, and may refuse.
void decodeTo(const fs::path& source, SourceFormat format, const fs::path& out,
              const std::function<void(std::uint64_t)>& reserve) {
    auto d = openDecoder(source, format);
    if (d->channels == 0 || d->channels > kMaxDecodeChannels)
        throw DecodeError(DecodeProblem::Unsupported, display(source) + ": " + std::to_string(d->channels) + " channels");
    if (d->rate == 0 || d->rate > kMaxDecodeRate)
        throw DecodeError(DecodeProblem::Unsupported, display(source) + ": " + std::to_string(d->rate) + " Hz, over 768 kHz");
    reserve(d->frames * d->channels * 4 + 4096);

    constexpr std::uint32_t block = 4096;
    std::vector<float> buffer(static_cast<std::size_t>(block) * d->channels);
    std::uint64_t done = 0;
    try {
        WavWriter w(out, d->rate, static_cast<std::uint16_t>(d->channels), WavFormat::Float32);
        for (;;) {
            const auto got = d->read(buffer.data(), block);
            if (got == 0) break;
            w.write(buffer.data(), static_cast<std::uint32_t>(got));
            done += got;
        }
        w.close();
    } catch (const DecodeError&) {
        throw;
    } catch (const std::exception& e) {
        throw DecodeError(DecodeProblem::CacheWrite, display(out) + ": " + e.what());
    }
    if (done == 0) throw DecodeError(DecodeProblem::Corrupt, display(source) + ": no audio decodes");
    if (d->frames != 0 && done != d->frames)
        throw DecodeError(DecodeProblem::Corrupt, display(source) + ": decoded " + std::to_string(done) +
                                                      " of " + std::to_string(d->frames) + " frames; the file is truncated or damaged");
}

bool isHash(const std::string& s) {
    return s.size() == 64 && std::all_of(s.begin(), s.end(), [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); });
}

std::string uniqueSuffix() {
    std::random_device rd;
    std::uniform_int_distribution<std::uint64_t> pick;
    const auto v = pick(rd) ^ static_cast<std::uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count());
    std::array<char, 17> hex{};
    for (std::size_t i = 0; i < 16; ++i) hex[i] = "0123456789abcdef"[(v >> (60 - 4 * i)) & 15U];
    return std::string(hex.data(), 16);
}

}  // namespace

SourceFormat sniff(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    if (!in) throw DecodeError(DecodeProblem::Unreadable, display(p) + ": cannot be opened");
    std::array<unsigned char, 12> b{};
    in.read(reinterpret_cast<char*>(b.data()), static_cast<std::streamsize>(b.size()));
    const auto n = static_cast<std::size_t>(in.gcount());
    auto is = [&](std::size_t at, const char* tag) {
        const auto len = std::char_traits<char>::length(tag);
        return n >= at + len && std::equal(tag, tag + len, b.begin() + static_cast<std::ptrdiff_t>(at),
                                           [](char a, unsigned char c) { return static_cast<unsigned char>(a) == c; });
    };
    if (is(0, "RIFF") || is(0, "RIFX") || is(0, "RF64") || is(0, "BW64") || is(0, "riff")) return SourceFormat::Riff;
    if (is(0, "FORM") && (is(8, "AIFF") || is(8, "AIFC"))) return SourceFormat::Aiff;
    if (is(0, "fLaC")) return SourceFormat::Flac;
    if (is(0, "OggS")) return SourceFormat::Ogg;
    if (is(0, "ID3")) return SourceFormat::Mp3;
    if (n >= 2 && b[0] == 0xFF && (b[1] & 0xE0) == 0xE0 && (b[1] & 0x06) != 0) return SourceFormat::Mp3;
    return SourceFormat::Unknown;
}

DecodeCacheLimits limitsFromSettings(const nlohmann::json& values) {
    DecodeCacheLimits l;
    auto mb = [&](const char* key, std::uint64_t& out) {
        if (!values.is_object()) return;
        const auto it = values.find(key);
        if (it == values.end() || !it->is_number()) return;
        const double v = it->get<double>();
        if (v >= 0 && v < 1e12) out = static_cast<std::uint64_t>(v) << 20;
    };
    mb(kCacheMaxSizeKey, l.maxBytes);
    mb(kCacheMinFreeKey, l.minFreeBytes);
    return l;
}

// --- the cache --------------------------------------------------------------------------

DecodeCache::DecodeCache(fs::path folder, DecodeCacheLimits limits, DecodeCacheHooks hooks)
    : folder_(std::move(folder)), limits_(limits), hooks_(std::move(hooks)) {
    if (!hooks_.clock)
        hooks_.clock = [] {
            return static_cast<std::int64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count());
        };
    if (!hooks_.freeSpace)
        hooks_.freeSpace = [](const fs::path& p) -> std::uint64_t {
            std::error_code ec;
            const auto s = fs::space(p, ec);
            return ec ? UINT64_MAX : static_cast<std::uint64_t>(s.available);
        };
    std::error_code ec;
    fs::create_directories(folder_, ec);
    loadIndex();
}

fs::path DecodeCache::fileFor(const std::string& hash) const { return folder_ / (hash + ".wav"); }

std::int64_t DecodeCache::stamp() {
    lastStamp_ = std::max(hooks_.clock(), lastStamp_ + 1);
    return lastStamp_;
}

std::uint64_t DecodeCache::freeBytes() const { return hooks_.freeSpace(folder_); }

void DecodeCache::loadIndex() {
    // The index is a hint (sizes and last use); the files are the truth.
    std::ifstream in(folder_ / "index.json", std::ios::binary);
    if (in) {
        const auto index = nlohmann::json::parse(in, nullptr, false);
        if (index.is_object() && index.contains("entries") && index["entries"].is_object()) {
            const auto& entries = index["entries"];
            for (const auto& [hash, e] : entries.items()) {
                if (!isHash(hash) || !e.is_object()) continue;
                Item item;
                item.lastUse = e.value("lastUse", std::int64_t{0});
                items_[hash] = item;
            }
        }
    }
    std::error_code ec;
    std::map<std::string, Item> present;
    for (fs::directory_iterator it(folder_, ec), end; !ec && it != end; it.increment(ec)) {
        const auto& p = it->path();
        const auto stem = display(p.stem());   // never .string(): a stray non-ASCII name throws on Windows
        if (p.extension() != ".wav" || !isHash(stem)) continue;
        std::error_code sizeError;
        const auto bytes = fs::file_size(p, sizeError);
        if (sizeError) continue;
        auto item = items_.count(stem) ? items_[stem] : Item{};
        item.bytes = static_cast<std::uint64_t>(bytes);
        present[stem] = item;
        lastStamp_ = std::max(lastStamp_, item.lastUse);
    }
    items_ = std::move(present);
}

void DecodeCache::saveIndex() const {
    nlohmann::json entries = nlohmann::json::object();
    for (const auto& [hash, item] : items_) entries[hash] = {{"bytes", item.bytes}, {"lastUse", item.lastUse}};
    const nlohmann::json index = {{"schema", 1}, {"entries", entries}};
    const auto tmp = folder_ / ("index.json.tmp-" + uniqueSuffix());
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        out << index.dump(1);
        if (!out) { std::error_code ec; fs::remove(tmp, ec); return; }
    }
    std::error_code ec;
    fs::rename(tmp, folder_ / "index.json", ec);
    if (ec) fs::remove(tmp, ec);
}

std::uint64_t DecodeCache::totalBytes() const {
    std::lock_guard lock(mutex_);
    std::uint64_t total = 0;
    for (const auto& [hash, item] : items_) total += item.bytes;
    return total;
}

void DecodeCache::evict(std::uint64_t incoming, const std::string& keep) {
    // Called with the lock held.
    auto total = [&] {
        std::uint64_t t = 0;
        for (const auto& [hash, item] : items_) t += item.bytes;
        return t;
    };
    auto over = [&] {
        return total() + incoming > limits_.maxBytes || freeBytes() < limits_.minFreeBytes + incoming;
    };
    std::vector<std::string> busy;
    while (over()) {
        const std::string* oldest = nullptr;
        std::int64_t when = 0;
        for (const auto& [hash, item] : items_) {
            if (item.pins > 0 || hash == keep || std::find(busy.begin(), busy.end(), hash) != busy.end()) continue;
            if (!oldest || item.lastUse < when) { oldest = &hash; when = item.lastUse; }
        }
        if (!oldest) {
            problems_.push_back(total() + incoming > limits_.maxBytes
                ? "decode cache over its maximum size: every file left in it is in use"
                : "decode cache below its minimum free space: every file left in it is in use");
            return;
        }
        const auto hash = *oldest;
        std::error_code ec;
        fs::remove(fileFor(hash), ec);
        // Another process may hold it open (Windows refuses the delete): skip
        // it, never fail a decode over it.
        if (ec) { busy.push_back(hash); continue; }
        items_.erase(hash);
        ++stats_.evictions;
    }
}

fs::path DecodeCache::playable(const fs::path& source) {
    if (onAudioThread)
        throw DecodeError(DecodeProblem::AudioThread, display(source) + ": decoding is never done on the audio thread");
    const auto format = sniff(source);
    if (format == SourceFormat::Riff) {
        try {
            WavReader probe(source);
            std::lock_guard lock(mutex_);
            ++stats_.passthrough;
            return source;
        } catch (const std::exception&) {
            // A WAV the reader refuses: decoded like any other format.
        }
    }
    if (format == SourceFormat::Unknown)
        throw DecodeError(DecodeProblem::UnknownFormat, display(source) + ": not WAV, AIFF, FLAC, MP3 or Ogg Vorbis");
    const auto hash = media::blake3File(source);
    if (!hash) throw DecodeError(DecodeProblem::Unreadable, display(source) + ": cannot be hashed");
    const auto target = fileFor(hash.hex);

    {
        std::lock_guard lock(mutex_);
        std::error_code ec;
        if (fs::exists(target, ec)) {   // decoded before, by this process or another
            auto& item = items_[hash.hex];
            item.bytes = static_cast<std::uint64_t>(fs::file_size(target, ec));
            item.lastUse = stamp();
            ++item.pins;
            ++stats_.hits;
            saveIndex();
            return target;
        }
    }

    std::error_code ec;
    fs::create_directories(folder_, ec);
    if (!fs::is_directory(folder_, ec))
        throw DecodeError(DecodeProblem::CacheWrite, display(folder_) + ": the cache folder cannot be made");
    const auto partial = folder_ / (hash.hex + ".wav.partial-" + uniqueSuffix());
    try {
        decodeTo(source, format, partial, [&](std::uint64_t estimate) {
            std::lock_guard lock(mutex_);
            evict(estimate, hash.hex);
            if (freeBytes() < limits_.minFreeBytes + estimate)
                throw DecodeError(DecodeProblem::NoSpace, display(source) + ": decoding needs " + std::to_string(estimate) +
                                                               " bytes and would leave less than the minimum free space");
        });
        fs::rename(partial, target);
    } catch (...) {
        fs::remove(partial, ec);
        if (!fs::exists(target, ec)) throw;
        // Another decoder of the same source finished first: use its file.
    }

    std::lock_guard lock(mutex_);
    auto& item = items_[hash.hex];
    item.bytes = static_cast<std::uint64_t>(fs::file_size(target, ec));
    item.lastUse = stamp();
    ++item.pins;
    ++stats_.decodes;
    evict(0, hash.hex);
    saveIndex();
    return target;
}

void DecodeCache::release(const fs::path& cached) {
    std::lock_guard lock(mutex_);
    const auto it = items_.find(display(cached.stem()));
    if (it != items_.end() && it->second.pins > 0 && cached.parent_path() == folder_) --it->second.pins;
}

void DecodeCache::setLimits(DecodeCacheLimits l) {
    std::lock_guard lock(mutex_);
    limits_ = l;
}

DecodeCacheLimits DecodeCache::limits() const {
    std::lock_guard lock(mutex_);
    return limits_;
}

void DecodeCache::trim() {
    std::lock_guard lock(mutex_);
    evict(0, {});
    saveIndex();
}

std::vector<DecodeCache::Entry> DecodeCache::entries() const {
    std::lock_guard lock(mutex_);
    std::vector<Entry> out;
    for (const auto& [hash, item] : items_) out.push_back({hash, item.bytes, item.lastUse, item.pins});
    std::sort(out.begin(), out.end(), [](const Entry& a, const Entry& b) { return a.lastUse < b.lastUse; });
    return out;
}

DecodeCache::Stats DecodeCache::stats() const {
    std::lock_guard lock(mutex_);
    return stats_;
}

std::vector<std::string> DecodeCache::problems() const {
    std::lock_guard lock(mutex_);
    return problems_;
}

DecodeCache& defaultDecodeCache() {
    static DecodeCache cache(appdata::pathsFor(appdata::App::Daw).cache / "decoded");
    return cache;
}

fs::path playableFile(const fs::path& source) {
    // A WAV the reader plays never touches the cache, so a session of plain
    // WAVs creates no cache folder at all.
    if (!onAudioThread && sniff(source) == SourceFormat::Riff) {
        try {
            WavReader probe(source);
            return source;
        } catch (const std::exception&) {
            // refused: the cache decodes it
        }
    }
    return defaultDecodeCache().playable(source);
}

}  // namespace adi::audio
