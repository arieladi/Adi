// SPDX-License-Identifier: AGPL-3.0-or-later
#include "BackgroundOwner.h"
#include <algorithm>
#include <mutex>
#include <thread>
#include <vector>

namespace adi::mix::limiter
{
struct BackgroundOwner::Factory
{
    std::mutex mutex;
    Owner* owner = nullptr;
    std::uint32_t clients = 0;
    std::atomic<std::uint64_t> threadsStarted{0}, waitEntries{0}, wakes{0}, dispatchAttempts{0};
};

struct BackgroundOwner::Owner
{
    Factory& counters;
    std::mutex registry;
    std::vector<Client*> clients;
    std::atomic<std::uint64_t> sequence{0}, waitingOn{0};
    std::atomic<bool> stopping{false}, parked{false};
    std::thread thread;

    explicit Owner(Factory& state) : counters(state), thread([this] { run(); })
    {
        counters.threadsStarted.fetch_add(1, std::memory_order_relaxed);
    }
    ~Owner()
    {
        stopping.store(true, std::memory_order_release);
        signal();
        thread.join();
    }
    void signal() noexcept
    {
        sequence.fetch_add(1, std::memory_order_release);
        sequence.notify_one();
    }
    void run() noexcept
    {
        std::uint64_t observed = 0;
        for (;;)
        {
            waitingOn.store(observed, std::memory_order_relaxed);
            counters.waitEntries.fetch_add(1, std::memory_order_relaxed);
            parked.store(true, std::memory_order_release);
            sequence.wait(observed, std::memory_order_acquire);
            parked.store(false, std::memory_order_release);
            counters.wakes.fetch_add(1, std::memory_order_relaxed);
            if (stopping.load(std::memory_order_acquire)) return;
            // Capture before dispatch: a notification arriving during a callback
            // must make the next wait return, rather than being swallowed.
            observed = sequence.load(std::memory_order_acquire);
            std::lock_guard lock(registry);
            for (auto* client : clients)
                if (client->pending.exchange(false, std::memory_order_acquire))
                {
                    counters.dispatchAttempts.fetch_add(1, std::memory_order_relaxed);
                    client->callback(client->context);
                }
        }
    }
};

BackgroundOwner::Factory& BackgroundOwner::factory()
{
    // Only this small coordinator survives process teardown. Owners themselves
    // join and disappear after their last client, including global engines.
    static auto* state = new Factory;
    return *state;
}

BackgroundOwner::Client::Client(void* value, Callback work) : context(value), callback(work)
{
    auto& state = factory();
    std::lock_guard factoryLock(state.mutex);
    if (!state.owner) state.owner = new Owner(state);
    owner = state.owner;
    try
    {
        std::lock_guard registryLock(owner->registry);
        owner->clients.push_back(this);
        ++state.clients;
    }
    catch (...)
    {
        if (state.clients == 0) { delete state.owner; state.owner = nullptr; }
        throw;
    }
}

BackgroundOwner::Client::~Client()
{
    auto& state = factory();
    std::lock_guard factoryLock(state.mutex);
    {
        // The worker holds this mutex throughout callbacks, so removal waits
        // for any in-flight access to the client's context before it returns.
        std::lock_guard registryLock(owner->registry);
        std::erase(owner->clients, this);
    }
    if (--state.clients == 0) { delete state.owner; state.owner = nullptr; }
}

void BackgroundOwner::Client::notify() noexcept
{
    if (!pending.exchange(true, std::memory_order_release)) owner->signal();
}

BackgroundOwner::Snapshot BackgroundOwner::snapshot()
{
    auto& state = factory();
    std::lock_guard lock(state.mutex);
    Snapshot result;
    result.threadsStarted = state.threadsStarted.load(std::memory_order_relaxed);
    result.waitEntries = state.waitEntries.load(std::memory_order_relaxed);
    result.wakes = state.wakes.load(std::memory_order_relaxed);
    result.dispatchAttempts = state.dispatchAttempts.load(std::memory_order_relaxed);
    result.owners = state.owner ? 1u : 0u;
    result.clients = state.clients;
    if (state.owner)
        result.parked = state.owner->parked.load(std::memory_order_acquire)
            && state.owner->sequence.load(std::memory_order_acquire)
                == state.owner->waitingOn.load(std::memory_order_relaxed);
    return result;
}
}
