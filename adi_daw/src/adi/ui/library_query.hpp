// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include "adi/device_presets.hpp"
#include "adi/ui/library_listing.hpp"
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <map>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace adi {
struct BuiltinInfo;
namespace ui {
struct PluginEntry;

struct LibraryPathHash {
    std::size_t operator()(const std::filesystem::path &path) const noexcept {
        return std::filesystem::hash_value(path);
    }
};

// Compile the text once per edit. Each word must occur as a substring of a searchable field.
class LibraryQuery {
  public:
    explicit LibraryQuery(std::string_view text);
    [[nodiscard]] bool empty() const noexcept { return words_.empty(); }
    [[nodiscard]] bool matches(std::string_view name, std::string_view other = {}) const;

  private:
    friend class BrowserLibrarySnapshot;
    [[nodiscard]] bool matchesFolded(std::string_view name, std::string_view other = {}) const;
    std::vector<std::string> words_;
};

// The library worker publishes the listing and its search index together. Callers can share a
// const snapshot, and pointers returned by the index are valid while the snapshot is retained.
class BrowserLibrarySnapshot {
  public:
    struct Folder {
        std::filesystem::path path; // relative to this Place's root
        std::string name;
        std::vector<std::size_t> children;
        std::vector<std::size_t> files;
    };
    explicit BrowserLibrarySnapshot(LibraryListing);
    [[nodiscard]] const LibraryListing &listing() const noexcept { return listing_; }
    // The Place itself is at index 0. The snapshot owns the folders and entries they index.
    [[nodiscard]] std::span<const Folder> folders(const std::string &volume) const noexcept;
    enum class FileFilter { All, Samples };
    // A null volume searches all Places. A named volume includes its descendant files.
    void appendFiles(const LibraryQuery &, const std::string *volume, std::vector<const LibraryFile *> &results,
                     FileFilter = FileFilter::All) const;
    void appendSamples(const LibraryQuery &query, const std::string *volume,
                       std::vector<const LibraryFile *> &results) const {
        appendFiles(query, volume, results, FileFilter::Samples);
    }
    // The first matching file at this path, in listing order.
    [[nodiscard]] const LibraryFile *matchingFileAtPath(const std::filesystem::path &, const LibraryQuery &) const;

  private:
    using FileIndices = std::vector<std::size_t>;
    struct PathEntry {
        std::size_t hash, index;
    };
    struct PlaceFiles {
        FileIndices ordered;
        std::vector<bool> contains;
        std::vector<Folder> folders;
    };
    LibraryListing listing_;
    std::vector<std::string> foldedNames_;
    std::vector<PathEntry> paths_;
    std::unordered_map<std::uint32_t, FileIndices> postings_;
    std::unordered_map<std::string, PlaceFiles> places_;
};

struct LibraryQuerySources {
    const BrowserLibrarySnapshot *files = nullptr;
    std::span<const BuiltinInfo> builtins;
    std::span<const PluginEntry> plugins;
    const std::map<std::string, std::vector<preset::Entry>> *presets = nullptr;
    const BrowserLibrarySnapshot *projectSamples = nullptr;
};

// Typed matches in source order. Results borrow their source entries, and views may order them.
struct LibraryQueryResults {
    std::vector<const LibraryFile *> files;
    std::vector<const BuiltinInfo *> builtins;
    std::vector<const PluginEntry *> plugins;
    std::vector<const preset::Entry *> presets;
};

// Run the same search for either surface. Reusing the result keeps the vectors' storage.
void queryLibrary(const LibraryQuery &, const LibraryQuerySources &, LibraryQueryResults &);
} // namespace ui
} // namespace adi
