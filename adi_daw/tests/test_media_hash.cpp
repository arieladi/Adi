// SPDX-License-Identifier: GPL-3.0-or-later
#include "adi/media/blake3.hpp"
#include "temp_directory.hpp"
#include <nlohmann/json.hpp>
#include <cstdio>
#include <array>
#include <thread>
#include <fstream>
#include <vector>
namespace {
int checks = 0, failures = 0;
void check(bool ok, const char* name) { ++checks; if (!ok) { ++failures; std::printf("FAIL %s\n", name); } }
void run() {
    std::ifstream vectors(ADI_BLAKE3_VECTORS);
    const auto cases = nlohmann::json::parse(vectors).at("cases");
    check(cases.size() == 35, "all 35 official BLAKE3 vectors loaded");
    for (const auto& item : cases) {
        std::vector<std::byte> bytes(item.at("input_len").get<std::size_t>());
        for (std::size_t i = 0; i < bytes.size(); ++i) bytes[i] = static_cast<std::byte>(i % 251);
        const auto result = adi::media::blake3Bytes(bytes);
        check(result && result.hex == item.at("hash").get<std::string>().substr(0, 64),
              "official unkeyed 32-byte hash vector");
    }
    adi::test::TempDirectory tmp("media_hash", "stream");
    for (const std::size_t size : {std::size_t{0}, std::size_t{65536}, std::size_t{3 * 65536 + 123}}) {
        std::vector<std::byte> bytes(size);
        for (std::size_t i = 0; i < size; ++i) bytes[i] = static_cast<std::byte>((i * 13 + i / 65536) % 251);
        const auto path = tmp.path() / "input.bin";
        { std::ofstream out(path, std::ios::binary); out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(size)); }
        const auto file = adi::media::blake3File(path);
        const auto memory = adi::media::blake3Bytes(bytes);
        check(file && memory && file.hex == memory.hex, "file hash matches memory across streaming boundaries");
    }
    const auto expected = adi::media::blake3File(tmp.path() / "input.bin").hex;
    std::array<bool, 4> matches{};
    std::array<std::thread, 4> threads;
    for (std::size_t i = 0; i < threads.size(); ++i) threads[i] = std::thread([&, i] {
        const auto result = adi::media::blake3File(tmp.path() / "input.bin");
        matches[i] = result && result.hex == expected;
    });
    for (auto& thread : threads) thread.join();
    check(matches[0] && matches[1] && matches[2] && matches[3], "independent concurrent file hashes");
    const auto missing = adi::media::blake3File(tmp.path() / "missing");
    check(!missing && missing.hex.empty() && missing.error == adi::media::HashError::open, "missing file reported without throwing");
    const auto directory = adi::media::blake3File(tmp.path());
    check(!directory && directory.hex.empty(), "directory is not a hashable file");
}
}
int main() {
    try { run(); } catch (const std::exception& e) { check(false, e.what()); }
    std::printf("%s -- %d checks, %d failure(s)\n", failures ? "FAIL" : "PASS", checks, failures);
    return failures ? 1 : 0;
}
