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
//      so a retried write is answered once and queued once;
//   4. the hop to the model's owner thread through the injected Executor,
//      where a request whose deadline passed while it waited is ABANDONED
//      and never runs late (PENDING -> TAKEN or ABANDONED, one CAS);
//   5. the handler, which validates its own params and refuses with
//      −32602; op payloads are validated by the registry's validateSubmission
//      and nothing else (ADR-0039 d2: no second validator).
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
/// 600000), `idempotencyKey` (1 to 128 characters). Anything else is refused.
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
    /// when the same credential sent the same key before.
    bool idempotent = false;
    bool readOnly = true;       ///< for MCP's readOnlyHint
    Json paramsSchema = Json::object();   ///< the closed schema of params; tools/list shows it
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
/// Ops are audited where they always were, in the op log. Payloads never
/// enter it, and every registered secret is redacted from what does.
class AuditRing {
public:
    static constexpr std::size_t kCapacity = 512;

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
    void remember(const std::string& key, const Outcome& outcome);
    [[nodiscard]] std::optional<Outcome> recall(const std::string& key) const;

    Executor owner_;
    GrantTable& grants_;
    ServiceOptions options_;
    std::vector<std::unique_ptr<MethodSpec>> methods_;
    AuditRing audit_;

    mutable std::mutex cacheMutex_;
    std::unordered_map<std::string, Outcome> cache_;
    std::deque<std::string> cacheOrder_;
};

}  // namespace adi::control
