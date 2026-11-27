// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include "adi/media/media_ops.hpp"
#include "adi/settings/store.hpp"
#include <algorithm>
#include <filesystem>
#include <span>

namespace adi::ui {
inline constexpr auto kContentFolders = "library.contentFolders";
enum class ContentFolderEdit { Add, Remove };

inline settings::Value editContentFolders(settings::Value folders, std::span<const std::filesystem::path> paths,
                                          ContentFolderEdit edit) {
    namespace fs = std::filesystem;
    for (const auto &path : paths) {
        std::error_code status;
        if (edit == ContentFolderEdit::Add && !fs::is_directory(path, status)) continue;
        const auto matches = [&path](const settings::Value &stored) {
            if (!stored.is_string() || stored.get_ref<const std::string &>().empty()) return false;
            const auto configured = media::pathFromUtf8(stored.get_ref<const std::string &>());
            if (configured.lexically_normal() == path.lexically_normal()) return true;
            std::error_code error;
            const bool equivalent = fs::equivalent(configured, path, error);
            if (equivalent || !error) return equivalent;
            const auto absolute = fs::absolute(configured, error);
            return !error && absolute.lexically_normal() == path.lexically_normal();
        };
        if (edit == ContentFolderEdit::Remove)
            folders.erase(std::remove_if(folders.begin(), folders.end(), matches), folders.end());
        else if (std::none_of(folders.begin(), folders.end(), matches)) {
            // Written as the system writes it, since Settings shows this text: on Windows with
            // backslashes, the same text the folder chooser hands over.
            const auto native = fs::path(path).make_preferred().u8string();
            folders.push_back(std::string(reinterpret_cast<const char *>(native.data()), native.size()));
        }
    }
    return folders;
}
} // namespace adi::ui
