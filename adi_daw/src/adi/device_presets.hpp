// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace adi::preset {
struct Identity {
    std::string format, uid;
    std::optional<std::int64_t> shellId;
    bool operator==(const Identity &) const = default;
};

struct Preset {
    Identity device;
    std::string name, deviceName;
    std::map<std::string, double> parameters;
    std::map<std::string, std::vector<std::byte>> state;
    // The plug-in panel's control IDs in order. An empty list is an explicitly empty panel.
    std::optional<std::vector<std::string>> panel;
};

struct Entry {
    std::string deviceKey, name;
    std::filesystem::path path;
};

// A stable, filesystem-safe key for an authored device identity.
[[nodiscard]] std::string deviceKey(const Identity &);
// Enumerate User Library presets without opening their payloads.
[[nodiscard]] std::vector<Entry> list(const std::filesystem::path &library);
[[nodiscard]] Preset read(const std::filesystem::path &file);
// Publish a new file, selecting a numbered name if the preferred name exists.
[[nodiscard]] std::filesystem::path write(const std::filesystem::path &library, const Preset &);
// A User Library has at most one default for an exact device identity. It is separate from the
// Browser's list of named presets. A missing default returns nullopt, and an invalid file throws.
[[nodiscard]] std::optional<Preset> readDefault(const std::filesystem::path &library, const Identity &);
// Atomically create or replace the device's default.
[[nodiscard]] std::filesystem::path writeDefault(const std::filesystem::path &library, const Preset &);
} // namespace adi::preset
