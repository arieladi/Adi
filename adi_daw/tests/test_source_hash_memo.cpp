// SPDX-License-Identifier: GPL-3.0-or-later
#include "adi/media/source_hash_memo.hpp"
#include "adi/audio/io_audit.hpp"
#include "adi/audio/decode.hpp"
#include "adi/audio/wav_file.hpp"
#include "adi/render/flac_writer.hpp"
#include "temp_directory.hpp"

#include <array>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string_view>
#include <vector>

namespace {
namespace fs = std::filesystem;
using namespace adi::media;
int checks = 0, failures = 0;
void check(bool ok, const char* note) {
    ++checks;
    if (!ok) { ++failures; std::printf("FAIL: %s\n", note); }
}
void write(const fs::path& path, std::string_view bytes) {
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    file.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    file.close();
    if (!file) throw std::runtime_error("source hash fixture write failed");
}
void warmAndChanged() {
    adi::test::TempDirectory temp{"source-hash-memo", "warm-edits"};
    SourceHashMemo memo;
    std::vector<std::string> hashes;
    const auto file = [&](std::size_t n) { return temp.path() / (std::to_string(n) + ".media"); };
    for (std::size_t i = 0; i < 50; ++i) {
        write(file(i), std::to_string(i) + " payload");
        const auto first = memo.verify(file(i));
        check(first.digest && !first.reused, "cold source reads its real bytes");
        hashes.push_back(first.digest.hex);
    }
    std::size_t hits = 0;
    for (std::size_t i = 0; i < hashes.size(); ++i) {
        const auto opened = memo.verify(file(i));
        if (opened.digest && opened.reused && opened.digest.hex == hashes[i]) ++hits;
    }
    check(hits == 50, "fifty warm sources serve verified digests through metadata only");

    const auto source = file(0);
    const auto changedAt = fs::last_write_time(source) + std::chrono::seconds(4);
    const auto sameBytes = fs::file_size(source);
    write(source, "0 changed");
    fs::last_write_time(source, changedAt);
    check(fs::file_size(source) == sameBytes, "source edit preserves size");
    const auto edited = memo.verify(source);
    check(edited.digest && !edited.reused && edited.digest.hex != hashes[0],
          "updated timestamp verifies a same-size edit against the saved identity");
    const auto stage = temp.path() / "unrelated-decoded-stage.wav";
    write(stage, "cached PCM");
    fs::remove(stage);
    const auto reopened = memo.verify(source);
    check(reopened.digest && reopened.reused && reopened.digest.hex == edited.digest.hex &&
          reopened.digest.hex != hashes[0], "same mismatch survives decoded-stage eviction");

    const auto replacement = temp.path() / "replacement.media";
    write(replacement, "0 payload");
    fs::last_write_time(replacement, changedAt);
    fs::rename(source, temp.path() / "retained-old.media");
    fs::rename(replacement, source);
    const auto rebound = memo.verify(source);
    check(rebound.digest && !rebound.reused && rebound.digest.hex == hashes[0],
          "same-size, same-time replacement verifies its new native file identity");

    std::stop_source cancelled;
    cancelled.request_stop();
    check(memo.verify(source, cancelled.get_token()).digest.error == HashError::cancelled,
          "cancelled warm lookup refuses the memo");
    fs::remove(source);
    check(!memo.verify(source).digest, "absence cannot serve an old digest by path");
    const auto callbackBefore = adi::audio::callbackFileIo.load();
    {
        adi::audio::CallbackScope callback;
        check(!memo.verify(file(1)).digest, "memo refuses filesystem work from an audio callback");
    }
    check(adi::audio::callbackFileIo.load() == callbackBefore + 1, "callback refusal records the audit point");
}
void compressedRewrite() {
    if (!adi::render::flacExportAvailable()) {
        std::puts("SKIP native FLAC encoder unavailable");
        return;
    }
    adi::test::TempDirectory temp{"source-hash-memo", "compressed-rewrite"};
    const auto wave = temp.path() / "encoder-input.wav";
    const auto source = temp.path() / "source.flac";
    const auto alternative = temp.path() / "replacement.flac";
    const auto encode = [&](const fs::path& target, float amplitude) {
        std::array<float, 4096> samples;
        samples.fill(amplitude);
        adi::audio::WavWriter writer(wave, 48000, 1, adi::audio::WavFormat::Pcm16);
        writer.write(samples.data(), static_cast<std::uint32_t>(samples.size()));
        writer.close();
        adi::render::encodeStagedFlac(wave, target);
    };
    encode(source, 3.0f / 32.0f);
    encode(alternative, 5.0f / 32.0f);
    check(fs::file_size(source) == fs::file_size(alternative), "real FLAC edits have equal encoded size");

    SourceHashMemo memo;
    const auto saved = memo.verify(source);
    adi::audio::DecodeCache cache(temp.path() / "cache", {1u << 20, 0});
    const auto originalPcm = cache.playable(source);
    cache.release(originalPcm);
    const auto modified = fs::last_write_time(source) + std::chrono::seconds(4);
    std::error_code error;
    const auto id = readFileIdentity(source, error);
    std::ifstream encoded(alternative, std::ios::binary);
    const std::string bytes(std::istreambuf_iterator<char>{encoded}, {});
    write(source, bytes);
    fs::last_write_time(source, modified);
    check(id && readFileIdentity(source, error) == id, "compressed bytes were rewritten in the original file");

    const auto edited = memo.verify(source);
    check(saved.digest && edited.digest && !edited.reused && edited.digest.hex != saved.digest.hex,
          "same-size FLAC re-encode refreshes the digest and differs from the project's saved source hash");
    cache.setLimits({0, 0});
    cache.trim();
    check(!fs::exists(originalPcm) && cache.stats().evictions == 1,
          "the production decode cache evicts the old hash's WAV");
    for (int reopen = 0; reopen < 2; ++reopen) {
        const auto actual = memo.verify(source);
        check(actual.digest && actual.reused && actual.digest.hex == edited.digest.hex &&
              actual.digest.hex != saved.digest.hex, "reopened compressed media still differs after PCM eviction");
    }

    cache.setLimits({1u << 20, 0});
    const auto changedPcm = cache.playable(source);
    adi::audio::WavReader changed(changedPcm);
    std::array<float, 2> heard{};
    check(changedPcm != originalPcm && changed.frames() == 4096 && changed.channels() == 1 &&
          changed.sampleRate() == 48000 && changed.read(heard.data(), 2) == 2 && heard[0] == 5.0f / 32.0f,
          "the edited audio opens at the same duration and uses new PCM");
    cache.release(changedPcm);
}

}

int main() {
    try { warmAndChanged(); compressedRewrite(); }
    catch (const std::exception& error) { check(false, error.what()); }
    std::printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
