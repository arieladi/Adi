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
//   * hostile lines never crash the loop, and the real adi-mcp binary over
//     pipes.

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
    Service service{inlineExecutor(), grants};
    ProjectSet projects;
    int ids = 0;
    ChangesetQueue queue{[this] { return "cs-" + std::to_string(++ids); }};
    ControlContext ctx{projects, queue, agent::Limits{}, [] { return kT0; }, {}, "adi", "test", "adi-mcp"};
    McpEndpoint endpoint{service, projects, McpServerInfo{"adi-mcp", "ADI", "test"}};
    McpEndpoint::Session session;

    explicit Bridge(std::shared_ptr<ProjectAccess> project, Tier tier = Tier::Propose) {
        projects.add(std::move(project));
        session.credential = "cred-test";
        grants.grant(session.credential, tier);
        registerControlMethods(service, ctx);
    }

    Json send(const Json& message) {
        const auto reply = endpoint.handle(parseMessage(dumpJson(message)), session);
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
        Json params = {{"name", name}, {"arguments", std::move(args)}};
        if (!meta.is_null()) params["_meta"] = std::move(meta);
        const Json reply = request(++calls, "tools/call", std::move(params));
        return reply.contains("result") ? reply.at("result") : reply;
    }
    int calls = 100;
};

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
          "exactly the four tools of ADR-0168 d3");
    check(closed, "every tool's input is a closed object");
    check(proposeWrites, "changeset_propose is not marked read-only");

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

    const Json s = b.tool("changeset_status", {{"id", "cs-1"}}).at("structuredContent");
    check(s.value("status", "") == "awaiting_review" && s.at("changeset").at("ops").at(0).value("op", "") ==
                                                            "mixer.setVolume",
          "status returns the changeset while it awaits review");
    const std::string actor = s.at("changeset").value("actorDetail", "");
    check(actor.find("Claude Code 2.1") != std::string::npos && actor.find("cs-1") != std::string::npos,
          "actor_detail names the client and the changeset: " + actor);
    check(isToolError(b.tool("changeset_status", {{"id", "cs-999"}})), "an unknown id is refused");

    Bridge stranger(openHeadless(f.path));
    stranger.session.credential = "cred-other";
    stranger.grants.grant("cred-other", Tier::Propose);
    stranger.initialize();
    check(isToolError(stranger.tool("changeset_status", {{"id", "cs-1"}})),
          "another client cannot see a changeset that is not its own");
}

void testRefusals() {
    section("refusals: op not allowed, invalid, empty, stale, too long, not granted");
    ProjectFile f("refusals");
    Bridge b(openHeadless(f.path));
    b.initialize();
    const auto propose = [&b](Json ops, Json extra = Json::object()) {
        Json args = {{"request", "test"}, {"ops", std::move(ops)}};
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

    Bridge observer(openHeadless(f.path), Tier::Observe);
    observer.initialize();
    const Json denied = observer.tool("changeset_propose",
                                      {{"request", "x"},
                                       {"ops", {{{"op", "mixer.setVolume"}, {"payload", {{"id", 3}, {"db", -1.0}}}}}}});
    check(isToolError(denied) &&
              denied.at("structuredContent").at("error").value("message", "") == "not granted",
          "at Observe, proposing is not granted");
    check(!isToolError(observer.tool("project_read", Json::object())), "but reading is");
}

void testIdempotentPropose() {
    section("one request id queues once (AGT-01)");
    ProjectFile f("idem");
    Bridge b(openHeadless(f.path));
    b.initialize();
    const Json args = {{"request", "quieter"}, {"ops", {{{"op", "mixer.setVolume"}, {"payload", {{"id", 1}, {"db", -2.0}}}}}}};
    const Json key = {{"io.github.arieladi.adi/idempotencyKey", "req-42"}};
    const std::string first = b.tool("changeset_propose", args, key).at("structuredContent").value("id", "");
    const std::string again = b.tool("changeset_propose", args, key).at("structuredContent").value("id", "");
    check(!first.empty() && first == again && b.queue.size() == 1, "the same key: the same changeset, queued once");
    const std::string fresh = b.tool("changeset_propose", args).at("structuredContent").value("id", "");
    check(fresh != first && b.queue.size() == 2, "no key: a new changeset");
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
    const Json missing = b.request(6, "resources/read", {{"uri", "adi://projects/nope/manifest"}});
    check(missing.at("error").at("code") == errc::InvalidParams && missing.at("error").at("data").value("uri", "") ==
                                                                        "adi://projects/nope/manifest",
          "an unknown project's resource: −32602 with the uri");
    check(b.request(7, "resources/read", {{"uri", "adi://weird"}}).at("error").at("code") == errc::InvalidParams,
          "an unknown resource: −32602");
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
    rows::Model readModel(std::string& e) override { return inner_->readModel(e); }
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
    check(replies.size() == 7, "five refusals, then initialize and the read: " + std::to_string(replies.size()));
    if (replies.size() == 7) {
        check(replies[0].at("error").at("code") == errc::InvalidRequest, "too deep: refused");
        check(replies[1].at("error").at("code") == errc::InvalidRequest, "a batch: refused");
        check(replies[2].at("error").at("code") == errc::InvalidRequest, "a null id: refused");
        check(replies[3].at("error").at("code") == errc::ParseError, "not JSON: a parse error");
        check(replies[4].at("error").at("code") == errc::InvalidRequest, "over 4 MiB: refused, and the loop goes on");
        check(replies[5].at("id") == 1 && replies[5].contains("result"), "then initialize is answered");
        const auto& tracks = replies[6].at("result").at("structuredContent").at("tracks");
        check(tracks.at(0).value("name", "").find("Ba") == 0, "a name that is not UTF-8 is replaced, not fatal");
    }
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
