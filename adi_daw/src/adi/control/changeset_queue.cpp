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

ChangesetQueue::ChangesetQueue(std::function<std::string()> newId) : newId_(std::move(newId)) {}

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
    // Bounded: drop the oldest settled entries first, then the oldest of all.
    while (entries_.size() > kCapacity) {
        auto settled = std::find_if(entries_.begin(), entries_.end(), [](const QueuedChangeset& e) {
            return e.state != Review::AwaitingReview;
        });
        entries_.erase(settled != entries_.end() ? settled : entries_.begin());
    }
}

bool ChangesetQueue::setState(std::string_view id, const std::string& credential, Review state) {
    std::lock_guard lock(mutex_);
    for (auto& e : entries_) {
        if (e.id == id && e.credential == credential) {
            e.state = state;
            return true;
        }
    }
    return false;
}

void ChangesetQueue::expire(std::int64_t nowMs) {
    std::lock_guard lock(mutex_);
    for (auto& e : entries_)
        if (e.state == Review::AwaitingReview && nowMs - e.createdMs > kReviewWindowMs)
            e.state = Review::Expired;
}

std::size_t ChangesetQueue::size() const {
    std::lock_guard lock(mutex_);
    return entries_.size();
}

}  // namespace adi::control
