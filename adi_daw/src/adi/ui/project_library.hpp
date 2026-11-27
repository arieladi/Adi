// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include "adi/applied_batches.hpp"
#include "adi/ui/library_query.hpp"
#include <memory>
#include <utility>

namespace adi::ui {

// Owned by the application. Current Project follows committed, local audio in the
// open song's media catalog, independently of shared library scans.
class ProjectLibrary {
  public:
    [[nodiscard]] std::shared_ptr<const BrowserLibrarySnapshot> update(const Store &);
    // The media catalog changed without a commit: Collect All and Save moves every file's place in one
    // write of its own, which does not advance the sequence `update` watches. The next `update` reads
    // the catalog and the folder again.
    void invalidate() noexcept { snapshot_.reset(); }

  private:
    AppliedSource source_;
    std::uint64_t sequence_ = 0;
    std::vector<std::pair<std::filesystem::path, std::string>> assets_;
    std::shared_ptr<const BrowserLibrarySnapshot> snapshot_;
};
} // namespace adi::ui
