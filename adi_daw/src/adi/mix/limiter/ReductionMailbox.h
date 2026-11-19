// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include "DspEngine.h"
#include <atomic>
#include <cstdint>
#include <memory>
#include <optional>

namespace adi::mix::limiter {

struct ReductionFrame {
    std::uint64_t renderBlock = 0, transportRevision = 0;
    // Device timeline, before downstream graph delay compensation.
    std::int64_t firstTimelineSample = 0;
    std::uint32_t observedFrames = 0;
    bool advancing = false;
    BlockReduction reduction;
};

// The device and its owner-side leases share the latest block summary. There is
// one serial callback publisher and one message-thread owner, with no queue to
// drain. Observers can coalesce successive snapshots of a split callback by ID.
class ReductionMailbox final {
public:
    // Owner-thread lease; observers never contend with the audio publisher.
    class Lease {
    public:
        Lease() = default;
        ~Lease();
        Lease(const Lease&) = delete;
        Lease& operator=(const Lease&) = delete;
        Lease(Lease&&) noexcept;
        Lease& operator=(Lease&&) noexcept;
        [[nodiscard]] explicit operator bool() const noexcept;
        [[nodiscard]] std::optional<ReductionFrame> read() const noexcept;
        void reset() noexcept;
    private:
        friend class ReductionMailbox;
        Lease(std::shared_ptr<ReductionMailbox>, std::uint64_t) noexcept;
        std::shared_ptr<ReductionMailbox> source_;
        std::uint64_t epoch_ = 0;
    };

    // Owner thread, including prepare/release and document source changes.
    void setPrepared(bool prepared) noexcept;
    void revokeObservers() noexcept;
    [[nodiscard]] static Lease observe(std::shared_ptr<ReductionMailbox>) noexcept;
    [[nodiscard]] bool observed() const noexcept { return observationEpoch() != 0; }

    // Callback. Snapshot demand before processing so a lease which appeared
    // during the callback cannot see measurements from before it enrolled.
    [[nodiscard]] std::uint64_t observationEpoch() const noexcept { return demand_.load(std::memory_order_relaxed); }
    void publishSegment(std::uint64_t epoch, const ReductionFrame&) noexcept;

private:
    void unobserve(std::uint64_t) noexcept;
    [[nodiscard]] std::optional<ReductionFrame> read(std::uint64_t) const noexcept;

    bool prepared_ = false;
    std::size_t observers_ = 0;
    std::uint64_t nextEpoch_ = 0;
    std::atomic<std::uint64_t> demand_{0};

    // The aggregation and serial are strictly audio-owned. The read surface
    // stays atomic even when an owner read races a subsequent segment write.
    std::uint64_t activeEpoch_ = 0, publishVersion_ = 0;
    ReductionFrame aggregate_;
    std::atomic<std::uint64_t> version_{0}, publishedEpoch_{0}, block_{0}, revision_{0}, payload_{0};
    std::atomic<std::int64_t> firstTimelineSample_{0};
    static_assert(std::atomic<std::uint64_t>::is_always_lock_free);
    static_assert(std::atomic<std::int64_t>::is_always_lock_free);
};

} // namespace adi::mix::limiter
