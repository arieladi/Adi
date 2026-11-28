// SPDX-License-Identifier: AGPL-3.0-or-later
#include "library_query.hpp"
#include "adi/builtin_catalog.hpp"
#include "adi/ui/plugin_listing.hpp"
#include <utf8proc.h>
#include <algorithm>
#include <cstdlib>
#include <limits>
#include <memory>
#include <utility>

namespace adi::ui {
namespace {
constexpr std::size_t kMaxGramBytes = 3;
std::uint32_t addGramByte(std::uint32_t key, char byte) {
    return (key << 8U) | static_cast<std::uint32_t>(static_cast<unsigned char>(byte));
}
std::string folded(std::string_view text) {
    if (text.empty())
        return {};
    if (text.size() <= static_cast<std::size_t>(std::numeric_limits<utf8proc_ssize_t>::max())) {
        utf8proc_uint8_t *data = nullptr;
        constexpr auto options = static_cast<utf8proc_option_t>(UTF8PROC_STABLE | UTF8PROC_COMPOSE | UTF8PROC_CASEFOLD);
        const auto length = utf8proc_map(reinterpret_cast<const utf8proc_uint8_t *>(text.data()),
                                         static_cast<utf8proc_ssize_t>(text.size()), &data, options);
        const std::unique_ptr<utf8proc_uint8_t, decltype(&std::free)> owned(data, &std::free);
        if (length >= 0)
            return {reinterpret_cast<const char *>(data), static_cast<std::size_t>(length)};
    }
    // Keep bytes from names supplied by plug-ins even if their text is malformed UTF-8.
    std::string bytes(text);
    for (auto &byte : bytes)
        if (byte >= 'A' && byte <= 'Z')
            byte = static_cast<char>(byte + ('a' - 'A'));
    return bytes;
}
bool isWhitespace(utf8proc_int32_t codepoint) {
    if (codepoint < 128)
        return codepoint == ' ' || (codepoint >= '\t' && codepoint <= '\r');
    const auto category = utf8proc_category(codepoint);
    return category == UTF8PROC_CATEGORY_ZS || category == UTF8PROC_CATEGORY_ZL ||
           category == UTF8PROC_CATEGORY_ZP || codepoint == 0x85;
}
} // namespace

LibraryQuery::LibraryQuery(std::string_view text) {
    const auto foldedText = folded(text);
    std::size_t start = 0;
    for (std::size_t pos = 0; pos < foldedText.size();) {
        utf8proc_int32_t codepoint = -1;
        const auto remaining = std::min<std::size_t>(foldedText.size() - pos, 4);
        const auto length = utf8proc_iterate(reinterpret_cast<const utf8proc_uint8_t *>(foldedText.data() + pos),
                                             static_cast<utf8proc_ssize_t>(remaining), &codepoint);
        const auto width = static_cast<std::size_t>(std::max<utf8proc_ssize_t>(length, 1));
        if (isWhitespace(codepoint)) {
            if (start < pos)
                words_.emplace_back(foldedText, start, pos - start);
            start = pos + width;
        }
        pos += width;
    }
    if (start < foldedText.size())
        words_.emplace_back(foldedText, start);
}

bool LibraryQuery::matches(std::string_view name, std::string_view other) const {
    if (empty())
        return true;
    return matchesFolded(folded(name), folded(other));
}

bool LibraryQuery::matchesFolded(std::string_view name, std::string_view other) const {
    return std::ranges::all_of(words_, [name, other](const auto &word) {
        return name.find(word) != std::string_view::npos || other.find(word) != std::string_view::npos;
    });
}

BrowserLibrarySnapshot::BrowserLibrarySnapshot(LibraryListing listing) : listing_(std::move(listing)) {
    foldedNames_.reserve(listing_.files.size());
    paths_.reserve(listing_.files.size());
    for (std::size_t i = 0; i < listing_.files.size(); ++i) {
        const auto &file = listing_.files[i];
        paths_.push_back({std::filesystem::hash_value(file.path), i});
        const auto &name = foldedNames_.emplace_back(folded(file.name));
        for (std::size_t start = 0; start < name.size(); ++start) {
            // A leading 1 distinguishes grams of different widths, even for non-ASCII names.
            std::uint32_t key = 1;
            const auto length = std::min(kMaxGramBytes, name.size() - start);
            for (std::size_t offset = 0; offset < length; ++offset) {
                key = addGramByte(key, name[start + offset]);
                auto &posting = postings_[key];
                if (posting.empty() || posting.back() != i)
                    posting.push_back(i);
            }
        }
    }
    std::sort(paths_.begin(), paths_.end(), [](const PathEntry &a, const PathEntry &b) {
        return a.hash < b.hash || (a.hash == b.hash && a.index < b.index);
    });
    // A file deduplicated by the library index may belong to several overlapping Places.
    // Keep their path-based membership here, so changing the query never compares paths.
    for (const auto &place : listing_.places) {
        if (place.volumeId.empty())
            continue;
        auto &scope = places_[place.volumeId];
        scope.ordered.clear();
        scope.contains.assign(listing_.files.size(), false);
        scope.folders.clear();
        if (place.root.empty())
            continue;
        scope.folders.emplace_back();
        std::unordered_map<std::filesystem::path, std::size_t, LibraryPathHash> folders{{{}, 0}};
        std::vector<std::string> folderSortKeys(1);
        const auto folderIndex = [&](auto &&self, const std::filesystem::path &path) -> std::size_t {
            if (const auto found = folders.find(path); found != folders.end())
                return found->second;
            const auto parent = self(self, path.parent_path());
            const auto index = scope.folders.size();
            auto name = media::pathUtf8(path.filename());
            folders.emplace(path, index);
            folderSortKeys.push_back(folded(name));
            scope.folders.push_back({path, std::move(name), {}, {}});
            scope.folders[parent].children.push_back(index);
            return index;
        };
        for (const auto &directory : place.directories)
            if (!directory.empty() && !directory.has_root_path() && directory != "." && *directory.begin() != "..")
                folderIndex(folderIndex, directory);
        for (std::size_t i = 0; i < listing_.files.size(); ++i) {
            const auto relative = listing_.files[i].path.lexically_relative(place.root);
            if (!relative.empty() && relative != "." && *relative.begin() != "..") {
                scope.ordered.push_back(i);
                scope.contains[i] = true;
                const auto parent = folderIndex(folderIndex, relative.parent_path());
                scope.folders[parent].files.push_back(i);
            }
        }
        for (auto &folder : scope.folders)
            std::sort(folder.children.begin(), folder.children.end(), [&](std::size_t a, std::size_t b) {
                if (folderSortKeys[a] != folderSortKeys[b])
                    return folderSortKeys[a] < folderSortKeys[b];
                return scope.folders[a].name < scope.folders[b].name;
            });
    }
}

std::span<const BrowserLibrarySnapshot::Folder> BrowserLibrarySnapshot::folders(const std::string &volume) const noexcept {
    if (const auto place = places_.find(volume); place != places_.end())
        return place->second.folders;
    return {};
}

void BrowserLibrarySnapshot::appendFiles(const LibraryQuery &query, const std::string *volume,
                                         std::vector<const LibraryFile *> &results, FileFilter fileFilter) const {
    const PlaceFiles *scope = nullptr;
    if (volume != nullptr) {
        const auto found = places_.find(*volume);
        if (found == places_.end())
            return;
        scope = &found->second;
    }

    const auto accepts = [fileFilter](const LibraryFile &file) {
        return fileFilter == FileFilter::All || file.isSample();
    };
    if (query.empty()) {
        if (scope != nullptr) {
            results.reserve(results.size() + scope->ordered.size());
            for (const auto i : scope->ordered)
                if (accepts(listing_.files[i]))
                    results.push_back(&listing_.files[i]);
        } else {
            results.reserve(results.size() + listing_.files.size());
            for (const auto &file : listing_.files)
                if (accepts(file))
                    results.push_back(&file);
        }
        return;
    }

    const FileIndices *shortest = nullptr;
    for (const std::string_view word : query.words_) {
        const auto gramSize = std::min(kMaxGramBytes, word.size());
        for (std::size_t start = 0; start <= word.size() - gramSize; ++start) {
            std::uint32_t key = 1;
            for (const char byte : word.substr(start, gramSize))
                key = addGramByte(key, byte);
            const auto found = postings_.find(key);
            if (found == postings_.end())
                return;
            if (shortest == nullptr || found->second.size() < shortest->size())
                shortest = &found->second;
        }
    }
    // Every posting follows library order. The rarest gram across the words (or a smaller
    // Place) limits the names to check. A single gram alone already matches exactly.
    const bool scanPlace = scope != nullptr && scope->ordered.size() < shortest->size();
    const auto &candidates = scanPlace ? scope->ordered : *shortest;
    const auto *filter = scanPlace ? nullptr : scope;
    const std::string_view singleWord = query.words_.size() == 1 ? std::string_view(query.words_.front())
                                                                 : std::string_view{};
    const bool exact = !scanPlace && !singleWord.empty() && singleWord.size() <= kMaxGramBytes;
    for (const auto i : candidates) {
        const auto &file = listing_.files[i];
        if ((filter != nullptr && !filter->contains[i]) || !accepts(file))
            continue;
        if (exact || (singleWord.empty() ? query.matchesFolded(foldedNames_[i])
                                         : foldedNames_[i].find(singleWord) != std::string::npos))
            results.push_back(&file);
    }
}

const LibraryFile *BrowserLibrarySnapshot::matchingFileAtPath(const std::filesystem::path &path,
                                                              const LibraryQuery &query) const {
    const auto hash = std::filesystem::hash_value(path);
    for (auto candidate = std::ranges::lower_bound(paths_, hash, {}, &PathEntry::hash);
         candidate != paths_.end() && candidate->hash == hash; ++candidate) {
        const auto &file = listing_.files[candidate->index];
        if (file.path == path && query.matchesFolded(foldedNames_[candidate->index]))
            return &file;
    }
    return nullptr;
}

void queryLibrary(const LibraryQuery &query, const LibraryQuerySources &sources, LibraryQueryResults &results) {
    results.files.clear();
    results.builtins.clear();
    results.plugins.clear();
    results.presets.clear();
    if (sources.files != nullptr)
        sources.files->appendFiles(query, nullptr, results.files);
    if (sources.projectSamples != nullptr) {
        const auto sharedCount = results.files.size();
        sources.projectSamples->appendSamples(query, nullptr, results.files);
        if (sharedCount != 0 && sharedCount < results.files.size()) {
            // appendFiles emits shared matches in listing order. Leave that prefix intact until
            // every replacement position has been found, then prefer the project's display names.
            const auto shared = std::span(results.files).first(sharedCount);
            std::unordered_map<std::size_t, const LibraryFile *> replacements;
            auto tail = sharedCount;
            for (auto index = sharedCount; index < results.files.size(); ++index) {
                const auto *project = results.files[index];
                if (const auto *duplicate = sources.files->matchingFileAtPath(project->path, query)) {
                    const auto slot = std::ranges::lower_bound(shared, duplicate);
                    if (slot != shared.end() && *slot == duplicate) {
                        replacements.try_emplace(static_cast<std::size_t>(slot - shared.begin()), project);
                        continue;
                    }
                }
                results.files[tail++] = project;
            }
            for (const auto &[slot, project] : replacements)
                results.files[slot] = project;
            results.files.resize(tail);
        }
    }
    for (const auto &device : sources.builtins)
        if (query.matches(device.name))
            results.builtins.push_back(&device);
    for (const auto &plugin : sources.plugins)
        if (query.matches(plugin.name, plugin.vendor))
            results.plugins.push_back(&plugin);
    if (sources.presets != nullptr)
        for (const auto &[key, presets] : *sources.presets)
            for (const auto &preset : presets)
                if (query.matches(preset.name))
                    results.presets.push_back(&preset);
}
} // namespace adi::ui
