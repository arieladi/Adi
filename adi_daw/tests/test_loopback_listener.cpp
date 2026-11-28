// SPDX-License-Identifier: AGPL-3.0-or-later

#include "adi/control/loopback_listener.hpp"

#ifndef CPPHTTPLIB_WEBSOCKET_MAX_PAYLOAD_LENGTH
#define CPPHTTPLIB_WEBSOCKET_MAX_PAYLOAD_LENGTH 245760
#endif
#ifndef CPPHTTPLIB_WEBSOCKET_CLOSE_TIMEOUT_SECOND
#define CPPHTTPLIB_WEBSOCKET_CLOSE_TIMEOUT_SECOND 1
#endif
#include <httplib.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <exception>
#include <memory>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace {

using namespace adi::control;
using namespace std::chrono_literals;
using Client = httplib::ws::WebSocketClient;
using SessionId = LoopbackListener::SessionId;

int checks = 0;
int failures = 0;
constexpr std::string_view token = "0123456789abcdeffedcba9876543210";

void check(bool result, const char* description) {
    ++checks;
    if (!result) {
        ++failures;
        std::printf("  FAIL  %s\n", description);
    }
}

std::string address(const LoopbackListener& server, std::string_view path) {
    return "ws://127.0.0.1:" + std::to_string(server.port()) + std::string(path);
}

std::unique_ptr<Client> nativeClient(const LoopbackListener& server, std::string_view path,
                                     std::string_view secret = token) {
    auto client = std::make_unique<Client>(address(server, path));
    client->set_bearer_token_auth(std::string(secret));
    client->set_connection_timeout(2s);
    client->set_read_timeout(2s);
    return client;
}

void testTransport() {
    std::puts("[authentication, framing, connection limit and shutdown]");
    std::atomic<int> connected{0};
    std::atomic<int> received{0};
    std::atomic<SessionId> mcpSession{0};
    LoopbackListener server({0, std::string(token), {"http://browser.test"}},
        [&](LoopbackRoute route, SessionId session, std::string_view message, LoopbackListener::Reply reply) {
            if (route == LoopbackRoute::Mcp) mcpSession = session;
            ++received;
            reply(std::string(route == LoopbackRoute::Mcp ? "mcp:" : "rpc:") + std::string(message));
            return LoopbackListener::Cancel{};
        }, [&](LoopbackRoute, SessionId, bool entered) {
            if (entered) ++connected;
            else --connected;
        });
    std::string error;
    const bool started = server.start(error);
    check(started && server.port() != 0, "an ephemeral port binds on loopback");
    if (!started) return;

    const auto unauthorized = nativeClient(server, "/rpc", "wrong")->connect();
    check(!unauthorized && unauthorized.status() == 401, "bad bearer fails before upgrade");
    {
        Client origin(address(server, "/rpc?token=" + std::string(token)),
                      httplib::Headers{{"Origin", "http://other.test"}});
        origin.set_read_timeout(2s);
        const auto refused = origin.connect();
        check(!refused && refused.status() == 403, "an unlisted browser origin fails before upgrade");
    }
    {
        Client browser(address(server, "/rpc?token=" + std::string(token)),
                       httplib::Headers{{"Origin", "http://browser.test"}});
        browser.set_read_timeout(2s);
        check(static_cast<bool>(browser.connect()), "the permitted browser can put its token in the query");
        std::string reply;
        check(browser.send("browser") && browser.read(reply) == httplib::ws::Text && reply == "rpc:browser",
              "the browser reaches the RPC route");
        browser.close();
    }
    {
        httplib::Client raw("127.0.0.1", server.port());
        raw.set_read_timeout(2s);
        const int before = received;
        const auto post = raw.Post("/mcp", "{}", "application/json");
        check(post && post->status == 405 && received == before, "plain HTTP POST never reaches dispatch");
        const auto malformed = raw.Get("/rpc", httplib::Headers{{"Upgrade", "websocket"},
                                                               {"Authorization", "Bearer " + std::string(token)}});
        check(malformed && malformed->status == 400 && received == before,
              "a malformed upgrade never reaches dispatch");
    }
    {
        auto client = nativeClient(server, "/mcp");
        check(static_cast<bool>(client->connect()), "MCP upgrades on the same listener and port");
        std::string reply;
        check(client->send("hello") && client->read(reply) == httplib::ws::Text && reply == "mcp:hello",
              "the MCP route reaches a distinct dispatcher");
        check(server.sendEvent(mcpSession, "notice") && client->read(reply) == httplib::ws::Text && reply == "notice",
              "the connection's writer delivers a queued event");
        const int before = received;
        const std::string oversized(240 * 1024 + 1, 'A');
        (void)client->send(oversized);
        check(client->read(reply) == httplib::ws::Fail && received == before,
              "an oversized frame is closed before it reaches dispatch");
    }

    const auto cleanedUp = std::chrono::steady_clock::now() + 2s;
    while (connected != 0 && std::chrono::steady_clock::now() < cleanedUp)
        std::this_thread::sleep_for(1ms);
    check(connected == 0, "closed peers release their slots");

    std::vector<std::unique_ptr<Client>> peers;
    for (int i = 0; i < 8; ++i) {
        auto client = nativeClient(server, "/rpc");
        check(static_cast<bool>(client->connect()), "each of the eight session slots remains usable");
        // A round-trip proves the server registered this connection before
        // the next preflight checks admission.
        std::string reply;
        check(client->send("slot") && client->read(reply) == httplib::ws::Text && reply == "rpc:slot",
              "the admitted session can dispatch");
        peers.push_back(std::move(client));
    }
    const auto full = nativeClient(server, "/rpc")->connect();
    check(!full && full.status() == 503, "the ninth connection is refused before the upgrade");
    const auto beforeStop = std::chrono::steady_clock::now();
    server.stop();
    check(server.port() == 0 && connected == 0 && std::chrono::steady_clock::now() - beforeStop < 3s,
          "stop interrupts idle readers and joins all active sessions promptly");
}

void testPendingLimit() {
    std::puts("[request limits cancel pending work]");
    std::atomic<int> pending{0};
    std::atomic<int> cancelled{0};
    LoopbackListener server({0, std::string(token), {}},
        [&](LoopbackRoute, SessionId, std::string_view, LoopbackListener::Reply) {
            ++pending;
            return [&] { ++cancelled; return true; };
        });
    std::string error;
    const auto started = server.start(error);
    check(started, "start a listener with deliberately pending work");
    if (!started) return;
    auto client = nativeClient(server, "/rpc");
    check(static_cast<bool>(client->connect()), "the client opens a session");
    for (int request = 0; request < 9; ++request)
        check(client->send("request"), "send another request without waiting");
    std::string response;
    check(client->read(response) == httplib::ws::Fail && pending == 8,
          "a ninth in-flight request closes the session before dispatch");
    server.stop();
    check(cancelled == 8, "disconnect cancels every pending request");
}

}  // namespace

int main() {
    try {
        testTransport();
        testPendingLimit();
    } catch (const std::exception& exception) {
        std::printf("  FAIL  exception: %s\n", exception.what());
        ++failures;
    }
    std::printf("%s -- %d checks, %d failure(s)\n", failures ? "FAIL" : "PASS", checks, failures);
    return failures != 0;
}
