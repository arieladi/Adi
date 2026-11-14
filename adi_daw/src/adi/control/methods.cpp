// SPDX-License-Identifier: AGPL-3.0-or-later
#include "adi/control/methods.hpp"

#include "adi/control/projection.hpp"
#include "adi/control/telemetry.hpp"
#include "adi/ops.hpp"
#include "adi/ops_schema.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <functional>
#include <initializer_list>
#include <limits>
#include <optional>
#include <string_view>
#include <utility>

namespace adi::control {
namespace {

constexpr std::size_t kMaxRequestText = 4096;   ///< characters
constexpr std::size_t kMaxLabel = 200;          ///< characters
constexpr std::size_t kMaxQuery = 128;          ///< characters
constexpr std::size_t kMaxActorLabel = 120;     ///< bytes, cut on a UTF-8 boundary
constexpr std::size_t kMaxChangesShown = 200;
constexpr std::size_t kMaxDiffBytes = 256 * 1024;

/// Method params are closed objects, like op payloads: an unknown key is a
/// typo the caller believes did something. The refusal repeats a bounded part
/// of the key, never an unbounded one.
std::optional<RpcError> closedKeys(const Json& params, std::initializer_list<std::string_view> keys) {
    if (params.is_null()) return std::nullopt;
    if (!params.is_object()) return RpcError{errc::InvalidParams, "params must be an object", nullptr};
    for (auto it = params.begin(); it != params.end(); ++it)
        if (std::find(keys.begin(), keys.end(), it.key()) == keys.end())
            return RpcError{errc::InvalidParams, "unknown parameter: " + clipEcho(it.key()), nullptr};
    return std::nullopt;
}

Outcome failWith(const RpcError& e) { return Outcome::fail(e.code, e.message, e.data); }

/// A JSON integer, or a float with no fraction (JSON Schema calls both an
/// integer), as an int64; nullopt otherwise.
std::optional<std::int64_t> asInt64(const Json& v) {
    if (v.is_number_integer() && !v.is_number_unsigned()) return v.get<std::int64_t>();
    if (v.is_number_unsigned() && v.get<std::uint64_t>() <= static_cast<std::uint64_t>(INT64_MAX))
        return static_cast<std::int64_t>(v.get<std::uint64_t>());
    if (v.is_number_float()) {
        const double d = v.get<double>();
        if (std::isfinite(d) && std::floor(d) == d && d >= -9223372036854775808.0 && d < 9223372036854775808.0)
            return static_cast<std::int64_t>(d);
    }
    return std::nullopt;
}

/// An optional integer param in [lo, hi].
bool readInt(const Json& params, const char* key, std::int64_t lo, std::int64_t hi,
             std::optional<std::int64_t>& out, RpcError& error) {
    const auto it = params.is_object() ? params.find(key) : params.end();
    if (!params.is_object() || it == params.end() || it->is_null()) return true;
    const auto v = asInt64(*it);
    if (!v || *v < lo || *v > hi) {
        error = {errc::InvalidParams,
                 std::string(key) + " is an integer from " + std::to_string(lo) + " to " + std::to_string(hi),
                 nullptr};
        return false;
    }
    out = v;
    return true;
}

bool readIdList(const Json& params, const char* key, std::vector<std::int64_t>& out, RpcError& error) {
    const auto it = params.is_object() ? params.find(key) : params.end();
    if (!params.is_object() || it == params.end() || it->is_null()) return true;
    if (!it->is_array() || it->size() > kRemoteMaxArrayItems) {
        error = {errc::InvalidParams, std::string(key) + " is a list of at most 4096 ids", nullptr};
        return false;
    }
    for (const auto& v : *it) {
        const auto id = asInt64(v);
        if (!id) {
            error = {errc::InvalidParams, std::string(key) + " holds integer ids", nullptr};
            return false;
        }
        out.push_back(*id);
    }
    return true;
}

/// A text param of 1..max characters (code points, as the schema counts).
bool readText(const Json& params, const char* key, std::size_t max, bool required, std::string& out,
              RpcError& error) {
    const auto it = params.is_object() ? params.find(key) : params.end();
    if (!params.is_object() || it == params.end() || it->is_null()) {
        if (!required) return true;
        error = {errc::InvalidParams, std::string(key) + " is required", nullptr};
        return false;
    }
    if (!it->is_string() || it->get_ref<const std::string&>().empty() ||
        utf8CodePoints(it->get_ref<const std::string&>()) > max) {
        error = {errc::InvalidParams, std::string(key) + " is text of 1 to " + std::to_string(max) + " characters",
                 nullptr};
        return false;
    }
    out = it->get<std::string>();
    return true;
}

std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

Json projectSchemaProperty() {
    return {{"type", "string"},
            {"description", "the project's handle (project.list); optional only when the server is bound to "
                            "one project for its life, as adi-mcp is"}};
}

Json onlyProjectSchema() {
    return {{"type", "object"},
            {"properties", {{"project", projectSchemaProperty()}}},
            {"additionalProperties", false}};
}

Json headJson(const std::optional<std::int64_t>& h) { return h ? Json(*h) : Json(nullptr); }

Json changeJson(const agent::Change& c) {
    Json row = {{"label", c.label}, {"op", c.opType}};
    Json target = {{"kind", c.targetKind}};
    target["id"] = c.targetId ? Json(*c.targetId) : Json(nullptr);
    row["target"] = std::move(target);
    if (c.before) row["before"] = *c.before;
    if (c.after) row["after"] = *c.after;
    return row;
}

Outcome refusal(int code, std::string message, const char* kind, Json extra = Json::object()) {
    extra["refusal"] = kind;
    return Outcome::fail(code, std::move(message), std::move(extra));
}

/// changeset.propose's screen: everything that needs no project and no owner,
/// on the calling thread (ADR-0232 d4). Normalises the payloads in place.
std::optional<Outcome> screenProposal(const ControlContext& ctx, const RequestMeta& meta, Json& p) {
    if (auto e = closedKeys(p, {"project", "request", "ops", "baseHead"})) return failWith(*e);
    RpcError e;
    std::string request;
    if (!readText(p, "request", kMaxRequestText, true, request, e))
        return refusal(errc::InvalidParams, "request is the user's request, 1 to 4096 characters (AI-AGENT 6.8)",
                       "invalid");
    if (const auto project = p.find("project"); project != p.end() && !project->is_null() && !project->is_string())
        return refusal(errc::InvalidParams, "project is a project handle (a string)", "invalid");
    // baseHead is required (ADR-0233 d5: the stale check keeps nextIds free
    // only when the head the caller read is checked). A caller on the RPC
    // path may say it as meta.expectHead instead, which is copied in here so
    // the run, the queue's idempotency digest and the stale check all see
    // one head. Both, disagreeing, is refused: never a silent choice of one.
    // Null is the root.
    if (const auto bh = p.find("baseHead"); bh != p.end()) {
        std::optional<std::int64_t> head;
        if (!bh->is_null()) {
            const auto v = asInt64(*bh);
            if (!v) return refusal(errc::InvalidParams, "baseHead is an integer or null", "invalid");
            *bh = *v;
            head = *v;
        }
        if (meta.expectHead && *meta.expectHead != head)
            return refusal(errc::InvalidParams, "baseHead and meta.expectHead disagree", "invalid");
    } else if (meta.expectHead) {
        p["baseHead"] = *meta.expectHead ? Json(**meta.expectHead) : Json(nullptr);
    } else {
        return refusal(errc::InvalidParams,
                       "baseHead is required: the head project_read returned (null is the root)", "invalid");
    }
    const auto opsIt = p.find("ops");
    if (opsIt == p.end() || !opsIt->is_array())
        return refusal(errc::InvalidParams, "ops is a list of {op, payload, label?}", "invalid");
    if (opsIt->empty()) return refusal(errc::InvalidParams, "the changeset is empty", "empty");
    if (opsIt->size() > ctx.limits.maxOps)
        return refusal(errc::OverLimit,
                       std::to_string(opsIt->size()) + " ops, over the per-request cap of " +
                           std::to_string(ctx.limits.maxOps),
                       "too many ops", {{"cap", ctx.limits.maxOps}});

    // Every op screened with the remote bounds (CTL-02): the allow-list, then
    // the registry's own validation -- the one validator.
    std::size_t index = 0;
    for (auto& o : *opsIt) {
        const std::string at = "ops[" + std::to_string(index) + "]";
        if (!o.is_object())
            return refusal(errc::InvalidParams, at + " is an object", "invalid", {{"index", index}});
        for (auto it = o.begin(); it != o.end(); ++it)
            if (it.key() != "op" && it.key() != "payload" && it.key() != "label")
                return refusal(errc::InvalidParams, at + ": unknown key " + clipEcho(it.key()), "invalid",
                               {{"index", index}});
        const auto op = o.find("op");
        if (op == o.end() || !op->is_string())
            return refusal(errc::InvalidParams, at + ".op is an op name", "invalid", {{"index", index}});
        const auto& name = op->get_ref<const std::string&>();
        const auto* d = OpRegistry::instance().find(name);
        if (!d || !agent::agentMayEmit(name))
            return refusal(errc::InvalidParams,
                           "'" + clipEcho(name) + "' is not an op a changeset may contain (ops.describe)",
                           "op not allowed", {{"index", index}});
        const auto payload = o.find("payload");
        if (payload == o.end() || !payload->is_object())
            return refusal(errc::InvalidParams, at + ".payload is an object", "invalid",
                           {{"index", index}, {"op", std::string(d->name)}});
        normalizeSubmission(*d, *payload);
        const auto issues = validateSubmission(*d, *payload, Bounds::Remote);
        if (!issues.empty()) {
            Json list = Json::array();
            for (std::size_t i = 0; i < issues.size() && i < 8; ++i)
                list.push_back({{"key", clipEcho(issues[i].key)}, {"problem", issues[i].problem}});
            return refusal(errc::InvalidParams,
                           std::string(d->name) + ": " + clipEcho(issues.front().key) + " " +
                               issues.front().problem,
                           "invalid", {{"index", index}, {"op", std::string(d->name)}, {"issues", std::move(list)}});
        }
        if (const auto label = o.find("label"); label != o.end()) {
            if (!label->is_string() || utf8CodePoints(label->get_ref<const std::string&>()) > kMaxLabel)
                return refusal(errc::InvalidParams, at + ".label is text of at most 200 characters", "invalid",
                               {{"index", index}});
        }
        ++index;
    }
    return std::nullopt;
}

}  // namespace

void registerControlMethods(Service& service, ControlContext& ctx) {
    if (!ctx.steadyMs) ctx.steadyMs = [&service] { return service.now(); };

    // --- system.describe ---------------------------------------------------------
    {
        MethodSpec m;
        m.name = "system.describe";
        m.summary = "The server, its methods and the open projects";
        m.paramsSchema = {{"type", "object"}, {"properties", Json::object()}, {"additionalProperties", false}};
        m.run = [&service, &ctx](CallContext& call, const Json& p) -> Outcome {
            if (auto e = closedKeys(p, {})) return failWith(*e);
            Json methods = Json::array();
            for (const auto* spec : service.methods())
                methods.push_back({{"name", spec->name}, {"summary", spec->summary},
                                   {"tier", std::string(toString(spec->minTier))}});
            Json projects = Json::array();
            for (const auto& pr : ctx.projects.all())
                projects.push_back({{"project", pr->handle()}, {"label", pr->label()}, {"live", pr->live()}});
            return Outcome::ok({{"server", {{"name", ctx.serverName}, {"version", ctx.serverVersion}}},
                                {"tier", std::string(toString(call.tier))},
                                {"methods", std::move(methods)},
                                {"projects", std::move(projects)}});
        };
        service.add(std::move(m));
    }

    // --- project.list ------------------------------------------------------------
    {
        MethodSpec m;
        m.name = "project.list";
        m.summary = "The open projects, by handle";
        m.paramsSchema = {{"type", "object"}, {"properties", Json::object()}, {"additionalProperties", false}};
        m.run = [&ctx](CallContext&, const Json& p) -> Outcome {
            if (auto e = closedKeys(p, {})) return failWith(*e);
            Json out = Json::array();
            for (const auto& pr : ctx.projects.all()) {
                std::string err;
                const auto head = pr->head(err);
                Json row = {{"project", pr->handle()}, {"label", pr->label()}, {"live", pr->live()}};
                // A head that could not be read is never reported as null,
                // which on the wire is the root.
                if (err.empty()) row["head"] = headJson(head);
                else row["headError"] = "cannot read the head";
                out.push_back(std::move(row));
            }
            return Outcome::ok({{"projects", std::move(out)}});
        };
        service.add(std::move(m));
    }

    // --- project.read (AGT-02) ---------------------------------------------------
    {
        MethodSpec m;
        m.name = "project.read";
        m.summary =
            "Read the project: level 0 is the song, key events, tracks with their devices, routing, markers "
            "and the next free ids; 1 adds clips, automation lanes, device parameters, remarks and tunings; "
            "2 adds notes";
        m.paramsSchema = {
            {"type", "object"},
            {"properties",
             {{"project", projectSchemaProperty()},
              {"level", {{"type", "integer"}, {"minimum", 0}, {"maximum", 2},
                         {"description", "0, 1 or 2 (3 is analysis, not built). Default 0"}}},
              {"tracks", {{"type", "array"}, {"items", {{"type", "integer"}}}, {"maxItems", 4096},
                          {"description", "levels 1-2: only these track ids"}}},
              {"clips", {{"type", "array"}, {"items", {{"type", "integer"}}}, {"maxItems", 4096},
                         {"description", "level 2: notes of only these clip ids"}}},
              {"fromBar", {{"type", "integer"}, {"minimum", -100000}, {"maximum", 100000},
                           {"description", "levels 1-2: from this bar (1-based, inclusive)"}}},
              {"toBar", {{"type", "integer"}, {"minimum", -100000}, {"maximum", 100000},
                         {"description", "levels 1-2: to this bar (exclusive)"}}}}},
            {"additionalProperties", false}};
        m.run = [&ctx](CallContext&, const Json& p) -> Outcome {
            if (auto e = closedKeys(p, {"project", "level", "tracks", "clips", "fromBar", "toBar"}))
                return failWith(*e);
            RpcError e;
            auto project = ctx.projects.resolve(p, e);
            if (!project) return failWith(e);
            // Level 3 is named before the range check, so the refusal says
            // what it is rather than only the range.
            if (const auto lv = p.find("level"); lv != p.end() && asInt64(*lv) == std::optional<std::int64_t>(3))
                return Outcome::fail(errc::InvalidParams,
                                     "level 3 is analysis (AGT-07, at P2): not built; use levels 0 to 2");
            std::optional<std::int64_t> level, fromBar, toBar;
            ReadScope scope;
            if (!readInt(p, "level", 0, 2, level, e) || !readInt(p, "fromBar", -100000, 100000, fromBar, e) ||
                !readInt(p, "toBar", -100000, 100000, toBar, e) || !readIdList(p, "tracks", scope.tracks, e) ||
                !readIdList(p, "clips", scope.clips, e))
                return failWith(e);
            // A scope the level ignores is refused, not silently dropped: the
            // caller believes it narrowed the read.
            const auto given = [&p](const char* key) {
                const auto it = p.find(key);
                return it != p.end() && !it->is_null();
            };
            const std::int64_t at = level.value_or(0);
            if (at == 0 && (given("tracks") || given("clips") || given("fromBar") || given("toBar")))
                return Outcome::fail(errc::InvalidParams,
                                     "tracks, clips, fromBar and toBar scope levels 1 and 2; level 0 reads "
                                     "the whole project");
            if (at < 2 && given("clips"))
                return Outcome::fail(errc::InvalidParams, "clips scopes the notes, which only level 2 reads");
            if (fromBar && toBar && *fromBar >= *toBar)
                return Outcome::fail(errc::InvalidParams, "fromBar is before toBar (toBar is exclusive)");
            scope.fromBar = fromBar;
            scope.toBar = toBar;
            std::string err;
            const auto snapshot = project->read(err);
            if (!err.empty()) return Outcome::fail(errc::Internal, "cannot read the project: " + err);
            return Outcome::ok(projectLevel(snapshot.model, static_cast<int>(level.value_or(0)), scope,
                                            snapshot.head, project->handle()));
        };
        service.add(std::move(m));
    }

    // --- project.manifest, project.graph, project.telemetry ----------------------
    {
        MethodSpec m;
        m.name = "project.manifest";
        m.summary = "Sample rate, ppq, length, tempo, meter, key, schema, head and counts; the transport "
                    "when a running engine plays the project";
        m.paramsSchema = onlyProjectSchema();
        m.run = [&ctx](CallContext&, const Json& p) -> Outcome {
            if (auto e = closedKeys(p, {"project"})) return failWith(*e);
            RpcError e;
            auto project = ctx.projects.resolve(p, e);
            if (!project) return failWith(e);
            std::string err;
            const auto snapshot = project->read(err);
            if (!err.empty()) return Outcome::fail(errc::Internal, "cannot read the project: " + err);
            Json out = manifest(snapshot.model, snapshot.head, project->handle(), project->label(),
                                project->fileSchema());
            if (project->live()) {
                const auto frame = project->telemetry();
                if (frame && frame->transport)
                    out["transport"] = {{"playing", frame->transport->playing},
                                        {"positionSamples", frame->transport->positionSamples},
                                        {"sampleRate", frame->transport->sampleRate}};
            }
            return Outcome::ok(std::move(out));
        };
        service.add(std::move(m));
    }
    {
        MethodSpec m;
        m.name = "project.graph";
        m.summary = "Tracks as a tree, every routing row by kind, implicit outputs and device chains";
        m.paramsSchema = onlyProjectSchema();
        m.run = [&ctx](CallContext&, const Json& p) -> Outcome {
            if (auto e = closedKeys(p, {"project"})) return failWith(*e);
            RpcError e;
            auto project = ctx.projects.resolve(p, e);
            if (!project) return failWith(e);
            std::string err;
            const auto snapshot = project->read(err);
            if (!err.empty()) return Outcome::fail(errc::Internal, "cannot read the project: " + err);
            return Outcome::ok(graph(snapshot.model, project->handle()));
        };
        service.add(std::move(m));
    }
    {
        MethodSpec m;
        m.name = "project.telemetry";
        m.summary = "Live meters the engine already publishes, labelled honestly, and what is not "
                    "measured yet";
        m.paramsSchema = onlyProjectSchema();
        m.run = [&ctx](CallContext&, const Json& p) -> Outcome {
            if (auto e = closedKeys(p, {"project"})) return failWith(*e);
            RpcError e;
            auto project = ctx.projects.resolve(p, e);
            if (!project) return failWith(e);
            const auto frame = project->telemetry();
            Json out = telemetryJson(frame.get(), ctx.steadyMs(), project->live());
            out["project"] = project->handle();
            return Outcome::ok(std::move(out));
        };
        service.add(std::move(m));
    }

    // --- ops.describe (ADR-0168 d3; CTL-02) --------------------------------------
    {
        MethodSpec m;
        m.name = "ops.describe";
        m.summary = "The ops a changeset may contain: with no arguments an index; with names their "
                    "closed JSON Schemas, generated from the registry";
        m.paramsSchema = {
            {"type", "object"},
            {"properties",
             {{"names", {{"type", "array"}, {"items", {{"type", "string"}, {"maxLength", 64}}}, {"maxItems", 64},
                         {"description", "op names, e.g. [\"routing.connect\"]: their full schemas"}}},
              {"query", {{"type", "string"}, {"maxLength", kMaxQuery},
                         {"description", "words to find in op names and summaries"}}}}},
            {"additionalProperties", false}};
        m.run = [](CallContext&, const Json& p) -> Outcome {
            if (auto e = closedKeys(p, {"names", "query"})) return failWith(*e);
            const auto& reg = OpRegistry::instance();
            // The query is checked first, whatever else came with it.
            std::string query;
            const bool hasQuery = p.is_object() && p.contains("query") && !p.at("query").is_null();
            if (hasQuery) {
                if (!p.at("query").is_string() || utf8CodePoints(p.at("query").get_ref<const std::string&>()) > kMaxQuery)
                    return Outcome::fail(errc::InvalidParams, "query is text of at most 128 characters");
                query = lower(p.at("query").get<std::string>());
            }
            if (p.is_object() && p.contains("names") && !p.at("names").is_null()) {
                if (hasQuery) return Outcome::fail(errc::InvalidParams, "names or query, not both");
                const auto& names = p.at("names");
                if (!names.is_array() || names.size() > 64)
                    return Outcome::fail(errc::InvalidParams, "names is a list of at most 64 op names");
                Json ops = Json::array();
                Json unknown = Json::array();
                for (const auto& n : names) {
                    if (!n.is_string()) return Outcome::fail(errc::InvalidParams, "names holds strings");
                    const auto* d = reg.find(n.get<std::string>());
                    if (!d) {
                        unknown.push_back(clipEcho(n.get_ref<const std::string&>()));
                        continue;
                    }
                    Json o = describeOp(*d);
                    o["agentMayEmit"] = agent::agentMayEmit(d->name);
                    ops.push_back(std::move(o));
                }
                if (!unknown.empty())
                    return Outcome::fail(errc::InvalidParams, "no op by these names", {{"unknown", unknown}});
                return Outcome::ok({{"ops", std::move(ops)}});
            }
            Json index = Json::array();
            std::size_t notAllowed = 0;
            for (const auto& d : reg.all()) {
                if (!agent::agentMayEmit(d.name)) {
                    ++notAllowed;
                    continue;
                }
                if (!query.empty() && lower(std::string(d.name)).find(query) == std::string::npos &&
                    lower(std::string(d.summary)).find(query) == std::string::npos)
                    continue;
                index.push_back({{"name", std::string(d.name)}, {"summary", std::string(d.summary)}});
            }
            return Outcome::ok(
                {{"ops", std::move(index)},
                 {"notAvailable", notAllowed},
                 {"hint", "call again with names for each op's closed JSON Schema; a changeset may "
                          "contain only these ops (transport, hardware and file-touching media ops are "
                          "the user's)"}});
        };
        service.add(std::move(m));
    }

    // --- changeset.propose (ADR-0148; AGT-01) ------------------------------------
    {
        MethodSpec m;
        m.name = "changeset.propose";
        m.summary =
            "Propose ops as ONE changeset: it is previewed (every op run and rolled back) and queued "
            "for the user; nothing is written until the user applies it, as one undo step";
        m.minTier = Tier::Propose;
        m.ownerThread = true;
        m.idempotent = true;
        m.readOnly = false;
        m.paramsSchema = {
            {"type", "object"},
            {"properties",
             {{"project", projectSchemaProperty()},
              {"request", {{"type", "string"}, {"minLength", 1}, {"maxLength", kMaxRequestText},
                           {"description", "what the user asked, in their words; stored beside the "
                                           "transaction so it can be audited later"}}},
              {"ops",
               {{"type", "array"},
                {"minItems", 1},
                {"maxItems", ctx.limits.maxOps},
                {"description", "the ops, in order; each payload is checked against the op's schema "
                                "(ops.describe)"},
                {"items",
                 {{"type", "object"},
                  {"properties",
                   {{"op", {{"type", "string"}, {"description", "an op name from ops.describe"}}},
                    {"payload", {{"type", "object"}}},
                    {"label", {{"type", "string"}, {"maxLength", kMaxLabel},
                               {"description", "what the undo menu and the review list show"}}}}},
                  {"required", Json::array({"op", "payload"})},
                  {"additionalProperties", false}}}}},
              {"baseHead", {{"type", Json::array({"integer", "null"})},
                            {"description", "the head project_read returned; null is the root (an empty "
                                            "history, as project_read reports it). A changeset built on a "
                                            "head that moved is refused as stale"}}}}},
            {"required", Json::array({"request", "ops", "baseHead"})},
            {"additionalProperties", false}};
        m.screen = [&ctx](CallContext& call, Json& p) -> std::optional<Outcome> {
            return screenProposal(ctx, call.meta, p);
        };
        m.run = [&ctx](CallContext& call, const Json& p) -> Outcome {
            RpcError e;
            auto project = ctx.projects.resolve(p, e);
            if (!project) return failWith(e);

            // What the key stands for: the screened params, with the project
            // they resolved to (an omitted project is the bound one).
            Json keyed = p;
            keyed["project"] = project->handle();
            const std::size_t digest = std::hash<std::string>{}(dumpJson(keyed));

            // The same key from the same caller answers with what it queued
            // the first time, even after the service's cache forgot it -- but
            // only for the same arguments: a corrected request under an old
            // key is refused, never answered with the old proposal (R2.4).
            if (call.meta.idempotencyKey) {
                if (auto prior = ctx.queue.findByKey(call.who.credential, *call.meta.idempotencyKey)) {
                    if (prior->paramsDigest != digest || prior->project != project->handle())
                        return refusal(errc::InvalidParams, "idempotencyKey reused with different arguments",
                                       "invalid");
                    return Outcome::ok(prior->proposal);
                }
            }

            // The head the caller built on: params.baseHead, which the screen
            // made required (or copied in from meta.expectHead) and an int64.
            // Null is the root.
            std::optional<std::int64_t> baseHead;
            if (const auto bh = p.find("baseHead"); bh != p.end() && !bh->is_null())
                baseHead = bh->get<std::int64_t>();

            agent::Changeset cs;
            cs.baseHead = baseHead;
            cs.request = p.at("request").get<std::string>();
            cs.createdUtc = ctx.nowUtcMicros ? ctx.nowUtcMicros() : 0;
            const std::string id = ctx.queue.nextId();
            std::string label = call.who.label.empty() ? std::string("unnamed client") : call.who.label;
            if (label.size() > kMaxActorLabel) {
                std::size_t cut = kMaxActorLabel;
                while (cut > 0 && (static_cast<unsigned char>(label[cut]) & 0xC0u) == 0x80u) --cut;
                label.resize(cut);
            }
            // ops.actor_detail: who wrote these, through what, and which
            // proposal -- so changeset.status can find it once applied.
            cs.actorDetail = "mcp:" + label + " via " + ctx.transport + " (" + id + ")";
            for (const auto& o : p.at("ops")) {
                OpRequest q;
                q.opType = o.at("op").get<std::string>();
                q.payload = o.at("payload");   // normalised by the screen
                q.actor = Actor::Agent;
                q.actorDetail = cs.actorDetail;
                q.label = o.contains("label") ? o.at("label").get<std::string>() : q.opType;
                cs.ops.push_back(std::move(q));
            }

            const agent::Preview pv = project->preview(cs, ctx.limits);
            if (!pv.ok) {
                Json data = {{"refusal", agent::toString(pv.refusal)}};
                if (pv.refusal == agent::Refusal::Stale) {
                    // The head the check found, read again now: what to build
                    // on. One that cannot be read is never reported as null,
                    // which is the root.
                    std::string headError;
                    const auto found = project->head(headError);
                    if (!headError.empty()) return Outcome::fail(errc::Internal, "cannot read the head");
                    data["headFound"] = headJson(found);
                    data["baseHead"] = headJson(baseHead);
                    if (!baseHead && found)
                        data["hint"] = "a null baseHead is the root; pass the head project_read returned";
                    return Outcome::fail(errc::Stale, pv.error, std::move(data));
                }
                if (pv.refusal == agent::Refusal::TooManyOps)
                    return Outcome::fail(errc::OverLimit, pv.error, std::move(data));
                return Outcome::fail(errc::InvalidParams, pv.error, std::move(data));
            }

            Json changes = Json::array();
            for (std::size_t i = 0; i < pv.changes.size() && i < kMaxChangesShown; ++i)
                changes.push_back(changeJson(pv.changes[i]));
            std::string diff = pv.unifiedDiff;
            const bool diffCut = diff.size() > kMaxDiffBytes;
            if (diffCut) {
                // At the last whole line that fits; a newline byte is never
                // inside a UTF-8 sequence. A single line longer than the cap
                // is cut on a character boundary instead.
                std::size_t cut = diff.rfind('\n', kMaxDiffBytes - 1);
                if (cut == std::string::npos || cut == 0) {
                    cut = kMaxDiffBytes;
                    while (cut > 0 && (static_cast<unsigned char>(diff[cut]) & 0xC0u) == 0x80u) --cut;
                } else {
                    ++cut;   // keep the newline
                }
                diff.resize(cut);
            }

            // The changeset itself, in adi_tool's form, so the user can save
            // and apply it without a second call (R1.20).
            Json proposal = {{"id", id},
                             {"status", std::string(toString(Review::AwaitingReview))},
                             {"project", project->handle()},
                             {"baseHead", headJson(baseHead)},
                             {"request", cs.request},
                             {"ops", cs.ops.size()},
                             {"diff", std::move(diff)},
                             {"changes", std::move(changes)},
                             {"changeset", changesetJson(cs)},
                             {"undo", "applied as one transaction: one undo step restores the project"},
                             {"apply", project->applyHint()}};
            if (diffCut) proposal["diffTruncated"] = true;
            if (pv.changes.size() > kMaxChangesShown) proposal["changesTruncated"] = true;

            QueuedChangeset entry;
            entry.id = id;
            entry.credential = call.who.credential;
            entry.idempotencyKey = call.meta.idempotencyKey.value_or(std::string{});
            entry.paramsDigest = digest;
            entry.project = project->handle();
            entry.changeset = std::move(cs);
            entry.proposal = proposal;
            entry.createdMs = ctx.steadyMs();
            ctx.queue.add(std::move(entry));
            return Outcome::ok(std::move(proposal));
        };
        service.add(std::move(m));
    }

    // --- changeset.status (AGT-01) -----------------------------------------------
    {
        MethodSpec m;
        m.name = "changeset.status";
        m.summary = "Where one of your changesets stands: awaiting_review, applied, rejected, stale "
                    "or expired; while it awaits review, the changeset itself. Applied means applied "
                    "once: it may since have been undone, so read the project for its current state";
        m.ownerThread = true;
        m.paramsSchema = {{"type", "object"},
                          {"properties", {{"id", {{"type", "string"}, {"maxLength", 64},
                                                  {"description", "the id changeset.propose returned"}}}}},
                          {"required", Json::array({"id"})},
                          {"additionalProperties", false}};
        m.run = [&ctx](CallContext& call, const Json& p) -> Outcome {
            if (auto e = closedKeys(p, {"id"})) return failWith(*e);
            if (!p.is_object() || !p.contains("id") || !p.at("id").is_string())
                return Outcome::fail(errc::InvalidParams, "id is the changeset's id");
            const auto& id = p.at("id").get_ref<const std::string&>();
            auto entry = ctx.queue.find(id, call.who.credential);
            // Another caller's id answers exactly like no id at all.
            if (!entry) return Outcome::fail(errc::InvalidParams, "no changeset of yours has that id");

            // Applied wins over everything, and is checked first: a changeset
            // applied within the window but first asked about after it is
            // applied, not expired. Stale is worked out on every call and
            // never stored, so a head that comes back makes it current again.
            auto project = ctx.projects.find(entry->project);
            Review state = entry->state;
            std::optional<std::int64_t> headNow;
            std::string headError;
            bool headRead = false;
            if (state != Review::Applied && state != Review::Rejected && project) {
                if (project->wasApplied(entry->changeset.actorDetail)) {
                    ctx.queue.setState(id, call.who.credential, Review::Applied);
                    state = Review::Applied;
                } else {
                    ctx.queue.expire(ctx.steadyMs());
                    if (auto fresh = ctx.queue.find(id, call.who.credential)) state = fresh->state;
                    if (state == Review::AwaitingReview) {
                        headNow = project->head(headError);
                        headRead = true;
                        // A head that cannot be read changes nothing.
                        if (headError.empty() && headNow != entry->changeset.baseHead) state = Review::Stale;
                    }
                }
            }
            if (!headRead && project) headNow = project->head(headError);
            Json out = {{"id", entry->id},
                        {"status", std::string(toString(state))},
                        {"project", entry->project},
                        {"baseHead", headJson(entry->changeset.baseHead)},
                        {"request", entry->changeset.request},
                        {"ops", entry->changeset.ops.size()}};
            // A head that could not be read is never reported as null, which
            // on the wire is the root.
            if (!project) out["headError"] = "the project is no longer open";
            else if (!headError.empty()) out["headError"] = "cannot read the head";
            else out["headNow"] = headJson(headNow);
            if (state == Review::Applied) {
                out["note"] = "applied means it was applied once; it may since have been undone: read the "
                              "project for its current state";
            } else if (state == Review::AwaitingReview) {
                if (project) out["apply"] = project->applyHint();
                out["changeset"] = changesetJson(entry->changeset);
            } else if (state == Review::Stale) {
                out["why"] = "the project changed since the changeset was built; read it again and "
                             "propose anew (a stale changeset is never rebased)";
            } else if (state == Review::Expired) {
                out["why"] = "it was not applied within 24 hours; read the project again and propose anew";
            }
            return Outcome::ok(std::move(out));
        };
        service.add(std::move(m));
    }
}

}  // namespace adi::control
