// SPDX-License-Identifier: AGPL-3.0-or-later
//
// CO-01: thread-safe in-memory fake object store with put-if-absent, lexical
// listing, seeded visibility delays, and transient drops.

#include "adi/collab/fake_object_store.hpp"

#include <utility>

namespace adi::collab {

namespace {

[[nodiscard]] bool rollDrop(std::mt19937_64& rng, double rate) {
    if (rate <= 0.0) {
        return false;
    }
    if (rate >= 1.0) {
        return true;
    }
    std::uniform_real_distribution<double> dist(0.0, 1.0);
    return dist(rng) < rate;
}

}  // namespace

FakeObjectStore::FakeObjectStore(FaultConfig faults) : faults_(faults), rng_(faults.seed) {}

void FakeObjectStore::setFaults(FaultConfig faults) {
    std::lock_guard<std::mutex> lock(mutex_);
    faults_ = faults;
    rng_.seed(faults.seed);
}

void FakeObjectStore::tick(std::uint64_t steps) {
    std::lock_guard<std::mutex> lock(mutex_);
    currentTick_ += steps;
}

void FakeObjectStore::healNetwork() {
    std::lock_guard<std::mutex> lock(mutex_);
    faults_.putDropRate = 0.0;
    faults_.putAckDropRate = 0.0;
    faults_.getDropRate = 0.0;
    faults_.listDropRate = 0.0;
    faults_.maxVisibilityDelayTicks = 0U;
    for (auto& [key, entry] : objects_) {
        entry.visibleAtTick = 0U;
    }
}

PutStatus FakeObjectStore::putIfAbsent(std::string_view key, std::vector<std::byte> bytes) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (rollDrop(rng_, faults_.putDropRate)) {
        return PutStatus::Dropped;
    }
    if (objects_.find(key) != objects_.end()) {
        return PutStatus::AlreadyExists;
    }

    std::uint64_t delay = 0U;
    if (faults_.maxVisibilityDelayTicks > 0U) {
        std::uniform_int_distribution<std::uint64_t> dist(0U, faults_.maxVisibilityDelayTicks);
        delay = dist(rng_);
    }

    objects_.emplace(std::string(key), Entry{std::move(bytes), currentTick_ + delay});

    if (rollDrop(rng_, faults_.putAckDropRate)) {
        return PutStatus::Dropped;
    }
    return PutStatus::Created;
}

GetResult FakeObjectStore::get(std::string_view key) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (rollDrop(rng_, faults_.getDropRate)) {
        return {GetStatus::Dropped, {}};
    }
    const auto it = objects_.find(key);
    if (it == objects_.end() || it->second.visibleAtTick > currentTick_) {
        return {GetStatus::NotFound, {}};
    }
    return {GetStatus::Ok, it->second.bytes};
}

ListResult FakeObjectStore::listKeys(std::string_view prefix) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (rollDrop(rng_, faults_.listDropRate)) {
        return {false, {}};
    }
    ListResult res;
    res.ok = true;
    for (const auto& [key, entry] : objects_) {
        if (entry.visibleAtTick > currentTick_) {
            continue;
        }
        if (prefix.empty() || key.starts_with(prefix)) {
            res.keys.push_back(key);
        }
    }
    return res;
}

std::size_t FakeObjectStore::committedCount() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return objects_.size();
}

}  // namespace adi::collab
