// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <filesystem>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <system_error>
namespace adi::media {
// Atomic replacement for optional app-owned cache data, same filesystem only.
// The source is private and closed. No exclusive-copy fallback exposes partial
// replacement bytes. This must never publish authoritative user media/settings.
[[nodiscard]] std::error_code replaceCacheFile(const std::filesystem::path& source,
                                              const std::filesystem::path& target) noexcept;
enum class PublishStatus { published, exists, failed };
struct FileIdentity {
    std::uint64_t volume = 0, file = 0;
    friend bool operator==(const FileIdentity&, const FileIdentity&) = default;
};
// Owner-only native protection for an open project, before SQLite opens and
// until it closes. Shared leases permit existing SQLite reader/writer policy;
// an authoritative replacement alone takes an exclusive lease. POSIX uses a
// private per-user lease inode keyed by database identity, never an extra DB
// descriptor whose close could drop SQLite's process record locks. No session row.
class ProjectFileLease final {
public:
    ~ProjectFileLease();
    ProjectFileLease(const ProjectFileLease&) = delete;
    ProjectFileLease& operator=(const ProjectFileLease&) = delete;
    static std::shared_ptr<ProjectFileLease> openShared(const std::filesystem::path&, std::error_code&) noexcept;
    static std::shared_ptr<ProjectFileLease> createShared(const std::filesystem::path&, std::error_code&) noexcept;
    static std::shared_ptr<ProjectFileLease> openExclusive(const std::filesystem::path&, std::error_code&) noexcept;
    FileIdentity identity() const noexcept;
    const std::filesystem::path& path() const noexcept;
    // Failed creation only; never removes a different file now at that name.
    bool removeCreated(std::error_code&) noexcept;
private:
    struct Impl;
    explicit ProjectFileLease(std::unique_ptr<Impl>);
    static std::shared_ptr<ProjectFileLease> open(const std::filesystem::path&, bool create,
                                                 bool exclusive, std::error_code&) noexcept;
    std::unique_ptr<Impl> impl_;
};
struct PublishResult {
    PublishStatus status = PublishStatus::failed;
    bool usedFallback = false; // Selected the copy path; status still decides success.
    std::string error;
    std::string warning{}; // Published, but post-publication durability failed.
    std::optional<FileIdentity> identity{};
    bool durabilityConfirmed = false;
};
enum class PublishPolicy { VerifiedCopyFallback, AtomicOnly };
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
    const std::filesystem::path& target, PublishHooks hooks = {},
    PublishPolicy policy = PublishPolicy::VerifiedCopyFallback) noexcept;
struct ReplaceHooks {
    std::error_code (*syncFile)(const std::filesystem::path&) = nullptr;
    std::error_code (*rename)(const std::filesystem::path&, const std::filesystem::path&) = nullptr;
    std::error_code (*syncDirectory)(const std::filesystem::path&) = nullptr;
    // Runs under the destination's exclusive lease, before any publication.
    std::error_code (*validateTarget)(const std::filesystem::path&) = nullptr;
    bool (*retryable)(const std::error_code&) = nullptr;
    void (*retryWait)(unsigned milliseconds) = nullptr;
};
// The replacing rename replaceFile uses (test hooks reuse it). On Windows it
// replaces a destination held open only by handles that share delete -- the
// replacement's own exclusive lease -- which std::filesystem::rename cannot.
// A volume without POSIX rename (SMB, FAT/exFAT, Windows before 10 1709)
// returns std::errc::operation_not_supported, never a retryable refusal.
[[nodiscard]] std::error_code renameReplacing(const std::filesystem::path& source,
                                              const std::filesystem::path& target) noexcept;
// Authoritative closed-stage replacement. No copy fallback. Every failure
// before rename leaves the destination unchanged; post-rename sync failure is
// published with a warning. A live Store lease refuses replacement.
[[nodiscard]] PublishResult replaceFile(const std::filesystem::path& source,
    const std::filesystem::path& target, ReplaceHooks hooks = {}) noexcept;
}
