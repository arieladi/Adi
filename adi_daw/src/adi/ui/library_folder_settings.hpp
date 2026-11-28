// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include "adi/media/media_ops.hpp"
#include "adi/settings/store.hpp"
#include <algorithm>
#include <filesystem>
#include <span>
#include <string>

namespace adi::ui {
inline constexpr auto kContentFolders = "library.contentFolders";
enum class ContentFolderEdit { Add, Remove };

// Whether a stored folder entry names this path, written either way.
inline bool contentFolderMatches(const settings::Value &stored, const std::filesystem::path &path) {
    namespace fs = std::filesystem;
    if (!stored.is_string() || stored.get_ref<const std::string &>().empty()) return false;
    const auto configured = media::pathFromUtf8(stored.get_ref<const std::string &>());
    if (configured.lexically_normal() == path.lexically_normal()) return true;
    std::error_code error;
    const bool equivalent = fs::equivalent(configured, path, error);
    if (equivalent || !error) return equivalent;
    const auto absolute = fs::absolute(configured, error);
    return !error && absolute.lexically_normal() == path.lexically_normal();
}
// Written as the system writes it, since Settings shows this text: on Windows with backslashes, the
// same text the folder chooser hands over.
inline std::string contentFolderText(const std::filesystem::path &path) {
    const auto native = std::filesystem::path(path).make_preferred().u8string();
    return std::string(reinterpret_cast<const char *>(native.data()), native.size());
}

inline settings::Value editContentFolders(settings::Value folders, std::span<const std::filesystem::path> paths,
                                          ContentFolderEdit edit) {
    namespace fs = std::filesystem;
    for (const auto &path : paths) {
        std::error_code status;
        if (edit == ContentFolderEdit::Add && !fs::is_directory(path, status)) continue;
        const auto matches = [&path](const settings::Value &stored) { return contentFolderMatches(stored, path); };
        if (edit == ContentFolderEdit::Remove)
            folders.erase(std::remove_if(folders.begin(), folders.end(), matches), folders.end());
        else if (std::none_of(folders.begin(), folders.end(), matches))
            folders.push_back(contentFolderText(path));
    }
    return folders;
}
// SC-142: a Place whose folder has gone is located again. Its entry takes the new folder where it
// stood, so the Places keep their order; a folder already listed only drops the gone one. A new path
// that is not a folder changes nothing.
inline settings::Value relocateContentFolder(settings::Value folders, const std::filesystem::path &gone,
                                             const std::filesystem::path &found) {
    std::error_code status;
    if (!folders.is_array() || !std::filesystem::is_directory(found, status))
        return folders;
    const auto matchesFound = [&found](const settings::Value &stored) { return contentFolderMatches(stored, found); };
    if (std::any_of(folders.begin(), folders.end(), matchesFound))
        return editContentFolders(std::move(folders), std::span{&gone, 1}, ContentFolderEdit::Remove);
    for (auto &stored : folders)
        if (contentFolderMatches(stored, gone)) {
            stored = contentFolderText(found);
            return folders;
        }
    return editContentFolders(std::move(folders), std::span{&found, 1}, ContentFolderEdit::Add);
}
} // namespace adi::ui
