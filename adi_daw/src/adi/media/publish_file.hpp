// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "blake3.hpp"
#include <array>
#include <filesystem>
#include <functional>
#include <cstdint>
#include <compare>
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
enum class FileIdentityScheme { Posix, Windows128, WindowsLegacy64 };
struct FileIdentity {
    // Windows FileIdInfo: full 64-bit volume and both little-endian halves of
    // its opaque 16-byte ID. POSIX/legacy use file as the inode/index, high=0.
    // Separate namespaces prevent a legacy query from impersonating a wide ID.
    std::uint64_t volume = 0, file = 0, fileHigh = 0;
    FileIdentityScheme scheme = FileIdentityScheme::Posix;
    friend auto operator<=>(const FileIdentity&, const FileIdentity&) = default;
};
// Off-callback identity of a regular file, following symlinks. Reuses the
// publisher's native volume/file key; this is not a persistent content hash or
// an open-file lease. Failure retains the native error (no path-only fallback).
[[nodiscard]] std::optional<FileIdentity> readFileIdentity(const std::filesystem::path&,
                                                          std::error_code&) noexcept;
/// Revision of one regular file from the same native stat or handle. Change
/// time catches in-place edits even when a writer restores modification time.
struct FileRevision {
    FileIdentity identity;
    std::uint64_t size;
    std::array<std::int64_t,2> modified, changed;
    friend bool operator==(const FileRevision&,const FileRevision&) = default;
};
/// Like readFileIdentity, but refuses a final symlink or reparse point.
[[nodiscard]] std::optional<FileRevision> readFileRevision(const std::filesystem::path&,
                                                          std::error_code&) noexcept;
struct FileRevisionDigest {
    HashResult hash;
    std::optional<FileRevision> revision;
};
/// Read the digest and its cacheable revision through the same open file.
/// Symlinks and storage without a reliable revision still produce an uncached digest.
[[nodiscard]] FileRevisionDigest blake3FileWithRevision(const std::filesystem::path&, std::stop_token) noexcept;
// Roll back one exposed publication only when a Windows DELETE handle proves
// its expected identity. The handle denies concurrent opens/rename/delete and
// never follows a final reparse point. Missing paths are already clean.
// Other hosts conservatively retain the path: pathname unlink is not a
// compare-and-remove operation. Errors require the owner to report retention.
// This verifies object identity, not unchanged contents; owner/off-callback.
[[nodiscard]] std::error_code removePublishedFile(const std::filesystem::path&,
                                                  const FileIdentity&) noexcept;
[[nodiscard]] std::error_code syncPublishedDirectory(const std::filesystem::path&) noexcept;
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
    // Remove a closed project under an exclusive lease if its revision still matches.
    bool removeExclusive(const FileRevision&, std::error_code&,
                         const std::function<std::error_code()>& validate) noexcept;
    // Failed creation only; never removes a different file now at that name.
    bool removeCreated(std::error_code&,
                       const std::function<std::error_code()>& validate = {}) noexcept;
private:
    struct Impl;
    explicit ProjectFileLease(std::unique_ptr<Impl>);
    static std::shared_ptr<ProjectFileLease> open(const std::filesystem::path&, bool create,
                                                 bool exclusive, std::error_code&) noexcept;
    std::unique_ptr<Impl> impl_;
};
struct PublishResult {
    PublishStatus status = PublishStatus::failed;
    bool usedFallback = false; // A publication fallback was selected.
    std::string error;
    std::string warning{}; // Published, but post-publication durability failed.
    std::optional<FileIdentity> identity{};
    bool durabilityConfirmed = false;
};
enum class PublishPolicy { VerifiedCopyFallback, AtomicOnly };
// Test support, off-callback: refuse native rename/sync, replace retry waits,
// or disturb a completed private copy. Null hooks use the real OS.
struct PublishHooks {
    std::error_code (*rename)(const std::filesystem::path&, const std::filesystem::path&) = nullptr;
    void (*afterCopy)(const std::filesystem::path&) = nullptr;
    std::error_code (*syncDirectory)(const std::filesystem::path&) = nullptr;
    std::error_code (*syncFile)(const std::filesystem::path&) = nullptr;
    void (*retryWait)(unsigned milliseconds) = nullptr;
};
// Source is a private, closed staging file on the destination filesystem.
// Never replaces a destination. Fallback flushes, closes and verifies a private
// copy, then links it at the destination exclusively. Filesystems without
// exclusive rename or hard links report failure with the source retained.
// Failure cleanup acts only on private copy staging.
// Windows sync/rename errors 5/32/33 share seven bounded retries (10..200 ms),
// checking the stage's ordinary-file type and identity before every attempt.
// A successful sync is not repeated. Other hosts never wait here.
[[nodiscard]] PublishResult publishFile(const std::filesystem::path& source,
    const std::filesystem::path& target, PublishHooks hooks = {},
    PublishPolicy policy = PublishPolicy::VerifiedCopyFallback) noexcept;
struct ReplaceHooks {
    std::error_code (*syncFile)(const std::filesystem::path&) = nullptr;
    std::error_code (*rename)(const std::filesystem::path&, const std::filesystem::path&) = nullptr;
    std::error_code (*syncDirectory)(const std::filesystem::path&) = nullptr;
    // Runs under the destination's exclusive lease, before any publication.
    std::function<std::error_code(const std::filesystem::path&)> validateTarget;
    bool (*retryable)(const std::error_code&) = nullptr;
    void (*retryWait)(unsigned milliseconds) = nullptr;
    // Optional existing-file binding. When set, a missing or different target
    // fails under the exclusive lease. Expected-absent callers instead use
    // publishFile(AtomicOnly). This is not an OS compare-and-swap rename.
    std::optional<FileIdentity> expectedTarget{};
    // Optional settings revision check immediately before a portable rename.
    std::function<std::error_code(const std::filesystem::path&)> validateFallback;
};
enum class ReplacePolicy { LeaseOnly, PortableSettings, PortableBundle };
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
    const std::filesystem::path& target, ReplaceHooks hooks = {},
    ReplacePolicy policy = ReplacePolicy::LeaseOnly) noexcept;
}
