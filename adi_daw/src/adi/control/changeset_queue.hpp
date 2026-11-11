// SPDX-License-Identifier: AGPL-3.0-or-later
//
// AGT-01: the changeset as a service. Each proposal gets an id, the client
// that made it and its request id; the same request id queues once. The queue
// is application-scoped and in memory -- never in the `.adi`, which people
// email -- and a caller sees only its own entries.
//
// States: awaiting_review, applied, rejected, stale, expired. Nothing here
// applies anything: Apply is the user's act (the app's review panel, or
// `adi_tool propose --apply`), and a stale changeset is refused, never rebased
// (ADR-0148 d4).

#pragma once

#include "adi/changeset.hpp"
#include "adi/control/jsonrpc.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace adi::control {

/// Rejected is reserved for the app host's review panel (AGT-05, CTL-09):
/// nothing in this slice sets it yet, and changeset.status passes it through.
enum class Review { AwaitingReview, Applied, Rejected, Stale, Expired };
[[nodiscard]] std::string_view toString(Review);

struct QueuedChangeset {
    std::string id;
    std::string credential;       ///< whose it is; never shown
    std::string idempotencyKey;   ///< empty: none
    /// A digest of the screened params, the project's handle included: the
    /// same key with other arguments is refused, never answered with this.
    std::size_t paramsDigest = 0;
    std::string project;
    agent::Changeset changeset;
    Json proposal = Json::object();   ///< what changeset.propose answered
    Review state = Review::AwaitingReview;
    std::int64_t createdMs = 0;
};

/// The `adi_tool propose` JSON for a changeset (changeset.hpp's
/// changesetFromJson reads it back).
[[nodiscard]] Json changesetJson(const agent::Changeset&);

/// "cs-" and 16 random hex digits.
[[nodiscard]] std::string randomChangesetId();

class ChangesetQueue {
public:
    static constexpr std::size_t kCapacity = 256;
    static constexpr std::int64_t kReviewWindowMs = 24LL * 60 * 60 * 1000;

    explicit ChangesetQueue(std::function<std::string()> newId = randomChangesetId);

    [[nodiscard]] std::string nextId();

    /// The entry this credential already queued under this key, if any. The
    /// caller compares its paramsDigest and project before replaying it.
    [[nodiscard]] std::optional<QueuedChangeset> findByKey(const std::string& credential,
                                                           const std::string& key) const;
    /// The entry with this id, only if this credential owns it.
    [[nodiscard]] std::optional<QueuedChangeset> find(std::string_view id,
                                                      const std::string& credential) const;
    void add(QueuedChangeset entry);
    /// Moves an owned entry to `state`; false when there is none to move.
    bool setState(std::string_view id, const std::string& credential, Review state);
    /// Awaiting entries older than the review window become expired.
    void expire(std::int64_t nowMs);
    [[nodiscard]] std::size_t size() const;

private:
    std::function<std::string()> newId_;
    mutable std::mutex mutex_;
    std::vector<QueuedChangeset> entries_;
};

}  // namespace adi::control
