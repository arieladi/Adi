// SPDX-License-Identifier: GPL-3.0-or-later
#include "blake3.hpp"
#include <blake3.h>
#include <array>
#include <fstream>

namespace adi::media {
namespace {
HashResult finish(const blake3_hasher& hasher) {
    std::array<unsigned char, BLAKE3_OUT_LEN> digest{};
    blake3_hasher_finalize(&hasher, digest.data(), digest.size());
    constexpr char digits[] = "0123456789abcdef";
    std::string hex(digest.size() * 2, '0');
    for (std::size_t i = 0; i < digest.size(); ++i) {
        hex[i * 2] = digits[digest[i] >> 4];
        hex[i * 2 + 1] = digits[digest[i] & 15];
    }
    return {std::move(hex), HashError::none};
}
}
HashResult blake3Bytes(std::span<const std::byte> bytes) noexcept {
    try {
        blake3_hasher hasher;
        blake3_hasher_init(&hasher);
        if (!bytes.empty()) blake3_hasher_update(&hasher, bytes.data(), bytes.size());
        return finish(hasher);
    } catch (...) { return {{}, HashError::resource}; }
}
HashResult blake3File(const std::filesystem::path& path) noexcept { return blake3File(path, {}); }
HashResult blake3File(const std::filesystem::path& path, std::stop_token stop) noexcept {
    try {
        std::error_code error;
        if (!std::filesystem::is_regular_file(path, error) || error)
            return {{}, HashError::open};
        std::ifstream input(path, std::ios::binary);
        if (!input) return {{}, HashError::open};
        blake3_hasher hasher;
        blake3_hasher_init(&hasher);
        std::array<char, 65536> buffer{};
        while (input) {
            if (stop.stop_requested()) return {{}, HashError::cancelled};
            input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
            const auto count = input.gcount();
            if (count > 0) blake3_hasher_update(&hasher, buffer.data(), static_cast<std::size_t>(count));
        }
        if (input.bad() || !input.eof()) return {{}, HashError::read};
        return finish(hasher);
    } catch (...) { return {{}, HashError::resource}; }
}
} // namespace adi::media
