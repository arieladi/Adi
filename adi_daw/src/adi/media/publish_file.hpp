// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <filesystem>
#include <string>
#include <system_error>
namespace adi::media {
// Atomic replacement for optional app-owned cache data, same filesystem only.
// The source is private and closed. No exclusive-copy fallback exposes partial
// replacement bytes. This must never publish authoritative user media/settings.
[[nodiscard]] std::error_code replaceCacheFile(const std::filesystem::path& source,
                                              const std::filesystem::path& target) noexcept;
enum class PublishStatus { published, exists, failed };
struct PublishResult {
    PublishStatus status = PublishStatus::failed;
    bool usedFallback = false; // Selected the copy path; status still decides success.
    std::string error;
};
// Test support, off-callback: refuse native rename or disturb a completed copy
// before verification. Null hooks use the real OS and unmodified bytes.
struct PublishHooks {
    std::error_code (*rename)(const std::filesystem::path&, const std::filesystem::path&) = nullptr;
    void (*afterCopy)(const std::filesystem::path&) = nullptr;
};
// Source is a private, closed staging file on the destination filesystem.
// Never replaces a destination. Fallback becomes successful only after flush,
// close and BLAKE3 verification; it is visible during copying, unlike rename.
// On ordinary failure only the destination created by this call is removed.
[[nodiscard]] PublishResult publishFile(const std::filesystem::path& source,
    const std::filesystem::path& target, PublishHooks hooks = {}) noexcept;
}
