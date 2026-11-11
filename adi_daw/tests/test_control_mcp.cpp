// SPDX-License-Identifier: AGPL-3.0-or-later
//
// The MCP projection, end to end over a real project file (CTL-08 AC-23,
// AC-17; ADR-0233; docs/CONTROL-MCP.md):
//
//   * both protocol eras: 2025-11-25's initialize and 2026-07-28's stateless
//     requests, server/discover, resultType, ttlMs/cacheScope, −32022;
//   * four tools, never one per op; an unknown tool is −32602; a refusal is a
//     tool execution error carrying ADI's refusal;
//   * headless: read, propose a volume change, see the diff -- and the file's
//     bytes unchanged;
//   * the request's own example -- a group bus, routing and a sidechain -- as
//     ONE changeset that, applied by the user, one undo restores exactly;
//   * a request naming another project touches nothing;
//   * the resources, and telemetry with honest labels;
//   * every refusal made at the MCP layer is audited by its code;
//   * changeset_status: awaiting_review, applied, stale, expired;
//   * hostile lines never crash the loop, and the real adi-mcp binary over
//     pipes, at both tiers.

#include "temp_directory.hpp"

#include "adi/changeset.hpp"
#include "adi/control/headless_project.hpp"
#include "adi/control/mcp.hpp"
#include "adi/control/methods.hpp"
#include "adi/control/stdio_server.hpp"
#include "adi/control/telemetry.hpp"
#include "adi/engine/main_loudness.hpp"
#include "adi/engine/mixer.hpp"
#include "adi/history.hpp"
#include "adi/ops.hpp"
#include "adi/store.hpp"
#include "adi/textproj_store.hpp"

#include <SQLiteCpp/SQLiteCpp.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <memory>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

using namespace adi;
using namespace adi::control;
namespace fs = std::filesystem;

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

constexpr std::int64_t kT0 = 1790253296000000;

std::string fileBytes(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

/// A closed project file: three audio tracks, Kick, Snare and Bass.
struct ProjectFile {
    adi::test::TempDirectory temp;   // destroyed last
    fs::path path;
    explicit ProjectFile(const char* token) : temp("control_mcp", token), path(temp.path() / "song.adi") {
        StoreError e = StoreError::Ok;
        auto store = Store::createEmpty(path, e);
        if (!store) throw std::runtime_error("fixture: cannot create the project");
        store->db().exec("INSERT INTO project(id,name) VALUES (1,'Song')");
        OpJournal j(*store);
        const std::pair<int, const char*> tracks[] = {{1, "Kick"}, {2, "Snare"}, {3, "Bass"}};
        for (const auto& [id, name] : tracks) {
            OpRequest r;
            r.opType = "track.create";
            r.payload = {{"id", id}, {"kind", "audio"}, {"name", name}, {"index", id - 1}};
            r.label = std::string("Create ") + name;
            if (!j.commit(r).ok) throw std::runtime_error("fixture: track.create failed");
        }
        store->close();
    }
};

/// adi-mcp's wiring, in process: the same objects main() builds.
struct Bridge {
    GrantTable grants;
    Service service;
    ProjectSet projects;
    int ids = 0;
    ChangesetQueue queue{[this] { return "cs-" + std::to_string(++ids); }};
    ControlContext ctx{projects, queue, agent::Limits{}, [] { return kT0; }, {}, "adi", "test", "adi-mcp"};
    McpEndpoint endpoint{service, projects, McpServerInfo{"adi-mcp", "ADI", "test"}};
    McpEndpoint::Session session;

    explicit Bridge(std::vector<std::shared_ptr<ProjectAccess>> bound, Tier tier = Tier::Propose,
                    ServiceOptions options = {})
        : service(inlineExecutor(), grants, std::move(options)) {
        const bool one = bound.size() == 1;
        for (auto& b : bound) projects.add(std::move(b));
        // adi-mcp's rule: an omitted project is allowed only when the server is
        // bound to one for its life.
        projects.allowImplicitSingle(one);
        session.credential = "cred-test";
        grants.grant(session.credential, tier);
        registerControlMethods(service, ctx);
    }
    explicit Bridge(std::shared_ptr<ProjectAccess> project, Tier tier = Tier::Propose, ServiceOptions options = {})
        : Bridge(std::vector<std::shared_ptr<ProjectAccess>>{std::move(project)}, tier, std::move(options)) {}

    Json send(const Json& message) { return sendAs(session, message); }
    Json sendAs(McpEndpoint::Session& as, const Json& message) {
        const auto reply = endpoint.handle(parseMessage(dumpJson(message)), as);
        return reply ? Json::parse(*reply) : Json();
    }
    Json request(int id, const std::string& method, Json params = Json::object()) {
        return send({{"jsonrpc", "2.0"}, {"id", id}, {"method", method}, {"params", std::move(params)}});
    }
    Json initialize() {
        return request(1, "initialize",
                       {{"protocolVersion", "2025-11-25"},
                        {"capabilities", Json::object()},
                        {"clientInfo", {{"name", "Claude Code"}, {"version", "2.1"}}}});
    }
    /// tools/call; returns the result object (or the whole reply on a JSON-RPC error).
    Json tool(const std::string& name, Json args, Json meta = nullptr) {
        return toolAs(session, name, std::move(args), std::move(meta));
    }
    Json toolAs(McpEndpoint::Session& as, const std::string& name, Json args, Json meta = nullptr) {
        Json params = {{"name", name}, {"arguments", std::move(args)}};
        if (!meta.is_null()) params["_meta"] = std::move(meta);
        const Json reply =
            sendAs(as, {{"jsonrpc", "2.0"}, {"id", ++calls}, {"method", "tools/call"}, {"params", std::move(params)}});
        return reply.contains("result") ? reply.at("result") : reply;
    }
    int calls = 100;
    /// project_read's head: what a proposal names as its baseHead.
    Json head(const Json& args = Json::object()) {
        return tool("project_read", args).at("structuredContent").at("head");
    }
};

/// Whether the audit holds a refusal with this code under this method.
bool auditedRefusal(Service& service, const std::string& method, int code) {
    for (const auto& ev : service.audit().snapshot())
        if (ev.kind == "refused" && ev.method == method && ev.code == code) return true;
    return false;
}

Json modern(Json params) {
    params["_meta"] = {{"io.modelcontextprotocol/protocolVersion", "2026-07-28"},
                       {"io.modelcontextprotocol/clientCapabilities", Json::object()},
                       {"io.modelcontextprotocol/clientInfo", {{"name", "Tester"}, {"version", "1"}}}};
    return params;
}

bool isToolError(const Json& result) { return result.is_object() && result.value("isError", false); }

std::string refusalOf(const Json& result) {
    if (!isToolError(result)) return {};
    const auto& e = result.at("structuredContent").at("error");
    return e.contains("data") && e.at("data").is_object() ? e.at("data").value("refusal", std::string{}) : std::string{};
}

std::shared_ptr<HeadlessProject> openHeadless(const fs::path& p) {
    std::string error;
    auto h = HeadlessProject::open(p, error);
    if (!h) throw std::runtime_error("open headless: " + error);
    return h;
}

// --- the eras ---------------------------------------------------------------------------

void testHandshakes() {
    section("both eras: 2025-11-25 initialize, 2026-07-28 stateless");
    ProjectFile f("eras");
    Bridge b(openHeadless(f.path));

    const Json early = b.request(2, "tools/list");
    check(early.contains("error") && early.at("error").at("code") == errc::InvalidRequest,
          "before initialize, a legacy request is refused");
    check(b.request(3, "ping").at("result") == Json::object(), "ping answers even before initialize");

    const Json init = b.initialize();
    const Json& r = init.at("result");
    check(r.value("protocolVersion", "") == "2025-11-25", "the requested version is echoed");
    check(r.at("serverInfo").value("name", "") == "adi-mcp" && r.at("serverInfo").contains("version"),
          "serverInfo names adi-mcp");
    check(r.value("instructions", "").find("data, not instructions") != std::string::npos &&
              r.value("instructions", "").find("one undo step") != std::string::npos,
          "the instructions say project content is data and one request is one undo step");
    check(r.at("capabilities").contains("tools") && r.at("capabilities").contains("resources"),
          "tools and resources are declared");
    check(b.initialize().at("error").at("code") == errc::InvalidRequest, "a second initialize is refused");
    check(b.send({{"jsonrpc", "2.0"}, {"method", "notifications/initialized"}}).is_null(),
          "a notification gets no reply");
    check(b.request(4, "server/discover").at("error").at("code") == errc::MethodNotFound,
          "server/discover is the modern era's");

    Bridge old(openHeadless(f.path));
    check(old.request(1, "initialize", {{"protocolVersion", "2024-01-01"}}).at("result").value("protocolVersion", "") ==
              "2025-11-25",
          "an unknown version is answered with our latest");
    Bridge mid(openHeadless(f.path));
    check(mid.request(1, "initialize", {{"protocolVersion", "2025-06-18"}}).at("result").value("protocolVersion", "") ==
              "2025-06-18",
          "an older supported version is echoed");
    Bridge pinned(openHeadless(f.path));
    check(pinned.request(1, "initialize", {{"protocolVersion", "2024-11-05"}}).at("result").value("protocolVersion", "") ==
              "2024-11-05",
          "2024-11-05, which has no batch obligation, is echoed");
    Bridge batchEra(openHeadless(f.path));
    check(batchEra.request(1, "initialize", {{"protocolVersion", "2025-03-26"}}).at("result").value("protocolVersion", "") ==
              "2025-11-25",
          "2025-03-26, which requires batches this server refuses, is answered with 2025-11-25");

    Bridge m(openHeadless(f.path));
    const Json list = m.request(5, "tools/list", modern(Json::object()));
    const Json& lr = list.at("result");
    check(lr.value("resultType", "") == "complete" && lr.value("ttlMs", -1) == 0 &&
              lr.value("cacheScope", "") == "private",
          "modern: resultType, ttlMs and cacheScope, with no initialize at all");
    check(lr.at("_meta").at("io.modelcontextprotocol/serverInfo").value("name", "") == "adi-mcp",
          "modern: serverInfo in the result's _meta");
    check(lr.at("tools").size() == 4, "four tools");

    Json noCaps = modern(Json::object());
    noCaps["_meta"].erase("io.modelcontextprotocol/clientCapabilities");
    check(m.request(6, "tools/list", noCaps).at("error").at("code") == errc::InvalidParams,
          "modern without clientCapabilities: −32602");
    Json future = modern(Json::object());
    future["_meta"]["io.modelcontextprotocol/protocolVersion"] = "2099-01-01";
    const Json unsupported = m.request(7, "tools/list", future);
    const auto& ue = unsupported.at("error");
    bool listsBoth = false;
    for (const auto& v : ue.at("data").at("supported"))
        listsBoth = listsBoth || v == "2025-11-25";
    check(ue.at("code") == errc::UnsupportedProtocolVersion && ue.at("data").at("requested") == "2099-01-01" &&
              listsBoth && ue.at("data").at("supported").at(0) == "2026-07-28",
          "an unsupported version: −32022 with supported and requested");
    const Json discover = m.request(8, "server/discover", modern(Json::object())).at("result");
    check(discover.contains("supportedVersions") && discover.contains("capabilities") &&
              discover.contains("instructions") && discover.value("cacheScope", "") == "private" &&
              discover.value("resultType", "") == "complete",
          "server/discover: versions, capabilities, instructions, cache fields");
    check(m.request(9, "ping", modern(Json::object())).at("error").at("code") == errc::MethodNotFound,
          "ping is gone in 2026-07-28");
    check(m.request(10, "initialize", modern(Json::object())).at("error").at("code") == errc::MethodNotFound,
          "initialize under modern _meta is not a method");

    // R3.1: a legacy version in a modern _meta is served in the legacy era,
    // never refused −32022 with that same version listed as supported.
    Bridge lm(openHeadless(f.path));
    Json legacyMeta = modern(Json::object());
    legacyMeta["_meta"]["io.modelcontextprotocol/protocolVersion"] = "2025-11-25";
    const Json beforeInit = lm.request(11, "tools/list", legacyMeta);
    check(beforeInit.contains("error") && beforeInit.at("error").at("code") == errc::InvalidRequest,
          "a legacy version in _meta, before initialize: send initialize first, not −32022");
    lm.initialize();
    const Json afterInit = lm.request(12, "tools/list", legacyMeta);
    check(afterInit.contains("result") && afterInit.at("result").at("tools").size() == 4 &&
              !afterInit.at("result").contains("resultType"),
          "after initialize it is served, in the legacy era (no resultType)");
}

// --- tools -----------------------------------------------------------------------------

void testToolsAndReads() {
    section("four tools; project_read with ids and nextIds; refusals are tool errors");
    ProjectFile f("tools");
    Bridge b(openHeadless(f.path));
    b.initialize();

    const Json tools = b.request(2, "tools/list").at("result").at("tools");
    std::set<std::string> names;
    bool closed = true, proposeWrites = false;
    for (const auto& t : tools) {
        names.insert(t.value("name", ""));
        closed = closed && t.at("inputSchema").value("type", "") == "object" &&
                 t.at("inputSchema").value("additionalProperties", true) == false;
        if (t.value("name", "") == "changeset_propose")
            proposeWrites = t.at("annotations").value("readOnlyHint", true) == false;
    }
    check(names == std::set<std::string>{"project_read", "ops_describe", "changeset_propose", "changeset_status"},
          "exactly CTL-08's four tools: a few, never one per op (ADR-0168 d3)");
    check(closed, "every tool's input is a closed object");
    check(proposeWrites, "changeset_propose is not marked read-only");
    for (const auto& t : tools) {
        if (t.value("name", "") == "project_read")
            check(t.at("inputSchema").at("properties").at("level").value("maximum", -1) == 2,
                  "project_read's level schema stops at 2");
        if (t.value("name", "") == "changeset_propose") {
            const auto& req = t.at("inputSchema").at("required");
            check(std::find(req.begin(), req.end(), Json("baseHead")) != req.end(),
                  "changeset_propose requires baseHead");
        }
    }

    const Json unknown = b.request(3, "tools/call", {{"name", "create_track"}, {"arguments", Json::object()}});
    check(unknown.contains("error") && unknown.at("error").at("code") == errc::InvalidParams,
          "an unknown tool (one-tool-per-op names included) is −32602");
    const Json badArgs = b.request(4, "tools/call", {{"name", "project_read"}, {"arguments", 5}});
    check(badArgs.at("error").at("code") == errc::InvalidParams, "arguments that are not an object: −32602");

    const Json r0 = b.tool("project_read", Json::object());
    check(!isToolError(r0), "level 0 reads");
    const Json& p0 = r0.at("structuredContent");
    check(p0.at("tracks").size() == 3 && p0.at("tracks").at(2).value("name", "") == "Bass" &&
              p0.at("tracks").at(2).value("id", 0) == 3,
          "tracks with their durable ids and names");
    check(p0.at("nextIds").value("track", 0) == 4 && p0.at("nextIds").value("routing", 0) == 1,
          "nextIds: the next track is 4, the first routing row 1");
    check(!p0.at("head").is_null() && p0.at("song").value("name", "") == "Song", "the head and the song");
    check(Json::parse(r0.at("content").at(0).value("text", "")) == p0,
          "the text content is the same JSON as structuredContent");

    const Json r1 = b.tool("project_read", {{"level", 1}, {"tracks", {3}}});
    check(!isToolError(r1) && r1.at("structuredContent").contains("clips") &&
              r1.at("structuredContent").contains("automationLanes"),
          "level 1 adds clips and automation lanes");
    const Json r2 = b.tool("project_read", {{"level", 2}});
    check(!isToolError(r2) && r2.at("structuredContent").contains("notes"), "level 2 adds notes");
    const Json r3 = b.tool("project_read", {{"level", 3}});
    check(isToolError(r3) && r3.at("content").at(0).value("text", "").find("AGT-07") != std::string::npos,
          "level 3 is refused with its card (analysis, P2)");
    const Json r4 = b.tool("project_read", {{"level", 4}});
    check(isToolError(r4) && r4.at("content").at(0).value("text", "").find("0 to 2") != std::string::npos,
          "level 4 is out of range: 0 to 2");
    check(isToolError(b.tool("project_read", {{"tracks", {3}}})), "level 0 refuses tracks, which it would ignore");
    check(isToolError(b.tool("project_read", {{"fromBar", 1}})), "level 0 refuses fromBar");
    check(isToolError(b.tool("project_read", {{"level", 1}, {"clips", {1}}})),
          "level 1 refuses clips, which only level 2 reads");
    check(!isToolError(b.tool("project_read", {{"level", 2}, {"clips", {1}}})), "level 2 takes clips");
    const Json emptyWindow = b.tool("project_read", {{"level", 1}, {"fromBar", 5}, {"toBar", 5}});
    check(isToolError(emptyWindow) &&
              emptyWindow.at("content").at(0).value("text", "").find("fromBar is before toBar") != std::string::npos,
          "fromBar must be before toBar");
    check(!isToolError(b.tool("project_read", {{"level", 1}, {"fromBar", 1}, {"toBar", 5}})), "a real window reads");
    check(isToolError(b.tool("project_read", {{"levl", 0}})), "an unknown argument is refused");
    const Json other = b.tool("project_read", {{"project", "file-0000000000000000"}});
    check(isToolError(other) && other.at("content").at(0).value("text", "").find("no open project") != std::string::npos,
          "AC-17: a request naming a project that is not open touches nothing");
    check(!isToolError(b.tool("project_read", {{"project", p0.value("project", "")}})), "its own handle reads");

    const Json idx = b.tool("ops_describe", Json::object()).at("structuredContent");
    bool connect = false, transport = false;
    for (const auto& o : idx.at("ops")) {
        connect = connect || o.value("name", "") == "routing.connect";
        transport = transport || o.value("name", "").rfind("transport.", 0) == 0;
    }
    check(connect && !transport && idx.value("notAvailable", 0) > 0,
          "the index lists what a changeset may contain, and no transport op");
    const Json rc = b.tool("ops_describe", {{"names", {"routing.connect"}}}).at("structuredContent").at("ops").at(0);
    const auto& kinds = rc.at("inputSchema").at("properties").at("kind").at("enum");
    check(std::find(kinds.begin(), kinds.end(), Json("sidechain")) != kinds.end() && rc.value("agentMayEmit", false),
          "routing.connect's generated schema lists sidechain");
    check(isToolError(b.tool("ops_describe", {{"names", {"no.such"}}})), "an unknown op name is refused");
    const Json both = b.tool("ops_describe", {{"names", {"routing.connect"}}, {"query", "route"}});
    check(isToolError(both) && both.at("content").at(0).value("text", "").find("not both") != std::string::npos,
          "names and query together are refused");
    const Json badQuery = b.tool("ops_describe", {{"names", {"routing.connect"}}, {"query", 5}});
    check(isToolError(badQuery) && badQuery.at("content").at(0).value("text", "").find("query is text") !=
                                       std::string::npos,
          "the query is validated first, even beside names");
}

// --- propose ---------------------------------------------------------------------------

void testProposeLeavesTheFile() {
    section("AC-23: headless read, propose a volume change, see the diff -- file unchanged");
    ProjectFile f("propose");
    const std::string before = fileBytes(f.path);
    Bridge b(openHeadless(f.path));
    b.initialize();
    const Json head = b.tool("project_read", Json::object()).at("structuredContent").at("head");

    const Json r = b.tool("changeset_propose",
                          {{"request", "Bring the bass down 6 dB"},
                           {"baseHead", head},
                           {"ops", {{{"op", "mixer.setVolume"}, {"payload", {{"id", 3}, {"db", -6.0}}},
                                     {"label", "Bass -6 dB"}}}}});
    check(!isToolError(r), "proposed: " + r.dump().substr(0, 300));
    const Json& p = r.at("structuredContent");
    check(p.value("status", "") == "awaiting_review" && p.value("id", "") == "cs-1", "queued, awaiting review");
    check(!p.value("diff", "").empty() && p.at("changes").size() == 1, "the diff and the change list");
    const auto& change = p.at("changes").at(0);
    check(change.at("before").value("db", 1.0) == 0.0 && change.at("after").value("db", 0.0) == -6.0,
          "the change shows before 0 dB and after -6 dB");
    check(fileBytes(f.path) == before, "the project file's bytes are unchanged");
    check(p.contains("changeset") && p.at("changeset").at("ops").at(0).value("op", "") == "mixer.setVolume" &&
              p.at("changeset").at("baseHead") == head,
          "the proposal carries the changeset itself, in adi_tool's form");
    check(p.value("apply", "").find("this result's") != std::string::npos,
          "the apply hint says to save this result's changeset");

    const Json s = b.tool("changeset_status", {{"id", "cs-1"}}).at("structuredContent");
    check(s.value("status", "") == "awaiting_review" && s.at("changeset").at("ops").at(0).value("op", "") ==
                                                            "mixer.setVolume",
          "status returns the changeset while it awaits review");
    const std::string actor = s.at("changeset").value("actorDetail", "");
    check(actor.find("Claude Code 2.1") != std::string::npos && actor.find("cs-1") != std::string::npos,
          "actor_detail names the client and the changeset: " + actor);
    check(isToolError(b.tool("changeset_status", {{"id", "cs-999"}})), "an unknown id is refused");

    // Another client of the SAME server, with its own credential.
    b.grants.grant("cred-other", Tier::Propose);
    McpEndpoint::Session other;
    other.credential = "cred-other";
    b.sendAs(other, {{"jsonrpc", "2.0"}, {"id", 1}, {"method", "initialize"},
                     {"params", {{"protocolVersion", "2025-11-25"}}}});
    const Json theirs = b.toolAs(other, "changeset_status", {{"id", "cs-1"}});
    check(isToolError(theirs) && theirs.at("content").at(0).value("text", "").find("no changeset of yours") !=
                                     std::string::npos,
          "another client of the same server cannot see a changeset that is not its own");
    check(!isToolError(b.tool("changeset_status", {{"id", "cs-1"}})), "while its owner still can");
}

void testRefusals() {
    section("refusals: op not allowed, invalid, empty, stale, too long, not granted");
    ProjectFile f("refusals");
    Bridge b(openHeadless(f.path));
    b.initialize();
    const Json head = b.head();
    const auto propose = [&b, &head](Json ops, Json extra = Json::object()) {
        Json args = {{"request", "test"}, {"ops", std::move(ops)}, {"baseHead", head}};
        for (auto it = extra.begin(); it != extra.end(); ++it) args[it.key()] = it.value();
        return b.tool("changeset_propose", std::move(args));
    };
    check(refusalOf(propose({{{"op", "transport.play"}, {"payload", Json::object()}}})) == "op not allowed",
          "transport is the user's: op not allowed");
    const Json bogus = propose({{{"op", "routing.connect"},
                                 {"payload", {{"id", 1}, {"srcKind", "track"}, {"src", 1}, {"dstKind", "track"},
                                              {"dst", 3}, {"kind", "bogus"}}}}});
    check(refusalOf(bogus) == "invalid" &&
              bogus.at("structuredContent").at("error").at("data").at("issues").at(0).value("key", "") == "kind",
          "a kind outside the CHECK list is refused, naming kind");
    check(refusalOf(propose(Json::array())) == "empty", "an empty changeset is refused");
    const Json noBase = b.tool("changeset_propose",
                               {{"request", "test"},
                                {"ops", {{{"op", "mixer.setVolume"}, {"payload", {{"id", 3}, {"db", -1.0}}}}}}});
    check(refusalOf(noBase) == "invalid" &&
              noBase.at("content").at(0).value("text", "").find("baseHead is required") != std::string::npos,
          "baseHead is required: the head project_read returned");
    const Json stale = propose({{{"op", "mixer.setVolume"}, {"payload", {{"id", 3}, {"db", -1.0}}}}},
                               {{"baseHead", 999}});
    check(refusalOf(stale) == "stale" && stale.at("structuredContent").at("error").at("code") == errc::Stale &&
              stale.at("structuredContent").at("error").at("data").contains("headFound"),
          "a changeset built on another head is stale (−32002, headFound), never rebased");
    check(refusalOf(propose({{{"op", "mixer.setVolume"}, {"payload", {{"id", 3}, {"db", -1.0}}}, {"x", 1}}})) ==
              "invalid",
          "an unknown key beside op and payload is refused");
    check(refusalOf(propose({{{"op", "track.rename"},
                              {"payload", {{"id", 3}, {"name", std::string(kRemoteMaxTextLength + 1, 'n')}}}}})) ==
              "invalid",
          "the remote bounds: a name over 16 KiB characters is refused");

    std::string hebrew;
    for (int i = 0; i < 2100; ++i) hebrew += "\xD7\x90";   // 2,100 letters, 4,200 bytes
    check(!isToolError(propose({{{"op", "mixer.setVolume"}, {"payload", {{"id", 3}, {"db", -1.0}}},
                                 {"label", hebrew.substr(0, 400)}}},
                               {{"request", hebrew}})),
          "lengths are characters, as the schema says: 2,100 Hebrew letters (4,200 bytes) are a valid "
          "request and 200 a valid label");
    check(refusalOf(propose({{{"op", "mixer.setVolume"}, {"payload", {{"id", 3}, {"db", -1.0}}},
                              {"label", std::string(201, 'l')}}})) == "invalid",
          "a 201-character label is one too many");
    std::string tooLong;
    for (int i = 0; i < 4097; ++i) tooLong += "\xD7\x90";
    check(refusalOf(propose({{{"op", "mixer.setVolume"}, {"payload", {{"id", 3}, {"db", -1.0}}}}},
                            {{"request", tooLong}})) == "invalid",
          "4,097 characters are one too many");

    Bridge capped(openHeadless(f.path));
    capped.ctx.limits.maxOps = 2;   // read at call time by the screen
    capped.initialize();
    const Json over = capped.tool(
        "changeset_propose",
        {{"request", "three"},
         {"baseHead", head},
         {"ops",
          {{{"op", "mixer.setVolume"}, {"payload", {{"id", 1}, {"db", -1.0}}}},
           {{"op", "mixer.setVolume"}, {"payload", {{"id", 2}, {"db", -1.0}}}},
           {{"op", "mixer.setVolume"}, {"payload", {{"id", 3}, {"db", -1.0}}}}}}});
    check(refusalOf(over) == "too many ops" &&
              over.at("structuredContent").at("error").at("data").value("cap", 0) == 2,
          "three ops over a cap of 2: too many ops, with the cap");

    Bridge observer(openHeadless(f.path), Tier::Observe);
    observer.initialize();
    const Json denied = observer.tool("changeset_propose",
                                      {{"request", "x"},
                                       {"baseHead", head},
                                       {"ops", {{{"op", "mixer.setVolume"}, {"payload", {{"id", 3}, {"db", -1.0}}}}}}});
    check(isToolError(denied) &&
              denied.at("structuredContent").at("error").value("message", "") == "not granted",
          "at Observe, proposing is not granted");
    check(!isToolError(observer.tool("project_read", Json::object())), "but reading is");
    std::set<std::string> offered;
    const Json observerTools = observer.request(2, "tools/list");
    for (const auto& t : observerTools.at("result").at("tools")) offered.insert(t.value("name", ""));
    check(!offered.count("changeset_propose") && offered.count("project_read"),
          "an Observe client is not offered changeset_propose at all");
}

void testIdempotentPropose() {
    section("one request id queues once (AGT-01)");
    ProjectFile f("idem");
    Bridge b(openHeadless(f.path));
    b.initialize();
    const Json args = {{"request", "quieter"},
                       {"baseHead", b.head()},
                       {"ops", {{{"op", "mixer.setVolume"}, {"payload", {{"id", 1}, {"db", -2.0}}}}}}};
    const Json key = {{"io.github.arieladi.adi/idempotencyKey", "req-42"}};
    const std::string first = b.tool("changeset_propose", args, key).at("structuredContent").value("id", "");
    const std::string again = b.tool("changeset_propose", args, key).at("structuredContent").value("id", "");
    check(!first.empty() && first == again && b.queue.size() == 1, "the same key: the same changeset, queued once");
    const std::string fresh = b.tool("changeset_propose", args).at("structuredContent").value("id", "");
    check(fresh != first && b.queue.size() == 2, "no key: a new changeset");
    for (const Json& bad : {Json(5), Json(""), Json(std::string(129, 'k'))}) {
        const Json r = b.tool("changeset_propose", args, {{"io.github.arieladi.adi/idempotencyKey", bad}});
        check(r.contains("error") && r.at("error").at("code") == errc::InvalidParams,
              "an idempotencyKey that is not 1 to 128 bytes of text is refused, not ignored: " + bad.dump().substr(0, 20));
    }
    check(b.queue.size() == 2, "and nothing was queued for them");

    // The queue's own memory (R5.8): a service that remembers nothing, so the
    // second call reaches the method and ChangesetQueue::findByKey answers.
    ServiceOptions forgetful;
    forgetful.idempotencyEntries = 0;
    Bridge q(openHeadless(f.path), Tier::Propose, std::move(forgetful));
    q.initialize();
    const Json qargs = {{"request", "quieter"},
                        {"baseHead", q.head()},
                        {"ops", {{{"op", "mixer.setVolume"}, {"payload", {{"id", 1}, {"db", -2.0}}}}}}};
    const std::string one = q.tool("changeset_propose", qargs, key).at("structuredContent").value("id", "");
    const std::string two = q.tool("changeset_propose", qargs, key).at("structuredContent").value("id", "");
    check(!one.empty() && one == two && q.queue.size() == 1,
          "with the service's cache off, the queue answers the same key with the same changeset");
    Json changed = qargs;
    changed["ops"][0]["payload"]["db"] = -3.0;
    const Json reused = q.tool("changeset_propose", changed, key);
    check(refusalOf(reused) == "invalid" &&
              reused.at("content").at(0).value("text", "").find("idempotencyKey reused") != std::string::npos &&
              q.queue.size() == 1,
          "the same key with other arguments is refused, never answered with the old proposal");
}

void testSidechainIsOneUndo() {
    section("the request's example: group, routing and a sidechain -- one changeset, one undo");
    ProjectFile f("sidechain");
    const std::string bytes = fileBytes(f.path);
    auto headless = openHeadless(f.path);
    auto bridge = std::make_unique<Bridge>(headless);
    Bridge& b = *bridge;
    b.initialize();

    const Json level0 = b.tool("project_read", Json::object()).at("structuredContent");
    const auto group = level0.at("nextIds").value("track", 0);
    const auto row = level0.at("nextIds").value("routing", 0);
    const Json r = b.tool(
        "changeset_propose",
        {{"request", "Group the drums and key the bass from the kick"},
         {"baseHead", level0.at("head")},
         {"ops",
          {{{"op", "track.create"}, {"payload", {{"id", group}, {"kind", "group"}, {"name", "Drums"}, {"index", 0}}}},
           {{"op", "track.setParent"}, {"payload", {{"id", 1}, {"parent", group}}}},
           {{"op", "track.setParent"}, {"payload", {{"id", 2}, {"parent", group}}}},
           {{"op", "routing.connect"},
            {"payload", {{"id", row}, {"srcKind", "track"}, {"src", 1}, {"dstKind", "track"}, {"dst", 3},
                         {"kind", "sidechain"}}}}}}});
    check(!isToolError(r), "the four ops preview as one changeset: " + r.dump().substr(0, 400));
    if (isToolError(r)) return;
    const std::string id = r.at("structuredContent").value("id", "");
    check(r.at("structuredContent").value("diff", "").find("Drums") != std::string::npos, "the diff shows the group");
    check(fileBytes(f.path) == bytes, "and the file is still untouched");
    const Json cs = b.tool("changeset_status", {{"id", id}}).at("structuredContent").at("changeset");
    check(r.at("structuredContent").at("changeset") == cs, "the proposal's changeset is the status's");

    // The user's act: Apply, as `adi_tool propose --apply` does it.
    StoreError e = StoreError::Ok;
    auto store = Store::open(f.path, e);
    check(store != nullptr, "the user opens the project");
    if (!store) return;
    const std::string projectionBefore = textproj::projectStore(*store).text;
    std::string err;
    const auto changeset = agent::changesetFromJson(*store, cs, err);
    check(changeset.has_value(), "the status's changeset is the adi_tool form: " + err);
    if (!changeset) return;
    const auto applied = agent::apply(*store, *changeset);
    check(applied.ok, "applied: " + applied.error);
    History history(*store);
    const auto step = history.nextUndo();
    check(step && step->opCount == 4 && step->actor == "agent",
          "ONE undo step holds all four ops, attributed to the agent");

    const Json status = b.tool("changeset_status", {{"id", id}}).at("structuredContent");
    check(status.value("status", "") == "applied", "status: applied (" + status.value("status", "") + ")");
    check(status.value("note", "").find("may since have been undone") != std::string::npos,
          "applied says it may since have been undone");
    const Json graph = Json::parse(b.request(50, "resources/read",
                                             {{"uri", "adi://projects/" + headless->handle() + "/graph"}})
                                       .at("result").at("contents").at(0).value("text", "{}"));
    check(graph.at("routing").at("sidechain").size() == 1 &&
              graph.at("routing").at("sidechain").at(0).at("from").value("id", 0) == 1,
          "the graph shows the sidechain row, kick to bass");

    const auto undone = history.undo();
    check(undone.ok && undone.opsApplied == 4, "one Ctrl+Z undoes all four");
    check(textproj::projectStore(*store).text == projectionBefore, "and the project is exactly as it was");

    bridge.reset();
    headless.reset();
    store->close();
}

void testResources() {
    section("resources: manifest, graph, telemetry, ops");
    ProjectFile f("resources");
    auto headless = openHeadless(f.path);
    Bridge b(headless);
    b.initialize();
    const std::string base = "adi://projects/" + headless->handle() + "/";
    std::set<std::string> uris;
    // Named, not iterated in place: before C++23 a range-for keeps only the
    // outermost temporary alive, and `.at()` of a temporary dangles.
    const Json listed = b.request(2, "resources/list");
    for (const auto& r : listed.at("result").at("resources")) uris.insert(r.value("uri", ""));
    check(uris.count(base + "manifest") && uris.count(base + "graph") && uris.count(base + "telemetry") &&
              uris.count("adi://ops"),
          "the bound project's three resources and the op index");
    check(b.request(3, "resources/templates/list").at("result").at("resourceTemplates").size() == 4, "four templates");

    const Json read = b.request(4, "resources/read", {{"uri", base + "manifest"}}).at("result").at("contents").at(0);
    const Json m = Json::parse(read.value("text", "{}"));
    std::int64_t rate = 0;
    {
        StoreError e = StoreError::Ok;
        auto ro = Store::open(f.path, e, /*readOnly=*/true);
        if (ro) rate = rows::readModel(*ro).project.sampleRate;
    }
    check(read.value("mimeType", "") == "application/json" && rate > 0 && m.value("sampleRate", 0) == rate &&
              m.value("tracks", 0) == 3 &&
              m.at("schema").value("build", "") == std::to_string(kSchemaMajor) + "." + std::to_string(kSchemaMinor),
          "the manifest: sample rate, tracks, schema");
    const Json op = Json::parse(b.request(5, "resources/read", {{"uri", "adi://ops/routing.connect"}})
                                    .at("result").at("contents").at(0).value("text", "{}"));
    check(op.at("ops").at(0).value("name", "") == "routing.connect", "one op's schema as a resource");
    constexpr int kResourceNotFound = -32002;   // 2025-11-25 resources, legacy sessions only
    const Json missing = b.request(6, "resources/read", {{"uri", "adi://projects/nope/manifest"}});
    check(missing.at("error").at("code") == kResourceNotFound &&
              missing.at("error").at("data").value("uri", "") == "adi://projects/nope/manifest",
          "legacy: an unknown project's resource is −32002 with the uri");
    const Json weird = b.request(7, "resources/read", {{"uri", "adi://weird"}});
    check(weird.at("error").at("code") == kResourceNotFound && weird.at("error").at("data").value("uri", "") ==
                                                                     "adi://weird",
          "legacy: an unknown resource is −32002 with the uri");
    check(b.request(70, "resources/read", {{"uri", "adi://ops/no.such"}}).at("error").at("code") == kResourceNotFound,
          "legacy: an unknown op's schema is not found");
    const Json missingModern = b.request(71, "resources/read", modern({{"uri", "adi://projects/nope/manifest"}}));
    check(missingModern.at("error").at("code") == errc::InvalidParams &&
              missingModern.at("error").at("data").value("uri", "") == "adi://projects/nope/manifest",
          "modern: not found is −32602 (2026-07-28 must not emit −32002)");
    check(b.request(72, "resources/read", modern({{"uri", "adi://weird"}})).at("error").at("code") ==
              errc::InvalidParams,
          "modern: an unknown resource is −32602");
    check(auditedRefusal(b.service, "resources/read", kResourceNotFound),
          "a not-found made at the MCP layer is audited");
    const Json tele = Json::parse(b.request(8, "resources/read", {{"uri", base + "telemetry"}})
                                      .at("result").at("contents").at(0).value("text", "{}"));
    check(tele.value("available", true) == false &&
              tele.value("reason", "").find("no audio engine") != std::string::npos,
          "telemetry, headless: unavailable, and says why");
    Json readModern = modern({{"uri", base + "manifest"}});
    const Json mr = b.request(9, "resources/read", readModern).at("result");
    check(mr.value("resultType", "") == "complete" && mr.value("cacheScope", "") == "private" && mr.contains("ttlMs"),
          "a modern read carries the cache fields");
}

/// A live project: the headless reads, plus a board a sampler fills, as the
/// app host (CTL-09) will provide.
class LiveProject final : public ProjectAccess {
public:
    LiveProject(std::shared_ptr<HeadlessProject> inner, const TelemetryBoard& board)
        : inner_(std::move(inner)), board_(board) {}
    const std::string& handle() const override { return inner_->handle(); }
    std::string label() const override { return inner_->label(); }
    ReadSnapshot read(std::string& e) override { return inner_->read(e); }
    std::optional<std::int64_t> head(std::string& e) override { return inner_->head(e); }
    std::string fileSchema() override { return inner_->fileSchema(); }
    agent::Preview preview(const agent::Changeset& c, const agent::Limits& l) override { return inner_->preview(c, l); }
    bool wasApplied(const std::string& a) override { return inner_->wasApplied(a); }
    bool live() const override { return true; }
    std::shared_ptr<const TelemetryFrame> telemetry() const override { return board_.latest(); }
    std::string applyHint() const override { return "apply it in ADI"; }

private:
    std::shared_ptr<HeadlessProject> inner_;
    const TelemetryBoard& board_;
};

void testTelemetry() {
    section("telemetry: what the engine already publishes, labelled honestly");
    engine::StripMeter kick, silent;
    kick.peak.store(0.5f);
    kick.rms.store(0.25f);
    engine::MainLoudnessReadout loudness;   // momentary stays -inf: not warmed up
    loudness.shortTermLufs.store(-15.0f);
    loudness.integratedLufs.store(-16.0f);
    loudness.truePeakDb.store(-1.0f);
    MeterTaps taps;
    taps.strips = {{1, &kick}, {2, &silent}};
    taps.main = &loudness;
    taps.transport = [] { return TransportReading{true, 96000, 48000.0}; };

    TelemetryBoard board;
    check(board.latest() == nullptr, "an empty board has no frame");
    board.publish(sample(taps, 7, 1000));
    const auto frame = board.latest();
    check(frame && frame->sequence == 7 && frame->strips.size() == 2, "a frame per sample");
    const Json t = telemetryJson(frame.get(), 1250, true);
    check(t.value("available", false) && t.value("ageMs", 0) == 250 && t.value("sequence", 0) == 7,
          "available, with its age and sequence");
    check(std::fabs(t.at("strips").at(0).value("peakDbfs", 0.0) - (-6.02)) < 0.01, "0.5 linear is -6.02 dBFS");
    check(t.at("strips").at(0).value("peak", 0.0) == 0.5 && t.at("strips").at(0).value("rms", 0.0) == 0.25,
          "the raw peak 0.5 and rms 0.25");
    check(std::fabs(t.at("strips").at(0).value("rmsDbfs", 0.0) - (-12.04)) < 0.01, "0.25 linear is -12.04 dBFS");
    check(t.at("main").value("shortTermLufs", 0.0) == -15.0 && t.at("main").value("truePeakDbtp", 0.0) == -1.0,
          "short-term -15 LUFS and true peak -1 dBTP, each in its own field");
    check(t.at("strips").at(1).at("peakDbfs").is_null(), "silence is null, never -inf");
    check(t.at("main").at("momentaryLufs").is_null() && t.at("main").value("integratedLufs", 0.0) == -16.0,
          "an unwarmed window is null; integrated -16 LUFS");
    check(t.at("transport").value("playing", false) && t.at("transport").value("positionSamples", 0) == 96000,
          "the transport");
    bool masking = false;
    for (const auto& u : t.at("unavailable")) masking = masking || u.value("what", "") == "masking pairs";
    check(masking && t.at("semantics").value("strips", "").find("not held") != std::string::npos,
          "what is not measured is listed, and what is measured says it is not held");
    check(telemetryJson(nullptr, 0, true).value("available", true) == false, "live but no frame yet: unavailable");

    ProjectFile f("telemetry");
    auto live = std::make_shared<LiveProject>(openHeadless(f.path), board);
    Bridge b(live);
    b.initialize();
    const Json read = Json::parse(b.request(2, "resources/read",
                                            {{"uri", "adi://projects/" + live->handle() + "/telemetry"}})
                                      .at("result").at("contents").at(0).value("text", "{}"));
    check(read.value("available", false) && read.at("strips").size() == 2, "the resource serves the live board");
    const Json manifestLive = Json::parse(b.request(3, "resources/read",
                                                    {{"uri", "adi://projects/" + live->handle() + "/manifest"}})
                                              .at("result").at("contents").at(0).value("text", "{}"));
    check(manifestLive.contains("transport") && manifestLive.at("transport").value("playing", false),
          "a live manifest carries the transport");
}

void testHostileLines() {
    section("hostile lines never crash the loop");
    ProjectFile f("hostile");
    {
        StoreError e = StoreError::Ok;
        auto s = Store::open(f.path, e);
        // A name that is not valid UTF-8, as a corrupt or foreign file may hold.
        s->db().exec("UPDATE tracks SET name = CAST(X'4261FF64' AS TEXT) WHERE id = 1");
        s->close();
    }
    Bridge b(openHeadless(f.path));
    std::string deep = R"({"jsonrpc":"2.0","id":9,"method":"x","params":{"a":)";
    for (int i = 0; i < 200; ++i) deep += "[";
    for (int i = 0; i < 200; ++i) deep += "]";
    deep += "}}";
    std::ostringstream input;
    input << deep << "\n"
          << R"([{"jsonrpc":"2.0","id":1,"method":"ping"}])" << "\n"
          << R"({"jsonrpc":"2.0","id":null,"method":"ping"})" << "\n"
          << "this is not json\n"
          << std::string((std::size_t{4} << 20) + 10, 'x') << "\n"
          << "\n"
          << R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2025-11-25"}})" << "\n"
          << R"({"jsonrpc":"2.0","id":2,"method":"tools/call","params":{"name":"project_read","arguments":{}}})"
          << "\r\n";
    std::istringstream in(input.str());
    std::ostringstream out, logs;
    runStdio(in, out, b.endpoint, b.session, WireLimits{}, &logs);

    std::istringstream lines(out.str());
    std::vector<Json> replies;
    std::string line;
    bool allJson = true;
    while (std::getline(lines, line)) {
        const Json j = Json::parse(line, nullptr, false);
        allJson = allJson && !j.is_discarded();
        replies.push_back(j);
    }
    check(allJson, "every reply is one line of valid JSON");
    bool connected = false, disconnected = false;
    for (const auto& ev : b.service.audit().snapshot()) {
        connected = connected || ev.kind == "connect";
        disconnected = disconnected || ev.kind == "disconnect";
    }
    check(connected && disconnected, "the audit records the connection and its end");
    check(replies.size() == 7, "five refusals, then initialize and the read: " + std::to_string(replies.size()));
    if (replies.size() == 7) {
        check(replies[0].at("error").at("code") == errc::InvalidRequest, "too deep: refused");
        bool noNullIds = true;
        for (std::size_t i = 0; i < 5; ++i) noNullIds = noNullIds && !replies[i].contains("id");
        check(noNullIds, "a refusal whose id could not be read carries no id (MCP ids are never null)");
        check(replies[1].at("error").at("code") == errc::InvalidRequest, "a batch: refused");
        check(replies[2].at("error").at("code") == errc::InvalidRequest, "a null id: refused");
        check(replies[3].at("error").at("code") == errc::ParseError, "not JSON: a parse error");
        check(replies[4].at("error").at("code") == errc::InvalidRequest, "over 4 MiB: refused, and the loop goes on");
        check(replies[5].at("id") == 1 && replies[5].contains("result"), "then initialize is answered");
        const auto& tracks = replies[6].at("result").at("structuredContent").at("tracks");
        check(tracks.at(0).value("name", "") == "Ba\xEF\xBF\xBD" "d",
              "a name that is not UTF-8 has the bad byte replaced by U+FFFD, not fatal");
    }
}

void testGrantGate() {
    section("no grant: the MCP layer answers nothing about tools or projects");
    ProjectFile f("gate");
    Bridge b(openHeadless(f.path));
    b.initialize();
    b.grants.revoke(b.session.credential);
    for (const char* method : {"tools/list", "resources/list", "resources/templates/list"}) {
        const Json r = b.request(7, method);
        check(r.contains("error") && r.at("error").at("code") == errc::NotGranted &&
                  r.at("error").value("message", "") == "not granted",
              std::string(method) + " without a grant: -32006, one text");
    }
    check(auditedRefusal(b.service, "tools/list", errc::NotGranted),
          "an ungranted tools/list leaves a refused event with -32006 in the audit");
    const Json unknownTool = b.request(8, "tools/call", {{"name", "nope"}, {"arguments", Json::object()}});
    check(unknownTool.at("error").at("code") == errc::NotGranted,
          "even an unknown tool: the grant comes first, so the tool names are not given away");
    const Json readUngranted = b.request(10, "resources/read", {{"uri", "adi://ops"}});
    check(readUngranted.contains("error") && readUngranted.at("error").at("code") == errc::NotGranted,
          "resources/read without a grant: -32006");
    b.grants.grant(b.session.credential, Tier::Propose);
    b.request(11, "no/such/method");
    check(auditedRefusal(b.service, "(unknown method)", errc::MethodNotFound),
          "an unknown method is audited under a fixed name, never the wire's text");
    b.request(12, "tools/call", {{"name", "nope"}, {"arguments", Json::object()}});
    check(auditedRefusal(b.service, "tools/call", errc::InvalidParams), "an unknown tool is audited");
    check(b.request(9, "server/discover", modern(Json::object())).contains("result"),
          "server/discover, the protocol's static self-description, still answers");
}

void testTwoProjects() {
    section("AC-17: two projects open, a request naming B never touches A");
    ProjectFile fa("two-a"), fb("two-b");
    const std::string bytesA = fileBytes(fa.path);
    auto a = openHeadless(fa.path);
    auto bProject = openHeadless(fb.path);
    check(a->handle() != bProject->handle(), "two files, two handles");
    Bridge b(std::vector<std::shared_ptr<ProjectAccess>>{a, bProject});
    b.initialize();
    const Json unnamed = b.tool("project_read", Json::object());
    check(isToolError(unnamed) &&
              unnamed.at("content").at(0).value("text", "").find("several projects are open") != std::string::npos,
          "with two open, a request must name its project");
    const Json readB = b.tool("project_read", {{"project", bProject->handle()}});
    check(!isToolError(readB) && readB.at("structuredContent").value("project", "") == bProject->handle(),
          "a read naming B reads B");
    const Json proposeB = b.tool("changeset_propose",
                                 {{"project", bProject->handle()},
                                  {"request", "B's bass down"},
                                  {"baseHead", readB.at("structuredContent").at("head")},
                                  {"ops", {{{"op", "mixer.setVolume"}, {"payload", {{"id", 3}, {"db", -4.0}}}}}}});
    check(!isToolError(proposeB) && proposeB.at("structuredContent").value("project", "") == bProject->handle(),
          "a proposal naming B is B's");
    check(a->scratchFolder().empty() && !bProject->scratchFolder().empty(),
          "only B was snapshotted: A was not touched");
    check(fileBytes(fa.path) == bytesA, "A's bytes are unchanged");
}

void testProjectionDetails() {
    section("the projection: devices inside racks, key events, the next free ids");
    ProjectFile f("racks");
    {
        StoreError e = StoreError::Ok;
        auto s = Store::open(f.path, e);
        s->db().exec("INSERT INTO device_chains(id, track_id, ord) VALUES (1, 3, 0)");
        s->db().exec("INSERT INTO devices(id, chain_id, ord, name, is_rack) VALUES (10, 1, 0, 'Rack', 1)");
        s->db().exec("INSERT INTO device_chains(id, parent_device_id, ord) VALUES (2, 10, 0)");
        s->db().exec("INSERT INTO devices(id, chain_id, ord, name) VALUES (11, 2, 0, 'Synth')");
        s->db().exec("INSERT INTO plugin_params(device_id, param_id, normalized_value) VALUES (11, 'cutoff', 0.5)");
        s->close();
    }
    Bridge b(openHeadless(f.path));
    b.initialize();
    const Json l0 = b.tool("project_read", Json::object()).at("structuredContent");
    const auto& bass = l0.at("tracks").at(2).at("devices");
    bool synthInRack = false;
    for (const auto& d : bass) synthInRack = synthInRack || (d.value("id", 0) == 11 && d.value("inRack", 0) == 10);
    check(bass.size() == 2 && synthInRack, "the synth inside the rack is listed under its track, in its rack");
    const auto& next = l0.at("nextIds");
    check(next.contains("keyEvent") && next.contains("remark") && next.contains("tuning") && next.contains("note") &&
              next.value("device", 0) == 12 && next.value("chain", 0) == 3,
          "nextIds covers key events, remarks, tunings and notes too");
    check(l0.contains("keyEvents") && l0.at("keyEvents").is_array(), "key events are listed with their ids");
    const Json l1 = b.tool("project_read", {{"level", 1}, {"tracks", {3.0}}}).at("structuredContent");
    bool cutoff = false;
    for (const auto& prm : l1.at("params")) cutoff = cutoff || (prm.value("device", 0) == 11 && prm.value("param", "") == "cutoff");
    check(cutoff, "level 1 reads the racked synth's parameters (tracks given as 3.0, an integer)");
    check(l1.contains("remarks") && l1.contains("tunings"), "level 1 carries remarks and tunings");
}

void testStatusStaleAndExpired() {
    section("changeset_status: stale after the head moves, expired after 24 hours");
    const auto volumeDown = [](const Json& head) {
        return Json{{"request", "quieter"},
                    {"baseHead", head},
                    {"ops", {{{"op", "mixer.setVolume"}, {"payload", {{"id", 2}, {"db", -3.0}}}}}}};
    };
    {
        ProjectFile f("stale");
        Bridge b(openHeadless(f.path));
        b.initialize();
        const Json head = b.head();
        const std::string id = b.tool("changeset_propose", volumeDown(head)).at("structuredContent").value("id", "");
        check(id == "cs-1", "proposed");

        // The user edits the file meanwhile: the head moves.
        {
            StoreError e = StoreError::Ok;
            auto store = Store::open(f.path, e);
            check(store != nullptr, "the user opens the project writable");
            if (!store) return;
            OpJournal j(*store);
            OpRequest r;
            r.opType = "track.rename";
            r.payload = {{"id", 1}, {"name", "Kick In"}};
            r.label = "Rename Kick";
            check(j.commit(r).ok, "an external commit");
            store->close();
        }
        const Json s1 = b.tool("changeset_status", {{"id", id}}).at("structuredContent");
        check(s1.value("status", "") == "stale" && !s1.contains("changeset") && s1.contains("why"),
              "stale once the head moved, with no changeset to apply (" + s1.value("status", "") + ")");
        check(s1.contains("headNow") && !s1.at("headNow").is_null() && s1.at("headNow") != s1.at("baseHead") &&
                  !s1.contains("headError"),
              "headNow is the moved head");
    }
    {
        ProjectFile f("expired");
        Bridge b(openHeadless(f.path));
        b.initialize();
        std::int64_t steadyNow = 1000;
        b.ctx.steadyMs = [&steadyNow] { return steadyNow; };   // read at call time by both methods
        const std::string id =
            b.tool("changeset_propose", volumeDown(b.head())).at("structuredContent").value("id", "");
        check(b.tool("changeset_status", {{"id", id}}).at("structuredContent").value("status", "") == "awaiting_review",
              "awaiting review within the window");
        steadyNow += ChangesetQueue::kReviewWindowMs + 1;
        const Json s2 = b.tool("changeset_status", {{"id", id}}).at("structuredContent");
        check(s2.value("status", "") == "expired" && !s2.contains("changeset") && s2.contains("why"),
              "expired more than 24 hours later (" + s2.value("status", "") + ")");
    }
}

void testSweepAbandonedScratch() {
    section("a snapshot folder whose process is gone is swept; this process's is kept");
    std::error_code ec;
    const fs::path base = fs::temp_directory_path(ec);
    check(!ec, "a temp folder");
    if (ec) return;
    // No process can have this id: above Linux's pid_max (2^22) and macOS's
    // 99999, not a multiple of 4 on Windows; and within pid_t, which the
    // sweep needs before it calls a process gone.
    const fs::path dead = base / "adi-mcp-2147483646-x";
    const fs::path mine = base / ("adi-mcp-" + std::to_string(adi::test::processId()) + "-x");
    fs::create_directories(dead, ec);
    fs::create_directories(mine, ec);
    {
        std::ofstream copy(dead / "snapshot.adi", std::ios::binary);
        copy << "somebody's unreleased project";
    }
    const std::size_t removed = HeadlessProject::sweepAbandonedScratch();
    check(!fs::exists(dead, ec) && removed >= 1, "the dead process's folder is removed");
    check(fs::exists(mine, ec), "this process's folder is kept");
    fs::remove_all(mine, ec);
    fs::remove_all(dead, ec);
}

// --- the real binary --------------------------------------------------------------------

std::string quote(const fs::path& path) {
#if defined(_WIN32)
    return "\"" + path.string() + "\"";
#else
    std::string result = "'";
    for (const char c : path.string()) result += c == '\'' ? "'\\''" : std::string(1, c);
    return result + "'";
#endif
}

void setHome(const fs::path& p) {
#if defined(_WIN32)
    _wputenv_s(L"ADI_HOME", p.wstring().c_str());
#else
    setenv("ADI_HOME", p.string().c_str(), 1);
#endif
}

void testBinary() {
    section("adi-mcp, the binary, over pipes");
    ProjectFile f("binary");
    const std::string bytes = fileBytes(f.path);
    const fs::path home = f.temp.path() / "home";
    fs::create_directories(home);
    setHome(home);
    const fs::path in = f.temp.path() / "in.jsonl", out = f.temp.path() / "out.jsonl", err = f.temp.path() / "err.txt";
    {
        std::ofstream o(in, std::ios::binary);
        o << R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2025-11-25","capabilities":{},"clientInfo":{"name":"smoke","version":"1"}}})"
          << "\n"
          << R"({"jsonrpc":"2.0","method":"notifications/initialized"})" << "\n"
          << R"({"jsonrpc":"2.0","id":2,"method":"tools/list"})" << "\n"
          << R"({"jsonrpc":"2.0","id":3,"method":"tools/call","params":{"name":"project_read","arguments":{}}})"
          << "\n";
    }
    std::string command = quote(fs::path(ADI_MCP_PATH)) + " --project " + quote(f.path) + " < " + quote(in) +
                          " > " + quote(out) + " 2> " + quote(err);
#if defined(_WIN32)
    command = "\"" + command + "\"";
#endif
    const int rc = std::system(command.c_str());
    check(rc == 0, "adi-mcp exits 0 at the end of its input (" + std::to_string(rc) + "): " + fileBytes(err));
    std::istringstream lines(fileBytes(out));
    std::vector<Json> replies;
    std::string line;
    while (std::getline(lines, line))
        if (!line.empty()) replies.push_back(Json::parse(line, nullptr, false));
    check(replies.size() == 3, "three requests, three replies, nothing else on stdout");
    if (replies.size() == 3) {
        check(replies[0].value("id", 0) == 1 && replies[0].at("result").value("protocolVersion", "") == "2025-11-25",
              "initialize");
        check(replies[1].at("result").at("tools").size() == 4, "tools/list");
        check(replies[2].at("result").at("structuredContent").at("tracks").size() == 3, "project_read");
    }
    check(fileBytes(f.path) == bytes, "the file is unchanged by the binary");
    check(fileBytes(err).find("read-only") != std::string::npos, "it says on stderr that it is read-only");

    // --tier observe: the user's lower tier; changeset_propose is not offered.
    const fs::path observeIn = f.temp.path() / "observe-in.jsonl", observeOut = f.temp.path() / "observe-out.jsonl";
    {
        std::ofstream o(observeIn, std::ios::binary);
        o << R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2025-11-25","capabilities":{},"clientInfo":{"name":"smoke","version":"1"}}})"
          << "\n"
          << R"({"jsonrpc":"2.0","id":2,"method":"tools/list"})" << "\n";
    }
    std::string observe = quote(fs::path(ADI_MCP_PATH)) + " --project " + quote(f.path) + " --tier observe < " +
                          quote(observeIn) + " > " + quote(observeOut) + " 2> " + quote(err);
#if defined(_WIN32)
    observe = "\"" + observe + "\"";
#endif
    const int observeRc = std::system(observe.c_str());
    check(observeRc == 0, "adi-mcp --tier observe exits 0 (" + std::to_string(observeRc) + "): " + fileBytes(err));
    std::istringstream observeLines(fileBytes(observeOut));
    std::vector<Json> observed;
    while (std::getline(observeLines, line))
        if (!line.empty()) observed.push_back(Json::parse(line, nullptr, false));
    check(observed.size() == 2, "two requests, two replies");
    if (observed.size() == 2 && observed[1].contains("result")) {
        std::set<std::string> offered;
        for (const auto& t : observed[1].at("result").at("tools")) offered.insert(t.value("name", ""));
        check(!offered.count("changeset_propose") && offered.count("project_read") && offered.size() == 3,
              "at --tier observe, tools/list omits changeset_propose");
    } else {
        check(false, "at --tier observe, tools/list answers");
    }
}

int runAll() {
    testHandshakes();
    testToolsAndReads();
    testProposeLeavesTheFile();
    testRefusals();
    testIdempotentPropose();
    testSidechainIsOneUndo();
    testResources();
    testTelemetry();
    testHostileLines();
    testGrantGate();
    testTwoProjects();
    testProjectionDetails();
    testStatusStaleAndExpired();
    testSweepAbandonedScratch();
    testBinary();
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
