// SPDX-License-Identifier: GPL-3.0-or-later
//
// The plugin capabilities registry -- ADR-0134 d7, ADR-0145 (F-12), ADR-0149.
//
// What this machine has learned about plugins, shared by the suite
// (appdata::pluginRegistryFile). Today one thing: the expression route a USER
// chose for a plugin ID, "MPE over MIDI for this synth", remembered across
// sessions and projects.
//
// THE REGISTRY PROPOSES; THE PROJECT DECIDES. It is read once, when a device is
// inserted, and its answer goes into the insert's payload as `route` -- so it
// becomes the project's row (ADR-0146) and travels with the file. A session
// never asks the registry again: a project must play the same on a machine
// whose registry says otherwise (ADR-0134 d7).
//
// Not the project format: its own file, application id and version. Several
// applications of the suite may hold it open at once, so it runs in WAL mode
// with a busy timeout.
#pragma once

#include "adi/engine/mpe_output.hpp"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>

namespace SQLite { class Database; }

namespace adi {

class PluginRegistry {
public:
    /// 'ADIP'. Distinct from the project's `application_id`, so neither file
    /// can be mistaken for the other.
    static constexpr std::int32_t kApplicationId = 0x41444950;
    static constexpr int kVersion = 1;

    /// Open, creating the file and its folder when missing. Null with `error`
    /// set when it cannot: the caller then proposes no defaults, which is Auto.
    /// A file with another application id, or a newer version, is refused and
    /// left alone.
    static std::unique_ptr<PluginRegistry> open(const std::filesystem::path& file,
                                                std::string& error);
    ~PluginRegistry();
    PluginRegistry(const PluginRegistry&) = delete;
    PluginRegistry& operator=(const PluginRegistry&) = delete;

    /// The route the user chose for this plugin, if they chose one. `format`
    /// and `uid` are `plugin_refs`' columns.
    [[nodiscard]] std::optional<engine::RouteChoice> route(const std::string& format,
                                                           const std::string& uid) const;

    /// Remember a user's choice. `Auto` forgets it. `nowUtc` is the caller's
    /// clock: nothing here reads one.
    bool remember(const std::string& format, const std::string& uid, engine::RouteChoice route,
                  std::int64_t nowUtc, std::string& error);

private:
    explicit PluginRegistry(std::unique_ptr<SQLite::Database> db);
    std::unique_ptr<SQLite::Database> db_;
};

}  // namespace adi
