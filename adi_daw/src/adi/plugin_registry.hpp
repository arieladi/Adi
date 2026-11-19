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

enum class ScanDisposition { Succeeded, Abandoned, Failed, Rejected };
enum class ScanFailureKind { Rejected, Crashed, TimedOut };
// Refresh retries metadata, never re-enables a manual/pending/two-strike block.
enum class ScanCacheMode { Reuse, Refresh };
struct ScanFailure {
    ScanFailureKind kind = ScanFailureKind::Rejected;
    std::string reason;
    unsigned strikes = 0; // rejected=0; crash/timeout=1 or 2
};
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
struct ScanAdmission {
    std::optional<ScanTicket> ticket;
    std::optional<ScanFailure> cachedFailure;
};
// Untrusted success bytes read without taking a durable scan ticket. The owner
// must validate the packet, then confirm it against a fresh registry snapshot.
class ScanSuccessRead {
public:
    [[nodiscard]] const std::string& packet() const noexcept { return packet_; }
private:
    friend class PluginRegistry;
    std::string format_, path_, stamp_, packet_;
    std::int64_t updatedUtc_ = 0;
    std::weak_ptr<const int> owner_;
};

class PluginRegistry {
public:
    /// 'ADIP'. Distinct from the project's `application_id`, so neither file
    /// can be mistaken for the other.
    static constexpr std::int32_t kApplicationId = 0x41444950;
    static constexpr int kVersion = 3;
    static constexpr std::size_t kMaximumScanBlocks = 65536;
    static constexpr std::size_t kMaximumScanPacket = 8U * 1024U * 1024U;

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
    /// Reliable stamped admission: changed stamps retire only typed automatic
    /// failures, never pending/manual/legacy blocks. Unchanged deterministic
    /// failures or two crashes/timeouts return cachedFailure without a ticket.
    /// A first crash/timeout permits one more explicit probe; no retry loop.
    /// Refresh bypasses matching rejected results only. Existing cache state
    /// stays until token-checked completion; abandonment preserves that state.
    std::optional<ScanAdmission> beginStampedScan(const std::string& format,
        const std::string& path, const std::string& stamp, std::int64_t nowUtc, std::string& error,
        ScanCacheMode mode = ScanCacheMode::Reuse);
    /// Atomically retire stale success metadata and finish this ticket with a
    /// typed failure. Only crash/timeout consume strikes; rejected metadata is
    /// immediately reusable as a negative result. Returns the resulting count.
    std::optional<unsigned> finishFailedScan(const ScanTicket&, const std::string& stamp,
        ScanFailureKind, const std::string& reason, std::int64_t nowUtc, std::string& error);
    /// Succeeded/Abandoned remove only this pending attempt (empty reason).
    /// Failed retains it as a legacy block with a nonempty diagnostic.
    /// Rejected removes pending and all stale cache state (empty reason), for
    /// a deterministic rejection without a reliable stamp; never quarantines.
    /// Use Abandoned
    /// for caller cancellation/helper launch failures, not plug-in crashes.
    /// False includes stale/foreign ticket: discard its metadata, never retry
    /// completion against a newer ticket. Existing quarantine remains intact.
    bool finishScan(const ScanTicket&, ScanDisposition, const std::string& reason,
                    std::int64_t nowUtc, std::string& error);
    /// Read-only warm-success path. Any pending/manual/automatic block or typed
    /// failure makes it ineligible; normal ticket admission owns those cases.
    /// A miss is nullopt with empty error. Bytes remain bounded and UNTRUSTED.
    std::optional<ScanSuccessRead> readScanSuccess(const std::string& format,
        const std::string& path, const std::string& stamp, std::string& error) const;
    /// Fresh single-statement read: same stamp, exact packet and update value,
    /// with no blockers. False (including a race) never authorizes publication.
    /// Neither call retains a SQL snapshot across the owner's format validator.
    bool confirmScanSuccess(const ScanSuccessRead&, std::string& error) const;
    /// Point observation for a rejected warm read: a concurrently installed
    /// block/pending ticket takes precedence over candidate/helper instability.
    std::optional<bool> hasScanBlock(const std::string& format, const std::string& path,
                                    std::string& error) const;
    /// Success-cache lookup under the still-pending admission ticket. A miss
    /// is nullopt with an empty error. Packet bytes are bounded but UNTRUSTED:
    /// the scanner must decode and format-validate them as a fresh child reply.
    /// Stamps are 64 lowercase hex digits; this layer does no filesystem I/O.
    std::optional<std::string> cachedScan(const ScanTicket&, const std::string& stamp,
                                          std::string& error) const;
    /// Replace the complete module packet and finish the exact pending ticket
    /// in ONE transaction. A stale/foreign ticket cannot publish a cache row.
    /// There is no successful-but-uncached capacity path: a write failure
    /// rolls back and is reported, so warm scans never silently reprobe.
    bool finishCachedScan(const ScanTicket&, const std::string& stamp,
                          const std::string& packet, std::int64_t nowUtc, std::string& error);
    /// Explicit owner/user controls; no automatic retry, expiry or rescan.
    /// Re-enable also removes any cached success, so the next probe is fresh.
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
