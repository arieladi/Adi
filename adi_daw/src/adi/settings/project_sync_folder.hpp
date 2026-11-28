// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <filesystem>
#include <optional>
#include <span>
#include <vector>

namespace adi::settings {

enum class SyncProvider { OneDrive, ICloudDrive };

struct SyncRoot {
    SyncProvider provider;
    std::filesystem::path directory;
};

// Inspect local metadata only. Use on the project-open worker.
[[nodiscard]] std::vector<SyncRoot> knownProjectSyncRoots();

// Match the actual location of the project folder. The project file may be
// absent (Save As preview); this never opens its contents.
[[nodiscard]] std::optional<SyncProvider> projectSyncProvider(
    const std::filesystem::path& projectFile, std::span<const SyncRoot> roots);

} // namespace adi::settings
