// SPDX-License-Identifier: AGPL-3.0-or-later

#include "adi/control/loopback_bridge.hpp"

#include "adi/control/jsonrpc.hpp"
#include "adi/control/stdio_server.hpp"

#ifndef CPPHTTPLIB_WEBSOCKET_MAX_PAYLOAD_LENGTH
#define CPPHTTPLIB_WEBSOCKET_MAX_PAYLOAD_LENGTH 245760
#endif
#ifndef CPPHTTPLIB_WEBSOCKET_CLOSE_TIMEOUT_SECOND
#define CPPHTTPLIB_WEBSOCKET_CLOSE_TIMEOUT_SECOND 1
#endif
#include <httplib.h>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <deque>
#include <istream>
#include <memory>
#include <mutex>
#include <ostream>
#include <string>
#include <string_view>
#include <thread>

namespace adi::control {
namespace {

using namespace std::chrono_literals;
using Client = httplib::ws::WebSocketClient;
using Clock = std::chrono::steady_clock;

constexpr WireLimits limits{240 * 1024, 64};
constexpr std::size_t maxInFlight = 4;
constexpr auto replyWait = 12s;
static_assert(limits.maxMessageBytes == CPPHTTPLIB_WEBSOCKET_MAX_PAYLOAD_LENGTH);

std::optional<SessionDiscovery> manualDiscovery(std::uint16_t port) {
    // The environment value is an ASCII bearer token on every platform.
#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4996)
#endif
    const auto* value = std::getenv("ADI_CONTROL_TOKEN");
#if defined(_MSC_VER)
#pragma warning(pop)
#endif
    const std::string_view token = value ? std::string_view(value) : std::string_view{};
    if (port == 0 || token.size() != 64 ||
        token.find_first_not_of("0123456789abcdef") != std::string_view::npos) return std::nullopt;
    return SessionDiscovery{port, std::string(token)};
}

std::unique_ptr<Client> connect(const SessionDiscovery& session) {
    auto socket = std::make_unique<Client>("ws://127.0.0.1:" + std::to_string(session.port) + "/mcp");
    socket->set_bearer_token_auth(session.token);
    socket->set_connection_timeout(2s);
    socket->set_write_timeout(2s);
    if (!socket->connect()) return {};
    socket->set_read_timeout(100ms);
    return socket;
}

class Relay {
public:
    Relay(Client& socket, std::ostream& out, std::ostream& diagnostics)
        : socket_(socket), out_(out), diagnostics_(diagnostics) {}

    int run(std::istream& in) {
        std::jthread receiver([this](std::stop_token stop) {
            try {
                receive(stop);
            } catch (...) {
                fail("could not forward an ADI response");
            }
        });
        std::string line;
        bool tooLong = false;
        while (readBoundedLine(in, line, limits.maxMessageBytes, tooLong)) {
            if (tooLong) {
                std::lock_guard lock(mutex_);
                write(encodeError(nullptr, {errc::InvalidRequest, "message larger than the bridge frame limit"}));
                continue;
            }
            if (line.find_first_not_of(" \t") == std::string::npos) continue;
            const auto message = parseMessage(line, limits);
            if (message.kind == Incoming::Kind::Invalid) {
                std::lock_guard lock(mutex_);
                write(encodeError(message.id, message.error));
                continue;
            }
            if (!reserve(message)) break;
            if (!socket_.send(line)) {
                fail("connection to ADI closed");
                break;
            }
        }

        // A host can close stdin after sending its final request. Keep
        // forwarding replies until those requests settle or the peer fails.
        {
            std::unique_lock lock(mutex_);
            changed_.wait(lock, [&] { return pending_.empty() || failed_; });
        }
        receiver.request_stop();
        receiver.join();
        socket_.close();
        return failed_ ? 1 : 0;
    }

private:
    struct Pending {
        Json id;
        Clock::time_point deadline;
    };

    bool reserve(const Incoming& message) {
        std::unique_lock lock(mutex_);
        if (message.kind == Incoming::Kind::Request) {
            changed_.wait(lock, [&] { return pending_.size() < maxInFlight || failed_; });
            if (!failed_) pending_.push_back({message.id, Clock::now() + replyWait});
        }
        if (failed_ && message.kind == Incoming::Kind::Request)
            write(encodeError(message.id, closedError()));
        return !failed_;
    }

    void receive(std::stop_token stop) {
        std::string frame;
        while (!stop.stop_requested()) {
            bool due;
            {
                std::lock_guard lock(mutex_);
                if (failed_) return;
                due = !pending_.empty() && pending_.front().deadline <= Clock::now();
            }
            if (due) {
                fail("waiting for ADI's reply timed out");
                return;
            }
            const auto received = socket_.read(frame);
            if (stop.stop_requested()) return;
            if (received == httplib::ws::Timeout) continue;
            if (received == httplib::ws::Fail) {
                fail("connection to ADI closed");
                return;
            }
            const auto message = parseMessage(frame, limits);
            if (received != httplib::ws::Text || frame.find_first_of("\r\n") != std::string::npos ||
                message.kind == Incoming::Kind::Invalid) {
                fail("ADI sent an invalid MCP frame");
                return;
            }
            std::lock_guard lock(mutex_);
            if (failed_) return;
            write(frame);
            if (message.kind == Incoming::Kind::Response && !message.id.is_null()) {
                const auto pending = std::find_if(pending_.begin(), pending_.end(),
                    [&](const Pending& entry) { return entry.id == message.id; });
                if (pending != pending_.end()) pending_.erase(pending);
                changed_.notify_all();
            }
        }
    }

    static RpcError closedError() { return {errc::Internal, "bridge disconnected from ADI"}; }

    void fail(std::string_view reason) {
        std::lock_guard lock(mutex_);
        if (failed_) return;
        failed_ = true;
        for (const auto& pending : pending_) write(encodeError(pending.id, closedError()));
        pending_.clear();
        diagnostics_ << "adi-mcp: " << reason << "; restart the bridge to reconnect\n" << std::flush;
        changed_.notify_all();
    }

    void write(std::string_view frame) { out_ << frame << '\n' << std::flush; }

    Client& socket_;
    std::ostream& out_;
    std::ostream& diagnostics_;
    std::mutex mutex_;
    std::condition_variable changed_;
    std::deque<Pending> pending_;
    bool failed_ = false;
};

}  // namespace

int runLoopbackBridge(std::istream& in, std::ostream& out, std::ostream& diagnostics,
                      std::optional<std::uint16_t> explicitPort, const std::filesystem::path& discoveryPath) {
    auto session = explicitPort ? manualDiscovery(*explicitPort) : readSessionDiscovery(discoveryPath);
    if (!session) {
        diagnostics << (explicitPort
            ? "adi-mcp: set ADI_CONTROL_TOKEN to the token from the running ADI session\n"
            : "adi-mcp: enable the local control listener and discovery file in ADI before connecting\n");
        return 1;
    }
    auto socket = connect(*session);
    if (!socket && !explicitPort) {
        // The application may have rotated the token or restarted just as
        // discovery was read. Retry only before forwarding any message.
        const auto fresh = readSessionDiscovery(discoveryPath);
        if (fresh && (fresh->port != session->port || fresh->token != session->token)) socket = connect(*fresh);
    }
    if (!socket) {
        diagnostics << "adi-mcp: could not connect to the running ADI listener; check the session token and port\n";
        return 1;
    }
    return Relay(*socket, out, diagnostics).run(in);
}

}  // namespace adi::control
