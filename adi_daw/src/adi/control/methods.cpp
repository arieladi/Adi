// SPDX-License-Identifier: AGPL-3.0-or-later
#include "adi/control/methods.hpp"

#include "adi/control/projection.hpp"
#include "adi/control/telemetry.hpp"
#include "adi/ops.hpp"
#include "adi/ops_schema.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <initializer_list>
#include <limits>
#include <optional>
#include <string_view>
#include <utility>

namespace adi::control {
namespace {

constexpr std::size_t kMaxRequestText = 4096;
constexpr std::size_t kMaxLabel = 200;
constexpr std::size_t kMaxActorLabel = 120;
constexpr std::size_t kMaxChangesShown = 200;
constexpr std::size_t kMaxDiffBytes = 256 * 1024;

/// Method params are closed objects, like op payloads: an unknown key is a
/// typo the caller believes did something.
std::optional<RpcError> closedKeys(const Json& params, std::initializer_list<std::string_view> keys) {
    if (params.is_null()) return std::nullopt;
    if (!params.is_object()) return RpcError{errc::InvalidParams, "params must be an object", nullptr};
    for (auto it = params.begin(); it != params.end(); ++it)
        if (std::find(keys.begin(), keys.end(), it.key()) == keys.end())
            return RpcError{errc::InvalidParams, "unknown parameter: " + it.key(), nullptr};
    return std::nullopt;
}

Outcome failWith(const RpcError& e) { return Outcome::fail(e.code, e.message, e.data); }

/// An optional integer param in [lo, hi]; a JSON float with no fraction counts.
bool readInt(const Json& params, const char* key, std::int64_t lo, std::int64_t hi,
             std::optional<std::int64_t>& out, RpcError& error) {
    const auto it = params.is_object() ? params.find(key) : params.end();
    if (!params.is_object() || it == params.end() || it->is_null()) return true;
    std::optional<std::int64_t> v;
    if (it->is_number_integer() && !it->is_number_unsigned()) v = it->get<std::int64_t>();
    else if (it->is_number_unsigned() && it->get<std::uint64_t>() <= static_cast<std::uint64_t>(hi))
        v = static_cast<std::int64_t>(it->get<std::uint64_t>());
    else if (it->is_number_float()) {
        const double d = it->get<double>();
        if (std::isfinite(d) && std::floor(d) == d && d >= -9.0e18 && d <= 9.0e18)
            v = static_cast<std::int64_t>(d);
    }
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
        if (!v.is_number_integer()) {
            error = {errc::InvalidParams, std::string(key) + " holds integer ids", nullptr};
            return false;
        }
        out.push_back(v.get<std::int64_t>());
    }
    return true;
}

std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

Json projectSchemaProperty() {
    return {{"type", "string"},
            {"description", "the project's handle (project.list); optional when exactly one is open"}};
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

const char* refusalText(agent::Refusal r) { return agent::toString(r); }

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
                out.push_back({{"project", pr->handle()}, {"label", pr->label()},
                               {"head", headJson(pr->head(err))}, {"live", pr->live()}});
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
            "Read the project: level 0 is the song, tracks with their devices, routing, markers and "
            "the next free ids; 1 adds clips, automation lanes and device parameters; 2 adds notes";
        m.paramsSchema = {
            {"type", "object"},
            {"properties",
             {{"project", projectSchemaProperty()},
              {"level", {{"type", "integer"}, {"minimum", 0}, {"maximum", 3},
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
            std::optional<std::int64_t> level, fromBar, toBar;
            ReadScope scope;
            if (!readInt(p, "level", 0, 3, level, e) || !readInt(p, "fromBar", -100000, 100000, fromBar, e) ||
                !readInt(p, "toBar", -100000, 100000, toBar, e) || !readIdList(p, "tracks", scope.tracks, e) ||
                !readIdList(p, "clips", scope.clips, e))
                return failWith(e);
            if (level.value_or(0) == 3)
                return Outcome::fail(errc::InvalidParams,
                                     "level 3 is analysis (AGT-07, at P2): not built; use levels 0 to 2");
            scope.fromBar = fromBar;
            scope.toBar = toBar;
            std::string err;
            const auto model = project->readModel(err);
            if (!err.empty()) return Outcome::fail(errc::Internal, "cannot read the project: " + err);
            const auto head = project->head(err);
            return Outcome::ok(projectLevel(model, static_cast<int>(level.value_or(0)), scope, head,
                                            project->handle()));
        };
        service.add(std::move(m));
    }

    // --- project.manifest, project.graph, project.telemetry ----------------------
    {
        MethodSpec m;
        m.name = "project.manifest";
        m.summary = "Sample rate, ppq, length, tempo, meter, key, schema, head and counts";
        m.paramsSchema = onlyProjectSchema();
        m.run = [&ctx](CallContext&, const Json& p) -> Outcome {
            if (auto e = closedKeys(p, {"project"})) return failWith(*e);
            RpcError e;
            auto project = ctx.projects.resolve(p, e);
            if (!project) return failWith(e);
            std::string err;
            const auto model = project->readModel(err);
            if (!err.empty()) return Outcome::fail(errc::Internal, "cannot read the project: " + err);
            return Outcome::ok(manifest(model, project->head(err), project->handle(), project->label(),
                                        project->fileSchema()));
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
            const auto model = project->readModel(err);
            if (!err.empty()) return Outcome::fail(errc::Internal, "cannot read the project: " + err);
            return Outcome::ok(graph(model, project->handle()));
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
              {"query", {{"type", "string"}, {"maxLength", 128},
                         {"description", "words to find in op names and summaries"}}}}},
            {"additionalProperties", false}};
        m.run = [](CallContext&, const Json& p) -> Outcome {
            if (auto e = closedKeys(p, {"names", "query"})) return failWith(*e);
            const auto& reg = OpRegistry::instance();
            if (p.is_object() && p.contains("names") && !p.at("names").is_null()) {
                const auto& names = p.at("names");
                if (!names.is_array() || names.size() > 64)
                    return Outcome::fail(errc::InvalidParams, "names is a list of at most 64 op names");
                Json ops = Json::array();
                Json unknown = Json::array();
                for (const auto& n : names) {
                    if (!n.is_string()) return Outcome::fail(errc::InvalidParams, "names holds strings");
                    const auto* d = reg.find(n.get<std::string>());
                    if (!d) {
                        unknown.push_back(n);
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
            std::string query;
            if (p.is_object() && p.contains("query") && !p.at("query").is_null()) {
                if (!p.at("query").is_string() || p.at("query").get_ref<const std::string&>().size() > 128)
                    return Outcome::fail(errc::InvalidParams, "query is a string of at most 128 characters");
                query = lower(p.at("query").get<std::string>());
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
                            {"description", "the head you read (project.read's head); a changeset "
                                            "built on a head that moved is refused as stale. "
                                            "Omitted: the head now"}}}}},
            {"required", Json::array({"request", "ops"})},
            {"additionalProperties", false}};
        m.run = [&ctx](CallContext& call, const Json& p) -> Outcome {
            if (auto e = closedKeys(p, {"project", "request", "ops", "baseHead"})) return failWith(*e);
            RpcError e;
            auto project = ctx.projects.resolve(p, e);
            if (!project) return failWith(e);

            const auto requestIt = p.find("request");
            if (requestIt == p.end() || !requestIt->is_string() || requestIt->get_ref<const std::string&>().empty() ||
                requestIt->get_ref<const std::string&>().size() > kMaxRequestText)
                return Outcome::fail(errc::InvalidParams,
                                     "request is the user's request, 1 to 4096 characters (AI-AGENT 6.8)");
            const auto opsIt = p.find("ops");
            if (opsIt == p.end() || !opsIt->is_array())
                return Outcome::fail(errc::InvalidParams, "ops is a list of {op, payload, label?}");
            if (opsIt->empty())
                return Outcome::fail(errc::InvalidParams, "the changeset is empty", {{"refusal", "empty"}});
            if (opsIt->size() > ctx.limits.maxOps)
                return Outcome::fail(errc::OverLimit,
                                     std::to_string(opsIt->size()) + " ops, over the per-request cap of " +
                                         std::to_string(ctx.limits.maxOps),
                                     {{"refusal", "too many ops"}, {"cap", ctx.limits.maxOps}});

            // The same key from the same caller answers with what it queued
            // the first time, even after the service's cache forgot it.
            if (call.meta.idempotencyKey) {
                if (auto prior = ctx.queue.findByKey(call.who.credential, *call.meta.idempotencyKey))
                    return Outcome::ok(prior->proposal);
            }

            // Every op screened at the boundary, with the remote bounds
            // (CTL-02): the allow-list, then the registry's own validation.
            std::vector<OpRequest> ops;
            ops.reserve(opsIt->size());
            std::size_t index = 0;
            for (const auto& o : *opsIt) {
                if (!o.is_object())
                    return Outcome::fail(errc::InvalidParams, "ops[" + std::to_string(index) + "] is an object",
                                         {{"refusal", "invalid"}, {"index", index}});
                for (auto it = o.begin(); it != o.end(); ++it)
                    if (it.key() != "op" && it.key() != "payload" && it.key() != "label")
                        return Outcome::fail(errc::InvalidParams,
                                             "ops[" + std::to_string(index) + "]: unknown key " + it.key(),
                                             {{"refusal", "invalid"}, {"index", index}});
                if (!o.contains("op") || !o.at("op").is_string())
                    return Outcome::fail(errc::InvalidParams, "ops[" + std::to_string(index) + "].op is an op name",
                                         {{"refusal", "invalid"}, {"index", index}});
                const auto name = o.at("op").get<std::string>();
                const auto* d = OpRegistry::instance().find(name);
                if (!d || !agent::agentMayEmit(name))
                    return Outcome::fail(errc::InvalidParams,
                                         "'" + name + "' is not an op a changeset may contain (ops.describe)",
                                         {{"refusal", "op not allowed"}, {"index", index}, {"op", name}});
                if (!o.contains("payload") || !o.at("payload").is_object())
                    return Outcome::fail(errc::InvalidParams,
                                         "ops[" + std::to_string(index) + "].payload is an object",
                                         {{"refusal", "invalid"}, {"index", index}, {"op", name}});
                Payload payload = o.at("payload");
                normalizeSubmission(*d, payload);
                const auto issues = validateSubmission(*d, payload, Bounds::Remote);
                if (!issues.empty()) {
                    Json list = Json::array();
                    for (std::size_t i = 0; i < issues.size() && i < 8; ++i)
                        list.push_back({{"key", issues[i].key}, {"problem", issues[i].problem}});
                    return Outcome::fail(errc::InvalidParams,
                                         name + ": " + issues.front().key + " " + issues.front().problem,
                                         {{"refusal", "invalid"}, {"index", index}, {"op", name},
                                          {"issues", std::move(list)}});
                }
                OpRequest q;
                q.opType = name;
                q.payload = std::move(payload);
                q.actor = Actor::Agent;
                if (o.contains("label")) {
                    if (!o.at("label").is_string() || o.at("label").get_ref<const std::string&>().size() > kMaxLabel)
                        return Outcome::fail(errc::InvalidParams,
                                             "ops[" + std::to_string(index) + "].label is at most 200 characters",
                                             {{"refusal", "invalid"}, {"index", index}});
                    q.label = o.at("label").get<std::string>();
                }
                ops.push_back(std::move(q));
                ++index;
            }

            // The head the caller built on: params.baseHead, then meta.expectHead,
            // then the head now.
            std::optional<std::int64_t> baseHead;
            std::string err;
            const auto headNow = project->head(err);
            if (const auto bh = p.find("baseHead"); bh != p.end()) {
                if (bh->is_null()) baseHead = std::nullopt;
                else if (bh->is_number_integer()) baseHead = bh->get<std::int64_t>();
                else return Outcome::fail(errc::InvalidParams, "baseHead is an integer or null");
            } else if (call.meta.expectHead) {
                baseHead = *call.meta.expectHead;
            } else {
                baseHead = headNow;
            }

            agent::Changeset cs;
            cs.baseHead = baseHead;
            cs.request = requestIt->get<std::string>();
            cs.createdUtc = ctx.nowUtcMicros ? ctx.nowUtcMicros() : 0;
            const std::string id = ctx.queue.nextId();
            std::string label = call.who.label.empty() ? std::string("unnamed client") : call.who.label;
            if (label.size() > kMaxActorLabel) label.resize(kMaxActorLabel);
            // ops.actor_detail: who wrote these, through what, and which
            // proposal -- so changeset.status can find it once applied.
            cs.actorDetail = "mcp:" + label + " via " + ctx.transport + " (" + id + ")";
            for (auto& q : ops) {
                q.actorDetail = cs.actorDetail;
                if (q.label.empty()) q.label = q.opType;
            }
            cs.ops = std::move(ops);

            const agent::Preview pv = project->preview(cs, ctx.limits);
            if (!pv.ok) {
                Json data = {{"refusal", refusalText(pv.refusal)}};
                if (pv.refusal == agent::Refusal::Stale) {
                    data["headFound"] = headJson(headNow);
                    data["baseHead"] = headJson(baseHead);
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
            if (diffCut) diff.resize(kMaxDiffBytes);

            Json proposal = {{"id", id},
                             {"status", std::string(toString(Review::AwaitingReview))},
                             {"project", project->handle()},
                             {"baseHead", headJson(baseHead)},
                             {"request", cs.request},
                             {"ops", cs.ops.size()},
                             {"diff", std::move(diff)},
                             {"changes", std::move(changes)},
                             {"undo", "applied as one transaction: one undo step restores the project"},
                             {"apply", project->applyHint()}};
            if (diffCut) proposal["diffTruncated"] = true;
            if (pv.changes.size() > kMaxChangesShown) proposal["changesTruncated"] = true;

            QueuedChangeset entry;
            entry.id = id;
            entry.credential = call.who.credential;
            entry.idempotencyKey = call.meta.idempotencyKey.value_or(std::string{});
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
                    "or expired; while it awaits review, the changeset itself";
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
            ctx.queue.expire(ctx.steadyMs());
            auto entry = ctx.queue.find(id, call.who.credential);
            // Another caller's id answers exactly like no id at all.
            if (!entry) return Outcome::fail(errc::InvalidParams, "no changeset of yours has that id");

            std::optional<std::int64_t> headNow;
            if (auto project = ctx.projects.find(entry->project)) {
                std::string err;
                headNow = project->head(err);
                if (entry->state == Review::AwaitingReview) {
                    Review next = entry->state;
                    if (project->wasApplied(entry->changeset.actorDetail)) next = Review::Applied;
                    else if (headNow != entry->changeset.baseHead) next = Review::Stale;
                    if (next != entry->state) {
                        ctx.queue.setState(id, call.who.credential, next);
                        entry->state = next;
                    }
                }
            }
            Json out = {{"id", entry->id},
                        {"status", std::string(toString(entry->state))},
                        {"project", entry->project},
                        {"baseHead", headJson(entry->changeset.baseHead)},
                        {"headNow", headJson(headNow)},
                        {"request", entry->changeset.request},
                        {"ops", entry->changeset.ops.size()}};
            if (entry->state == Review::AwaitingReview) {
                if (auto project = ctx.projects.find(entry->project)) out["apply"] = project->applyHint();
                out["changeset"] = changesetJson(entry->changeset);
            } else if (entry->state == Review::Stale) {
                out["why"] = "the project changed since the changeset was built; read it again and "
                             "propose anew (a stale changeset is never rebased)";
            }
            return Outcome::ok(std::move(out));
        };
        service.add(std::move(m));
    }
}

}  // namespace adi::control
