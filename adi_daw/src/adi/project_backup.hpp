// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "adi/media/publish_file.hpp"
#include "adi/store.hpp"

namespace adi::backup {

struct Job {
    StoreFileSave file;
    std::filesystem::path path;
};

struct Expired {
    std::filesystem::path path;
    media::FileRevision revision;
};

struct Result {
    StoreFileResult file;
    std::filesystem::path path;
    std::vector<Expired> expired;
};

// Hold the returned lease while handing this exact revision to the system trash.
[[nodiscard]] std::shared_ptr<media::ProjectFileLease> leaseForTrash(const Expired&);

[[nodiscard]] std::filesystem::path directoryFor(const std::filesystem::path& project);
[[nodiscard]] bool isBackup(Store&);

// Pin the current committed image on the owning thread, then write on a worker.
[[nodiscard]] StoreFileResult prepare(Store&, std::int64_t utcMicros, Job&);
[[nodiscard]] Result write(Job&, std::stop_token = {});

// Restore to a new path. The backup and the original project remain intact.
[[nodiscard]] StoreFileResult recover(const std::filesystem::path& backup, const std::filesystem::path& destination,
                                      std::stop_token = {});

}  // namespace adi::backup
