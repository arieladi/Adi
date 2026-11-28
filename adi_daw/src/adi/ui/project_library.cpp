// SPDX-License-Identifier: AGPL-3.0-or-later
#include "project_library.hpp"
#include "adi/store.hpp"
#include <SQLiteCpp/SQLiteCpp.h>
#include <algorithm>
#include <set>
#include <system_error>

namespace adi::ui {
std::shared_ptr<const BrowserLibrarySnapshot> ProjectLibrary::update(const Store &store) {
    auto source = appliedSource(store);
    const auto sequence = source.sequence();
    const bool sameProject = snapshot_ && source_.sameConnection(source);
    if (sameProject && sequence_ == sequence)
        return snapshot_;

    try {
        namespace fs = std::filesystem;
        decltype(assets_) assets;
        SQLite::Statement query(store.db(), "SELECT rel_path, orig_name FROM media_files "
                                           "WHERE embedded = 0 AND rel_path IS NOT NULL ORDER BY id");
        while (query.executeStep()) {
            const auto path = media::pathFromUtf8(query.getColumn(0).getString()).lexically_normal();
            if (!path.empty() && !path.has_root_path() && *path.begin() != ".." &&
                libraryFileKind(path) == LibraryFile::Kind::Audio)
                assets.emplace_back(path, query.getColumn(1).getString());
        }
        if (!sameProject || assets != assets_ || !snapshot_->listing().error.empty()) {
            LibraryListing listing;
            const auto folder = fs::canonical(media::projectFolder(store));
            std::set<fs::path> seen;
            for (const auto &[relative, original] : assets) {
                std::error_code error;
                const auto path = fs::canonical(folder / relative, error);
                const auto local = path.lexically_relative(folder);
                // Resolve aliases before checking ownership. An external symlink is a linked sample.
                if (error || local.empty() || *local.begin() == ".." || !fs::is_regular_file(path, error) ||
                    !seen.insert(path).second)
                    continue;
                listing.files.push_back({original.empty() ? media::pathUtf8(path.filename()) : original, path, {}});
                readLibraryFileFacts(listing.files.back());
            }
            sortLibraryFiles(listing.files);
            snapshot_ = std::make_shared<const BrowserLibrarySnapshot>(std::move(listing));
            assets_ = std::move(assets);
        }
    } catch (const std::exception &error) {
        LibraryListing listing;
        listing.error = std::string("The current project's audio could not be read: ") + error.what();
        snapshot_ = std::make_shared<const BrowserLibrarySnapshot>(std::move(listing));
    }
    source_ = std::move(source);
    sequence_ = sequence;
    return snapshot_;
}
} // namespace adi::ui
