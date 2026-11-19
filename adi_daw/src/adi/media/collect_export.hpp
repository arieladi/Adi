// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstddef>
#include <filesystem>
#include <functional>
#include <stop_token>
#include <string>
namespace adi { class Store; }
namespace adi::media {
struct FileResult {
    bool ok = false;
    std::string error;
    std::string warning{};
    bool cancelled = false;
};
// Maintenance, not a schema upgrade or an undoable embedding operation.
// One SQL transaction covers all extracted rows. Normal failures remove only
// newly published files. A crash may leave harmless unreferenced files.
FileResult extractMedia(Store&);
enum class ArchivePhase { Snapshot, CollectMedia, WriteArchive, Publish };
struct ArchiveProgress {
    ArchivePhase phase;
    std::size_t entriesWritten = 0;
};
struct ArchiveControl {
    std::stop_token stop;
    std::function<void(ArchiveProgress)> progress;
};
// Snapshot includes committed WAL data. The source is opened read-only;
// all relocation/extraction affects the copy. Existing ZIPs are never replaced.
// Progress runs on the worker. Cancellation is observed between phases and
// completed ZIP entries, until the final atomic publication starts.
FileResult collectExport(const std::filesystem::path& input,
                         const std::filesystem::path& output,
                         const ArchiveControl& control = {});
} // namespace adi::media
