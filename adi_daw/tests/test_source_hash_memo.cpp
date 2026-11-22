// SPDX-License-Identifier: GPL-3.0-or-later
#include "adi/media/source_hash_memo.hpp"
#include "adi/audio/io_audit.hpp"
#include "temp_directory.hpp"

#include <chrono>
#include <cstdio>
#include <fstream>
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
}

int main() {
    try { warmAndChanged(); }
    catch (const std::exception& error) { check(false, error.what()); }
    std::printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
