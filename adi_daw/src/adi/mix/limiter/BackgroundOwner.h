// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include <atomic>
#include <cstdint>

namespace adi::mix::limiter
{
// One parked owner serves all prepared engines. Registration and destruction
// happen off audio; notify is a coalesced, allocation-free audio-side operation.
class BackgroundOwner
{
    static_assert(std::atomic<bool>::is_always_lock_free);
    static_assert(std::atomic<std::uint64_t>::is_always_lock_free);
    struct Owner;
    struct Factory;

public:
    struct Snapshot
    {
        std::uint64_t threadsStarted = 0, waitEntries = 0, wakes = 0, dispatchAttempts = 0;
        std::uint32_t owners = 0, clients = 0;
        bool parked = false;
    };

    class Client
    {
    public:
        using Callback = void (*)(void*) noexcept;
        Client(void* context, Callback callback);
        ~Client();
        Client(const Client&) = delete;
        Client& operator=(const Client&) = delete;
        void notify() noexcept;

    private:
        friend struct Owner;
        Owner* owner;
        void* context;
        Callback callback;
        std::atomic<bool> pending{false};
    };

    // Off-audio diagnostics count each wait and dispatch before client work can
    // return early. Parked acknowledges the exact sequence the owner waits on.
    static Snapshot snapshot();

private:
    static Factory& factory();
};
}
