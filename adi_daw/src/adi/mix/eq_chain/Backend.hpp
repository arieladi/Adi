// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include "Core.hpp"
#include <cstdint>
#include <memory>

namespace adi::mix::eq_chain {
enum class OwnerMode : std::uint8_t { Threaded, Manual };
enum class BackendFailure : std::uint8_t { None, WorkerStart, OwnerService };
struct BackendStatus {
    bool prepared = false, workerStarted = false, ownerBusy = false;
    BackendFailure failure = BackendFailure::None;
    std::uint64_t requestedWork = 0, completedWork = 0, ownerPasses = 0, workerStarts = 0;
    // Actual outer-loop visits and predicate-wait returns, not OS wake counts.
    bool ownerParked = false;
    std::uint64_t ownerLoopVisits = 0, ownerWaitReturns = 0;
};
// Framework-free processing adapter, not a device-route or parameter provider.
// The caller supplies an actual key bus and its logical width. It must reset /
// reprepare after changing key-source identity or a connected key layout.
//
// One serialized audio publisher calls process. One Main/owner caller calls
// pumpMainThread and stopped prepare/release/destruction. The lazy worker is the
// sole serviceConfiguration caller; Manual mode makes that owner pass explicit
// for deterministic component drivers. Main never reads Core::pendingPlan.
class Backend final {
  public:
    explicit Backend(OwnerMode = OwnerMode::Threaded);
    ~Backend();
    Backend(const Backend &) = delete;
    Backend &operator=(const Backend &) = delete;

    // Processing must be stopped. Existing owner work is joined first. Failed
    // prepare leaves the adapter explicitly unprepared and may be retried.
    void prepare(PrepareSpec, const Settings &);
    void release() noexcept;
    bool process(float *const *main, const float *const *key, std::int32_t keyChannels,
                 std::int32_t frames, const Settings &, std::uint64_t physicalBlock,
                 std::int32_t configuredTotalLatency) noexcept;
    bool process(double *const *main, const double *const *key, std::int32_t keyChannels,
                 std::int32_t frames, const Settings &, std::uint64_t physicalBlock,
                 std::int32_t configuredTotalLatency) noexcept;

    // Main relay with no synchronous configuration completion. It reads atomic
    // metadata, then may create a thread or briefly lock its wake mutex to
    // schedule at most one outstanding owner pass. Audio does neither. No idle
    // worker or polling is started by preparation or ordinary processing.
    void pumpMainThread() noexcept;
    [[nodiscard]] std::int32_t advertisedLatency() const noexcept;
    [[nodiscard]] BackendStatus status() const noexcept;
    // Audio-owner diagnostics, or after stopping; not a concurrent UI snapshot.
    [[nodiscard]] double learnedThreshold(std::size_t band) const noexcept;
    [[nodiscard]] double learnedKnee(std::size_t band) const noexcept;

    // Manual mode only, on its single owner thread. Identical service/failure
    // path to the real worker. A service failure parks further work until a
    // stopped prepare; the last admitted audio plan remains usable meanwhile.
    bool serviceOwnerOnce() noexcept;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace adi::mix::eq_chain
