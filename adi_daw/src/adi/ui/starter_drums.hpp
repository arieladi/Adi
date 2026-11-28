// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include <filesystem>
#include <initializer_list>
#include <system_error>

namespace adi::ui {
// A development build and the Windows installer place the kit beside ADI.
// System installs and app bundles give the executable their relative data path.
inline std::filesystem::path starterDrumsDirectory(const std::filesystem::path &executable,
                                                   const std::filesystem::path &installedRelative) {
    if (!executable.is_absolute())
        return {};
    const auto bin = executable.parent_path();
    for (const auto &candidate : {bin / "starter-drums", (bin / installedRelative).lexically_normal()}) {
        std::error_code error;
        if (std::filesystem::is_directory(candidate / "Hits", error) &&
            std::filesystem::is_directory(candidate / "Loops", error))
            return candidate;
    }
    return {};
}
} // namespace adi::ui
