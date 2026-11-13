// SPDX-License-Identifier: AGPL-3.0-or-later
#include "Backend.hpp"
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <thread>

namespace adi::mix::eq_chain {
static_assert(std::atomic<std::int32_t>::is_always_lock_free);
static_assert(std::atomic<std::uint64_t>::is_always_lock_free);
static_assert(std::atomic<bool>::is_always_lock_free);

struct Backend::Impl {
    explicit Impl(OwnerMode value) : mode(value) {}
    Core core;
    const OwnerMode mode;
    std::atomic<bool> prepared{false}, started{false}, busy{false};
    std::atomic<BackendFailure> failure{BackendFailure::None};
    std::atomic<std::int32_t> audioLatency{0}, advertised{0};
    std::atomic<std::uint64_t> passes{0}, starts{0};
    // These primitives are touched only by Main and the owner, never by audio.
    std::mutex wakeMutex;
    std::condition_variable wake;
    std::thread owner;
    bool stop = false, job = false;

    void stopOwner() noexcept {
        if (owner.joinable()) {
            {
                const std::lock_guard lock(wakeMutex);
                stop = true;
            }
            wake.notify_one();
            owner.join();
        }
        stop = false;
        job = false;
        busy.store(false, std::memory_order_release);
        started.store(false, std::memory_order_release);
    }
    bool workPending() const noexcept {
        return core.requestedWorkVersion() > core.completedWorkVersion();
    }
    bool service() noexcept {
        if (!prepared.load(std::memory_order_acquire) ||
            failure.load(std::memory_order_acquire) != BackendFailure::None || !workPending())
            return false;
        passes.fetch_add(1, std::memory_order_relaxed);
        try {
            return core.serviceConfiguration();
        } catch (...) {
            // Do not fabricate an acknowledgment or discard the active plan.
            // Owner resource failure requires an explicit stopped retry.
            failure.store(BackendFailure::OwnerService, std::memory_order_release);
            return false;
        }
    }
    void runOwner() noexcept {
        for (;;) {
            {
                std::unique_lock lock(wakeMutex);
                wake.wait(lock, [this] { return stop || job; });
                if (stop)
                    return;
                job = false;
            }
            static_cast<void>(service());
            busy.store(false, std::memory_order_release);
        }
    }
    void pump() noexcept {
        if (!prepared.load(std::memory_order_acquire))
            return;
        // Audio is the only running writer of this mirror. A late worker can
        // therefore never restore an older child sum after audio admission.
        advertised.store(audioLatency.load(std::memory_order_acquire), std::memory_order_release);
        if (mode != OwnerMode::Threaded ||
            failure.load(std::memory_order_acquire) != BackendFailure::None || !workPending())
            return;
        bool idle = false;
        if (!busy.compare_exchange_strong(idle, true, std::memory_order_acq_rel))
            return;
        try {
            if (!owner.joinable()) {
                owner = std::thread([this] { runOwner(); });
                starts.fetch_add(1, std::memory_order_relaxed);
                started.store(true, std::memory_order_release);
            }
            {
                const std::lock_guard lock(wakeMutex);
                job = true;
            }
            wake.notify_one();
        } catch (...) {
            failure.store(BackendFailure::WorkerStart, std::memory_order_release);
            busy.store(false, std::memory_order_release);
        }
    }
    template <class Sample>
    bool process(Sample *const *main, const Sample *const *key, std::int32_t keyChannels,
                 std::int32_t frames, const Settings &settings, std::uint64_t physicalBlock,
                 std::int32_t configured) noexcept {
        if (!prepared.load(std::memory_order_acquire))
            return false;
        const auto block = core.beginBlock(physicalBlock, configured, settings);
        const bool result = core.process(main, key, keyChannels, frames, settings, block);
        const auto plan = core.pendingPlan();
        audioLatency.store(plan.token ? plan.totalLatency : core.latencySamples(),
                           std::memory_order_release);
        return result;
    }
};

Backend::Backend(OwnerMode mode) : impl_(std::make_unique<Impl>(mode)) {}
Backend::~Backend() { release(); }
void Backend::prepare(PrepareSpec spec, const Settings &settings) {
    auto &i = *impl_;
    i.stopOwner();
    i.prepared.store(false, std::memory_order_release);
    i.failure.store(BackendFailure::None, std::memory_order_release);
    i.audioLatency.store(0, std::memory_order_release);
    i.advertised.store(0, std::memory_order_release);
    i.passes.store(0, std::memory_order_relaxed);
    i.starts.store(0, std::memory_order_relaxed);
    if (i.core.isPrepared())
        i.core.reprepareStopped(spec, settings);
    else
        i.core.prepare(spec, settings);
    const auto latency = i.core.latencySamples();
    i.audioLatency.store(latency, std::memory_order_release);
    i.advertised.store(latency, std::memory_order_release);
    i.prepared.store(true, std::memory_order_release);
}
void Backend::release() noexcept {
    auto &i = *impl_;
    i.prepared.store(false, std::memory_order_release);
    i.stopOwner();
}
bool Backend::process(float *const *main, const float *const *key, std::int32_t keyChannels,
                      std::int32_t frames, const Settings &settings, std::uint64_t physicalBlock,
                      std::int32_t configured) noexcept {
    return impl_->process(main, key, keyChannels, frames, settings, physicalBlock, configured);
}
bool Backend::process(double *const *main, const double *const *key, std::int32_t keyChannels,
                      std::int32_t frames, const Settings &settings, std::uint64_t physicalBlock,
                      std::int32_t configured) noexcept {
    return impl_->process(main, key, keyChannels, frames, settings, physicalBlock, configured);
}
void Backend::pumpMainThread() noexcept { impl_->pump(); }
std::int32_t Backend::advertisedLatency() const noexcept {
    return impl_->advertised.load(std::memory_order_acquire);
}
BackendStatus Backend::status() const noexcept {
    const auto &i = *impl_;
    return {i.prepared.load(std::memory_order_acquire), i.started.load(std::memory_order_acquire),
            i.busy.load(std::memory_order_acquire), i.failure.load(std::memory_order_acquire),
            i.core.requestedWorkVersion(), i.core.completedWorkVersion(),
            i.passes.load(std::memory_order_relaxed), i.starts.load(std::memory_order_relaxed)};
}
double Backend::learnedThreshold(std::size_t band) const noexcept {
    return impl_->core.learnedThreshold(band);
}
double Backend::learnedKnee(std::size_t band) const noexcept {
    return impl_->core.learnedKnee(band);
}
bool Backend::serviceOwnerOnce() noexcept {
    if (impl_->mode != OwnerMode::Manual)
        return false;
    return impl_->service();
}
} // namespace adi::mix::eq_chain
