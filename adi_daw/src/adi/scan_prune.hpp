// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace adi::scan {
struct ScanPruneRoot { std::string format; std::filesystem::path path; };
struct ScanPruneBinding; // opaque receipt made only by bindScanPruneRoots
enum class ScanPrunePhase { AfterWalk, BeforeDelete };
struct ScanPruneRequest {
    std::vector<ScanPruneRoot> roots;
    // True only after BOTH format scans finish, including all their candidates.
    // Ordinary candidate rejection is complete; helper/owner/partial failure is not.
    bool discoveryComplete = false;
    const std::atomic<bool>* cancel = nullptr;
    // Optional synchronous diagnostic seam. May throw to refuse the whole pass;
    // never re-enter this operation or mutate its registry connection. The final
    // path is supplied at BeforeDelete; empty at AfterWalk. No callback runs RT.
    void (*checkpoint)(ScanPrunePhase, const std::filesystem::path&, void*) = nullptr;
    void* context = nullptr;
    std::shared_ptr<const ScanPruneBinding> binding;
};
enum class ScanPruneStatus { Complete, Incomplete, Cancelled, Invalid, Failed, CallbackRefused };
struct ScanPruneResult {
    ScanPruneStatus status = ScanPruneStatus::Incomplete;
    std::uint64_t cacheRowsRemoved = 0, failureRowsRemoved = 0, automaticBlocksRemoved = 0;
    std::uint64_t rootsWalked = 0, missingRootsSkipped = 0, unverifiableKeysSkipped = 0;
    bool legacySkipped = false; // validated v1/v2 or early success-only v3; no migration/write
    std::string error;
    std::string unreadablePath; // any permission-denied evidence leaves pruning incomplete
    explicit operator bool() const noexcept { return status == ScanPruneStatus::Complete; }
};
// Bind before either format scan. Records canonical roots/confirmed absence,
// without opening a registry or walking descendants. Failure clears old binding.
[[nodiscard]] ScanPruneResult bindScanPruneRoots(ScanPruneRequest&);
// Owner-worker post-discovery maintenance, once across the two formats. Strict
// second walk because legacy candidate enumeration suppresses directory errors.
// Requires a successful pre-scan binding and the same roots/bindings at cleanup.
// Missing optional roots are excluded; permission/partial/cancel aborts all writes.
// Manual and pending blocks survive. Only owned missing keys and obsolete
// spellings of canonical modules observed in the walk are eligible. Aliases
// outside the walked roots remain unowned.
// No PluginRegistry::open/schema changes: no admissible roots => no registry I/O;
// no missing keys => read-only inventory, no writer. At most 1024 roots, 262144
// walked entries and 131072 registry keys/16 MiB path text. SQL deletion is atomic;
// filesystem absence is rechecked, not a snapshot or cross-process namespace CAS.
[[nodiscard]] ScanPruneResult pruneMissingScans(const std::filesystem::path& registryPath,
                                               const ScanPruneRequest&);
} // namespace adi::scan
