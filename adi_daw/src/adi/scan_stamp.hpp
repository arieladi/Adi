// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <filesystem>
#include <optional>
#include <string>

namespace adi::scan {
/// Owner-only stat fingerprint, not a content hash of the plug-in. The input
/// is an existing canonical file/bundle. A bundle stamps native modules and
/// moduleinfo.json, never its directory's modification time. No plug-in code
/// is loaded. A deterministic bounded walk resolves directory links and detects
/// cycles without traversing directory links outside the bundle.
/// Unrecognized/empty layouts still reach the scanner child; their
/// ordinary files/listing are stamped instead. Candidate aliases are resolved
/// by Scanner before this call. A bound/I/O failure produces no weak stamp;
/// Scanner reports cacheWarning and can still discover through its child.
std::optional<std::string> candidateStamp(const std::string& format,
    const std::filesystem::path& canonical, const std::string& helperBuildHash,
    std::string& error);
} // namespace adi::scan
