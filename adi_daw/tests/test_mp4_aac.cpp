// SPDX-License-Identifier: GPL-3.0-or-later
#include "adi/audio/aac_codec.hpp"
#include "adi/audio/wav_file.hpp"
#include "temp_directory.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <functional>
#include <iterator>
#include <stdexcept>
#include <vector>
namespace {
using namespace adi::audio;
int checks = 0, failures = 0;
void check(bool ok, const char *message) {
    ++checks;
    if (!ok) {
        ++failures;
        std::printf("FAIL: %s\n", message);
    }
}
void refuses(const std::function<void()> &call, const char *message) {
    bool threw = false;
    try {
        call();
    } catch (const std::exception &) {
        threw = true;
    }
    check(threw, message);
}
using Bytes = std::vector<unsigned char>;
Bytes bytes(const std::filesystem::path &path) {
    std::ifstream f(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(f), {}};
}
void save(const std::filesystem::path &path, const Bytes &b) {
    std::ofstream f(path, std::ios::binary);
    f.write(reinterpret_cast<const char *>(b.data()), static_cast<std::streamsize>(b.size()));
}
std::size_t tag(const Bytes &b, const std::string &name) {
    for (std::size_t i = 4; i + name.size() <= b.size(); ++i) {
        bool match = true;
        for (std::size_t j = 0; j < name.size(); ++j)
            if (b[i + j] != static_cast<unsigned char>(name[j]))
                match = false;
        if (match)
            return i;
    }
    throw std::runtime_error("test box missing");
}
void put(Bytes &b, std::size_t at, std::uint64_t value, unsigned n) {
    for (unsigned i = 0; i < n; ++i)
        b.at(at + i) = static_cast<unsigned char>(value >> ((n - i - 1) * 8));
}
void structure() {
    adi::test::TempDirectory dir{"mp4_aac", "structure"};
    const auto raw = dir.path() / "packets";
    save(raw, {1, 2, 3, 4, 5, 6, 7, 8, 9});
    AacTrack t;
    t.file = raw;
    t.sampleRate = 48000;
    t.frames = 1811;
    t.priming = 1024;
    t.config = {0x11, 0x90};
    t.packets = {{0, 3}, {3, 2}, {5, 4}};
    const auto path = dir.path() / "five.stem.mp4";
    std::vector<AacTrack> tracks(5, t);
    const std::string manifest = "{\"version\":1,\"stems\":[]}";
    writeAacMovie(path, tracks, manifest);
    const auto movie = readAacMovie(path);
    check(movie.tracks.size() == 5 && movie.stemManifest == manifest,
          "master and four tracks retain raw manifest");
    for (const auto &track : movie.tracks) {
        check(track.frames == 1811 && track.priming == 1024 && track.sampleRate == 48000 &&
                  track.config == t.config,
              "sample-exact edits and ASC round trip");
        Bytes packetBytes;
        const auto all = bytes(path);
        for (const auto &p : track.packets)
            packetBytes.insert(packetBytes.end(), all.begin() + static_cast<std::ptrdiff_t>(p.offset),
                               all.begin() + static_cast<std::ptrdiff_t>(p.offset + p.size));
        check(packetBytes == bytes(raw), "co64 packet references address exact mdat bytes");
    }
    const auto valid = bytes(path);
    const auto broken = dir.path() / "bad.mp4";
    auto mutate = [&](const std::function<void(Bytes &)> &edit, const char *message) {
        auto b = valid;
        edit(b);
        save(broken, b);
        refuses([&] { (void)readAacMovie(broken); }, message);
    };
    mutate([](Bytes &b) { b.resize(b.size() - 1); }, "truncated moov refused");
    mutate([](Bytes &b) { put(b, tag(b, "co64") + 12, 0, 8); }, "packet outside mdat refused");
    mutate([](Bytes &b) { put(b, tag(b, "stsz") + 12, 1000001, 4); },
           "packet allocation budget checked before allocation");
    mutate([](Bytes &b) { put(b, tag(b, "stsc") + 12, 0, 4); }, "zero first chunk refused");
    mutate([](Bytes &b) { put(b, tag(b, "stsc") + 20, 2, 4); }, "unknown sample description refused");
    mutate([](Bytes &b) { put(b, tag(b, "stts") + 16, 2048, 4); }, "non-AAC packet timing refused");
    mutate([](Bytes &b) { put(b, tag(b, "mvhd") + 24, 0, 4); },
           "zero movie clock refused without division by zero");
    mutate([](Bytes &b) { put(b, tag(b, "elst") + 20, 0x7fffffffffffffffULL, 8); },
           "edit beyond decoder output refused");
    mutate([](Bytes &b) { put(b, tag(b, "elst") + 28, 131072, 4); }, "non-unit edit rate refused");
    mutate(
        [](Bytes &b) {
            auto at = tag(b, "mp4a");
            b[at + 20] = 0;
            b[at + 21] = 1;
        },
        "non-stereo sample entry refused");
    mutate(
        [](Bytes &b) {
            auto at = tag(b, "mp4a");
            b[at] = 'e';
            b[at + 1] = 'n';
            b[at + 2] = 'c';
            b[at + 3] = 'a';
        },
        "encrypted audio refused");
    mutate([](Bytes &b) { put(b, tag(b, "moov") - 4, 0xffffffff, 4); },
           "oversized box rejected before allocation");
    auto extra = valid;
    const Bytes future{0, 0, 0, 12, 'z', 'z', 'z', 'z', 9, 8, 7, 6};
    extra.insert(extra.end(), future.begin(), future.end());
    save(broken, extra);
    check(readAacMovie(broken).tracks.size() == 5, "unknown future root box skipped");
    auto fragmentation = valid;
    const Bytes moof{0, 0, 0, 8, 'm', 'o', 'o', 'f'};
    fragmentation.insert(fragmentation.end(), moof.begin(), moof.end());
    save(broken, fragmentation);
    refuses([&] { (void)readAacMovie(broken); }, "fragmentation explicitly refused");
    refuses([] { (void)aacSampleRate(std::array<std::uint8_t, 2>{0x2b, 0x92}); }, "HE-AAC refused");
    refuses([] { (void)aacSampleRate(std::array<std::uint8_t, 2>{0x11, 0x88}); }, "mono ASC refused");
    refuses([] { (void)aacSampleRate(std::array<std::uint8_t, 2>{0x11, 0x94}); }, "960-sample AAC refused");
    refuses([] { (void)aacSampleRate(std::array<std::uint8_t, 5>{0x11, 0x90, 0x56, 0xe5, 0x80}); },
            "enabled SBR sync extension refused even with LC base config");
#if !defined(_WIN32)
    // POSIX scratch files are sparse: exercise real >4 GiB packet offsets
    // without allocating or reading gigabytes. Windows lacks that guarantee.
    auto sparseBytes = valid;
    constexpr std::uint64_t prefix = std::uint64_t{1} << 32;
    for (std::size_t i = 4; i + 16 < sparseBytes.size(); ++i)
        if (sparseBytes[i] == 'c' && sparseBytes[i + 1] == 'o' && sparseBytes[i + 2] == '6' &&
            sparseBytes[i + 3] == '4') {
            std::uint32_t n = 0;
            for (std::size_t j = 0; j < 4; ++j)
                n = (n << 8) | sparseBytes[i + 8 + j];
            for (std::uint32_t k = 0; k < n; ++k) {
                const auto at = i + 12 + static_cast<std::size_t>(k) * 8;
                std::uint64_t old = 0;
                for (std::size_t j = 0; j < 8; ++j)
                    old = (old << 8) | sparseBytes.at(at + j);
                put(sparseBytes, at, old + prefix, 8);
            }
        }
    const auto sparse = dir.path() / "sparse.mp4";
    {
        std::ofstream file(sparse, std::ios::binary);
        Bytes head{0, 0, 0, 1, 'f', 'r', 'e', 'e', 0, 0, 0, 1, 0, 0, 0, 0};
        file.write(reinterpret_cast<const char *>(head.data()), 16);
        file.seekp(static_cast<std::streamoff>(prefix));
        file.write(reinterpret_cast<const char *>(sparseBytes.data()),
                   static_cast<std::streamsize>(sparseBytes.size()));
    }
    const auto large = readAacMovie(sparse);
    check(large.tracks[0].packets[0].offset > prefix,
          "co64 and extended root boxes address actual sparse >4 GiB input");
    writeAacMovie(dir.path() / "repacked.mp4", large.tracks, large.stemManifest);
    check(readAacMovie(dir.path() / "repacked.mp4").tracks[0].frames == 1811,
          "mux copies packets from 64-bit source offsets without scanning sparse space");
#endif
    tracks[1].frames = 1000;
    refuses([&] { writeAacMovie(broken, tracks, manifest); }, "unaligned output durations refused");
}
float signal(std::uint32_t frame, unsigned channel, std::uint32_t rate) {
    const double time = static_cast<double>(frame) / rate;
    const double envelope = frame < 1973 ? 0.0 : 0.5 + 0.3 * std::sin(time * 29);
    return static_cast<float>(envelope * (0.23 * std::sin(time * (channel ? 1234 : 654) * 6.283185307179586) +
                                          0.11 * std::cos(time * 2311 * 6.283185307179586)));
}
void codecs() {
    if (!aacCodecAvailable()) {
        std::puts("AAC codec runtime unavailable; structural tests still run");
        return;
    }
    adi::test::TempDirectory dir{"mp4_aac", "codec"};
    for (const auto rate : std::array<std::uint32_t, 2>{44100, 48000}) {
        const auto wav = dir.path() / "input.wav", packets = dir.path() / ("encoded-" + std::to_string(rate)),
                   mp4 = dir.path() / "output.m4a", decoded = dir.path() / "decoded.wav";
        const auto count = rate + 337;
        std::vector<float> samples(static_cast<std::size_t>(count) * 2);
        for (std::uint32_t f = 0; f < count; ++f)
            for (unsigned c = 0; c < 2; ++c)
                samples[f * 2 + c] = signal(f, c, rate);
        {
            WavWriter writer(wav, rate, 2, WavFormat::Float32);
            writer.write(samples.data(), count);
            writer.close();
        }
        auto track = encodeAac(wav, packets, 256000);
        check(track.priming > 0 && track.frames == count,
              "real encoder reports priming and uncropped source length");
        writeAacMovie(mp4, std::span<const AacTrack>(&track, 1));
        auto movie = readAacMovie(mp4);
        check(movie.tracks[0].frames == count && movie.tracks[0].priming == track.priming,
              "real AAC sample-exact MP4 duration");
        decodeAac(movie.tracks[0], decoded);
        WavReader reader(decoded);
        check(reader.frames() == count && reader.sampleRate() == rate && reader.channels() == 2,
              "real decoder trims priming and tail exactly");
        std::vector<float> output(samples.size());
        check(reader.read(output.data(), count) == count, "complete real decoded WAV");
        double energy = 0, error = 0;
        std::array<double, 17> lagError{};
        for (std::uint32_t f = 4096; f + 4096 < count; ++f)
            for (unsigned c = 0; c < 2; ++c) {
                const auto want = samples[f * 2 + c], got = output[f * 2 + c];
                energy += want * want;
                error += (got - want) * (got - want);
                for (int lag = -8; lag <= 8; ++lag) {
                    const auto shifted =
                        output[static_cast<std::size_t>(static_cast<std::int64_t>(f) + lag) * 2 + c];
                    lagError[static_cast<std::size_t>(lag + 8)] += (shifted - want) * (shifted - want);
                }
            }
        check(error / energy < 0.02, "AAC round trip retains stereo audio at lossy-codec tolerance");
        check(std::min_element(lagError.begin(), lagError.end()) - lagError.begin() == 8,
              "actual decoded waveform aligns to original at zero samples");
        refuses(
            [&] {
                (void)encodeAac(wav, dir.path() / ("cancelled-" + std::to_string(rate)), 256000,
                                [] { throw std::runtime_error("cancel"); });
            },
            "worker checkpoint cancels real encoding");
        refuses(
            [&] {
                decodeAac(movie.tracks[0], dir.path() / "cancelled.wav",
                          [] { throw std::runtime_error("cancel"); });
            },
            "worker checkpoint cancels real decoding");
    }
}
} // namespace
int main() {
    try {
        structure();
        codecs();
    } catch (const std::exception &e) {
        check(false, e.what());
    }
    std::printf("mp4 AAC: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
