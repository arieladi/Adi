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
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace adi::control {

class ProjectAccess;

/// Stale is computed from the current head and can clear after Undo.
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
    /// Mandatory review even when the effective tier is Apply (ADR-0261 d8).
    bool requiresReview = false;
    std::string projectUuid;      ///< machine-local log routing, never shown on the wire
    std::weak_ptr<ProjectAccess> projectAccess;  ///< exactly the attachment that previewed the edit
};

struct RejectedChangeset {
    std::string id;
    std::string projectUuid;
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

    /// The local observer runs in transition order and must not call the queue.
    using Observer = std::function<void(const QueuedChangeset&, bool initial, std::string_view actor,
                                        std::string_view source)>;
    explicit ChangesetQueue(std::function<std::string()> newId = randomChangesetId, Observer = {});

    [[nodiscard]] std::string nextId();

    /// The entry this credential already queued under this key, if any. The
    /// caller compares its paramsDigest and project before replaying it.
    [[nodiscard]] std::optional<QueuedChangeset> findByKey(const std::string& credential,
                                                           const std::string& key) const;
    /// The entry with this id, only if this credential owns it.
    [[nodiscard]] std::optional<QueuedChangeset> find(std::string_view id,
                                                      const std::string& credential) const;
    /// Owner-side review seam. Never exposed by the control transport.
    [[nodiscard]] std::vector<QueuedChangeset> forReview() const;
    /// Stop Agent: the local user rejects every proposal still awaiting a decision.
    [[nodiscard]] std::vector<RejectedChangeset> rejectPending(std::string_view surface);
    void add(QueuedChangeset entry);
    /// Moves an owned entry to `state`; false when there is none to move.
    bool setState(std::string_view id, const std::string& credential, Review state,
                  std::string_view actor = "system", std::string_view source = "control");
    /// Awaiting entries older than the review window become expired.
    void expire(std::int64_t nowMs);
    [[nodiscard]] std::size_t size() const;

private:
    std::function<std::string()> newId_;
    Observer observer_;
    mutable std::mutex mutex_;
    std::vector<QueuedChangeset> entries_;
};

}  // namespace adi::control
