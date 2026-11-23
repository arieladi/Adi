// SPDX-License-Identifier: AGPL-3.0-or-later
//
// CO-01: in-memory fake object store with atomic put-if-absent, lexical key
// listing, seeded network delays, and transient drops (including dropped ACKs
// after a write succeeds). Thread-safe for TSan verification.

#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <mutex>
#include <optional>
#include <random>
#include <string>
#include <string_view>
#include <vector>

namespace adi::collab {

enum class PutStatus {
    Created = 0,
    AlreadyExists,
    Dropped,
};

enum class GetStatus {
    Ok = 0,
    NotFound,
    Dropped,
};

struct GetResult {
    GetStatus status = GetStatus::NotFound;
    std::vector<std::byte> bytes;
};

struct ListResult {
    bool ok = false;  ///< false when simulated drop occurred
    std::vector<std::string> keys;
};

/// Deterministic fault-injection parameters for FakeObjectStore.
struct FaultConfig {
    std::uint64_t seed = 1;
    double putDropRate = 0.0;       ///< Drop before write
    double putAckDropRate = 0.0;    ///< Write succeeds, response dropped
    double getDropRate = 0.0;
    double listDropRate = 0.0;
    std::uint64_t maxVisibilityDelayTicks = 0;
};

/// Thread-safe in-memory object store supporting atomic put-if-absent,
/// visibility delays, and transient drops.
class FakeObjectStore {
public:
    explicit FakeObjectStore(FaultConfig faults = {});

    void setFaults(FaultConfig faults);

    /// Advance the simulated clock by `steps` ticks so delayed writes become visible.
    void tick(std::uint64_t steps = 1U);

    /// Clear all pending visibility delays and disable drops (network healed).
    void healNetwork();

    /// Atomic put-if-absent (simulates S3 If-None-Match: *).
    [[nodiscard]] PutStatus putIfAbsent(std::string_view key, std::vector<std::byte> bytes);

    /// Fetch an object if present and visible at the current tick.
    [[nodiscard]] GetResult get(std::string_view key);

    /// List visible keys starting with `prefix` in strict lexical order.
    [[nodiscard]] ListResult listKeys(std::string_view prefix = {});

    /// Total committed objects (ignoring visibility delay), for test assertions.
    [[nodiscard]] std::size_t committedCount() const;

private:
    struct Entry {
        std::vector<std::byte> bytes;
        std::uint64_t visibleAtTick = 0;
    };

    mutable std::mutex mutex_;
    FaultConfig faults_;
    std::mt19937_64 rng_;
    std::uint64_t currentTick_ = 0;
    std::map<std::string, Entry, std::less<>> objects_;
};

}  // namespace adi::collab
