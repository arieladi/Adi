// SPDX-License-Identifier: GPL-3.0-or-later
//
// ADR-0156: one decoder for imported audio, into the decoding cache.
// Fixtures: tests/fixtures/decode (README.md there says how each was made).
#include "adi/appdata.hpp"
#include "adi/audio/decode.hpp"
#include "adi/audio/ffprobe_audio.hpp"
#include "adi/audio/io_audit.hpp"
#include "adi/audio/wav_file.hpp"
#include "adi/media/analysis_cache.hpp"
#include "temp_directory.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>
#if defined(_WIN32)
#include <fcntl.h>
#include <io.h>
#endif

namespace fs = std::filesystem;
using namespace adi::audio;

namespace {
int checks = 0, failures = 0;
void check(bool ok, const std::string& name) {
    ++checks;
    if (!ok) { ++failures; std::printf("FAIL %s\n", name.c_str()); }
}

const fs::path fixtures = ADI_DECODE_FIXTURES;
fs::path fixture(const char* name) { return fixtures / name; }

struct Pcm {
    std::uint32_t rate = 0;
    std::uint16_t channels = 0;
    WavFormat format = WavFormat::Pcm16;
    std::vector<float> samples;
    [[nodiscard]] std::size_t frames() const { return channels ? samples.size() / channels : 0; }
};

Pcm load(const fs::path& p) {
    WavReader r(p);
    Pcm out;
    out.rate = r.sampleRate(); out.channels = r.channels(); out.format = r.format();
    out.samples.resize(static_cast<std::size_t>(r.frames()) * r.channels());
    const auto got = r.read(out.samples.data(), static_cast<std::uint32_t>(r.frames()));
    out.samples.resize(static_cast<std::size_t>(got) * r.channels());
    return out;
}

/// Best signal-to-noise ratio in dB over lags of +-2400 frames: MP3 and Vorbis
/// may shift the signal by their encoder delay.
double snr(const Pcm& ref, const Pcm& got, long* lagOut = nullptr) {
    if (ref.channels != got.channels || ref.channels == 0) return -1000;
    const long c = ref.channels;
    const long rf = static_cast<long>(ref.frames()), gf = static_cast<long>(got.frames());
    double best = -1000;
    for (long lag = -2400; lag <= 2400; ++lag) {
        double signal = 0, noise = 0;
        long used = 0;
        for (long n = 0; n < rf; ++n) {
            const long m = n + lag;
            if (m < 0 || m >= gf) continue;
            for (long k = 0; k < c; ++k) {
                const double a = ref.samples[static_cast<std::size_t>(n * c + k)];
                const double b = got.samples[static_cast<std::size_t>(m * c + k)];
                signal += a * a; noise += (a - b) * (a - b);
            }
            ++used;
        }
        if (used < rf * 9 / 10 || noise <= 0) continue;
        const double db = 10 * std::log10(signal / noise);
        if (db > best) { best = db; if (lagOut) *lagOut = lag; }
    }
    return best;
}

std::uint64_t folderBytes(const fs::path& folder) {
    std::uint64_t total = 0;
    std::error_code ec;
    for (fs::directory_iterator it(folder, ec), end; !ec && it != end; it.increment(ec))
        if (it->is_regular_file(ec)) total += static_cast<std::uint64_t>(it->file_size(ec));
    return total;
}

int leftovers(const fs::path& folder) {
    int n = 0;
    std::error_code ec;
    for (fs::directory_iterator it(folder, ec), end; !ec && it != end; it.increment(ec))
        if (it->path().filename().string().find(".partial-") != std::string::npos) ++n;
    return n;
}

/// A clock the test moves by hand.
struct Clock {
    std::int64_t now = 1000;
    DecodeCacheHooks hooks(std::function<std::uint64_t(const fs::path&)> space = {}) {
        DecodeCacheHooks h;
        h.clock = [this] { return now; };
        h.freeSpace = std::move(space);
        if (!h.freeSpace) h.freeSpace = [](const fs::path&) { return std::uint64_t{1} << 50; };
        return h;
    }
};

DecodeCacheLimits unlimited() { return {std::uint64_t{1} << 50, 0}; }

template <class F>
DecodeProblem problemOf(F&& f, std::string* message = nullptr) {
    try { f(); }
    catch (const DecodeError& e) { if (message) *message = e.what(); return e.problem(); }
    catch (const std::exception& e) { if (message) *message = e.what(); return static_cast<DecodeProblem>(-1); }
    return static_cast<DecodeProblem>(-2);
}

// --- formats ------------------------------------------------------------------------------

void formats(const fs::path& root) {
    check(sniff(fixture("tone16.flac")) == SourceFormat::Flac, "sniff: FLAC");
    check(sniff(fixture("tone.mp3")) == SourceFormat::Mp3, "sniff: MP3");
    check(sniff(fixture("tone.ogg")) == SourceFormat::Ogg, "sniff: Ogg");
    check(sniff(fixture("tone.aiff")) == SourceFormat::Aiff && sniff(fixture("tone.aifc")) == SourceFormat::Aiff, "sniff: AIFF and AIFC");
    check(sniff(fixture("u8.wav")) == SourceFormat::Riff, "sniff: RIFF");
    check(sniff(fixture("not_audio.bin")) == SourceFormat::Unknown, "sniff: not audio");

    Clock clock;
    DecodeCache cache(root / "formats", unlimited(), clock.hooks());
    const auto src16 = load(fixture("src16.wav"));
    const auto src24 = load(fixture("src24.wav"));

    const auto flac16 = load(cache.playable(fixture("tone16.flac")));
    check(flac16.format == WavFormat::Float32, "decoded copies are 32-bit float");
    check(flac16.rate == 48000 && flac16.channels == 2 && flac16.frames() == 9600, "FLAC 16-bit: rate, channels, frame count");
    check(flac16.samples == src16.samples, "FLAC 16-bit decodes bit-exact to its source PCM");
    const auto flac24 = load(cache.playable(fixture("tone24.flac")));
    check(flac24.rate == 44100 && flac24.channels == 1 && flac24.frames() == 4410, "FLAC 24-bit: rate, channels, frame count");
    check(flac24.samples == src24.samples, "FLAC 24-bit decodes bit-exact to its source PCM");

    const auto hi = load(cache.playable(fixture("rate768.flac")));
    check(hi.rate == 768000 && hi.frames() == 7680, "768 kHz FLAC decodes at 768 kHz, every frame");
    const auto lo = load(cache.playable(fixture("rate8k.flac")));
    check(lo.rate == 8000 && lo.frames() == 1600, "8 kHz FLAC decodes at 8 kHz: lower rates are kept, the clip path converts");

    const auto aiff = load(cache.playable(fixture("tone.aiff")));
    check(aiff.rate == 48000 && aiff.channels == 2 && aiff.samples == src16.samples, "AIFF: big-endian 16-bit decodes bit-exact");
    const auto aifc = load(cache.playable(fixture("tone.aifc")));
    check(aifc.samples == src16.samples, "AIFC 'sowt': little-endian inside the big-endian container, bit-exact");

    bool refused = false;
    try { WavReader r(fixture("u8.wav")); } catch (const std::exception&) { refused = true; }
    check(refused, "u8.wav is a WAV the WavReader refuses (the fixture tests what it claims)");
    const auto u8 = load(cache.playable(fixture("u8.wav")));
    bool exact = u8.frames() == 2400 && u8.rate == 48000;
    for (std::size_t i = 0; exact && i < u8.frames(); ++i) {
        // The fixture's byte is the 16-bit source's top byte plus 128. dr_wav
        // maps an unsigned byte u to u / 127.5 - 1 (ADR-0156 d5), as a
        // multiply and a subtract that a compiler may fuse (Apple clang on
        // arm64 does), so the last bit of the float is the compiler's: the
        // check allows 1e-6, far under the 1/128 step of 8-bit audio.
        const auto v16 = static_cast<long>(std::lround(src16.samples[2 * i] * 32768.0f));
        const double u = static_cast<double>(((v16 >> 8) + 128) & 255);
        exact = std::fabs(static_cast<double>(u8.samples[i]) - (u / 127.5 - 1.0)) <= 1e-6;
    }
    check(exact, "8-bit WAV the reader refuses: decoded to dr_wav's u8 mapping");

    long lag = 0;
    const auto mp3 = load(cache.playable(fixture("tone.mp3")));
    const double mp3Snr = snr(src16, mp3, &lag);
    std::printf("  MP3 192 kbps: %.1f dB SNR at lag %ld, %zu frames\n", mp3Snr, lag, mp3.frames());
    check(mp3.rate == 48000 && mp3.channels == 2, "MP3: rate and channels");
    check(mp3Snr >= 25.0, "MP3 decodes above 25 dB SNR");
    check(mp3.frames() == 9600 && lag == 0, "MP3: LAME's gapless header honoured, no encoder delay or padding");
    const auto ogg = load(cache.playable(fixture("tone.ogg")));
    const double oggSnr = snr(src16, ogg, &lag);
    std::printf("  Vorbis q6: %.1f dB SNR at lag %ld, %zu frames\n", oggSnr, lag, ogg.frames());
    check(ogg.rate == 48000 && ogg.channels == 2 && ogg.frames() == 9600, "Vorbis: rate, channels, exact length from the granule position");
    check(oggSnr >= 32.0, "Vorbis decodes above 32 dB SNR");

    // A WAV the reader plays is not copied.
    const auto before = cache.stats();
    check(cache.playable(fixture("src16.wav")) == fixture("src16.wav"), "a WAV the reader plays is used where it is");
    check(cache.stats().passthrough == before.passthrough + 1 && cache.stats().decodes == before.decodes, "... and nothing is decoded");
    check(playableFile(fixture("src16.wav")) == fixture("src16.wav"), "the clip worker's call returns a playable WAV unchanged");
}

// --- refusals -----------------------------------------------------------------------------

void refusals(const fs::path& root) {
    Clock clock;
    const auto folder = root / "refusals";
    DecodeCache cache(folder, unlimited(), clock.hooks());
    std::string why;
    check(problemOf([&] { (void)cache.playable(fixture("truncated.flac")); }, &why) == DecodeProblem::Corrupt,
          "a truncated FLAC is refused as decode.corrupt");
    check(why.find("decode.corrupt") == 0 && why.find("truncated.flac") != std::string::npos, "... the message names the problem and the file: " + why);
    check(problemOf([&] { (void)cache.playable(fixture("not_audio.bin")); }) == DecodeProblem::UnknownFormat, "not audio: decode.unknown_format");
    check(problemOf([&] { (void)cache.playable(root / "absent.flac"); }) == DecodeProblem::Unreadable, "a missing file: decode.unreadable");
    check(cache.entries().empty() && leftovers(folder) == 0, "a refused decode leaves no file in the cache, not even a partial one");
    check(std::string(toString(DecodeProblem::AudioThread)) == "decode.audio_thread", "problem names");
}

void audioThread(const fs::path& root) {
    Clock clock;
    DecodeCache cache(root / "audio-thread", unlimited(), clock.hooks());
    const auto io = callbackFileIo.load();
    DecodeProblem p{};
    {
        CallbackScope scope;
        p = problemOf([&] { (void)cache.playable(fixture("tone24.flac")); });
    }
    check(p == DecodeProblem::AudioThread, "decoding on the audio thread is refused: decode.audio_thread");
    check(callbackFileIo.load() == io, "... before any file I/O on the callback (the decoder's reads are instrumented)");
    check(cache.stats().decodes == 0 && cache.entries().empty(), "... and nothing is decoded");
    (void)cache.playable(fixture("tone24.flac"));
    check(callbackFileIo.load() == io && cache.stats().decodes == 1, "off the audio thread it decodes, and counts no callback I/O");
}

// --- the cache ----------------------------------------------------------------------------

void keyedByHash(const fs::path& root) {
    Clock clock;
    const auto folder = root / "keyed";
    DecodeCache cache(folder, unlimited(), clock.hooks());
    const auto first = cache.playable(fixture("tone16.flac"));
    const auto second = cache.playable(fixture("tone16.flac"));
    check(first == second && cache.stats().decodes == 1 && cache.stats().hits == 1, "cache hit on the second open");

    // The same bytes under another name in another folder: a second project.
    fs::create_directories(root / "project2");
    const auto copy = root / "project2" / "renamed.flac";
    fs::copy_file(fixture("tone16.flac"), copy);
    check(cache.playable(copy) == first && cache.stats().decodes == 1, "the same file at another path decodes nothing: keyed by BLAKE3, not path");

    // Other bytes at a path the cache has seen: a new decode, the right audio.
    fs::copy_file(fixture("tone24.flac"), copy, fs::copy_options::overwrite_existing);
    const auto other = cache.playable(copy);
    check(other != first && cache.stats().decodes == 2 && load(other).samples == load(fixture("src24.wav")).samples,
          "a changed file at the same path is decoded afresh");

    // Another process, or the next session: the files are found on disk.
    DecodeCache again(folder, unlimited(), clock.hooks());
    check(again.playable(fixture("tone16.flac")) == first && again.stats().decodes == 0 && again.stats().hits == 1,
          "a second cache on the same folder (another application, the next launch) hits");
    check(first.parent_path() == folder && first.filename().string().size() == 64 + 4, "the file is <folder>/<64 hex digits>.wav");
}

std::uint64_t decodedSize(const fs::path& root, const char* name) {
    Clock clock;
    DecodeCache probe(root / (std::string("size-") + name), unlimited(), clock.hooks());
    return fs::file_size(probe.playable(fixture(name)));
}

void eviction(const fs::path& root) {
    const auto a = decodedSize(root, "tone24.flac"), b = decodedSize(root, "rate8k.flac"), c = decodedSize(root, "tone.aiff");
    Clock clock;
    const auto folder = root / "lru";
    // Room for A and C, not for all three.
    DecodeCache cache(folder, {a + b + c - 1, 0}, clock.hooks());
    const auto pa = cache.playable(fixture("tone24.flac")); clock.now += 10;
    const auto pb = cache.playable(fixture("rate8k.flac")); clock.now += 10;
    cache.release(pa); cache.release(pb);
    (void)cache.playable(fixture("tone24.flac")); cache.release(pa); clock.now += 10;   // A used again: B is now the oldest
    const auto pc = cache.playable(fixture("tone.aiff"));
    check(fs::exists(pa) && !fs::exists(pb) && fs::exists(pc), "the least recently used file goes first (B), not the oldest decoded (A)");
    check(cache.stats().evictions == 1 && cache.entries().size() == 2, "exactly one eviction");
    check(cache.totalBytes() <= a + b + c - 1, "the size limit holds");
    const auto order = cache.entries();
    check(order.size() == 2 && fs::path(folder / (order[0].hash + ".wav")) == pa, "entries() lists least recently used first");

    // A limit below everything: all released files go, the size limit holds.
    cache.release(pc);
    cache.setLimits({c, 0});
    cache.trim();
    check(!fs::exists(pa) && fs::exists(pc) && cache.totalBytes() <= c, "trim to a smaller limit: the size limit holds, least recently used first");
}

void pinned(const fs::path& root) {
    const auto a = decodedSize(root, "tone24.flac");
    Clock clock;
    const auto folder = root / "pinned";
    // Each individual estimate must fit, including its 4 KiB allowance. The
    // two actual files together still exceed this cap while A remains pinned.
    DecodeCache cache(folder, {a + 4096, 0}, clock.hooks());
    const auto pa = cache.playable(fixture("tone24.flac")); clock.now += 10;   // in use: never released
    const auto pb = cache.playable(fixture("rate8k.flac"));
    check(fs::exists(pa), "a file a live session is reading is never evicted, even over the size limit");
    check(fs::exists(pb), "the file just decoded is kept to be played");
    const auto problems = cache.problems();
    check(!problems.empty() && problems.back().find("in use") != std::string::npos, "the cache says why it is over its limit");
    cache.release(pa);
    cache.trim();
    check(!fs::exists(pa) && cache.totalBytes() <= a, "released, it is evicted by the next trim");
}

void freeSpace(const fs::path& root) {
    const auto a = decodedSize(root, "tone24.flac");
    Clock clock;
    const auto folder = root / "space";
    // A disk of `capacity` bytes that holds only the cache.
    std::uint64_t capacity = 0;
    auto space = [&](const fs::path&) { const auto used = folderBytes(folder); return capacity > used ? capacity - used : 0; };
    const std::uint64_t minFree = 64 * 1024;
    // The decoder reserves frames * channels * 4 + 4096 bytes before writing.
    // After A, the disk is short of B's reservation by the index file's size:
    // A must go.
    capacity = minFree + a + (1600 * 1 * 4 + 4096);
    DecodeCache cache(folder, {std::uint64_t{1} << 50, minFree}, clock.hooks(space));
    fs::path pa, pb;
    std::string made;
    const auto p = problemOf([&] {
        pa = cache.playable(fixture("tone24.flac")); cache.release(pa); clock.now += 10;
        pb = cache.playable(fixture("rate8k.flac")); cache.release(pb);
    }, &made);
    check(p == static_cast<DecodeProblem>(-2), "room for B is made by evicting A, not refused: " + made);
    check(!pa.empty() && !pb.empty() && !fs::exists(pa) && fs::exists(pb), "the minimum free space evicts the least recently used file");

    capacity = minFree + 1024;   // nothing fits, even after eviction
    std::string why;
    check(problemOf([&] { (void)cache.playable(fixture("tone.aiff")); }, &why) == DecodeProblem::NoSpace, "no room even after eviction: decode.no_space");
    check(leftovers(folder) == 0, "... and no partial file is left");
}

void settings() {
    const auto d = limitsFromSettings(nlohmann::json::object());
    check(d.maxBytes == kCacheMaxSizeMbDefault << 20 && d.minFreeBytes == kCacheMinFreeMbDefault << 20, "limits: defaults when the settings are absent");
    const auto l = limitsFromSettings({{kCacheMaxSizeKey, 100}, {kCacheMinFreeKey, 7}});
    check(l.maxBytes == std::uint64_t{100} << 20 && l.minFreeBytes == std::uint64_t{7} << 20, "limits: read from the Decoding Cache settings, in MB");
    const auto bad = limitsFromSettings({{kCacheMaxSizeKey, -5}, {kCacheMinFreeKey, "x"}});
    check(bad.maxBytes == d.maxBytes && bad.minFreeBytes == d.minFreeBytes, "limits: an illegal value keeps the default");
}

void defaultCacheSettings(const fs::path& root) {
    const auto home = root / "owned-application";
#if defined(_WIN32)
    if (_wputenv_s(L"ADI_HOME", home.c_str()) != 0) throw std::runtime_error("set test ADI home");
#else
    if (setenv("ADI_HOME", home.c_str(), 1) != 0) throw std::runtime_error("set test ADI home");
#endif
    const auto fallback = adi::appdata::pathsFor(adi::appdata::App::Daw).cache / "decoded";
    const auto parent = root / fs::path(u8"cache-音");
    const auto folder = parent / "decoded";
    fs::create_directories(parent);
    const auto other = parent / "keep.wav";
    std::ofstream(other, std::ios::binary) << "User media outside decoded storage";
    check(!setDefaultMediaCacheRoot("relative-cache"), "a relative cache folder is refused");
    auto terminated = parent.native();
    terminated.push_back(fs::path::value_type{});
    check(!setDefaultMediaCacheRoot(fs::path(terminated)), "a NUL in a cache path is refused");
    {
        CallbackScope callback;
        check(!setDefaultMediaCacheRoot(parent), "an audio callback cannot configure a cache folder");
    }
    check(setDefaultMediaCacheRoot(parent), "an absolute Unicode cache root is accepted");
    setDefaultDecodeCacheLimits({64ULL << 20, 0});
    check(!fs::exists(folder), "setting initial limits leaves the process decode folder lazy");
    check(playableFile(fixture("src16.wav")) == fixture("src16.wav") && !fs::exists(folder),
          "configured limits keep an ordinary WAV on its direct path");
    DecodeProblem refused{};
    {
        CallbackScope callback;
        refused = problemOf([&] { (void)playableFile(fixture("tone16.flac")); });
    }
    check(refused == DecodeProblem::AudioThread && !fs::exists(folder),
          "a refused first audio callback cannot initialize the cache");
    const auto next = root / "next-launch";
    const auto& analysis = adi::media::defaultAnalysisCache();
    check(analysis.folder() == parent / "analysed" && !fs::exists(analysis.folder()) && !fs::exists(folder),
          "the analysis cache uses the selected Unicode parent without creating either cache folder");
    check(!setDefaultMediaCacheRoot(next), "analysis use alone fixes the common root until relaunch");
    auto& configured = defaultDecodeCache();
    check(configured.folder() == folder && configured.limits().maxBytes == 64ULL << 20 &&
              configured.limits().minFreeBytes == 0,
          "first decoder use takes the selected folder and configured limits");
    const auto held = configured.playable(fixture("tone16.flac"));
    check(held.parent_path() == folder && !fs::exists(fallback) &&
              !setDefaultMediaCacheRoot(next) && !fs::exists(next),
          "live cache use keeps the root until relaunch");
    setDefaultDecodeCacheLimits({0, 0});
    check(&defaultDecodeCache() == &configured && configured.limits().maxBytes == 0 && fs::exists(held),
          "updating active limits keeps the cache object and the owned pinned PCM");
    configured.release(held);
    configured.trim();
    check(!fs::exists(held) && fs::is_regular_file(other),
          "a later owner trim stays in the decoded child rather than touching neighboring files");
}

void ffprobeMetadata(const fs::path& folder) {
    const std::string valid = R"({"streams":[{"index":2,"codec_type":"audio","sample_rate":"48000","channels":2}]})";
    const auto stream = parseFfprobeAudio(valid);
    check(stream.index == 2 && stream.sampleRate == 48000 && stream.channels == 2,
          "ffprobe retains the absolute audio stream index and PCM layout");
    check(problemOf([&] { (void)parseFfprobeAudio(R"({"streams":[]})"); }) == DecodeProblem::NoAudio,
          "ffprobe's empty audio selection means no audio");

    const auto refuse = [](std::string_view text, DecodeProblem code) {
        return problemOf([&] { (void)parseFfprobeAudio(text); }) == code;
    };
    check(refuse(valid + std::string("\0suffix", 7), DecodeProblem::Corrupt), "ffprobe refuses bytes after a null");
    check(refuse(std::string(65537, ' '), DecodeProblem::Corrupt), "ffprobe refuses oversized JSON");
    check(refuse(R"({"streams":[],"x":[[[[[[[[[[{"y":0}]]]]]]]]]]})", DecodeProblem::Corrupt),
          "ffprobe refuses excessive nesting in ignored fields");
    check(refuse(R"({"streams":[{},{}]})", DecodeProblem::Corrupt), "ffprobe refuses an ambiguous selection");
    for (const char* field : {"-1", "2.5", "4294967296", "true", "\"2\""}) {
        auto packet = nlohmann::json::parse(valid);
        packet["streams"][0]["index"] = nlohmann::json::parse(field);
        check(refuse(packet.dump(), DecodeProblem::Corrupt), std::string("ffprobe refuses invalid index ") + field);
    }
    auto packet = nlohmann::json::parse(valid);
    auto& audio = packet["streams"][0];
    audio["channels"] = 65;
    check(refuse(packet.dump(), DecodeProblem::Unsupported), "ffprobe enforces the decode channel limit");
    audio["channels"] = 1;
    audio["sample_rate"] = "768001";
    check(refuse(packet.dump(), DecodeProblem::Unsupported), "ffprobe enforces the decode sample-rate limit");
    audio["sample_rate"] = "48000junk";
    check(refuse(packet.dump(), DecodeProblem::Corrupt), "ffprobe requires a complete decimal sample rate");

    const auto absent = folder / "absent-ffprobe";
    check(problemOf([&] { (void)probeFfprobeAudio(absent, {}); }) == DecodeProblem::Unsupported,
          "a missing ffprobe reports unsupported");
    std::stop_source stopped;
    stopped.request_stop();
    check(problemOf([&] { (void)probeFfprobeAudio(absent, {}, stopped.get_token()); }) == DecodeProblem::Cancelled,
          "a cancelled ffprobe refuses before looking for the tool");
}

constexpr std::size_t kTestFfmpegFrames = 700;
constexpr std::size_t kTestFfmpegChannels = 3;
constexpr std::array<float, kTestFfmpegChannels> kTestFfmpegSample{0.5f, -0.25f, 0.125f};

int ffmpegTestChild(int argc, char** argv) {
    const auto input = std::find_if(argv, argv + argc - 1, [](const char* arg) {
        return std::string_view(arg) == "-i";
    });
    if (input == argv + argc - 1) return 2;
    const auto mode = fs::path(input[1]).filename();
    if (mode == "empty") return 0;
#if defined(_WIN32)
    if (_setmode(_fileno(stdout), _O_BINARY) == -1) return 2;
#endif
    constexpr std::array<unsigned char, kTestFfmpegChannels * 4> frame{
        0, 0, 0, 0x3f, 0, 0, 0x80, 0xbe, 0, 0, 0, 0x3e};
    if (mode == "partial") return std::fwrite(frame.data(), 1, 5, stdout) == 5 ? 0 : 2;
    if (std::fwrite(frame.data(), 1, 3, stdout) != 3 || std::fflush(stdout) != 0) return 2;
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    if (std::fwrite(frame.data() + 3, 1, frame.size() - 3, stdout) != frame.size() - 3) return 2;
    for (std::size_t i = 1; i < kTestFfmpegFrames; ++i) {
        if (std::fwrite(frame.data(), 1, frame.size(), stdout) != frame.size()) return 2;
    }
    return mode == "failed" ? 7 : 0;
}

void ffmpegPcm(const fs::path& executable, const fs::path& folder) {
    const auto source = [&](const char* name) {
        auto path = folder / name;
        std::ofstream(path, std::ios::binary) << "owned process test fixture";
        return path;
    };
    constexpr FfprobeAudioStream layout{1, 48000, 3};
    const auto accepted = [](std::span<const float>) { return true; };
    const auto complete = source("complete");
    std::vector<float> samples;
    const auto frames = streamFfmpegAudio(executable, complete, layout, kTestFfmpegFrames,
        [&](std::span<const float> chunk) {
            check(chunk.size() % kTestFfmpegChannels == 0, "every PCM sink batch is frame-aligned");
            samples.insert(samples.end(), chunk.begin(), chunk.end());
            return true;
        });
    check(frames == kTestFfmpegFrames && samples.size() == frames * kTestFfmpegChannels,
          "FFmpeg PCM accepts the exact frame budget and reports its delivered frames");
    bool exact = true;
    for (std::size_t i = 0; i < samples.size(); ++i)
        if (samples[i] != kTestFfmpegSample[i % kTestFfmpegChannels]) exact = false;
    check(exact, "split process reads reconstruct the exact float words and channel order");
    check(problemOf([&] {
        (void)streamFfmpegAudio(executable, complete, layout, kTestFfmpegFrames - 1, accepted);
    }) == DecodeProblem::NoSpace, "FFmpeg PCM enforces the frame budget");
    for (const char* input : {"empty", "partial", "failed"}) {
        check(problemOf([&] {
            (void)streamFfmpegAudio(executable, source(input), layout, kTestFfmpegFrames, accepted);
        }) == DecodeProblem::Corrupt, std::string("FFmpeg PCM refuses ") + input + " output");
    }
    check(problemOf([&] {
        (void)streamFfmpegAudio(executable, complete, layout, kTestFfmpegFrames,
                               [](std::span<const float>) { return false; });
    }) == DecodeProblem::CacheWrite, "FFmpeg PCM reports a sink refusal");
    check(problemOf([&] {
        (void)streamFfmpegAudio(executable, complete, layout, kTestFfmpegFrames,
            [](std::span<const float>) -> bool { throw DecodeError(DecodeProblem::NoSpace, "sink quota"); });
    }) == DecodeProblem::NoSpace, "FFmpeg PCM preserves the sink's typed error");
}
}  // namespace

int main(int argc, char** argv) {
    try {
        if (argc > 2 && std::string_view(argv[1]) == "-nostdin")
            return ffmpegTestChild(argc, argv);
        if (argc == 6 && std::string_view(argv[1]) == "--ffmpeg-stream") {
            const auto stream = probeFfprobeAudio(argv[3], argv[4]);
            double peak = 0.0;
            const auto frames = streamFfmpegAudio(argv[2], argv[4], stream, std::stoull(argv[5]),
                [&](std::span<const float> samples) {
                    for (float value : samples) peak = std::max(peak, std::abs(static_cast<double>(value)));
                    return true;
                });
            std::printf("%u %u %u %llu %.9f\n", stream.index, stream.sampleRate, stream.channels,
                        static_cast<unsigned long long>(frames), peak);
            return 0;
        }
        // Optional native gate against an explicitly chosen ffprobe and owned media.
        if (argc == 4 && std::string_view(argv[1]) == "--ffprobe") {
            const auto stream = probeFfprobeAudio(argv[2], argv[3]);
            std::printf("%u %u %u\n", stream.index, stream.sampleRate, stream.channels);
            return 0;
        }
        adi::test::TempDirectory tmp("decode", "suite");
        formats(tmp.path());
        refusals(tmp.path());
        audioThread(tmp.path());
        keyedByHash(tmp.path());
        eviction(tmp.path());
        pinned(tmp.path());
        freeSpace(tmp.path());
        settings();
        defaultCacheSettings(tmp.path());
        ffprobeMetadata(tmp.path());
        ffmpegPcm(fs::absolute(argv[0]), tmp.path());
    } catch (const std::exception& e) {
        check(false, std::string("unexpected exception: ") + e.what());
    }
    std::printf("%s -- %d checks, %d failure(s)\n", failures ? "FAIL" : "PASS", checks, failures);
    return failures ? 1 : 0;
}
