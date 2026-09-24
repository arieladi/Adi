// SPDX-License-Identifier: GPL-3.0-or-later
//
// The Propose-tier changeset: ADR-0145 d9, AI-AGENT §6, ADR-0148.
//
// An agent at Propose tier does not commit. It builds a Changeset -- the ops
// it wants, the head it saw, and what the user asked -- and the user sees a
// diff. Nothing reaches the file until Apply, and Apply is ONE transaction and
// ONE undo step (AI-AGENT §6.2), with the request text written beside it in
// the same transaction (§6.8, SPEC §8.7).
//
// Three things here are guardrails rather than conveniences, and each is
// enforced at preview AND again at apply, because a preview is advice and the
// apply is what writes:
//
//   * the op cap (§6.6)            -- a request is bounded;
//   * the op allowlist             -- only project edits, never transport or
//                                     hardware, and no media op that could
//                                     reach a file on disk (§6.4);
//   * the stale check              -- a changeset built on a head that has
//                                     since moved describes a project that no
//                                     longer exists, so it is refused, never
//                                     rebased silently.

#pragma once

#include "adi/ops.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace adi {
class Store;
}

namespace adi::agent {

struct Limits {
    /// §6.6: per-request op cap. Visible and editable in the UI; this is the
    /// default a request gets when nobody set one.
    std::size_t maxOps = 256;
};

struct Changeset {
    /// The head the agent built on. Apply refuses if the head has moved.
    std::optional<std::int64_t> baseHead;
    std::vector<OpRequest> ops;
    std::string actorDetail;    // model and version, as ops.actor_detail
    std::string request;        // what the user asked, for agent_requests
    std::int64_t createdUtc = 0;   // passed in, never read from a clock
};

/// A changeset on the store's current head, with nothing queued yet.
[[nodiscard]] Changeset begin(const Store&, std::string request, std::string actorDetail,
                              std::int64_t createdUtc);

enum class Refusal {
    None,
    Empty,          // nothing queued
    TooManyOps,     // over Limits::maxOps
    OpNotAllowed,   // outside what the agent may emit
    Invalid,        // a payload that fails validation, or an op that fails to apply
    Stale,          // the head moved since the changeset was built
    ReadOnly,
    NoRequestTable, // a file older than 1.4 opened read-only: nowhere to write §6.8's row
};
[[nodiscard]] const char* toString(Refusal);

/// One queued op as the user reviews it. `before` is the state the op will
/// overwrite (its inverse, built from the file); `after` is what it writes.
/// A creation has no `before`, a deletion no `after`.
struct Change {
    std::string label;
    std::string opType;
    std::string targetKind;
    std::optional<std::int64_t> targetId;
    std::optional<Payload> before;
    std::optional<Payload> after;
};

struct Preview {
    bool ok = false;
    Refusal refusal = Refusal::None;
    std::string error;
    std::string textBefore;     // the text projection now
    std::string textAfter;      // ... and as the changeset would leave it
    std::string unifiedDiff;    // of the two, `--- before` / `+++ after`
    std::vector<Change> changes;
};

/// Runs every op inside a transaction that is always rolled back, projects
/// the result, and throws the writes away (ADR-0148). The file is not written.
[[nodiscard]] Preview preview(Store&, const Changeset&, const Limits& = {});

struct ApplyResult {
    bool ok = false;
    Refusal refusal = Refusal::None;
    std::string error;
    std::int64_t txnId = 0;
};

/// Commits the whole changeset as one transaction, actor `agent`, with its
/// agent_requests row in that same transaction; or refuses and writes nothing.
[[nodiscard]] ApplyResult apply(Store&, const Changeset&, const Limits& = {});

/// The allowlist. Project edits only (Scope::Edit, not ephemeral), and of
/// the media ops only those that change a reference, never a file.
[[nodiscard]] bool agentMayEmit(std::string_view opType);

/// A unified diff of two texts, line by line (Myers), with `context` lines
/// around each hunk. Empty when the texts are equal.
[[nodiscard]] std::string unifiedDiff(const std::string& before, const std::string& after,
                                      std::string_view nameBefore = "before",
                                      std::string_view nameAfter = "after",
                                      std::size_t context = 3);

/// The JSON form `adi_tool propose` reads:
///   {"request": "...", "actorDetail": "...", "createdUtc": 179...,
///    "baseHead": 12 | null (optional),
///    "ops": [{"op": "mixer.setVolume", "payload": {...},
///             "label": "...", "targetKind": "track", "targetId": 2}]}
/// `baseHead` absent means "the head now", which is what a changeset built
/// in the same session means; a file written earlier should carry it.
[[nodiscard]] std::optional<Changeset> changesetFromJson(const Store&, const Payload&,
                                                         std::string& error);

}  // namespace adi::agent
