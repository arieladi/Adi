// SPDX-License-Identifier: AGPL-3.0-or-later
//
// The control core's permission and dispatch rules (ADR-0232 d5; CTL-03;
// docs/CONTROL-MCP.md §4.3, §8), in process, with no project and no socket:
//
//   * the grant comes before anything else, with one refusal text;
//   * a write needs more than Observe, and a revoked grant is never replayed;
//   * idempotent replay is keyed by the credential and the caller's key;
//   * a request whose deadline passed while it waited never runs late;
//   * secrets never reach the audit, and payloads never enter it;
//   * the wire is strict and bounded before it is parsed.

#include "adi/control/jsonrpc.hpp"
#include "adi/control/service.hpp"
#include "adi/control/stdio_server.hpp"

#include <cstdio>
#include <deque>
#include <exception>
#include <functional>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>

using namespace adi::control;

namespace {

int g_failures = 0;
int g_checks = 0;

void check(bool cond, const std::string& what) {
    ++g_checks;
    if (!cond) {
        ++g_failures;
        std::printf("  FAIL  %s\n", what.c_str());
    }
}

void section(const char* s) { std::printf("[%s]\n", s); }

/// An executor the test drains by hand, so "waiting on the message thread"
/// is something a test can arrange.
struct ManualExecutor {
    std::deque<Task> tasks;
    Executor executor() {
        return [this](Task t) { tasks.push_back(std::move(t)); };
    }
    void drain() {
        while (!tasks.empty()) {
            auto t = std::move(tasks.front());
            tasks.pop_front();
            t();
        }
    }
};

struct Rig {
    GrantTable grants;
    std::int64_t clock = 1000;
    int readRuns = 0;
    int writeRuns = 0;
    Service service;
    explicit Rig(Executor ex)
        : service(std::move(ex), grants, ServiceOptions{10000, 256, [this] { return clock; }}) {
        MethodSpec read;
        read.name = "test.read";
        read.summary = "a read";
        read.run = [this](CallContext&, const Json& p) -> Outcome {
            ++readRuns;
            if (p.is_object() && p.contains("bad")) return Outcome::fail(errc::InvalidParams, "bad param");
            return Outcome::ok({{"read", true}});
        };
        service.add(std::move(read));
        MethodSpec write;
        write.name = "test.write";
        write.summary = "a write";
        write.minTier = Tier::Propose;
        write.readOnly = false;
        write.idempotent = true;
        write.ownerThread = true;
        write.run = [this](CallContext& c, const Json&) -> Outcome {
            ++writeRuns;
            return Outcome::ok({{"n", writeRuns}, {"by", c.who.label}});
        };
        service.add(std::move(write));
    }
    std::optional<Outcome> call(const Principal& who, const char* method, Json params = Json::object(),
                                RequestMeta meta = {}) {
        std::optional<Outcome> out;
        service.call(who, method, params, meta, [&out](Outcome o) { out = std::move(o); });
        return out;
    }
};

bool refusedNotGranted(const std::optional<Outcome>& o) {
    return o && o->error && o->error->code == errc::NotGranted && o->error->message == "not granted";
}

void testGrantsComeFirst() {
    section("the grant before anything: no grant learns nothing, one refusal text");
    Rig rig(inlineExecutor());
    const Principal stranger{"cred-stranger", "Stranger 1.0"};
    check(refusedNotGranted(rig.call(stranger, "test.read")), "no grant: a read is refused");
    check(refusedNotGranted(rig.call(stranger, "no.such.method")),
          "no grant: an unknown method gets the same refusal, so methods cannot be probed");
    check(refusedNotGranted(rig.call(stranger, "test.read", {{"bad", 1}})),
          "permission is checked before validation: bad params are not even looked at");
    check(rig.readRuns == 0, "and no handler ran");

    const Principal empty{"", "Nobody"};
    rig.grants.grant("", Tier::Apply);
    check(refusedNotGranted(rig.call(empty, "test.read")), "an empty credential is never granted");

    const Principal namedLikeOwner{"cred-other", "Owner"};
    rig.grants.grant("cred-owner", Tier::Apply);
    check(refusedNotGranted(rig.call(namedLikeOwner, "test.write")),
          "grants are keyed by credential: calling yourself by a granted client's name gets nothing");
}

void testTiers() {
    section("tiers: a write needs more than Observe; a revoked grant is never replayed");
    Rig rig(inlineExecutor());
    const Principal p{"cred-a", "Client A"};
    rig.grants.grant(p.credential, Tier::Observe);
    auto r = rig.call(p, "test.read");
    check(r && !r->error && r->result.value("read", false), "Observe reads");
    check(refusedNotGranted(rig.call(p, "test.write")), "Observe cannot write: the same refusal text");
    auto unknown = rig.call(p, "no.such.method");
    check(unknown && unknown->error && unknown->error->code == errc::MethodNotFound,
          "a granted caller learns that a method does not exist (−32601)");

    rig.grants.grant(p.credential, Tier::Propose);
    RequestMeta keyed;
    keyed.idempotencyKey = "k1";
    auto w = rig.call(p, "test.write", Json::object(), keyed);
    check(w && !w->error && rig.writeRuns == 1, "Propose writes");

    rig.grants.revoke(p.credential);
    check(refusedNotGranted(rig.call(p, "test.write", Json::object(), keyed)),
          "revoked: the cached answer for the same key is NOT replayed");
    check(rig.writeRuns == 1, "and nothing ran");
}

void testIdempotency() {
    section("idempotent replay: the credential plus the key");
    Rig rig(inlineExecutor());
    const Principal a{"cred-a", "A"}, b{"cred-b", "B"};
    rig.grants.grant(a.credential, Tier::Propose);
    rig.grants.grant(b.credential, Tier::Propose);
    RequestMeta k;
    k.idempotencyKey = "same";
    auto first = rig.call(a, "test.write", Json::object(), k);
    auto again = rig.call(a, "test.write", Json::object(), k);
    check(rig.writeRuns == 1 && first && again && first->result == again->result,
          "the same caller and key: answered once, the same answer");
    auto other = rig.call(b, "test.write", Json::object(), k);
    check(rig.writeRuns == 2 && other && other->result.value("by", "") == "B",
          "another caller with the same key is a different request");
    rig.call(a, "test.write");
    rig.call(a, "test.write");
    check(rig.writeRuns == 4, "no key: every call runs");
}

void testDeadlines() {
    section("deadlines: PENDING, TAKEN or ABANDONED; never applied late");
    ManualExecutor manual;
    Rig rig(manual.executor());
    const Principal p{"cred-a", "A"};
    rig.grants.grant(p.credential, Tier::Propose);

    std::optional<Outcome> out;
    RequestMeta m;
    m.deadlineMs = 50;
    auto pending = rig.service.call(p, "test.write", Json::object(), m, [&out](Outcome o) { out = std::move(o); });
    check(pending->state() == CallState::Pending && !out, "queued for the owner thread: pending");
    check(pending->abandon(), "the transport's deadline fires first: abandoned");
    manual.drain();
    check(rig.writeRuns == 0 && !out, "the owner thread later skips it: never run, never answered twice");
    check(!pending->abandon(), "abandoning twice does nothing");

    std::optional<Outcome> late;
    rig.service.call(p, "test.write", Json::object(), m, [&late](Outcome o) { late = std::move(o); });
    rig.clock += 51;   // the deadline passes while it waits, and nobody abandons it
    manual.drain();
    check(rig.writeRuns == 0 && late && late->error && late->error->code == errc::DeadlinePassed,
          "run after its deadline: refused (−32003), nothing applied");

    std::optional<Outcome> ok;
    auto p3 = rig.service.call(p, "test.write", Json::object(), m, [&ok](Outcome o) { ok = std::move(o); });
    manual.drain();
    check(rig.writeRuns == 1 && ok && !ok->error && p3->state() == CallState::Taken, "in time: taken and run");
    check(!p3->abandon(), "a call already taken cannot be abandoned");
}

void testAudit() {
    section("the audit: refusals recorded, secrets redacted, payloads never stored");
    Rig rig(inlineExecutor());
    const std::string secret = "tok-0123456789abcdef";
    rig.service.audit().registerSecret(secret);
    const Principal p{"cred-x", "client " + secret};
    rig.call(p, "test.read", {{"payload-marker", "do not keep"}});
    const auto events = rig.service.audit().snapshot();
    check(!events.empty() && events.back().kind == "refused" && events.back().code == errc::NotGranted,
          "a refusal is audited");
    bool leaked = false, payload = false;
    for (const auto& e : events) {
        leaked = leaked || e.label.find(secret) != std::string::npos || e.detail.find(secret) != std::string::npos;
        payload = payload || e.detail.find("payload-marker") != std::string::npos;
    }
    check(!leaked, "a registered secret never reaches the audit");
    check(!payload, "a payload never enters it");
    check(rig.service.audit().redact("a " + secret + " b") == "a [redacted] b", "redact replaces every occurrence");

    // A GRANTED caller whose request reaches a handler that refuses, quoting
    // what it was sent: the audit keeps the code and the kind, not the text.
    MethodSpec echo;
    echo.name = "test.echo";
    echo.run = [](CallContext&, const Json& params) -> Outcome {
        return Outcome::fail(errc::InvalidParams, "rejected " + params.dump(), {{"refusal", "invalid"}});
    };
    rig.service.add(std::move(echo));
    const Principal granted{"cred-g", "granted client"};
    rig.grants.grant(granted.credential, Tier::Observe);
    rig.call(granted, "test.echo", {{"payload-marker", std::string(4096, 'p')}});
    const auto after = rig.service.audit().snapshot();
    check(!after.empty() && after.back().detail == "invalid params: invalid" &&
              after.back().detail.find("payload-marker") == std::string::npos,
          "a granted caller's refusal is audited by its code and kind, never its text");

    AuditRing ring;
    ring.record({0, "connect", std::string(1000, 'L'), std::string(1000, 'M'), 0, std::string(1000, 'D')});
    const auto capped = ring.snapshot();
    check(capped.back().label.size() <= AuditRing::kMaxField && capped.back().method.size() <= AuditRing::kMaxField &&
              capped.back().detail.size() <= AuditRing::kMaxField,
          "every field is capped");
    for (int i = 0; i < 600; ++i) ring.record({i, "connect", "c", "", 0, std::to_string(i)});
    const auto kept = ring.snapshot();
    check(kept.size() == AuditRing::kCapacity && kept.front().detail == "88", "a ring of 512: the oldest go first");
}

void testScreen() {
    section("the screen runs on the calling thread, before the hop (ADR-0232 d4)");
    ManualExecutor manual;
    GrantTable grants;
    Service service(manual.executor(), grants);
    int runs = 0;
    MethodSpec m;
    m.name = "test.screened";
    m.minTier = Tier::Propose;
    m.readOnly = false;
    m.ownerThread = true;
    m.screen = [](CallContext&, Json& params) -> std::optional<Outcome> {
        if (params.contains("bad")) return Outcome::fail(errc::InvalidParams, "bad");
        params["normalised"] = true;
        return std::nullopt;
    };
    m.run = [&runs](CallContext&, const Json& params) -> Outcome {
        ++runs;
        return Outcome::ok(params);
    };
    service.add(std::move(m));
    const Principal p{"cred-s", "S"};
    grants.grant(p.credential, Tier::Propose);

    std::optional<Outcome> refused;
    service.call(p, "test.screened", {{"bad", 1}}, {}, [&refused](Outcome o) { refused = std::move(o); });
    check(refused && refused->error && refused->error->code == errc::InvalidParams && manual.tasks.empty(),
          "a refusal comes from the calling thread: nothing was queued for the owner");
    std::optional<Outcome> ok;
    service.call(p, "test.screened", {{"x", 1}}, {}, [&ok](Outcome o) { ok = std::move(o); });
    check(!ok && manual.tasks.size() == 1, "an accepted call is queued for the owner");
    manual.drain();
    check(ok && !ok->error && ok->result.value("normalised", false) && runs == 1,
          "and the owner runs it with the params the screen normalised");
}

void testSecureEquals() {
    section("secureEquals");
    check(secureEquals("abc", "abc"), "equal");
    check(!secureEquals("abc", "abd"), "one byte differs");
    check(!secureEquals("abc", "abcd") && !secureEquals("abcd", "abc"), "lengths differ");
    check(secureEquals("", ""), "two empties are equal (the grant table refuses an empty credential)");
    check(!secureEquals(std::string("a\0b", 3), std::string("a\0c", 3)), "embedded zeros compared");
}

void testRegistration() {
    section("registration refuses a write reachable at Observe");
    GrantTable grants;
    Service s(inlineExecutor(), grants);
    MethodSpec bad;
    bad.name = "bad.write";
    bad.readOnly = false;
    bad.run = [](CallContext&, const Json&) { return Outcome::ok(nullptr); };
    bool threw = false;
    try {
        s.add(bad);
    } catch (const std::logic_error&) {
        threw = true;
    }
    check(threw, "a write method below Propose does not register");
    MethodSpec ok;
    ok.name = "dup";
    ok.run = [](CallContext&, const Json&) { return Outcome::ok(nullptr); };
    s.add(ok);
    threw = false;
    try {
        s.add(ok);
    } catch (const std::logic_error&) {
        threw = true;
    }
    check(threw, "a duplicate name does not register");
}

void testMeta() {
    section("meta: expectHead, deadlineMs, idempotencyKey, nothing else");
    RpcError e;
    auto m = parseMeta({{"expectHead", 7}, {"deadlineMs", 100}, {"idempotencyKey", "k"}}, e);
    check(m && m->expectHead && *m->expectHead == 7 && m->deadlineMs == 100 && m->idempotencyKey == "k",
          "all three read");
    auto root = parseMeta({{"expectHead", nullptr}}, e);
    check(root && root->expectHead && !*root->expectHead, "a null expectHead means the root");
    check(!parseMeta({{"other", 1}}, e) && e.code == errc::InvalidParams, "an unknown key is refused");
    check(!parseMeta({{"deadlineMs", 0}}, e), "a zero deadline is refused");
    check(!parseMeta({{"idempotencyKey", std::string(129, 'k')}}, e), "a key over 128 characters is refused");
    check(!parseMeta({{"expectHead", "7"}}, e), "a head is an integer");
    check(parseMeta(nullptr, e).has_value(), "no meta at all is fine");
}

void testWire() {
    section("the wire: strict, bounded before parsing");
    auto ok = parseMessage(R"({"jsonrpc":"2.0","id":1,"method":"m","params":{"a":1}})");
    check(ok.kind == Incoming::Kind::Request && ok.id == 1 && ok.method == "m" && ok.params.at("a") == 1,
          "a request");
    check(parseMessage(R"({"jsonrpc":"2.0","method":"n"})").kind == Incoming::Kind::Notification, "a notification");
    check(parseMessage(R"({"jsonrpc":"2.0","id":"x","result":{}})").kind == Incoming::Kind::Response,
          "a client's response is recognised, and dropped");
    const auto noVersion = parseMessage(R"({"id":3,"method":"m"})");
    check(noVersion.kind == Incoming::Kind::Invalid && noVersion.error.code == errc::InvalidRequest &&
              noVersion.id == 3,
          "no jsonrpc: invalid, answered to its id");
    check(parseMessage(R"({"jsonrpc":"2.0","id":null,"method":"m"})").error.code == errc::InvalidRequest,
          "a null id is refused (MCP)");
    check(parseMessage(R"([{"jsonrpc":"2.0","id":1,"method":"m"}])").error.code == errc::InvalidRequest,
          "a batch is refused");
    check(parseMessage("{nope").error.code == errc::ParseError, "not JSON: a parse error");
    const auto arr = parseMessage(R"({"jsonrpc":"2.0","id":2,"method":"m","params":[1]})");
    check(arr.kind == Incoming::Kind::Invalid && arr.error.code == errc::InvalidParams, "positional params refused");

    std::string deep;
    for (int i = 0; i < 100; ++i) deep += "[";
    for (int i = 0; i < 100; ++i) deep += "]";
    check(parseMessage(R"({"jsonrpc":"2.0","id":1,"method":"m","params":{"x":)" + deep + "}}").error.code ==
              errc::InvalidRequest,
          "100 levels deep is refused before parsing");
    check(nestingDepth(R"({"s":"[[[[[[[[[[["})", 4) == 1, "brackets inside strings do not count");
    check(nestingDepth(R"({"s":"\"[[[[[[[["})", 4) == 1, "nor after an escaped quote");
    WireLimits small;
    small.maxMessageBytes = 16;
    check(parseMessage(R"({"jsonrpc":"2.0","id":1,"method":"m"})", small).error.code == errc::InvalidRequest,
          "over the byte bound: refused");

    const std::string text = encodeResult(1, {{"name", std::string("bad \xff utf8")}});
    check(Json::parse(text, nullptr, false).is_discarded() == false, "invalid UTF-8 is replaced, never thrown");
}

void testLines() {
    section("lines: CRLF, blank lines, an over-long line read to its end");
    std::istringstream in("one\r\n\ntwo-is-long\nthree\n");
    std::string line;
    bool tooLong = false;
    check(readBoundedLine(in, line, 8, tooLong) && line == "one" && !tooLong, "CR stripped");
    check(readBoundedLine(in, line, 8, tooLong) && line.empty() && !tooLong, "a blank line");
    check(readBoundedLine(in, line, 8, tooLong) && tooLong && line.empty(), "over the bound: flagged, discarded");
    check(readBoundedLine(in, line, 8, tooLong) && line == "three" && !tooLong, "and the next line is whole");
    check(!readBoundedLine(in, line, 8, tooLong), "then the end");
}

int runAll() {
    testGrantsComeFirst();
    testTiers();
    testIdempotency();
    testDeadlines();
    testAudit();
    testScreen();
    testSecureEquals();
    testRegistration();
    testMeta();
    testWire();
    testLines();
    std::printf("\n%s -- %d checks, %d failure(s)\n", g_failures ? "FAILED" : "PASS", g_checks, g_failures);
    return g_failures ? 1 : 0;
}

}  // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    try {
        return runAll();
    } catch (const std::exception& e) {
        std::printf("FAILED -- exception: %s\n", e.what());
        return 1;
    }
}
