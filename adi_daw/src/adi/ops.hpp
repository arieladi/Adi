// SPDX-License-Identifier: GPL-3.0-or-later
//
// The op vocabulary: registry, CBOR codec, and the transactional journal.
// OPS.md is the specification; this is the implementation of §3–§8.
//
// One rule governs everything here (ADR-0003):
//
//     Every mutation to the project appends its op row in the SAME SQLite
//     transaction that performs it. There is no valid state in which the
//     project changed and the log did not.
//
// And one more that is easy to lose (ADR-0021): an op never reads ambient
// state. Every value it acts on is in its payload, including the ids of objects
// it creates. That is what makes the log replayable rather than merely
// undoable, and replayability is what the round-trip corpus tests.

#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

namespace adi {

class Store;

/// A decoded op payload. CBOR on the wire (ADR-0025), this in memory.
using Payload = nlohmann::json;

// ---------------------------------------------------------------------------
// Scopes and impact — OPS.md §4, §5
// ---------------------------------------------------------------------------

enum class Scope { Read, Edit, Transport, Session, Hardware };

enum class EngineImpact {
    None,           // no audio-domain effect (renaming a track)
    Snapshot,       // applied live by publishing a new snapshot
    GraphRebuild,   // topology changed; graph prepared off-thread, then swapped
    RequiresPause,  // cannot be done live: engine stops, applies, restarts
};

/// OPS.md §8.1. The accountability column: "what did the agent change, and
/// when" is a query, not a forensic exercise.
enum class Actor { User, Agent, Script, Import, Migration, Remote };

std::string_view toString(Scope);
std::string_view toString(EngineImpact);
std::string_view toString(Actor);
std::optional<Actor> actorFromString(std::string_view);

// ---------------------------------------------------------------------------
// Payload schema — OPS.md §3 invariant 2
// ---------------------------------------------------------------------------

enum class FieldType { Int, Real, Text, Bool, Array, Object };

struct Field {
    std::string_view key;   // short, and as permanent as the op name (ADR-0025)
    FieldType type;
    bool required = true;
};

// ---------------------------------------------------------------------------
// The descriptor — OPS.md §3
// ---------------------------------------------------------------------------

struct OpContext;  // what a handler is allowed to touch

struct OpDescriptor {
    std::string_view name;     // "clip.move" -- domain.verb, permanent
    std::string_view summary;
    Scope scope = Scope::Read;
    EngineImpact engineImpact = EngineImpact::None;
    std::span<const Field> fields;
    bool coalescable = false;
    bool ephemeral = false;    // performance, not editing -- undo skips it

    /// Applies the op. Returns false and sets `err` on failure.
    using Apply = bool (*)(OpContext&, const Payload&, std::string& err);

    /// Builds the inverse from the state ABOUT TO BE OVERWRITTEN, before apply
    /// runs and inside the same transaction (OPS.md §6.1). If it cannot produce
    /// a correct inverse, the op does not commit.
    using BuildInverse = bool (*)(OpContext&, const Payload& forward,
                                  Payload& inverseOut, std::string& err);

    /// On the descriptor rather than in a name-keyed table, following MAGDA:
    /// a caller cannot reach a different implementation than the one declared,
    /// and there is one place to look.
    Apply apply = nullptr;
    BuildInverse buildInverse = nullptr;

    /// The op that undoes this one. Empty when the inverse is the same op with
    /// swapped arguments (OPS.md §6.2 "symmetric") or a state capture replayed
    /// through this same op.
    std::string_view inverseOp;
};

// ---------------------------------------------------------------------------
// The registry — OPS.md §3 invariants
// ---------------------------------------------------------------------------

class OpRegistry {
public:
    static const OpRegistry& instance();

    [[nodiscard]] const OpDescriptor* find(std::string_view name) const;
    [[nodiscard]] std::span<const OpDescriptor> all() const;

    /// Every invariant in OPS.md §3, as a list of human-readable violations.
    /// Empty means the registry is well-formed.
    ///
    /// `instance()` runs this and throws if it is non-empty, which is the
    /// "refuses to start" part: a write op that forgets its scope is caught
    /// here rather than by a client discovering it can edit.
    static std::vector<std::string> selfCheck(std::span<const OpDescriptor>);

private:
    OpRegistry();
    std::vector<OpDescriptor> ops_;
};

// ---------------------------------------------------------------------------
// Validation — and why there are two functions, not one
// ---------------------------------------------------------------------------
//
// OPS.md §3 says payload schemas are CLOSED: unknown fields are rejected. §8
// rule 3 says unknown keys are PRESERVED and re-emitted. Those look
// contradictory and are not — they are different directions of travel:
//
//   validateSubmission()  a client (UI, script, agent) is asking us to DO
//                         something. An unknown field is a typo or a version
//                         mismatch, and accepting it silently means the caller
//                         believes it set something it did not. Reject.
//
//   decodeFromLog()       we are READING an op some other build already
//                         committed. An unknown field is a newer version's, and
//                         dropping it would corrupt a log we are only carrying.
//                         Preserve.
//
// Conflating the two is how a format quietly loses data, so they are separate
// functions with separate names rather than a boolean parameter.

struct ValidationIssue {
    std::string key;
    std::string problem;
};

/// Strict: every required field present and correctly typed, no unknown keys,
/// no non-finite floats. For payloads arriving from a caller.
std::vector<ValidationIssue> validateSubmission(const OpDescriptor&, const Payload&);

/// Permissive: decodes CBOR and keeps everything, including fields this build
/// does not know. For payloads already in the log.
std::optional<Payload> decodeFromLog(std::span<const std::byte> cbor, std::string& err);

/// Deterministic CBOR (ADR-0025). Identical input encodes to identical bytes,
/// because nlohmann orders object keys with std::less<std::string>. That is
/// deterministic but NOT RFC 8949 §4.2 canonical, which orders by encoded bytes
/// and is therefore length-first. We need the former and do not claim the latter.
std::vector<std::byte> encodePayload(const Payload&);

// ---------------------------------------------------------------------------
// Submitting and committing ops
// ---------------------------------------------------------------------------

struct OpRequest {
    std::string opType;
    Payload payload;
    Actor actor = Actor::User;
    std::string actorDetail;
    std::string label;      // what the undo menu shows
    std::string targetKind;
    std::optional<std::int64_t> targetId;
};

struct CommitResult {
    bool ok = false;
    std::int64_t txnId = 0;
    std::vector<std::int64_t> seqs;   // one per op, in order
    std::string error;
    std::vector<ValidationIssue> issues;
};

/// Applies a batch of ops as ONE transaction and appends their rows in it.
///
/// All-or-nothing (SPEC §3.5). However many ops an agent request produces, they
/// share a txn_id and revert as one (OPS.md §6.4), and a failure anywhere rolls
/// back everything — including the log rows, so a partially-applied batch
/// cannot be left describing itself as complete.
class OpJournal {
public:
    explicit OpJournal(Store&);

    CommitResult commit(std::span<const OpRequest>);
    CommitResult commit(const OpRequest& one) { return commit({&one, 1}); }

    /// Ops in the log, newest first. For audit and for tests.
    struct LoggedOp {
        std::int64_t seq = 0;
        std::int64_t txnId = 0;
        std::int64_t tsUtc = 0;
        std::string actor;
        std::string actorDetail;
        std::string opType;
        std::string label;
        Payload payload;
        std::optional<Payload> inverse;
    };
    [[nodiscard]] std::vector<LoggedOp> recent(int limit = 50) const;
    [[nodiscard]] std::int64_t count() const;

private:
    Store& store_;
};

}  // namespace adi
