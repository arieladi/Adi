// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "adi/media/media_ops.hpp"
#include "adi/store.hpp"
#include <SQLiteCpp/SQLiteCpp.h>
#include <limits>
#include <optional>
#include <vector>
#include <stop_token>

namespace adi::ui {
struct ProjectMediaFile {
    std::int64_t id = 0;
    std::string name, hash;
    std::filesystem::path relative, hint, location;
    std::optional<std::int64_t> bytes;
    std::string problem;
};

struct ProjectMediaInventory {
    std::filesystem::path folder;
    std::vector<ProjectMediaFile> files;
};

// Capture the project's media pool on the database owner. Disk inspection below
// can take much longer and uses only the captured rows on a worker.
inline ProjectMediaInventory captureProjectMedia(Store &store) {
    ProjectMediaInventory out{media::projectFolder(store), {}};
    SQLite::Statement read(store.db(), "SELECT id,orig_name,hash_blake3,rel_path,abs_path_hint,size_bytes "
                                       "FROM media_files WHERE embedded=0 ORDER BY orig_name COLLATE NOCASE,id");
    while (read.executeStep()) {
        auto &file = out.files.emplace_back();
        file.id = read.getColumn(0).getInt64();
        file.name = read.getColumn(1).getString();
        file.hash = read.getColumn(2).getString();
        if (!read.isColumnNull(3)) file.relative = media::pathFromUtf8(read.getColumn(3).getString());
        if (!read.isColumnNull(4)) file.hint = media::pathFromUtf8(read.getColumn(4).getString());
        if (!read.isColumnNull(5)) file.bytes = read.getColumn(5).getInt64();
    }
    return out;
}

inline void inspectProjectMedia(ProjectMediaInventory &inventory, std::stop_token stop = {}) {
    for (auto &file : inventory.files) {
        if (stop.stop_requested()) return;
        auto found = media::resolveMedia(inventory.folder, file.relative, file.hint, file.name, file.hash, file.problem, stop);
        if (!found.empty()) {
            file.location = found.lexically_normal();
            std::error_code error;
            const auto bytes = std::filesystem::file_size(found, error);
            if (!error && bytes <= static_cast<std::uintmax_t>(std::numeric_limits<std::int64_t>::max()))
                file.bytes = static_cast<std::int64_t>(bytes);
        } else {
            if (file.problem.empty()) file.problem = "File not found";
            if (!file.relative.empty()) file.location = (inventory.folder / file.relative).lexically_normal();
            else if (!file.hint.empty()) file.location = file.hint;
        }
    }
}
} // namespace adi::ui
