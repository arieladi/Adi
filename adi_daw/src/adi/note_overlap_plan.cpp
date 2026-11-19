// SPDX-License-Identifier: AGPL-3.0-or-later
#include "adi/note_overlap_detail.hpp"
#include <algorithm>
#include <array>
#include <limits>
#include <map>
#include <set>
#include <stdexcept>
#include <tuple>
#include <utility>

namespace adi::notes {
namespace {

NoteValues checkedValues(const Record& record) {
    std::string error;
    auto result = values(record, error);
    if (!result) {
        throw std::runtime_error(error);
    }
    return *result;
}

std::int64_t checkedDuration(OverlapSpan span) {
    if (span.end <= span.begin) {
        throw std::runtime_error("note overlap interval must have positive duration");
    }
    // Adding a negative begin to MAX is safe, even for INT64_MIN. Do not
    // subtract endpoints until their positive difference is known to fit.
    if (span.begin < 0
        && span.end > std::numeric_limits<std::int64_t>::max() + span.begin) {
        throw std::runtime_error("note overlap interval duration exceeds signed 64-bit ticks");
    }
    return span.end - span.begin;
}

OverlapSpan spanOf(const NoteValues& value) noexcept {
    // values() already checked the positive duration and this endpoint.
    return {value.start, value.start + value.duration};
}

OverlapRelation relationOf(OverlapSpan victim, OverlapSpan incoming) noexcept {
    if (victim.end <= incoming.begin || incoming.end <= victim.begin) {
        return OverlapRelation::Disjoint;
    }
    if (victim == incoming) {
        return OverlapRelation::Equal;
    }
    if (incoming.begin <= victim.begin && victim.end <= incoming.end) {
        return OverlapRelation::IncomingCoversVictim;
    }
    if (victim.begin < incoming.begin && incoming.end < victim.end) {
        return OverlapRelation::IncomingInsideVictim;
    }
    if (victim.begin < incoming.begin) {
        return OverlapRelation::IncomingStartsInside;
    }
    return OverlapRelation::IncomingEndsInside;
}

struct RecordIndex {
    std::vector<NoteValues> known;
    std::map<std::uint64_t, std::size_t> ordinals;
};

RecordIndex indexRecords(const detail::Stream& stream) {
    RecordIndex result;
    result.known.reserve(stream.records.size());
    for (std::size_t ordinal = 0; ordinal < stream.records.size(); ++ordinal) {
        const auto known = checkedValues(stream.records[ordinal]);
        if (!result.ordinals.emplace(known.id, ordinal).second) {
            throw std::runtime_error("notes stream has duplicate note IDs");
        }
        result.known.push_back(known);
    }
    return result;
}

detail::OverlapPlan refused(OverlapStatus status, std::string error,
                           std::optional<IncomingOverlapConflict> conflict = std::nullopt) {
    detail::OverlapPlan result;
    result.status = status;
    result.error = std::move(error);
    if (status == OverlapStatus::IncomingOverlap) {
        result.conflict = std::move(conflict);
    }
    return result;
}

struct Driver {
    std::uint64_t id = 0;
    OverlapSpan span;
};

bool driverOrder(const Driver& left, const Driver& right) noexcept {
    return std::tie(left.span.begin, left.span.end, left.id)
           < std::tie(right.span.begin, right.span.end, right.id);
}

const std::string& emptyText() noexcept {
    static const std::string empty;
    return empty;
}

const Snapshot& emptySnapshot() noexcept {
    static const Snapshot empty;
    return empty;
}

const Diff& emptyDiff() noexcept {
    static const Diff empty;
    return empty;
}

const std::optional<IncomingOverlapConflict>& emptyConflict() noexcept {
    static const std::optional<IncomingOverlapConflict> empty;
    return empty;
}

} // namespace

namespace detail {

std::optional<OverlapRelation> classifyOverlap(OverlapSpan victim, OverlapSpan incoming,
                                              std::string& error) {
    error.clear();
    try {
        (void)checkedDuration(victim);
        (void)checkedDuration(incoming);
        return relationOf(victim, incoming);
    } catch (const std::exception& exception) {
        error = exception.what();
        return std::nullopt;
    }
}

std::optional<Diff> orderOverlapDiff(const Stream& stream, const Diff& diff,
                                   std::string& error) {
    error.clear();
    try {
        const auto baseline = indexRecords(stream);
        std::set<std::uint64_t> touched;
        const auto touch = [&](std::uint64_t id) {
            if (!touched.insert(id).second) {
                throw std::runtime_error("note ID is repeated within or between overlap diff sections");
            }
        };
        const auto ordinalOf = [&](std::uint64_t id) {
            const auto found = baseline.ordinals.find(id);
            if (found == baseline.ordinals.end()) {
                throw std::runtime_error("overlap diff old note identity does not exist");
            }
            return found->second;
        };

        std::vector<std::pair<std::size_t, const Record*>> removed;
        std::vector<std::pair<std::size_t, const Change*>> changed;
        std::vector<std::pair<std::uint64_t, const Record*>> added;
        removed.reserve(diff.removed.size());
        changed.reserve(diff.changed.size());
        added.reserve(diff.added.size());
        for (const auto& record : diff.removed) {
            const auto known = checkedValues(record);
            touch(known.id);
            removed.emplace_back(ordinalOf(known.id), &record);
        }
        for (const auto& change : diff.changed) {
            const auto before = checkedValues(change.oldRecord);
            const auto after = checkedValues(change.newRecord);
            if (before.id != after.id) {
                throw std::runtime_error("a changed overlap note must retain its nonzero identity");
            }
            touch(before.id);
            changed.emplace_back(ordinalOf(before.id), &change);
        }
        for (const auto& record : diff.added) {
            const auto known = checkedValues(record);
            touch(known.id);
            if (baseline.ordinals.contains(known.id)) {
                throw std::runtime_error("added overlap note identity already exists");
            }
            added.emplace_back(known.id, &record);
        }
        const auto byFirst = [](const auto& left, const auto& right) {
            return left.first < right.first;
        };
        std::sort(removed.begin(), removed.end(), byFirst);
        std::sort(changed.begin(), changed.end(), byFirst);
        std::sort(added.begin(), added.end(), byFirst);
        Diff result;
        result.removed.reserve(removed.size());
        result.changed.reserve(changed.size());
        result.added.reserve(added.size());
        for (const auto& entry : removed) {
            result.removed.push_back(*entry.second);
        }
        for (const auto& entry : changed) {
            // Ordering alone must not hide an extra outer no-op entry.
            result.changed.push_back(*entry.second);
        }
        for (const auto& entry : added) {
            result.added.push_back(*entry.second);
        }
        return result;
    } catch (const std::exception& exception) {
        error = exception.what();
        return std::nullopt;
    }
}

OverlapPlan resolveOverlap(const Stream& stream, const Diff& intent) {
    try {
        OverlapPlan result;
        {
            // Validate the COMPLETE original intent before any composition.
            // A later driver may remove a non-geometrically changed victim;
            // that must not hide stale old bytes, bad stride or changed tails.
            const auto validated = planRecords(stream, intent);
            if (!validated.ok) {
                return refused(OverlapStatus::InvalidIntent, validated.error);
            }
            std::string error;
            auto ordered = orderOverlapDiff(stream, validated.effects, error);
            if (!ordered) {
                return refused(OverlapStatus::InvalidIntent, std::move(error));
            }
            result.intent = std::move(*ordered);
        }

        const auto baseline = indexRecords(stream);
        std::array<std::vector<Driver>, 128> byKey;
        std::vector<bool> removed(stream.records.size(), false);
        std::vector<bool> relocated(stream.records.size(), false);
        std::vector<const Record*> replacements(stream.records.size(), nullptr);
        const auto addDriver = [&](const NoteValues& known) {
            byKey[static_cast<std::size_t>(known.key)].push_back({known.id, spanOf(known)});
        };
        for (const auto& record : result.intent.removed) {
            removed[baseline.ordinals.at(checkedValues(record).id)] = true;
        }
        for (const auto& change : result.intent.changed) {
            const auto before = checkedValues(change.oldRecord);
            const auto after = checkedValues(change.newRecord);
            const auto ordinal = baseline.ordinals.at(before.id);
            replacements[ordinal] = &change.newRecord;
            if (before.start != after.start || before.duration != after.duration
                || before.key != after.key) {
                relocated[ordinal] = true;
                addDriver(after);
            }
        }
        for (const auto& record : result.intent.added) {
            addDriver(checkedValues(record));
        }
        for (std::size_t key = 0; key < byKey.size(); ++key) {
            auto& drivers = byKey[key];
            std::sort(drivers.begin(), drivers.end(), driverOrder);
            for (std::size_t index = 1; index < drivers.size(); ++index) {
                const auto& first = drivers[index - 1];
                const auto& second = drivers[index];
                if (first.span.end > second.span.begin) {
                    IncomingOverlapConflict conflict;
                    conflict.first = first.id;
                    conflict.second = second.id;
                    conflict.key = static_cast<std::uint8_t>(key);
                    conflict.firstSpan = first.span;
                    conflict.secondSpan = second.span;
                    return refused(OverlapStatus::IncomingOverlap,
                        "IncomingOverlap: final same-key incoming notes overlap", conflict);
                }
            }
        }

        Diff effective;
        effective.added = result.intent.added;
        for (std::size_t ordinal = 0; ordinal < stream.records.size(); ++ordinal) {
            const auto& original = stream.records[ordinal];
            const auto& known = baseline.known[ordinal];
            if (removed[ordinal]) {
                effective.removed.push_back(original);
                continue;
            }
            const auto* replacement = replacements[ordinal];
            if (relocated[ordinal]) {
                effective.changed.push_back({original, *replacement});
                continue;
            }

            const auto victimSpan = spanOf(known);
            const auto& drivers = byKey[static_cast<std::size_t>(known.key)];
            // Disjoint positive driver spans have strictly increasing ends,
            // so the first end > victim.begin is an indexed overlap candidate.
            auto incoming = std::upper_bound(drivers.begin(), drivers.end(), victimSpan.begin,
                [](std::int64_t point, const Driver& driver) {
                    return point < driver.span.end;
                });
            const Driver* cause = nullptr;
            for (; incoming != drivers.end() && incoming->span.begin < victimSpan.end; ++incoming) {
                if (result.collisions.size() == noteOverlapCollisionLimit) {
                    return refused(OverlapStatus::CollisionLimitExceeded,
                        "CollisionLimitExceeded: note overlap exceeds 100000 victim/driver collisions");
                }
                if (cause == nullptr) {
                    cause = &*incoming;
                }
                OverlapCollision collision;
                collision.incoming = incoming->id;
                collision.victim = known.id;
                collision.key = static_cast<std::uint8_t>(known.key);
                collision.relation = relationOf(victimSpan, incoming->span);
                collision.incomingSpan = incoming->span;
                collision.victimSpan = victimSpan;
                result.collisions.push_back(collision);
            }
            if (cause == nullptr) {
                if (replacement != nullptr) {
                    effective.changed.push_back({original, *replacement});
                }
                continue;
            }

            OverlapEffect effect;
            effect.victim = known.id;
            effect.causes.push_back(cause->id);
            effect.before = original;
            if (cause->span.begin <= victimSpan.begin) {
                // The candidate's end is already known to exceed this start.
                effect.action = OverlapAction::Remove;
                effective.removed.push_back(original);
            } else {
                effect.action = OverlapAction::ShortenEnd;
                const auto& candidate = replacement != nullptr ? *replacement : original;
                auto finalValues = checkedValues(candidate);
                finalValues.duration = checkedDuration({victimSpan.begin, cause->span.begin});
                std::string error;
                auto finalRecord = withValues(candidate, finalValues, error);
                if (!finalRecord) {
                    return refused(OverlapStatus::InvalidIntent, std::move(error));
                }
                effect.after = *finalRecord;
                effective.changed.push_back({original, std::move(*finalRecord)});
            }
            result.sideEffects.push_back(std::move(effect));
        }

        // effective was built in baseline order, with additions in uint64 ID
        // order. This is the sole final encode; the owner consumes this plan.
        result.stream = planRecords(stream, effective);
        if (!result.stream.ok) {
            return refused(OverlapStatus::InvalidIntent, result.stream.error);
        }
        result.status = OverlapStatus::Ready;
        return result;
    } catch (const std::exception& exception) {
        return refused(OverlapStatus::InvalidIntent, exception.what());
    }
}

PreparedOverlap PreparedOverlapAccess::seal(Snapshot source, OverlapPlan plan) {
    PreparedOverlapData data;
    data.source = std::move(source);
    data.plan = std::move(plan);
    try {
        if (data.plan.ok()) {
            const auto* captured = SnapshotAccess::get(data.source);
            if (!data.source.ok || captured == nullptr) {
                data.plan = refused(OverlapStatus::InvalidSnapshot, "invalid notes overlap snapshot");
            } else {
                const auto& effects = data.plan.stream.effects;
                const bool hasEffects = !effects.added.empty() || !effects.removed.empty()
                                        || !effects.changed.empty();
                if (!data.plan.stream.ok || data.plan.stream.changed != hasEffects) {
                    throw std::runtime_error("incoherent prepared note overlap plan");
                }
                if (data.plan.stream.changed) {
                    if (!captured->streamId && !captured->creationId) {
                        throw std::runtime_error("creating notes requires a caller-owned captured stream ID");
                    }
                    // There is no caller-supplied request to trust or compare:
                    // build it exactly once from the same plan we seal below.
                    OpRequest request;
                    std::string error;
                    if (!makeOverlapRequest(*captured, data.plan, request, error)) {
                        throw std::runtime_error(error.empty() ? "cannot prepare note overlap packet" : error);
                    }
                    data.requests.push_back(std::move(request));
                }
            }
        }
    } catch (const std::exception& exception) {
        data.plan = refused(OverlapStatus::PacketRejected, exception.what());
    }
    if (!data.plan.ok()) {
        data.plan = refused(data.plan.status, std::move(data.plan.error), std::move(data.plan.conflict));
        data.requests.clear();
    }
    PreparedOverlap result;
    result.data_ = std::make_shared<const PreparedOverlapData>(std::move(data));
    return result;
}

} // namespace detail

bool PreparedOverlap::ok() const noexcept { return data_ && data_->plan.ok(); }

OverlapStatus PreparedOverlap::status() const noexcept {
    return data_ ? data_->plan.status : OverlapStatus::NotPrepared;
}

const std::string& PreparedOverlap::error() const noexcept {
    return data_ ? data_->plan.error : emptyText();
}

const Snapshot& PreparedOverlap::source() const noexcept {
    return data_ ? data_->source : emptySnapshot();
}

std::optional<std::int64_t> PreparedOverlap::expectedHead() const noexcept {
    if (!ok()) {
        return std::nullopt;
    }
    const auto* captured = detail::SnapshotAccess::get(data_->source);
    return captured != nullptr ? captured->captured.head : std::nullopt;
}

std::span<const OpRequest> PreparedOverlap::requests() const noexcept {
    return ok() ? std::span<const OpRequest>(data_->requests) : std::span<const OpRequest>{};
}

const Diff& PreparedOverlap::intent() const noexcept {
    return ok() ? data_->plan.intent : emptyDiff();
}

const Diff& PreparedOverlap::effects() const noexcept {
    return ok() ? data_->plan.stream.effects : emptyDiff();
}

std::span<const OverlapCollision> PreparedOverlap::collisions() const noexcept {
    return ok() ? std::span<const OverlapCollision>(data_->plan.collisions)
                : std::span<const OverlapCollision>{};
}

std::span<const OverlapEffect> PreparedOverlap::sideEffects() const noexcept {
    return ok() ? std::span<const OverlapEffect>(data_->plan.sideEffects)
                : std::span<const OverlapEffect>{};
}

const std::optional<IncomingOverlapConflict>& PreparedOverlap::conflict() const noexcept {
    return data_ ? data_->plan.conflict : emptyConflict();
}

PreparedOverlap prepareOverlapDiff(const Snapshot& snapshot, const Diff& intent) {
    detail::OverlapPlan plan;
    try {
        const auto* captured = detail::SnapshotAccess::get(snapshot);
        if (!snapshot.ok || captured == nullptr) {
            plan = refused(OverlapStatus::InvalidSnapshot,
                snapshot.error.empty() ? "invalid notes overlap snapshot" : snapshot.error);
        } else {
            plan = detail::resolveOverlap(captured->baseline, intent);
        }
    } catch (const std::exception& exception) {
        plan = refused(OverlapStatus::PacketRejected, exception.what());
    }
    return detail::PreparedOverlapAccess::seal(snapshot, std::move(plan));
}

} // namespace adi::notes
