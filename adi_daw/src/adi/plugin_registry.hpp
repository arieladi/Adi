// SPDX-License-Identifier: GPL-3.0-or-later
//
// The plugin capabilities registry -- ADR-0134 d7, ADR-0145 (F-12), ADR-0149.
//
// What this machine has learned about plugins, shared by the suite
// (appdata::pluginRegistryFile): the expression route a USER
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
#include <vector>

namespace SQLite { class Database; }

namespace adi {

enum class ScanDisposition { Succeeded, Abandoned, Failed };
enum class ScanBlockState { Pending, Blocked };
struct ScanBlock {
    std::string format, path, reason;
    ScanBlockState state = ScanBlockState::Pending;
    std::int64_t updatedUtc = 0; // Unix microseconds, supplied by the owner
};

/// Opaque attempt identity, valid only on the still-open registry that began
/// it. An explicit block/re-enable/new attempt invalidates older completion.
class ScanTicket {
public:
    [[nodiscard]] const std::string& format() const noexcept { return format_; }
    [[nodiscard]] const std::string& path() const noexcept { return path_; }
private:
    friend class PluginRegistry;
    std::string format_, path_;
    std::int64_t id_ = 0;
    std::weak_ptr<const int> owner_;
};

class PluginRegistry {
public:
    /// 'ADIP'. Distinct from the project's `application_id`, so neither file
    /// can be mistaken for the other.
    static constexpr std::int32_t kApplicationId = 0x41444950;
    static constexpr int kVersion = 2;
    static constexpr std::size_t kMaximumScanBlocks = 65536;

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

    /// OWNER APIs, never callbacks; one owner at a time per connection. Scan
    /// times are explicit nonnegative Unix microseconds. Key is
    /// (clap|vst3, absolute UTF-8 path).
    /// The scanner must resolve aliases/canonical paths BEFORE calling these;
    /// the registry performs no filesystem lookup or case/Unicode guessing.
    /// Begin commits a pending quarantine BEFORE any child launch. Any existing
    /// pending/blocked key refuses: pending survives parent death/reopen and
    /// requires explicit re-enable. Never scan when this write fails.
    std::optional<ScanTicket> beginScan(const std::string& format, const std::string& path,
                                      std::int64_t nowUtc, std::string& error);
    /// Succeeded/Abandoned remove only this pending attempt (empty reason).
    /// Failed retains it as blocked with a nonempty diagnostic. Use Abandoned
    /// for caller cancellation/helper launch failures, not plug-in crashes.
    /// False includes stale/foreign ticket: discard its metadata, never retry
    /// completion against a newer ticket. Existing quarantine remains intact.
    bool finishScan(const ScanTicket&, ScanDisposition, const std::string& reason,
                    std::int64_t nowUtc, std::string& error);
    /// Explicit owner/user controls; no automatic retry, expiry or rescan.
    bool blockScan(const std::string& format, const std::string& path,
                   const std::string& reason, std::int64_t nowUtc, std::string& error);
    bool unblockScan(const std::string& format, const std::string& path, std::string& error);
    /// Whole sorted list (including pending), never a silently truncated list.
    /// 1..65536 rows and 16 MiB aggregate text; error/nullopt on any invalid row.
    std::optional<std::vector<ScanBlock>> scanBlocks(std::string& error,
                                                   std::size_t maximumRows = 4096) const;

private:
    explicit PluginRegistry(std::unique_ptr<SQLite::Database> db);
    std::unique_ptr<SQLite::Database> db_;
    std::shared_ptr<const int> scanOwner_ = std::make_shared<const int>(0);
};

}  // namespace adi
