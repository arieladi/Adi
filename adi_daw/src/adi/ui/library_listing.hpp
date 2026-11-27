// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include "adi/library/index.hpp"
#include "adi/library/volume.hpp"
#include "adi/media/media_ops.hpp"
#include <algorithm>
#include <cctype>
#include <filesystem>
#include <map>
#include <set>
#include <stop_token>
#include <string>
#include <utility>
#include <vector>
namespace adi::ui {
// The browser's Samples and Places (Live 12 manual 4.1, 4.6, 4.7; MR 3.1; worklist D1, N15's index
// part): the folders the settings name (`library.userLibrary`, `library.contentFolders`) are
// registered as volumes of the library index (#102) and scanned into it; what the index then holds
// under them is listed. Places are those folders, by name; Samples are their audio files. The index
// records every file; the listing keeps the ones whose extension is an audio format the engine
// decodes. A scan blocks for its whole walk, so the app runs this on a worker of its own.
struct LibraryPlace {
    std::string name;
    std::filesystem::path root;
    std::string volumeId;      // the indexed volume ID, or a listing-only identity for an unindexed Place
    bool userLibrary = false;  // the User Library's, which Live's own Places label shows
    std::string problem{};
    std::vector<std::filesystem::path> directories{}; // relative paths, including empty folders
};
struct LibraryFile {
    std::string name;            // the file's name, as Live's list shows it
    std::filesystem::path path;  // absolute
    std::string volumeId;        // the place it is under
};
// One folder the settings name: what the browser calls it, where it is, and whether it is the User
// Library.
struct LibraryFolder {
    std::string name;
    std::filesystem::path root;
    bool userLibrary = false;
};
struct LibraryListing {
    std::vector<LibraryPlace> places;
    std::vector<LibraryFile> samples; // by name, case-insensitive
    std::string error;                // the first folder that could not be read, and why
};
// Provision a new default User Library once. An existing library belongs to
// the user, who may move or remove any of its initial folders.
inline bool provisionDefaultUserLibrary(const std::filesystem::path& root, std::string& error) {
    namespace fs = std::filesystem;
    error.clear();
    std::error_code status;
    if (!fs::create_directories(root, status)) {
        if (!status) return true;
        error = "The User Library could not be created: " + status.message();
        return false;
    }
    std::vector<fs::path> created;
    for (const auto* name : {"Clips", "Defaults", "Grooves", "Presets", "Samples"}) {
        const auto folder = root / name;
        if (fs::create_directory(folder, status))
            created.push_back(folder);
        if (status) {
            error = std::string("The User Library's ") + name + " folder could not be created: " + status.message();
            // Leave anything another owner put here. Only undo empty directories we created.
            const auto removeDirectory = [&status](const fs::path& path) {
                if (fs::is_directory(fs::symlink_status(path, status)))
                    (void)fs::remove(path, status);
            };
            for (auto it = created.rbegin(); it != created.rend(); ++it)
                removeDirectory(*it);
            removeDirectory(root);
            return false;
        }
    }
    return true;
}
// The extensions the engine's decoders read (adi/audio/decode.hpp's formats).
inline bool isAudioFile(const std::filesystem::path &path) {
    auto ext = media::pathUtf8(path.extension());
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    for (const char *audio : {".wav", ".wave", ".aif", ".aiff", ".aifc", ".flac", ".mp3", ".ogg", ".m4a", ".aac"})
        if (ext == audio)
            return true;
    return false;
}
// A place without a usable portable marker can still be read on the library worker. Its key only
// scopes this listing. It is never registered as a volume or used to reuse the index's cached hashes.
inline std::vector<LibraryFile> readUnindexedPlace(LibraryPlace &place, std::stop_token stop) {
    namespace fs = std::filesystem;
    std::vector<LibraryFile> files;
    std::vector<fs::path> directories;
    for (fs::recursive_directory_iterator it(place.root), end; it != end && !stop.stop_requested(); ++it) {
        const auto status = it->symlink_status();
        if (fs::is_directory(status))
            directories.push_back(it->path().lexically_relative(place.root));
        else if (fs::is_regular_file(status) && isAudioFile(it->path()))
            files.push_back({media::pathUtf8(it->path().filename()), it->path(), place.volumeId});
    }
    place.directories = std::move(directories);
    return files;
}

// Lists each audio path once, including read-only Places. A missing folder still has a Places row
// so it can be removed from Settings there. A stopped scan leaves the index's last complete scan intact.
inline LibraryListing scanLibrary(library::Index &index, const std::vector<LibraryFolder> &folders,
                                  library::VolumeIdentityProvider &volumes, std::stop_token stop = {}) {
    namespace fs = std::filesystem;
    LibraryListing out;
    try {
        std::map<std::string, std::size_t> byVolume;
        std::set<fs::path> seen, roots;
        for (const auto &folder : folders) {
            if (stop.stop_requested())
                break;
            if (folder.root.empty())
                continue;
            library::Volume volume;
            std::string why;
            const bool identified = volumes.identify(folder.root, volume, why);
            auto root = volume.root;
            if (!identified) {
                std::error_code status;
                const auto absolute = fs::absolute(folder.root, status);
                if (!status) root = fs::weakly_canonical(absolute, status);
                if (status) root = folder.root.lexically_normal();
            }
            if (!roots.insert(root).second) continue;

            LibraryPlace place{folder.name, root, "folder:" + media::pathUtf8(root), folder.userLibrary};
            if (identified && !byVolume.contains(volume.id)) {
                auto scanned = index.scan(volume, {}, stop);
                if (scanned.cancelled) break;
                if (scanned.ok) {
                    place.volumeId = volume.id;
                    place.directories = std::move(scanned.directories);
                    byVolume.emplace(volume.id, out.places.size());
                } else {
                    place.problem = folder.name + " could not be scanned: " + scanned.error;
                }
            } else {
                // Another root can carry a copy of the same marker after syncing. Keep the marker
                // intact and list the second root without claiming its portable identity in the index.
                try {
                    auto files = readUnindexedPlace(place, stop);
                    if (stop.stop_requested()) break;
                    for (auto &file : files)
                        if (seen.insert(file.path).second)
                            out.samples.push_back(std::move(file));
                } catch (const fs::filesystem_error &error) {
                    place.problem = folder.name + " could not be read: " + error.code().message();
                }
            }
            if (out.error.empty() && !place.problem.empty()) out.error = place.problem;
            out.places.push_back(std::move(place));
        }
        for (const auto &entry : index.entries()) {
            const auto place = byVolume.find(entry.volumeId);
            if (!entry.present || place == byVolume.end())
                continue;
            const auto &where = out.places[place->second];
            auto path = (where.root / media::pathFromUtf8(entry.relativePath)).lexically_normal();
            if (!isAudioFile(path) || !seen.insert(path).second)
                continue;
            out.samples.push_back({media::pathUtf8(path.filename()), std::move(path), where.volumeId});
        }
    } catch (const std::exception &e) {
        out.error = std::string("The library index could not be read: ") + e.what();
    }
    std::stable_sort(out.samples.begin(), out.samples.end(), [](const LibraryFile &a, const LibraryFile &b) {
        return std::lexicographical_compare(a.name.begin(), a.name.end(), b.name.begin(), b.name.end(),
                                            [](unsigned char x, unsigned char y) { return std::tolower(x) < std::tolower(y); });
    });
    return out;
}
} // namespace adi::ui
