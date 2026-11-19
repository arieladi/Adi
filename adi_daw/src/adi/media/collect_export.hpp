// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstddef>
#include <cstdint>
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
// One SQL transaction covers all extracted rows. Windows rollback removes only
// the expected published file by an exclusive DELETE handle. Changed, busy or
// unsupported paths remain and are named in warning; other hosts conservatively
// retain public files rather than race a pathname unlink. A crash may leave
// harmless unreferenced files.
FileResult extractMedia(Store&);
enum class ArchivePhase { Snapshot, CollectMedia, WriteArchive, Publish };
struct ArchiveProgress {
    ArchivePhase phase;
    std::size_t entriesWritten = 0;
    // WriteArchive only: current entry's uncompressed source bytes read. These
    // reset for each entry; empty entry means a phase/completed-entry boundary.
    // Reading the last byte is not ZIP finalization or publication.
    std::uint64_t entryBytesRead = 0, entryBytesTotal = 0;
    std::string entry{};
};
struct ArchiveControl {
    std::stop_token stop;
    std::function<void(ArchiveProgress)> progress;
};
// Snapshot includes committed WAL data. The source is opened read-only;
// all relocation/extraction affects the copy. Existing ZIPs are never replaced.
// Progress runs on the worker. During ZIP entries, stop is checked before/after
// each <=64 KiB input read. Snapshot/media collection remain phase-only;
// finalization observes stop at the next phase before atomic publication starts.
FileResult collectExport(const std::filesystem::path& input,
                         const std::filesystem::path& output,
                         const ArchiveControl& control = {});
} // namespace adi::media
