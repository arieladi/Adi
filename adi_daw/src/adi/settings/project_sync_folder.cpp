// SPDX-License-Identifier: GPL-3.0-or-later
#include "adi/settings/project_sync_folder.hpp"

#include <cstdlib>
#include <system_error>
#include <utility>

#ifdef _WIN32
#include <windows.h>
#include <knownfolders.h>
#include <shlobj.h>

#include <memory>
#endif

namespace adi::settings {

namespace fs = std::filesystem;

std::vector<SyncRoot> knownProjectSyncRoots() {
    std::vector<SyncRoot> roots;
#if defined(_WIN32) || defined(__APPLE__)
    const auto addIfPresent = [&roots](SyncProvider provider, fs::path path) {
        std::error_code error;
        if (path.is_absolute() && fs::is_directory(path, error)) {
            roots.push_back({provider, std::move(path)});
        }
    };
#endif
#ifdef _WIN32
    PWSTR directory = nullptr;
    const auto status = SHGetKnownFolderPath(FOLDERID_SkyDrive, KF_FLAG_DONT_VERIFY, nullptr,
                                             &directory);
    const std::unique_ptr<wchar_t, decltype(&CoTaskMemFree)> known{directory, &CoTaskMemFree};
    if (SUCCEEDED(status) && known) {
        addIfPresent(SyncProvider::OneDrive, known.get());
    }
    for (const auto* key : {L"OneDrive", L"OneDriveCommercial", L"OneDriveConsumer"}) {
        const DWORD size = GetEnvironmentVariableW(key, nullptr, 0);
        if (size == 0) {
            continue;
        }
        std::wstring value(size, L'\0');
        const DWORD length = GetEnvironmentVariableW(key, value.data(), size);
        if (length > 0 && length < size) {
            value.resize(length);
            addIfPresent(SyncProvider::OneDrive, value);
        }
    }
#elif defined(__APPLE__)
    if (const auto* home = std::getenv("HOME"); home && *home) {
        addIfPresent(SyncProvider::ICloudDrive,
                     fs::path(home) / "Library/Mobile Documents/com~apple~CloudDocs");
    }
#endif
    return roots;
}

std::optional<SyncProvider> projectSyncProvider(const fs::path& projectFile,
                                                std::span<const SyncRoot> roots) {
    if (projectFile.filename().empty() || roots.empty()) {
        return std::nullopt;
    }
    std::error_code error;
    const auto absolute = fs::absolute(projectFile, error);
    if (error) {
        return std::nullopt;
    }
    const auto resolved = fs::weakly_canonical(absolute, error);
    if (error) {
        return std::nullopt;
    }
    auto directory = resolved.parent_path();
    if (error || !fs::is_directory(directory, error)) {
        return std::nullopt;
    }

    while (!directory.empty()) {
        for (const auto& root : roots) {
            if (root.directory.is_absolute() && fs::equivalent(directory, root.directory, error)) {
                return root.provider;
            }
        }
        const auto parent = directory.parent_path();
        if (parent == directory) {
            break;
        }
        directory = parent;
    }
    return std::nullopt;
}

} // namespace adi::settings
