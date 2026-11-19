// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include "adi/library/index.hpp"
#include "adi/library/volume.hpp"
#include "adi/media/media_ops.hpp"
#include <algorithm>
#include <atomic>
#include <cctype>
#include <filesystem>
#include <map>
#include <set>
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
    std::string volumeId;      // what tells two places apart: their names may match
    bool userLibrary = false;  // the User Library's, which Live's own Places label shows
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
// The extensions the engine's decoders read (adi/audio/decode.hpp's formats).
inline bool isAudioFile(const std::filesystem::path &path) {
    auto ext = media::pathUtf8(path.extension());
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    for (const char *audio : {".wav", ".wave", ".aif", ".aiff", ".aifc", ".flac", ".mp3", ".ogg", ".m4a", ".aac"})
        if (ext == audio)
            return true;
    return false;
}
// Registers and scans each folder, then lists the index's present files under them, each file once
// (a folder inside another lists through the outer one). A folder that cannot be registered or
// scanned is left out and named in `error`; the others list. `stop`, when it reads true between two
// folders, ends the scan early: what was scanned so far is listed. An index that throws is an error,
// never an exception out of here.
inline LibraryListing scanLibrary(library::Index &index, const std::vector<LibraryFolder> &folders,
                                  library::VolumeIdentityProvider &volumes, const std::atomic<bool> *stop = nullptr) {
    LibraryListing out;
    try {
        std::map<std::string, std::size_t> byVolume;
        for (const auto &folder : folders) {
            if (stop != nullptr && stop->load())
                break;
            if (folder.root.empty())
                continue;
            library::Volume volume;
            std::string why;
            if (!volumes.identify(folder.root, volume, why)) {
                if (out.error.empty())
                    out.error = folder.name + " could not be read: " + why;
                continue;
            }
            if (byVolume.contains(volume.id))
                continue; // the same folder named twice
            if (const auto scanned = index.scan(volume); !scanned.ok) {
                if (out.error.empty())
                    out.error = folder.name + " could not be scanned: " + scanned.error;
                continue;
            }
            byVolume[volume.id] = out.places.size();
            out.places.push_back({folder.name, volume.root, volume.id, folder.userLibrary});
        }
        std::set<std::filesystem::path> seen;
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
