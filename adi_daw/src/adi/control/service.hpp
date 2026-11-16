// SPDX-License-Identifier: AGPL-3.0-or-later
//
// The control core: one dispatcher for every caller (ADR-0232; CTL-03).
//
// Transport-free and JUCE-free. A transport (stdio in adi-mcp, the loopback
// WebSocket of CTL-04, or an in-process caller such as a model-backed button)
// turns its bytes into `call()`s. Every call takes the same path, in this
// order, and nothing skips a step:
//
//   1. the GRANT, before anything else: a caller with no grant learns nothing,
//      not even which methods exist (−32006, one text whatever was wrong);
//   2. the method, and the tier it needs (a write needs more than Observe);
//   3. the idempotent replay, keyed by the credential plus the caller's key,
//      so a retried write is answered once and queued once; the same key
//      with other params is refused, never answered with the old outcome;
//   4. the method's screen, on the CALLING thread (ADR-0232 d4: validation
//      runs on the calling thread): params checked and normalised, every op
//      payload validated by the registry's validateSubmission and nothing
//      else (ADR-0039 d2: no second validator), refused with −32602;
//   5. the hop to the model's owner thread through the injected Executor:
//      the call is PENDING until the owner TAKES it (one CAS). A transport
//      whose deadline fires first ABANDONS it, and it never runs; one that
//      waited past its deadline without being abandoned is taken and
//      answered −32003, unrun -- never applied late;
//   6. the grant again, as the owner takes the call: one revoked while the
//      call waited is refused −32006 and the handler never runs;
//   7. the handler, which does what needs the owner: previews, commits.
//
// Grants are keyed by a credential the transport holds -- a paired surface's
// token, adi-mcp's per-process secret -- and never by the name a caller gives
// itself, which is a label for the audit and actor_detail only.

#pragma once

#include "adi/control/jsonrpc.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace adi::control {

/// AI-AGENT §2. Ordered: a higher tier can do everything a lower one can.
enum class Tier { Observe = 0, Propose = 1, Apply = 2 };
[[nodiscard]] std::string_view toString(Tier);
[[nodiscard]] std::optional<Tier> tierFromString(std::string_view);

/// Who is calling.
struct Principal {
    /// What grants are keyed by. Never logged, never echoed, never audited.
    std::string credential;
    /// What the caller calls itself ("Claude Code 2.1"): display and
    /// actor_detail only. Never consulted for a permission.
    std::string label;
};

/// The user's grants, by credential (ADR-0232 d5). Only the user raises a
/// tier: nothing reachable from the wire calls `grant`.
class GrantTable {
public:
    void grant(const std::string& credential, Tier tier);
    void revoke(const std::string& credential);
    [[nodiscard]] std::optional<Tier> tierOf(const std::string& credential) const;

private:
    mutable std::mutex mutex_;
    std::unordered_map<std::string, Tier> tiers_;
};

/// ADR-0232 d2: the `meta` sibling of `params`.
struct RequestMeta {
    /// Outer: check at all. Inner nullopt: the root, nothing done yet.
    std::optional<std::optional<std::int64_t>> expectHead;
    std::optional<std::int64_t> deadlineMs;
    std::optional<std::string> idempotencyKey;
};

/// A closed object: `expectHead` (integer or null), `deadlineMs` (1 to
/// 600000), `idempotencyKey` (1 to 128 bytes). Anything else is refused.
[[nodiscard]] std::optional<RequestMeta> parseMeta(const Json& meta, RpcError& error);

struct Outcome {
    Json result = nullptr;
    std::optional<RpcError> error;

    [[nodiscard]] static Outcome ok(Json result);
    [[nodiscard]] static Outcome fail(int code, std::string message, Json data = nullptr);
};

using Task = std::function<void()>;
/// Runs a task on the thread that owns the model. In the app,
/// MessageManager::callAsync (CTL-09). In adi-mcp --project, the bridge's own
/// thread, which owns every connection it holds (see inlineExecutor).
using Executor = std::function<void(Task)>;

/// Runs the task at once, on the calling thread. Only for a process whose
/// calling thread IS the owner of everything the handlers touch -- adi-mcp
/// --project, and tests. ADR-0232 rejected this as a fallback for a missing
/// message loop; here it is the declared owner, not a fallback.
[[nodiscard]] Executor inlineExecutor();

struct CallContext {
    const Principal& who;
    Tier tier;                  ///< the granted tier
    const RequestMeta& meta;
};

struct MethodSpec {
    std::string name;           ///< "project.read"
    std::string summary;
    Tier minTier = Tier::Observe;
    /// True: the handler touches the model's owner (a preview, a commit), so
    /// it runs through the Executor. False: it reads a read-only connection
    /// or an immutable snapshot, and runs on the calling thread.
    bool ownerThread = false;
    /// True: a call carrying an idempotencyKey is answered from the cache
    /// when the same credential sent the same key, with the same params,
    /// before; the same key with other params is refused (−32602).
    bool idempotent = false;
    bool readOnly = true;       ///< for MCP's readOnlyHint
    Json paramsSchema = Json::object();   ///< the closed schema of params; tools/list shows it
    /// Optional, on the CALLING thread before the hop: checks and normalises
    /// `params` in place, or returns the refusal. A heavy check -- thousands
    /// of op payloads -- then costs the owner thread nothing.
    std::function<std::optional<Outcome>(CallContext&, Json& params)> screen;
    std::function<Outcome(CallContext&, const Json& params)> run;
};

enum class CallState : int { Pending = 0, Taken = 1, Abandoned = 2 };

/// The transport's handle on one call it is waiting for.
class PendingCall {
public:
    /// The transport's deadline fired. True when this won the race: the
    /// handler will never run and `done` will never be called, so the
    /// transport answers −32003 itself. False: the call already started.
    bool abandon() noexcept;
    [[nodiscard]] CallState state() const noexcept;

private:
    friend class Service;
    bool take() noexcept;
    std::atomic<int> state_{static_cast<int>(CallState::Pending)};
};

struct AuditEvent {
    std::int64_t atMs = 0;
    std::string kind;     ///< connect, disconnect, refused, granted, revoked
    std::string label;    ///< the caller's own label; never its credential
    std::string method;
    int code = 0;
    std::string detail;   ///< secrets redacted
};

/// ADR-0232 d5: a ring of the last 512 connection events and refusals.
/// Ops are audited where they always were, in the op log. What a caller sent
/// never enters it -- a refusal is recorded by its code and its refusal kind,
/// not its message -- every field is capped, and every registered secret is
/// redacted.
class AuditRing {
public:
    static constexpr std::size_t kCapacity = 512;
    static constexpr std::size_t kMaxField = 256;   ///< bytes, cut on a UTF-8 boundary

    void registerSecret(std::string secret);
    void record(AuditEvent event);
    [[nodiscard]] std::vector<AuditEvent> snapshot() const;
    [[nodiscard]] std::string redact(std::string text) const;

private:
    std::string redactLocked(std::string text) const;
    mutable std::mutex mutex_;
    std::deque<AuditEvent> events_;
    std::vector<std::string> secrets_;
};

/// Compares in time that depends only on the lengths, never on where the
/// first difference is (ADR-0232 d5; the token check of CTL-04).
[[nodiscard]] bool secureEquals(std::string_view a, std::string_view b) noexcept;

struct ServiceOptions {
    std::int64_t defaultDeadlineMs = 10000;
    std::size_t idempotencyEntries = 256;
    /// A steady clock in milliseconds. Injected so tests own time.
    std::function<std::int64_t()> nowMs;
    /// Optional diagnostics sink (adi-mcp: stderr). An internal error says
    /// only "internal error" on the wire, which may reach a cloud model; what
    /// was thrown comes here instead, as "internal error in <method>: <what>",
    /// with every registered secret redacted.
    std::function<void(std::string_view)> log;
};

class Service {
public:
    Service(Executor ownerThread, GrantTable& grants, ServiceOptions options = {});

    /// Registers a method. Throws std::logic_error on a duplicate name, an
    /// empty handler, or a write method (readOnly false) below Propose.
    void add(MethodSpec spec);
    [[nodiscard]] const MethodSpec* find(std::string_view name) const;
    [[nodiscard]] std::vector<const MethodSpec*> methods() const;

    using Done = std::function<void(Outcome)>;

    /// One call. `done` runs exactly once -- on the owner thread for an
    /// ownerThread method, otherwise before this returns -- unless the
    /// returned handle's abandon() returned true first.
    std::shared_ptr<PendingCall> call(const Principal& who, std::string_view method,
                                      const Json& params, const RequestMeta& meta, Done done);

    [[nodiscard]] AuditRing& audit() { return audit_; }
    [[nodiscard]] GrantTable& grants() { return grants_; }
    [[nodiscard]] std::int64_t now() const;

private:
    /// A remembered answer, with a digest of the params it answered, so the
    /// same key sent with other params is refused rather than replayed.
    struct Cached {
        std::size_t paramsDigest = 0;
        Outcome outcome;
    };

    void remember(const std::string& key, std::size_t paramsDigest, const Outcome& outcome);
    [[nodiscard]] std::optional<Cached> recall(const std::string& key) const;
    /// The fixed wire answer for an exception out of a screen or a handler;
    /// what was thrown goes to options_.log, never to the caller.
    [[nodiscard]] Outcome internalError(std::string_view method, std::string_view what) const;

    Executor owner_;
    GrantTable& grants_;
    ServiceOptions options_;
    std::vector<std::unique_ptr<MethodSpec>> methods_;
    AuditRing audit_;

    mutable std::mutex cacheMutex_;
    std::unordered_map<std::string, Cached> cache_;
    std::deque<std::string> cacheOrder_;
};

}  // namespace adi::control
