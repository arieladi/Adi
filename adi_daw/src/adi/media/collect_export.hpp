// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <filesystem>
#include <string>
namespace adi { class Store; }
namespace adi::media {
struct FileResult { bool ok = false; std::string error; };
// Maintenance, not a schema upgrade or an undoable embedding operation.
// One SQL transaction covers all extracted rows. Normal failures remove only
// newly published files. A crash may leave harmless unreferenced files.
FileResult extractMedia(Store&);
// Snapshot includes committed WAL data. The source is opened read-only;
// all relocation/extraction affects the copy. Existing ZIPs are never replaced.
FileResult collectExport(const std::filesystem::path& input,
                         const std::filesystem::path& output);
} // namespace adi::media
