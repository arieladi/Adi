// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include "Settings.hpp"
#include <array>
#include <atomic>
#include <cstdint>

namespace adi::mix::focus::detail {
// One nonblocking publisher and one owner reader. Three fixed slots make a
// complete Settings snapshot immutable for the entire read lease.
class ConfigurationMailbox final {
  public:
    static_assert(std::atomic<int>::is_always_lock_free);
    static_assert(std::atomic<std::uint64_t>::is_always_lock_free);
    struct Item { std::atomic<int> readers{0};Settings settings;std::uint64_t version=0; };
    std::array<Item,3> slots;
    // Packed serial/index also rejects a reused slot read through an old identity.
    std::atomic<std::uint64_t> published{0};
    std::atomic<std::uint64_t> sequence{0};
    std::uint64_t nextVersion=0;
    void reset() noexcept {
        for(auto &slot:slots) {slot.readers.store(0,std::memory_order_relaxed);slot.version=0;}
        nextVersion=0;published.store(0,std::memory_order_relaxed);sequence.store(0,std::memory_order_relaxed);
    }
    std::uint64_t publish(const Settings &s) noexcept {
        const int current=static_cast<int>(published.load(std::memory_order_acquire)&3U);
        for(int index=0;index<3;++index) {
            if(index==current) continue;
            auto &slot=slots[static_cast<std::size_t>(index)];int zero=0;
            if(!slot.readers.compare_exchange_strong(zero,-1,std::memory_order_acq_rel)) continue;
            slot.settings=s;slot.version=++nextVersion;
            slot.readers.store(0,std::memory_order_release);
            published.store((nextVersion<<2U)|static_cast<std::uint64_t>(index),std::memory_order_release);sequence.store(nextVersion,std::memory_order_release);
            return nextVersion;
        }
        return 0;
    }
    bool read(Settings &s,std::uint64_t &version) noexcept {
        const auto identity=published.load(std::memory_order_acquire);
        const auto index=static_cast<std::size_t>(identity&3U);
        auto &slot=slots[index];
        int readers=slot.readers.load(std::memory_order_relaxed);
        for(int attempt=0;attempt<3&&readers>=0;++attempt) {
            if(slot.readers.compare_exchange_weak(readers,readers+1,std::memory_order_acquire)) {
                const bool current=slot.version==(identity>>2U);
                if(current) {s=slot.settings;version=slot.version;}
                slot.readers.fetch_sub(1,std::memory_order_release);return current;
            }
        }
        return false;
    }
};
} // namespace adi::mix::focus::detail
