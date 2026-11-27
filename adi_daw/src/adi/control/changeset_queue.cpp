// SPDX-License-Identifier: AGPL-3.0-or-later
#include "adi/control/changeset_queue.hpp"

#include <algorithm>
#include <random>
#include <utility>

namespace adi::control {

std::string_view toString(Review r) {
    switch (r) {
        case Review::AwaitingReview: return "awaiting_review";
        case Review::Applied: return "applied";
        case Review::Rejected: return "rejected";
        case Review::Stale: return "stale";
        case Review::Expired: return "expired";
    }
    return "awaiting_review";
}

Json changesetJson(const agent::Changeset& cs) {
    Json ops = Json::array();
    for (const auto& q : cs.ops) {
        Json op = {{"op", q.opType}, {"payload", q.payload}};
        if (!q.label.empty()) op["label"] = q.label;
        if (!q.targetKind.empty()) op["targetKind"] = q.targetKind;
        if (q.targetId) op["targetId"] = *q.targetId;
        ops.push_back(std::move(op));
    }
    return {{"request", cs.request},
            {"actorDetail", cs.actorDetail},
            {"createdUtc", cs.createdUtc},
            {"baseHead", cs.baseHead ? Json(*cs.baseHead) : Json(nullptr)},
            {"ops", std::move(ops)}};
}

std::string randomChangesetId() {
    std::random_device rd;
    static constexpr char kHex[] = "0123456789abcdef";
    std::string out = "cs-";
    for (int i = 0; i < 8; ++i) {
        const auto b = static_cast<unsigned>(rd() & 0xffu);
        out += kHex[b >> 4];
        out += kHex[b & 0xfu];
    }
    return out;
}

ChangesetQueue::ChangesetQueue(std::function<std::string()> newId, Observer observer)
    : newId_(std::move(newId)), observer_(std::move(observer)) {}

std::string ChangesetQueue::nextId() { return newId_(); }

std::optional<QueuedChangeset> ChangesetQueue::findByKey(const std::string& credential,
                                                         const std::string& key) const {
    if (key.empty()) return std::nullopt;
    std::lock_guard lock(mutex_);
    for (const auto& e : entries_)
        if (e.credential == credential && e.idempotencyKey == key) return e;
    return std::nullopt;
}

std::optional<QueuedChangeset> ChangesetQueue::find(std::string_view id, const std::string& credential) const {
    std::lock_guard lock(mutex_);
    for (const auto& e : entries_)
        if (e.id == id && e.credential == credential) return e;
    return std::nullopt;
}

void ChangesetQueue::add(QueuedChangeset entry) {
    std::lock_guard lock(mutex_);
    entries_.push_back(std::move(entry));
    if (observer_) {
        const auto& added = entries_.back();
        observer_(added, true, "agent", "changeset");
        if (added.state == Review::Applied) observer_(added, false, "agent", "automatic apply");
    }
    // Bounded: drop the oldest settled entries first, then the oldest of all.
    while (entries_.size() > kCapacity) {
        auto settled = std::find_if(entries_.begin(), entries_.end(), [](const QueuedChangeset& e) {
            return e.state != Review::AwaitingReview;
        });
        if (settled == entries_.end()) {
            settled = entries_.begin();
            settled->state = Review::Expired;
            if (observer_) observer_(*settled, false, "system", "queue capacity");
        }
        entries_.erase(settled);
    }
}

std::vector<QueuedChangeset> ChangesetQueue::forReview() const {
    std::lock_guard lock(mutex_);
    std::vector<QueuedChangeset> result;
    for (const auto& entry : entries_)
        if (entry.state == Review::AwaitingReview) result.push_back(entry);
    return result;
}

std::vector<RejectedChangeset> ChangesetQueue::rejectPending(std::string_view surface) {
    std::lock_guard lock(mutex_);
    std::vector<RejectedChangeset> rejected;
    for (auto& entry : entries_) {
        if (entry.state == Review::AwaitingReview) {
            entry.state = Review::Rejected;
            rejected.push_back({entry.id, entry.projectUuid});
            if (observer_) observer_(entry, false, "user", surface);
        }
    }
    return rejected;
}

bool ChangesetQueue::setState(std::string_view id, const std::string& credential, Review state,
                              std::string_view actor, std::string_view source) {
    std::lock_guard lock(mutex_);
    for (auto& e : entries_) {
        if (e.id == id && e.credential == credential) {
            if (e.state != state) {
                e.state = state;
                if (observer_) observer_(e, false, actor, source);
            }
            return true;
        }
    }
    return false;
}

void ChangesetQueue::expire(std::int64_t nowMs) {
    std::lock_guard lock(mutex_);
    for (auto& e : entries_) {
        if (e.state == Review::AwaitingReview && nowMs - e.createdMs > kReviewWindowMs) {
            e.state = Review::Expired;
            if (observer_) observer_(e, false, "system", "review window");
        }
    }
}

std::size_t ChangesetQueue::size() const {
    std::lock_guard lock(mutex_);
    return entries_.size();
}

}  // namespace adi::control
