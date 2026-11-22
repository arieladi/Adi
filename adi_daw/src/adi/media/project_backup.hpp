// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "adi/media/collect_export.hpp"

#include <chrono>

namespace adi::media {

struct ProjectBackupResult {
    FileResult archive;
    std::filesystem::path destination;
};

// Run on the Collect and Export worker. The caller chooses the destination
// directory each time and supplies the local wall time shown in the file name.
[[nodiscard]] ProjectBackupResult backupProject(
    const std::filesystem::path& project, const std::filesystem::path& directory,
    std::chrono::local_seconds localTime, const ArchiveControl& control = {});

} // namespace adi::media
