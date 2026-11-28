// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <stop_token>
#include <string>
#include <string_view>
namespace adi { class Store; }
namespace adi::media {
namespace detail {
std::string portableFilename(std::string name, std::string_view fallback);
}
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
// Successful extraction can return a path-qualified warning for media whose
// publication completed but directory durability could not be confirmed.
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
    // CollectMedia only: current external source bytes successfully read and
    // written to its private copy, before flush/hash/publication. Reset per
    // file. Empty mediaFile means a phase boundary; this is not a ZIP count.
    std::uint64_t mediaBytesCopied = 0, mediaBytesTotal = 0;
    std::string mediaFile{};
};
struct ArchiveControl {
    std::stop_token stop;
    // Synchronous worker observer. Must not close, mutate or reenter the
    // operation's private Store; copied progress owns its names.
    std::function<void(ArchiveProgress)> progress;
};
// Snapshot includes committed WAL data. Undo is optional. The source is opened read-only;
// all relocation/extraction affects the copy. Existing ZIPs are never replaced.
// Progress runs on the worker. External-media copy and ZIP input use <=64 KiB
// chunks with stop checks before/after. Snapshot, embedded extraction and
// hashing/verification remain phase-only. Finalization observes stop at the
// next phase before atomic publication starts.
FileResult collectExport(const std::filesystem::path& input,
                         const std::filesystem::path& output,
                         const ArchiveControl& control = {},
                         bool keepUndoHistory = false);
} // namespace adi::media
