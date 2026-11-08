// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include <cstddef>
#include <cstdint>
#include <memory>
namespace adi::engine {
// Persistent, owner-created workers. start only posts nonblocking OS wakeups;
// finish spins/yields on completion flags without taking a callback mutex or
// entering a semaphore wait. Jobs inherit the caller's floating-point
// environment (including x86 FTZ/DAZ), then restore their prior environment. Jobs and their context live until finish returns.
class RenderWorkers {
public:
    using Function=void(*)(void*,std::size_t) noexcept;
    // Runs once on each worker before owner construction returns, outside
    // CallbackScope. Drivers can establish priority/affinity/thread identity.
    // Context must live through construction; false refuses the whole pool.
    using Initializer=bool(*)(void*,std::size_t) noexcept;
    static constexpr std::size_t maximum=16;
    explicit RenderWorkers(std::size_t count,Initializer initialize=nullptr,void* context=nullptr);
    ~RenderWorkers();
    RenderWorkers(const RenderWorkers&)=delete;
    RenderWorkers& operator=(const RenderWorkers&)=delete;
    [[nodiscard]] std::size_t size() const noexcept;
    // One serialized caller, no overlapping dispatch. Returned bits identify
    // posted workers; caller runs an unposted worker's partition itself.
    [[nodiscard]] std::uint32_t start(std::size_t count,Function,void* context) noexcept;
    // Completion bits exclude a worker whose OS wait failed; run that
    // partition on the caller after the remaining jobs finish.
    [[nodiscard]] std::uint32_t finish(std::uint32_t mask) noexcept;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace adi::engine
