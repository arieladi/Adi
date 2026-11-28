// SPDX-License-Identifier: AGPL-3.0-or-later

#include "adi/control/jsonrpc.hpp"
#include "adi/control/loopback_listener.hpp"
#include "adi/control/session_discovery.hpp"
#include "temp_directory.hpp"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <sstream>
#include <string>

namespace {

using namespace adi::control;
namespace fs = std::filesystem;

int failures = 0;

void check(bool good, const char* description) {
    if (!good) {
        ++failures;
        std::printf("FAIL: %s\n", description);
    }
}

std::string quote(const fs::path& path) {
#if defined(_WIN32)
    return "\"" + path.string() + "\"";
#else
    std::string escaped = "'";
    for (const char ch : path.string()) escaped += ch == '\'' ? "'\\''" : std::string(1, ch);
    return escaped + "'";
#endif
}

bool setHome(const fs::path& path) {
#if defined(_WIN32)
    return _wputenv_s(L"ADI_HOME", path.wstring().c_str()) == 0;
#else
    return setenv("ADI_HOME", path.c_str(), 1) == 0;
#endif
}

std::string readFile(const fs::path& path) {
    std::ifstream input(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

void connectsToLiveMcp() {
    adi::test::TempDirectory temp("loopback_bridge", "connect");
    check(setHome(temp.path() / "profile"), "the child uses a private test profile");

    const auto token = makeSessionToken();
    std::atomic<int> notifications{0};
    const auto handle = [&](LoopbackRoute route, LoopbackListener::SessionId, std::string_view frame,
                            LoopbackListener::Reply reply) -> LoopbackListener::Cancel {
            const auto request = parseMessage(frame);
            if (route != LoopbackRoute::Mcp || request.kind == Incoming::Kind::Invalid) {
                reply(encodeError(request.id, {errc::InvalidRequest, "expected a complete MCP frame"}));
            } else if (request.kind == Incoming::Kind::Notification) {
                ++notifications;
                reply(std::nullopt);
            } else {
                reply(encodeResult(request.id, {{"methodSeen", request.method}, {"from", "live test listener"}}));
            }
            return {};
        };
    LoopbackListener server({0, token, {}}, handle);

    std::string why;
    const auto listening = server.start(why);
    check(listening, "a real loopback listener starts");
    if (!listening) return;
    SessionDiscoveryFile discovery;
    const auto published = discovery.publish({server.port(), token});
    check(published, "the listener publishes a private discovery record");
    if (!published) return;

    const auto incoming = temp.path() / "input.jsonl";
    const auto outgoing = temp.path() / "output.jsonl";
    const auto errors = temp.path() / "errors.txt";
    {
        std::ofstream input(incoming, std::ios::binary);
        input << R"({"jsonrpc":"2.0","id":"first","method":"initialize","params":{}})" << "\r\n";
        input << R"({"jsonrpc":"2.0","method":"notifications/initialized"})" << '\n';
        input << R"({"jsonrpc":"2.0","id":2,"method":"tools/list"})" << '\n';
        check(input.good(), "the child has a complete stdio request stream");
    }
    const auto bridge = [&] {
        auto command = quote(fs::path(ADI_MCP_PATH)) + " --connect < " + quote(incoming) +
                       " > " + quote(outgoing) + " 2> " + quote(errors);
#if defined(_WIN32)
        command = "\"" + command + "\"";
#endif
        return std::system(command.c_str());
    };
    check(bridge() == 0, "adi-mcp --connect talks to the discovered listener");
    const auto stdoutText = readFile(outgoing);
    const auto stderrText = readFile(errors);
    check(!token.empty() && stdoutText.find(token) == std::string::npos && stderrText.find(token) == std::string::npos,
          "the session credential appears on neither process stream");
    std::istringstream replies(stdoutText);
    std::string line;
    int responses = 0;
    bool sawInitialize = false, sawTools = false;
    for (; std::getline(replies, line); ++responses) {
        const auto response = Json::parse(line, nullptr, false);
        check(response.is_object() && response.value("jsonrpc", "") == "2.0" &&
                  response.contains("result") && response.at("result").value("from", "") == "live test listener",
              "stdout contains only JSON-RPC results from the listener");
        if (!response.is_object() || !response.contains("result")) continue;
        const auto& method = response.at("result").at("methodSeen");
        sawInitialize |= response.at("id") == "first" && method == "initialize";
        sawTools |= response.at("id") == 2 && method == "tools/list";
    }
    check(sawInitialize && sawTools, "both request ids and methods survive the stdio and WebSocket round trip");
    check(responses == 2 && notifications.load() == 1,
          "the proxy drains both replies after stdin closes and forwards the notification without a reply");

    discovery.reset();
    server.stop();
    check(bridge() != 0 && readFile(outgoing).empty() && readFile(errors).find("discovery") != std::string::npos,
          "a second bridge reports how to connect when there is no live discovery record");
    const auto rotated = makeSessionToken();
    LoopbackListener restarted({0, rotated, {}}, handle);
    const auto rebound = rotated != token && restarted.start(why) && discovery.publish({restarted.port(), rotated});
    check(rebound, "a restarted listener publishes its rotated token");
    if (rebound)
        check(bridge() == 0 && readFile(outgoing).find("live test listener") != std::string::npos,
              "a fresh bridge discovers and connects after restart and token rotation");
}

}  // namespace

int main() {
    connectsToLiveMcp();
    return failures == 0 ? 0 : 1;
}
