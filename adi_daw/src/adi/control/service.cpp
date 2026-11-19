// SPDX-License-Identifier: AGPL-3.0-or-later
#include "adi/control/service.hpp"

#include <algorithm>
#include <chrono>
#include <exception>
#include <stdexcept>
#include <utility>

namespace adi::control {
namespace {

/// ADR-0232 d5: one refusal text whatever was wrong -- no grant, a revoked
/// grant, or a tier too low -- so a refusal never tells a caller which.
constexpr const char* kNotGranted = "not granted";

/// At most `max` bytes, never ending inside a UTF-8 sequence.
std::string clipUtf8(std::string text, std::size_t max) {
    if (text.size() <= max) return text;
    std::size_t cut = max;
    while (cut > 0 && (static_cast<unsigned char>(text[cut]) & 0xC0u) == 0x80u) --cut;
    text.resize(cut);
    return text;
}

/// What the audit keeps of a refusal: its code's name and, when the method
/// gave one, its refusal kind. Never the message, which may quote the request.
std::string auditDetail(const RpcError& e) {
    std::string name;
    switch (e.code) {
        case errc::ParseError: name = "parse error"; break;
        case errc::InvalidRequest: name = "invalid request"; break;
        case errc::MethodNotFound: name = "unknown method"; break;
        case errc::InvalidParams: name = "invalid params"; break;
        case errc::Stale: name = "stale"; break;
        case errc::DeadlinePassed: name = "deadline passed"; break;
        case errc::OverLimit: name = "over a limit"; break;
        case errc::NotGranted: name = kNotGranted; break;
        default: name = "error " + std::to_string(e.code); break;
    }
    if (e.data.is_object()) {
        const auto r = e.data.find("refusal");
        if (r != e.data.end() && r->is_string()) name += ": " + clipUtf8(r->get<std::string>(), 32);
    }
    return name;
}

}  // namespace

std::string_view toString(Tier t) {
    switch (t) {
        case Tier::Observe: return "observe";
        case Tier::Propose: return "propose";
        case Tier::Apply: return "apply";
    }
    return "observe";
}

std::optional<Tier> tierFromString(std::string_view s) {
    if (s == "observe") return Tier::Observe;
    if (s == "propose") return Tier::Propose;
    if (s == "apply") return Tier::Apply;
    return std::nullopt;
}

// --- grants ------------------------------------------------------------------------

void GrantTable::grant(const std::string& credential, Tier tier) {
    std::lock_guard lock(mutex_);
    tiers_[credential] = tier;
}

void GrantTable::revoke(const std::string& credential) {
    std::lock_guard lock(mutex_);
    tiers_.erase(credential);
}

std::optional<Tier> GrantTable::tierOf(const std::string& credential) const {
    if (credential.empty()) return std::nullopt;   // an empty credential is never granted
    std::lock_guard lock(mutex_);
    const auto it = tiers_.find(credential);
    if (it == tiers_.end()) return std::nullopt;
    return it->second;
}

// --- meta --------------------------------------------------------------------------

std::optional<RequestMeta> parseMeta(const Json& meta, RpcError& error) {
    RequestMeta m;
    if (meta.is_null()) return m;
    if (!meta.is_object()) {
        error = {errc::InvalidParams, "\"meta\" must be an object", nullptr};
        return std::nullopt;
    }
    for (auto it = meta.begin(); it != meta.end(); ++it) {
        const auto& key = it.key();
        const auto& v = it.value();
        if (key == "expectHead") {
            if (v.is_null()) m.expectHead = std::optional<std::int64_t>{};
            else if (v.is_number_integer() && !v.is_number_unsigned())
                m.expectHead = std::optional<std::int64_t>{v.get<std::int64_t>()};
            else if (v.is_number_unsigned() &&
                     v.get<std::uint64_t>() <= static_cast<std::uint64_t>(INT64_MAX))
                m.expectHead = std::optional<std::int64_t>{static_cast<std::int64_t>(v.get<std::uint64_t>())};
            else {
                error = {errc::InvalidParams, "meta.expectHead is an integer or null", nullptr};
                return std::nullopt;
            }
        } else if (key == "deadlineMs") {
            if (!v.is_number_integer() || v.get<std::int64_t>() < 1 || v.get<std::int64_t>() > 600000) {
                error = {errc::InvalidParams, "meta.deadlineMs is an integer from 1 to 600000", nullptr};
                return std::nullopt;
            }
            m.deadlineMs = v.get<std::int64_t>();
        } else if (key == "idempotencyKey") {
            if (!v.is_string() || v.get_ref<const std::string&>().empty() ||
                v.get_ref<const std::string&>().size() > 128) {
                error = {errc::InvalidParams, "meta.idempotencyKey is a string of 1 to 128 bytes",
                         nullptr};
                return std::nullopt;
            }
            m.idempotencyKey = v.get<std::string>();
        } else {
            error = {errc::InvalidParams, "unknown meta key: " + clipEcho(key), nullptr};
            return std::nullopt;
        }
    }
    return m;
}

// --- outcomes ----------------------------------------------------------------------

Outcome Outcome::ok(Json result) {
    Outcome o;
    o.result = std::move(result);
    return o;
}

Outcome Outcome::fail(int code, std::string message, Json data) {
    Outcome o;
    o.error = RpcError{code, std::move(message), std::move(data)};
    return o;
}

Executor inlineExecutor() {
    return [](Task task) { task(); };
}

// --- pending calls -----------------------------------------------------------------

bool PendingCall::abandon() noexcept {
    int expected = static_cast<int>(CallState::Pending);
    return state_.compare_exchange_strong(expected, static_cast<int>(CallState::Abandoned),
                                          std::memory_order_acq_rel);
}

bool PendingCall::take() noexcept {
    int expected = static_cast<int>(CallState::Pending);
    return state_.compare_exchange_strong(expected, static_cast<int>(CallState::Taken),
                                          std::memory_order_acq_rel);
}

CallState PendingCall::state() const noexcept {
    return static_cast<CallState>(state_.load(std::memory_order_acquire));
}

// --- audit -------------------------------------------------------------------------

void AuditRing::registerSecret(std::string secret) {
    if (secret.empty()) return;
    std::lock_guard lock(mutex_);
    secrets_.push_back(std::move(secret));
}

std::string AuditRing::redactLocked(std::string text) const {
    for (const auto& s : secrets_) {
        for (std::size_t at = text.find(s); at != std::string::npos; at = text.find(s, at)) {
            text.replace(at, s.size(), "[redacted]");
            at += 10;
        }
    }
    return text;
}

std::string AuditRing::redact(std::string text) const {
    std::lock_guard lock(mutex_);
    return redactLocked(std::move(text));
}

void AuditRing::record(AuditEvent event) {
    std::lock_guard lock(mutex_);
    event.detail = clipUtf8(redactLocked(std::move(event.detail)), kMaxField);
    event.label = clipUtf8(redactLocked(std::move(event.label)), kMaxField);
    event.method = clipUtf8(redactLocked(std::move(event.method)), kMaxField);
    event.kind = clipUtf8(std::move(event.kind), kMaxField);
    events_.push_back(std::move(event));
    while (events_.size() > kCapacity) events_.pop_front();
}

std::vector<AuditEvent> AuditRing::snapshot() const {
    std::lock_guard lock(mutex_);
    return {events_.begin(), events_.end()};
}

bool secureEquals(std::string_view a, std::string_view b) noexcept {
    // The lengths are not secret (a token's length is fixed and public); the
    // content is. Every byte of the longer string is visited and nothing
    // branches on content. The accumulator is volatile, an optimisation
    // barrier: every byte's difference must be stored, so the compiler cannot
    // turn the loop into an early-exit compare. It is updated by plain
    // assignment, since C++20 deprecates compound assignment to a volatile.
    volatile unsigned char diff = 0;
    const std::size_t n = std::max(a.size(), b.size());
    for (std::size_t i = 0; i < n; ++i) {
        const auto x = i < a.size() ? static_cast<unsigned char>(a[i]) : static_cast<unsigned char>(0);
        const auto y = i < b.size() ? static_cast<unsigned char>(b[i]) : static_cast<unsigned char>(0);
        diff = static_cast<unsigned char>(diff | static_cast<unsigned char>(x ^ y));
    }
    const unsigned char contentDiff = diff;
    return (static_cast<std::size_t>(contentDiff) | (a.size() ^ b.size())) == 0;
}

// --- the service -------------------------------------------------------------------

Service::Service(Executor ownerThread, GrantTable& grants, ServiceOptions options)
    : owner_(std::move(ownerThread)), grants_(grants), options_(std::move(options)) {
    if (!owner_) throw std::logic_error("control::Service needs an executor");
    if (!options_.nowMs) {
        options_.nowMs = [] {
            using namespace std::chrono;
            return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
        };
    }
}

std::int64_t Service::now() const { return options_.nowMs(); }

void Service::add(MethodSpec spec) {
    if (spec.name.empty() || !spec.run) throw std::logic_error("a control method needs a name and a handler");
    if (find(spec.name)) throw std::logic_error("duplicate control method: " + spec.name);
    // ADR-0039 d3 at registration: a method that writes cannot be reachable
    // at Observe. "Refuses to start" means this throws, not a log line.
    if (!spec.readOnly && spec.minTier < Tier::Propose)
        throw std::logic_error("a write method below Propose: " + spec.name);
    methods_.push_back(std::make_unique<MethodSpec>(std::move(spec)));
}

const MethodSpec* Service::find(std::string_view name) const {
    for (const auto& m : methods_)
        if (m->name == name) return m.get();
    return nullptr;
}

std::vector<const MethodSpec*> Service::methods() const {
    std::vector<const MethodSpec*> out;
    out.reserve(methods_.size());
    for (const auto& m : methods_) out.push_back(m.get());
    return out;
}

void Service::remember(const std::string& key, std::size_t paramsDigest, const Outcome& outcome) {
    std::lock_guard lock(cacheMutex_);
    if (cache_.count(key)) return;
    cache_.emplace(key, Cached{paramsDigest, outcome});
    cacheOrder_.push_back(key);
    while (cacheOrder_.size() > options_.idempotencyEntries) {
        cache_.erase(cacheOrder_.front());
        cacheOrder_.pop_front();
    }
}

std::optional<Service::Cached> Service::recall(const std::string& key) const {
    std::lock_guard lock(cacheMutex_);
    const auto it = cache_.find(key);
    if (it == cache_.end()) return std::nullopt;
    return it->second;
}

Outcome Service::internalError(std::string_view method, std::string_view what) const {
    if (options_.log) {
        // A sink that throws (or a line that cannot be allocated) must not
        // turn one failed call into a crash of the dispatcher; the caller
        // still gets its fixed answer.
        try {
            std::string line = "internal error in ";
            line += method;
            line += ": ";
            line += what;
            options_.log(audit_.redact(std::move(line)));
        } catch (...) {
            // nothing more to do: the log is diagnostics only
        }
    }
    return Outcome::fail(errc::Internal, "internal error");
}

std::shared_ptr<PendingCall> Service::call(const Principal& who, std::string_view method,
                                           const Json& params, const RequestMeta& meta, Done done) {
    auto pending = std::make_shared<PendingCall>();
    const std::string methodName(method);

    // Every refusal is audited, by its code and kind. Of what the caller sent,
    // only its self-declared label is kept (capped and redacted by the ring);
    // nothing else it sent is.
    Done finish = [this, label = who.label, methodName, done = std::move(done)](Outcome out) {
        if (out.error && out.error->code != errc::Internal)
            audit_.record({now(), "refused", label, methodName, out.error->code, auditDetail(*out.error)});
        done(std::move(out));
    };

    // 1. The grant, before anything else.
    const auto tier = grants_.tierOf(who.credential);
    if (!tier) {
        pending->take();
        finish(Outcome::fail(errc::NotGranted, kNotGranted));
        return pending;
    }
    // 2. The method, and the tier it needs.
    const MethodSpec* spec = find(method);
    if (!spec) {
        pending->take();
        finish(Outcome::fail(errc::MethodNotFound, "unknown method: " + clipEcho(methodName)));
        return pending;
    }
    if (*tier < spec->minTier) {
        pending->take();
        finish(Outcome::fail(errc::NotGranted, kNotGranted));
        return pending;
    }
    // 3. The idempotent replay: the same credential, method and key, and the
    //    same params. The digest is of the params as sent, before the screen
    //    normalises them, so equal requests match byte for byte (an object's
    //    keys dump in order). The same key with other params is a caller's
    //    mistake -- a corrected retry under the old key -- and is refused, not
    //    answered with the old outcome. A hash collision could only replay
    //    the caller's own earlier answer to itself. meta.expectHead is an
    //    argument too (a method may take the head from it), so it is in the
    //    digest: a retry with another head is not answered with the old one.
    std::string cacheKey;
    std::size_t paramsDigest = 0;
    if (spec->idempotent && meta.idempotencyKey) {
        cacheKey = who.credential;
        cacheKey += '\n';
        cacheKey += spec->name;
        cacheKey += '\n';
        cacheKey += *meta.idempotencyKey;
        std::string digested = dumpJson(params);
        digested += '\n';
        digested += !meta.expectHead    ? std::string("-")
                    : *meta.expectHead ? std::to_string(**meta.expectHead)
                                       : std::string("null");
        paramsDigest = std::hash<std::string>{}(digested);
        if (auto hit = recall(cacheKey)) {
            pending->take();
            if (hit->paramsDigest != paramsDigest)
                finish(Outcome::fail(errc::InvalidParams, "idempotencyKey reused with different arguments",
                                     {{"refusal", "invalid"}}));
            else
                finish(std::move(hit->outcome));
            return pending;
        }
    }
    // 4. The screen, on this (the calling) thread: params checked and
    //    normalised, payloads validated, before the owner sees anything.
    Json prepared = params;
    if (spec->screen) {
        CallContext screenCtx{who, *tier, meta};
        std::optional<Outcome> refusal;
        try {
            refusal = spec->screen(screenCtx, prepared);
        } catch (const std::exception& e) {
            refusal = internalError(methodName, e.what());
        } catch (...) {
            refusal = internalError(methodName, "unknown exception");
        }
        if (refusal) {
            pending->take();
            // A refusal other than Internal is deterministic for equal params,
            // so it is remembered; an Internal one may pass on a retry.
            if (!cacheKey.empty() && !(refusal->error && refusal->error->code == errc::Internal))
                remember(cacheKey, paramsDigest, *refusal);
            finish(std::move(*refusal));
            return pending;
        }
    }
    // 5. The deadline, and the hop to the owner.
    const std::int64_t deadlineAt = now() + meta.deadlineMs.value_or(options_.defaultDeadlineMs);
    Task run = [this, spec, who, meta, params = std::move(prepared), cacheKey, paramsDigest, deadlineAt,
                pending, finish]() mutable {
        if (!pending->take()) return;   // the transport abandoned it and answered
        if (now() > deadlineAt) {
            // It waited past its deadline and nobody abandoned it: refuse
            // rather than apply late (ADR-0232 d5).
            finish(Outcome::fail(errc::DeadlinePassed,
                                 "the deadline passed before the request ran; nothing was applied"));
            return;
        }
        // The grant again, now that the owner has the call: the user may have
        // revoked or lowered it while the call waited in the queue (ADR-0232
        // d5). The same one refusal text; never remembered, so a later grant
        // is not answered from a stale refusal.
        const auto granted = grants_.tierOf(who.credential);
        if (!granted || *granted < spec->minTier) {
            finish(Outcome::fail(errc::NotGranted, kNotGranted));
            return;
        }
        CallContext ctx{who, *granted, meta};
        Outcome out;
        try {
            out = spec->run(ctx, params);
        } catch (const std::exception& e) {
            out = internalError(spec->name, e.what());
        } catch (...) {
            out = internalError(spec->name, "unknown exception");
        }
        if (!cacheKey.empty() && !(out.error && out.error->code == errc::Internal))
            remember(cacheKey, paramsDigest, out);
        finish(std::move(out));
    };
    if (spec->ownerThread) owner_(std::move(run));
    else run();
    return pending;
}

}  // namespace adi::control
