// SPDX-License-Identifier: AGPL-3.0-or-later
//
// adi-mcp: ADI's MCP server over stdio (CTL-08; ADR-0233; docs/CONTROL-MCP.md).
//
//   adi-mcp --project <file.adi> [--tier observe|propose]
//   adi-mcp --connect [--port <port>]
//   adi-mcp --version
//
// A client (Claude Code, Cursor, Claude Desktop, the adi-agent sidecar)
// starts it as a child and speaks newline-delimited JSON-RPC on its stdin and
// stdout. Headless: it reads the file through a read-only connection,
// previews on a private snapshot, and applies nothing -- the user applies a
// changeset in ADI or with `adi_tool propose ... --apply`.
//
// The tier is the user's: the `agent.tier` setting caps it, and --tier can
// only lower it in headless mode. With `--connect`, the live ADI host owns
// permissions, disclosures, and the user's review queue.

#include "adi/control/headless_project.hpp"
#include "adi/control/loopback_bridge.hpp"
#include "adi/control/mcp.hpp"
#include "adi/control/methods.hpp"
#include "adi/control/stdio_server.hpp"
#include "adi/settings/store.hpp"
#include "adi/version.hpp"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <iostream>
#include <limits>
#include <optional>
#include <random>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#if defined(_WIN32)
#include <fcntl.h>
#include <io.h>
#endif

namespace {

namespace fs = std::filesystem;
using namespace adi::control;

int usage() {
    std::fprintf(stderr,
                 "adi-mcp -- ADI's MCP server over stdio\n"
                 "\n"
                 "  adi-mcp --project <file.adi> [--tier observe|propose]\n"
                 "                 read the project, propose changesets; never applies them\n"
                 "  adi-mcp --connect [--port <port>]\n"
                 "                 use the running ADI session; --port reads ADI_CONTROL_TOKEN\n"
                 "  adi-mcp --version\n");
    return 2;
}

std::string utf8(const fs::path& p) {
    const auto bytes = p.u8string();
    return {reinterpret_cast<const char*>(bytes.data()), bytes.size()};
}

/// The per-process credential the grant is keyed by. It never leaves this
/// process: stdio is private to the client that started us.
std::string processSecret() {
    std::random_device rd;
    static constexpr char kHex[] = "0123456789abcdef";
    std::string out;
    for (int i = 0; i < 32; ++i) {
        const auto b = static_cast<unsigned>(rd() & 0xffu);
        out += kHex[b >> 4];
        out += kHex[b & 0xfu];
    }
    return out;
}

int run(const std::vector<fs::path>& args) {
    std::optional<fs::path> project;
    std::optional<Tier> asked;
    bool connect = false;
    std::optional<std::uint16_t> port;
    for (std::size_t i = 0; i < args.size(); ++i) {
        const std::string word = utf8(args[i]);
        if (word == "--version") {
            std::printf("adi-mcp %s\n", adi::kVersion);
            return 0;
        }
        if (word == "--project" && i + 1 < args.size()) {
            project = args[++i];
        } else if (word == "--connect") {
            connect = true;
        } else if (word == "--port" && i + 1 < args.size()) {
            const auto text = utf8(args[++i]);
            unsigned value = 0;
            const auto converted = std::from_chars(text.data(), text.data() + text.size(), value);
            if (converted.ec != std::errc{} || converted.ptr != text.data() + text.size() ||
                value == 0 || value > std::numeric_limits<std::uint16_t>::max()) return usage();
            port = static_cast<std::uint16_t>(value);
        } else if (word == "--tier" && i + 1 < args.size()) {
            asked = tierFromString(utf8(args[++i]));
            if (!asked || *asked == Tier::Apply) return usage();
        } else {
            return usage();
        }
    }
    if (connect ? project.has_value() || asked.has_value() : !project || port) return usage();

#if defined(_WIN32)
    // MCP frames are lines of UTF-8; text mode would turn \n into \r\n.
    (void)_setmode(_fileno(stdin), _O_BINARY);
    (void)_setmode(_fileno(stdout), _O_BINARY);
#endif
    std::ios::sync_with_stdio(false);
    if (connect) return runLoopbackBridge(std::cin, std::cout, std::cerr, port);

    // The user's settings: agent.tier caps the tier (AI-AGENT 2), and
    // agent.maxOpsPerRequest is the per-request cap (ADR-0203 d20).
    Tier tier = Tier::Propose;
    adi::agent::Limits limits;
    std::size_t maxOpsPerMinute = 6000;
    try {
        const auto settings = adi::settings::AppSettings::forApp(adi::appdata::App::Daw);
        const auto t = settings.get("agent.tier");
        if (t.is_string() && t.get<std::string>() == "observe") tier = Tier::Observe;
        const auto cap = settings.get("agent.maxOpsPerRequest");
        if (cap.is_number_integer() && cap.get<std::int64_t>() > 0)
            limits.maxOps = static_cast<std::size_t>(cap.get<std::int64_t>());
        const auto minute = settings.get("agent.maxOpsPerMinute");
        if (minute.is_number_integer() && minute.get<std::int64_t>() > 0)
            maxOpsPerMinute = static_cast<std::size_t>(minute.get<std::int64_t>());
    } catch (const std::exception& e) {
        std::cerr << "adi-mcp: settings unreadable, using defaults: " << e.what() << '\n';
    }
    if (asked && *asked < tier) tier = *asked;

    // A copy of a project left by a bridge that crashed or was killed is
    // removed before anything else (ADR-0233; headless_project.hpp).
    if (const auto swept = HeadlessProject::sweepAbandonedScratch())
        std::cerr << "adi-mcp: removed " << swept << " abandoned snapshot folder(s)\n";

    std::string error;
    auto headless = HeadlessProject::open(*project, error);
    if (!headless) {
        std::cerr << "adi-mcp: " << error << '\n';
        return 1;
    }
    if (const auto cap = headless->agentTierOverride()) tier = std::min(tier, *cap);

    GrantTable grants;
    const std::string credential = processSecret();
    grants.grant(credential, tier);
    // What the service has to say (a call that failed inside, say) goes to
    // stderr, where the user's MCP host shows it; stdout carries only frames.
    ServiceOptions serviceOptions;
    serviceOptions.log = [](std::string_view line) { std::cerr << "adi-mcp: " << line << '\n'; };
    Service service(inlineExecutor(), grants, std::move(serviceOptions));
    service.audit().registerSecret(credential);

    ProjectSet projects;
    projects.add(headless);
    projects.allowImplicitSingle(true);   // one file for the bridge's whole life
    ChangesetQueue queue;
    ControlContext context{projects, queue, limits, {}, {}, "adi", adi::kVersion, "adi-mcp"};
    context.nowUtcMicros = [] {
        using namespace std::chrono;
        return duration_cast<microseconds>(system_clock::now().time_since_epoch()).count();
    };
    context.appTier = [tier] { return tier; };
    context.maxOpsPerMinute = [maxOpsPerMinute] { return maxOpsPerMinute; };
    registerControlMethods(service, context);

    McpEndpoint endpoint(service, projects, McpServerInfo{"adi-mcp", "ADI", adi::kVersion});
    McpEndpoint::Session session;
    session.credential = credential;
    // What reaches the client, said once where the user's MCP host shows it:
    // the client may be a cloud model, and project content goes to it.
    std::cerr << "adi-mcp " << adi::kVersion << ": " << headless->label() << " ("
              << headless->handle() << "), tier " << toString(tier) << ", read-only\n"
              << "adi-mcp: the client reads this project's structure -- names, routing, devices, "
                 "parameters, clips, notes and remarks -- never audio or media files; nothing is "
                 "written unless you apply a changeset yourself\n";
    runStdio(std::cin, std::cout, endpoint, session, WireLimits{}, &std::cerr);
    return 0;
}

}  // namespace

#if defined(_WIN32)
int wmain(int argc, wchar_t** argv) {
    std::vector<fs::path> args;
    for (int i = 1; i < argc; ++i) args.emplace_back(argv[i]);
    try {
        return run(args);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "adi-mcp: %s\n", e.what());
        return 1;
    }
}
#else
int main(int argc, char** argv) {
    std::vector<fs::path> args;
    for (int i = 1; i < argc; ++i) args.emplace_back(argv[i]);
    try {
        return run(args);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "adi-mcp: %s\n", e.what());
        return 1;
    }
}
#endif
