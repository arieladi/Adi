// SPDX-License-Identifier: AGPL-3.0-or-later
#include "BackgroundOwner.h"
#include "DspEngine.h"
#include "Dsp/KernelStage.h"
#include "Dsp/Latency.h"
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <iostream>
#include <memory>
#include <thread>
#include <vector>

using namespace adi::mix::limiter;
namespace
{
int failures = 0;
void check(bool value, const char* message)
{
    if (!value) { ++failures; std::cerr << "FAIL: " << message << '\n'; }
}
template<class Predicate> bool await(Predicate predicate)
{
    // This is a failure watchdog for synchronization, not a CPU measurement.
    const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!predicate())
    {
        if (std::chrono::steady_clock::now() >= end) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return true;
}
bool parked() { return BackgroundOwner::snapshot().parked; }
void idleCounters(const char* message)
{
    check(await(parked), "owner acknowledges its current parked sequence");
    const auto before = BackgroundOwner::snapshot();
    std::this_thread::sleep_for(std::chrono::milliseconds(25));
    const auto after = BackgroundOwner::snapshot();
    check(before.parked && after.parked && before.waitEntries == after.waitEntries
        && before.wakes == after.wakes && before.dispatchAttempts == after.dispatchAttempts, message);
}
void countedClients()
{
    const auto before = BackgroundOwner::snapshot();
    std::array<std::atomic<unsigned>, 48> calls{};
    std::vector<std::unique_ptr<BackgroundOwner::Client>> clients;
    for (auto& count : calls)
        clients.push_back(std::make_unique<BackgroundOwner::Client>(&count, [](void* context) noexcept {
            static_cast<std::atomic<unsigned>*>(context)->fetch_add(1, std::memory_order_release);
        }));
    const auto shared = BackgroundOwner::snapshot();
    check(shared.owners == 1 && shared.clients == 48 && shared.threadsStarted == before.threadsStarted + 1,
        "48 registered engine-owner stand-ins share exactly one thread");
    idleCounters("an idle shared owner performs no wake, loop or dispatch attempts");
    for (auto& client : clients) client->notify();
    check(await([&] {
        for (const auto& count : calls) if (count.load(std::memory_order_acquire) != 1) return false;
        return parked();
    }), "all 48 genuine notifications are dispatched exactly once");
    const auto dispatched = BackgroundOwner::snapshot();
    check(dispatched.dispatchAttempts == before.dispatchAttempts + 48,
        "dispatch instrumentation counts calls before any client can return without work");
    idleCounters("notification completion returns the shared owner to a stable atomic wait");
    clients.clear();
    check(BackgroundOwner::snapshot().owners == 0 && BackgroundOwner::snapshot().clients == 0,
        "the last registration joins and destroys the shared owner");
}
struct GatedWork
{
    std::atomic<unsigned> calls{0};
    std::atomic<bool> entered{false}, release{false};
    static void run(void* context) noexcept
    {
        auto& self = *static_cast<GatedWork*>(context);
        const auto pass = self.calls.fetch_add(1, std::memory_order_release);
        if (pass == 0)
        {
            self.entered.store(true, std::memory_order_release);
            self.release.wait(false, std::memory_order_acquire);
        }
    }
};
void coalescingAndTeardown()
{
    GatedWork work;
    auto client = std::make_unique<BackgroundOwner::Client>(&work, GatedWork::run);
    client->notify();
    const auto entered = await([&] { return work.entered.load(std::memory_order_acquire); });
    check(entered, "a real worker callback is in flight");
    for (int index = 0; index < 1000; ++index) client->notify();
    work.release.store(true, std::memory_order_release); work.release.notify_one();
    check(await([&] { return work.calls.load(std::memory_order_acquire) == 2 && parked(); }),
        "notifications during a callback coalesce without losing the following dispatch");
    client.reset();

    GatedWork teardown;
    auto retained = std::make_unique<BackgroundOwner::Client>(&teardown, GatedWork::run);
    retained->notify();
    check(await([&] { return teardown.entered.load(std::memory_order_acquire); }),
        "teardown fixture enters its callback");
    std::atomic<bool> destroying{false}, destroyed{false};
    std::thread destroyer([clientToDestroy = std::move(retained), &destroying, &destroyed]() mutable {
        destroying.store(true, std::memory_order_release);
        clientToDestroy.reset();
        destroyed.store(true, std::memory_order_release);
    });
    check(await([&] { return destroying.load(std::memory_order_acquire); }), "off-audio teardown starts");
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    check(!destroyed.load(std::memory_order_acquire), "unregistration waits for in-flight context access");
    teardown.release.store(true, std::memory_order_release); teardown.release.notify_one();
    destroyer.join();
    check(destroyed.load(std::memory_order_acquire), "unregistration completes after the callback returns");
}
void kernelNotifications()
{
    struct Work
    {
        KernelStage stage;
        std::atomic<unsigned> successes{0};
        static void run(void* context) noexcept
        {
            auto& self = *static_cast<Work*>(context);
            if (self.stage.service()) self.successes.fetch_add(1, std::memory_order_release);
        }
    } work;
    KernelStage reference;
    const std::array<float, 3> initial{.2f, .3f, .4f}, changed{.5f, .4f, .3f};
    work.stage.prepare(48000, 128, 64, 0, initial); reference.prepare(48000, 128, 64, 0, initial);
    BackgroundOwner::Client client(&work, Work::run);
    work.stage.request(96, 1, changed); reference.request(96, 1, changed);
    check(!work.stage.needsService(), "an unconsumed prepared kernel cannot request an owner retry");
    client.notify();
    check(await(parked), "a blocked kernel callback finishes and parks");
    check(work.successes.load() == 0 && !reference.service(), "a pending kernel preserves service admission");
    idleCounters("a kernel waiting for audio never queues or polls itself");
    bool exact = true;
    for (int sample = 0; sample < 2048; ++sample)
    {
        const auto input = static_cast<float>(std::sin(sample * .031));
        exact = exact && work.stage.process(input) == reference.process(input);
    }
    check(work.stage.needsService(), "real audio consumption admits the next kernel generation");
    client.notify();
    check(await([&] { return work.successes.load(std::memory_order_acquire) == 1 && parked(); }),
        "an explicit block notification completes a genuine kernel generation");
    check(reference.service() && !work.stage.needsService(), "served generation is published atomically");
    for (int sample = 0; sample < 2048; ++sample)
    {
        const auto input = static_cast<float>(std::sin(sample * .047));
        exact = exact && work.stage.process(input) == reference.process(input);
    }
    check(exact, "shared-owner kernel scheduling retains exact scalar stage output");
    idleCounters("a completed kernel generation leaves no owner work behind");
}
void engineLifecycle()
{
    check(BackgroundOwner::snapshot().clients == 0, "engine fixture starts without registrations");
    {
        DspEngine first, second;
        check(BackgroundOwner::snapshot().clients == 0, "unprepared engines start no worker");
        EngineSettings settings;
        settings.styleIndex = 5;
        first.prepare(48000, 64, ChannelLayout::stereo(), settings);
        second.prepare(48000, 64, ChannelLayout::stereo(), settings);
        check(BackgroundOwner::snapshot().owners == 1 && BackgroundOwner::snapshot().clients == 2,
            "two real prepared limiter engines share the process owner");
        idleCounters("prepared real engines do no idle owner work");
        first.setBackgroundActive(false);
        settings.styleIndex = 1;
        first.requestConfiguration(settings);
        idleCounters("a background-disabled engine retains its structural request without waking");
        first.setBackgroundActive(true);
        check(await(parked), "reenabling background work finishes the requested structure");
        std::array<float, 64> left{}, right{};
        std::array<float*, 2> audio{left.data(), right.data()};
        for (int block = 0; block < 128; ++block)
        {
            const auto change = first.pendingLatencyChange();
            if (change.token) first.acknowledgeLatencyChange(change.token);
            first.process(audio.data(), nullptr, 64, settings);
        }
        check(first.latencySamples() == processingLatency(Style::punchy, 48000, 1, false),
            "real structural notification reaches the audio-side latency transition");
        second.setBackgroundActive(false);
        second.prepare(44100, 64, ChannelLayout::mono(), settings);
        check(BackgroundOwner::snapshot().owners == 1 && BackgroundOwner::snapshot().clients == 2,
            "stop and reprepare preserve one owner and replace the registration safely");
        settings.styleIndex = 7;
        const auto beforeRepreparedRequest = BackgroundOwner::snapshot().dispatchAttempts;
        second.requestConfiguration(settings);
        check(await([&] { return BackgroundOwner::snapshot().dispatchAttempts > beforeRepreparedRequest && parked(); }),
            "reprepare restores structural work after a preceding release disabled background service");
        std::array<float*, 1> mono{left.data()};
        for (int block = 0; block < 128; ++block)
        {
            const auto change = second.pendingLatencyChange();
            if (change.token) second.acknowledgeLatencyChange(change.token);
            second.process(mono.data(), nullptr, 64, settings);
        }
        check(second.latencySamples() == processingLatency(Style::safe, 44100, 1, false),
            "reprepared structure reaches the real mono audio transition");
        idleCounters("retirement and reprepare settle without recurring work");
    }
    check(BackgroundOwner::snapshot().owners == 0 && BackgroundOwner::snapshot().clients == 0,
        "engine destruction leaves no worker or registered callback");
}
}
int main()
{
    countedClients();
    coalescingAndTeardown();
    kernelNotifications();
    engineLifecycle();
    if (!failures) std::cout << "Shared limiter owner checks passed\n";
    return failures ? 1 : 0;
}
