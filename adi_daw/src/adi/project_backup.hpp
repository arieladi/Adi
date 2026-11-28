// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "adi/store.hpp"

#include <cstdint>
#include <optional>

namespace adi::backup {

struct Job {
    StoreFileSave file;
    std::filesystem::path path;
    std::filesystem::path project;
};

struct Result {
    StoreFileResult file;
    std::filesystem::path path;
    std::vector<ProjectBackup> expired;
};

[[nodiscard]] std::filesystem::path directoryFor(const std::filesystem::path& project);
[[nodiscard]] bool isBackup(Store&);

// Pin a local WAL snapshot on the owning thread, then write on a worker.
// On synchronized rollback-journal volumes use Store::writeProjectBackup,
// which takes only a short source read lock per copy step.
[[nodiscard]] StoreFileResult prepare(Store&, std::int64_t utcMicros, Job&);
[[nodiscard]] Result write(Job&, std::stop_token = {});
// The owned backups past the ten kept, newest first by stored time (ADR-0213 d4). A backup stamped
// `writtenUtc` (the one just written) is never among them, and counts as one of the ten: a clock set
// back gives it the earliest stamp, and it must not be binned the moment it is written.
[[nodiscard]] std::vector<ProjectBackup> expiredBackups(const std::filesystem::path& project,
                                                        std::optional<std::int64_t> writtenUtc = std::nullopt);

// Restore to a new path. The backup and the original project remain intact.
[[nodiscard]] StoreFileResult recover(const std::filesystem::path& backup, const std::filesystem::path& destination,
                                      std::stop_token = {});

}  // namespace adi::backup
