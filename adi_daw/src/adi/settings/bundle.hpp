// SPDX-License-Identifier: GPL-3.0-or-later
//
// Settings bundles: R-05, ADR-0127 d5, ADR-0152.
//
// One .zip, no project data:
//   manifest.json   {"kind": "adi-settings-bundle", "schema": 1, "app": "ADI DAW"}
//   settings.json   the application's settings, App scope only
//   presets/N.json  the presets that went with them
//
// Paths never travel as paths. A folder is stored as a ROLE plus the part
// below it -- "role:user library/Drums" -- and resolved again on import against
// the importing machine's own folders, so a bundle from Windows lands right on
// macOS. A path under no role is left out and reported: an absolute path in a
// bundle is a path to someone else's disk.

#pragma once

#include "adi/settings/store.hpp"

#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace adi::settings {

/// Role name -> folder on this machine: "user library", "content folder 1",
/// "content folder 2", ...
using Roles = std::map<std::string, std::filesystem::path>;

/// The roles this machine's settings define.
[[nodiscard]] Roles rolesOf(const AppSettings&);

/// "role:<name>/<rest>" for a path under a role (the longest match wins), or
/// nothing when no role holds it.
[[nodiscard]] std::optional<std::string> toRole(const std::filesystem::path&, const Roles&);
/// The path a "role:" string names on this machine, or nothing if this
/// machine has no such role. A string that is not a role is refused too.
[[nodiscard]] std::optional<std::filesystem::path> fromRole(const std::string&, const Roles&);

struct BundleReport {
    bool ok = false;
    std::string why;
    std::vector<std::string> leftOut;    // "key: path" that no role holds (export)
    std::vector<std::string> unresolved; // "key: role:..." this machine lacks (import)
    std::vector<std::string> skipped;    // keys refused on import
    std::vector<Preset> presets;         // import: the presets it carried
    int applied = 0;                     // import: settings changed
};

/// Writes the bundle. Only App-scope settings the registry knows go in: a key
/// a newer build wrote stays in settings.json and does not travel, because
/// this build cannot tell whether it holds a path.
BundleReport exportBundle(const AppSettings&, const std::vector<Preset>&,
                          const std::filesystem::path& zipFile);

/// Reads a bundle into `target`, resolving roles against `target`'s own
/// folders. A bundle from another application is refused (ADR-0145 d10).
/// Does not save.
BundleReport importBundle(const std::filesystem::path& zipFile, AppSettings& target,
                          const ChangeContext& who);

/// True if any string anywhere in `v` is an absolute path, on any platform's
/// rules ("/x", "C:\\x", "\\\\server\\x"). What export guarantees never ships.
[[nodiscard]] bool containsAbsolutePath(const Value& v);

}  // namespace adi::settings
