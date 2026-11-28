// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "publish_file.hpp"
namespace adi { class Store; }
namespace adi::media {
struct ArchiveControl;
struct FileResult;
namespace detail {
// Shared production implementations with per-call diagnostic hooks. These do
// not change the public Store API or install process-global test state.
FileResult collectMedia(Store&, PublishHooks);
FileResult extractMedia(Store&, PublishHooks);
FileResult collectExport(const std::filesystem::path&, const std::filesystem::path&,
                        const ArchiveControl&, PublishHooks mediaPublication, bool keepUndoHistory = false);
// Called before the enclosing SQL commit, after recording rollback ownership.
void appendMediaPublicationWarning(std::string&, const std::filesystem::path&, const PublishResult&);
// Archive worker only: an open, private, writable snapshot whose owner does
// not close/mutate/reenter its Store from a progress observer. Ordinary Collect
// All and Save retains the public Store API and its unobserved copy path.
FileResult collectSnapshotMedia(Store&, const ArchiveControl&, PublishHooks = {});
}
}
