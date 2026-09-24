// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstddef>
#include <filesystem>
#include <span>
#include <string>
#include <stop_token>

namespace adi::media {
enum class HashError { none, open, read, resource, cancelled };
struct HashResult {
    std::string hex; // 64 lowercase hex digits on success; empty on failure.
    HashError error = HashError::none;
    explicit operator bool() const noexcept { return error == HashError::none; }
};
// Off-callback helpers. Ordinary I/O failures and allocation exceptions are
// returned, never thrown. File hashing uses a fixed 64 KiB buffer.
[[nodiscard]] HashResult blake3Bytes(std::span<const std::byte> bytes) noexcept;
[[nodiscard]] HashResult blake3File(const std::filesystem::path& path) noexcept;
[[nodiscard]] HashResult blake3File(const std::filesystem::path& path, std::stop_token stop) noexcept;
} // namespace adi::media
