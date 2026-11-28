// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include <cstddef>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace adi::control::detail {

inline constexpr std::size_t kConsentFileMaxBytes = 256 * 1024;

/// An app-owned, machine-local file. An unavailable home yields an empty path.
[[nodiscard]] std::filesystem::path consentFilePath();

/// A missing file is empty. Invalid, unreadable, or oversized storage yields nullopt.
[[nodiscard]] std::optional<std::string> readConsentFile(const std::filesystem::path&,
                                                         std::size_t maximum);

/// Publish a complete ledger atomically in its owner-only directory.
[[nodiscard]] bool writeConsentFile(const std::filesystem::path&, std::string_view);

/// Run a read-modify-write under the stable sibling lock across processes.
/// Acquiring a busy or unsafe lock fails closed and does not call the action.
[[nodiscard]] bool withConsentFileLock(const std::filesystem::path&, const std::function<void()>&);

enum class PrivateFileStatus { Missing, Private, Unavailable };

/// Check that an ordinary file is owner-only within its private directory.
/// Does not read the file or create its parents.
[[nodiscard]] PrivateFileStatus privateFileStatus(const std::filesystem::path&);

struct PrivateFileLock;
struct PrivateFileLockAttempt {
    std::shared_ptr<PrivateFileLock> lock;
    bool busy = false;
};

/// Acquire the stable sibling lock, optionally creating it. A busy lock is
/// distinct from a missing, unreadable, or unsafe lock file.
[[nodiscard]] PrivateFileLockAttempt lockPrivateFile(const std::filesystem::path&, bool create);

/// Remove an ordinary owner-only file while its stable lock is held.
[[nodiscard]] bool removePrivateFile(const std::filesystem::path&);

}  // namespace adi::control::detail
