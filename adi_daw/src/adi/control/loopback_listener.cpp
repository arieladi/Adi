// SPDX-License-Identifier: AGPL-3.0-or-later

#include "adi/control/loopback_listener.hpp"

#include "adi/control/jsonrpc.hpp"
#include "adi/control/service.hpp"

#ifndef CPPHTTPLIB_WEBSOCKET_MAX_PAYLOAD_LENGTH
#define CPPHTTPLIB_WEBSOCKET_MAX_PAYLOAD_LENGTH 245760
#endif
#ifndef CPPHTTPLIB_WEBSOCKET_CLOSE_TIMEOUT_SECOND
#define CPPHTTPLIB_WEBSOCKET_CLOSE_TIMEOUT_SECOND 1
#endif
#include <httplib.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace adi::control {
namespace {

using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;
using Close = httplib::ws::CloseStatus;

constexpr std::size_t kConnections = 8;
constexpr std::size_t kInFlight = 8;
constexpr std::size_t kFrameBytes = 240 * 1024;
constexpr std::size_t kReplies = 8;
constexpr std::size_t kEvents = 8;
constexpr auto kDeadline = 10s;
constexpr double kRequestsPerSecond = 50;

static_assert(CPPHTTPLIB_WEBSOCKET_MAX_PAYLOAD_LENGTH == kFrameBytes);

thread_local socket_t readerSocket = INVALID_SOCKET;

// The pinned server releases only its listener in stop(). Its connection
// entry point is virtual and process_request is protected, so we can use
// the same HTTP and WebSocket handling with an interruptible socket lifetime.
class InterruptibleServer final : public httplib::Server {
public:
    void resetInterrupt() noexcept { interrupted_.store(false); }
    [[nodiscard]] bool interrupted() const noexcept { return interrupted_.load(); }

    void interrupt() noexcept {
        std::lock_guard lock(mutex_);
        interrupted_ = true;
        for (const auto socket : sockets_) httplib::detail::shutdown_socket(socket);
    }

private:
    bool process_and_close_socket(socket_t socket) override {
        const auto close = httplib::detail::scope_exit([&] { httplib::detail::drain_and_close_socket(socket); });
        return serve_guarded([&] {
            {
                std::lock_guard lock(mutex_);
                if (interrupted_) return false;
                sockets_.insert(socket);
            }
            const auto remove = httplib::detail::scope_exit([&] {
                readerSocket = INVALID_SOCKET;
                std::lock_guard lock(mutex_);
                sockets_.erase(socket);
            });
            readerSocket = socket;

            std::string remoteAddress, localAddress;
            int remotePort = 0, localPort = 0;
            httplib::detail::get_remote_ip_and_port(socket, remoteAddress, remotePort);
            httplib::detail::get_local_ip_and_port(socket, localAddress, localPort);
            return httplib::detail::process_server_socket(
                svr_sock_, socket, keep_alive_max_count_, keep_alive_timeout_sec_,
                read_timeout_sec_, read_timeout_usec_, write_timeout_sec_, write_timeout_usec_,
                [&](httplib::Stream& stream, bool closeConnection, bool& connectionClosed) {
                    return process_request(stream, remoteAddress, remotePort, localAddress, localPort,
                                           closeConnection, connectionClosed, nullptr);
                });
        });
    }

    std::mutex mutex_;
    std::unordered_set<socket_t> sockets_;
    std::atomic<bool> interrupted_{false};
};

struct Peer {
    struct Pending {
        Clock::time_point deadline;
        std::string request;
        LoopbackListener::Cancel cancel;
    };

    explicit Peer(socket_t nativeSocket) : socket(nativeSocket) {}

    bool acceptRead() {
        std::lock_guard lock(mutex);
        return !closing;
    }

    std::uint64_t begin(std::string_view message) {
        std::lock_guard lock(mutex);
        if (closing || pending.size() >= kInFlight) return 0;
        const auto now = Clock::now();
        allowance = std::min(kRequestsPerSecond, allowance +
            std::chrono::duration<double>(now - lastRefill).count() * kRequestsPerSecond);
        lastRefill = now;
        if (allowance < 1) return 0;
        allowance -= 1;
        const auto ticket = nextTicket++;
        pending.emplace(ticket, Pending{now + kDeadline, std::string(message), {}});
        return ticket;
    }

    void setCancellation(std::uint64_t ticket, LoopbackListener::Cancel cancel) {
        std::lock_guard lock(mutex);
        if (const auto entry = pending.find(ticket); entry != pending.end()) {
            entry->second.cancel = std::move(cancel);
            changed.notify_one();
        }
    }

    void reply(std::uint64_t ticket, std::optional<std::string> frame) {
        std::lock_guard lock(mutex);
        const auto entry = pending.find(ticket);
        if (entry == pending.end() || closing) return;
        if (frame && frame->size() > kFrameBytes) {
            const auto request = parseMessage(entry->second.request);
            if (request.kind == Incoming::Kind::Request)
                frame = encodeError(request.id, {errc::OverLimit, "the response exceeds the frame limit"});
            else
                frame.reset();
        }
        pending.erase(entry);
        if (frame) {
            if (replies.size() >= kReplies) {
                endLocked(Close::PolicyViolation);
                return;
            }
            replies.push_back(std::move(*frame));
        }
        changed.notify_one();
    }

    bool event(std::string frame) {
        std::lock_guard lock(mutex);
        if (closing || events.size() >= kEvents || frame.size() > kFrameBytes) return false;
        events.push_back(std::move(frame));
        changed.notify_one();
        return true;
    }

    void end(std::optional<Close> status = {}) {
        std::lock_guard lock(mutex);
        endLocked(status);
    }

    void cancelRemaining() {
        decltype(pending) dropped;
        {
            std::lock_guard lock(mutex);
            dropped.swap(pending);
        }
        for (auto& [ticket, call] : dropped) {
            (void)ticket;
            if (call.cancel) {
                try { (void)call.cancel(); } catch (...) {}
            }
        }
    }

    void write(httplib::ws::WebSocket& socketStream) {
        try {
            std::unique_lock lock(mutex);
            while (!closing) {
                auto wake = Clock::time_point::max();
                auto overdue = pending.end();
                for (auto entry = pending.begin(); entry != pending.end(); ++entry) {
                    if (!entry->second.cancel) continue;
                    if (entry->second.deadline <= Clock::now()) {
                        overdue = entry;
                        break;
                    }
                    wake = std::min(wake, entry->second.deadline);
                }
                if (overdue != pending.end()) {
                    const auto ticket = overdue->first;
                    const auto request = parseMessage(overdue->second.request);
                    auto cancel = std::move(overdue->second.cancel);
                    lock.unlock();
                    if (cancel()) {
                        if (request.kind == Incoming::Kind::Request)
                            reply(ticket, encodeError(request.id, {errc::DeadlinePassed,
                                "the deadline passed before the request ran; nothing was applied"}));
                        else
                            reply(ticket, std::nullopt);
                    }
                    lock.lock();
                    continue;
                }
                if (replies.empty() && events.empty()) {
                    if (wake == Clock::time_point::max()) changed.wait(lock);
                    else changed.wait_until(lock, wake);
                    continue;
                }
                // Give terminal replies priority. An event flood consumes no
                // reply reservations and never blocks the caller's thread.
                auto& queue = replies.empty() ? events : replies;
                auto frame = std::move(queue.front());
                queue.pop_front();
                lock.unlock();
                if (!socketStream.send(frame)) {
                    end();
                    break;
                }
                lock.lock();
            }
            if (lock.owns_lock()) {
                const auto status = closeStatus;
                lock.unlock();
                if (status) socketStream.close(*status);
            }
        } catch (...) {
            end();
        }
        // Wake a concurrent reader even if it is halfway through a frame.
        httplib::detail::shutdown_socket(socket);
    }

private:
    void endLocked(std::optional<Close> status) {
        if (closing) return;
        closing = true;
        closeStatus = status;
        changed.notify_one();
    }

    const socket_t socket;
    std::mutex mutex;
    std::condition_variable changed;
    bool closing = false;
    std::optional<Close> closeStatus;
    std::unordered_map<std::uint64_t, Pending> pending;
    std::deque<std::string> replies, events;
    std::uint64_t nextTicket = 1;
    Clock::time_point lastRefill = Clock::now();
    double allowance = kRequestsPerSecond;
};

}  // namespace

class LoopbackListener::Impl {
public:
    Impl(LoopbackOptions options, FrameHandler frame, SessionHandler session)
        : options_(std::move(options)), onFrame_(std::move(frame)), onSession_(std::move(session)) {
        server_.new_task_queue = [] {
            // A bounded handshake allowance lets the ninth client get an
            // HTTP refusal while all eight WebSocket handlers are active.
            return new httplib::ThreadPool(kConnections + 2, kConnections + 2, kConnections);
        };
        server_.set_keep_alive_max_count(1);
        server_.set_read_timeout(250ms);
        server_.set_write_timeout(1s);
        server_.set_idle_interval(100ms);
        server_.set_websocket_ping_interval(15s);
        server_.set_websocket_max_missed_pongs(2);
        server_.set_pre_routing_handler([this](const httplib::Request& request, httplib::Response& response) {
            return preflight(request, response);
        });
        server_.WebSocket("/rpc", [this](const httplib::Request&, httplib::ws::WebSocket& ws) {
            serve(LoopbackRoute::Rpc, ws);
        });
        server_.WebSocket("/mcp", [this](const httplib::Request&, httplib::ws::WebSocket& ws) {
            serve(LoopbackRoute::Mcp, ws);
        });
    }

    bool start(std::string& error) {
        error.clear();
        if (thread_.joinable()) {
            error = "the control listener has already been started";
            return false;
        }
        if (options_.token.empty() || !onFrame_) {
            error = "the control listener needs a session token and dispatcher";
            return false;
        }
        try {
            server_.resetInterrupt();
            const auto selected = options_.port == 0
                ? server_.bind_to_any_port("127.0.0.1")
                : (server_.bind_to_port("127.0.0.1", options_.port) ? static_cast<int>(options_.port) : -1);
            if (selected <= 0) {
                error = "could not bind the control listener to 127.0.0.1";
                return false;
            }
            thread_ = std::thread([this] {
                try { (void)server_.listen_after_bind(); } catch (...) { server_.decommission(); }
            });
            server_.wait_until_ready();
            if (!server_.is_running()) {
                stop();
                error = "could not start the control listener";
                return false;
            }
            port_ = static_cast<std::uint16_t>(selected);
            return true;
        } catch (...) {
            stop();
            error = "could not start the control listener";
            return false;
        }
    }

    void stop() noexcept {
        port_ = 0;
        server_.interrupt();
        server_.stop();
        if (thread_.joinable()) thread_.join();
    }

    std::uint16_t port() const noexcept { return port_.load(); }

    bool sendEvent(SessionId id, std::string frame) {
        std::shared_ptr<Peer> peer;
        {
            std::lock_guard lock(sessionsMutex_);
            const auto entry = sessions_.find(id);
            if (entry == sessions_.end()) return false;
            peer = entry->second;
        }
        return peer->event(std::move(frame));
    }

private:
    httplib::Server::HandlerResponse preflight(const httplib::Request& request, httplib::Response& response) {
        using Response = httplib::Server::HandlerResponse;
        const auto refuse = [&](int status) {
            response.status = status;
            return Response::Handled;
        };
        if (request.path != "/rpc" && request.path != "/mcp") return refuse(404);
        if (request.method != "GET") return refuse(405);
        if (!httplib::detail::is_websocket_upgrade(request) || httplib::detail::has_framed_body(request))
            return refuse(400);

        const auto originCount = request.get_header_value_count("Origin");
        const auto bearerCount = request.get_header_value_count("Authorization");
        const auto queryCount = request.get_param_value_count("token");
        std::string candidate;
        if (originCount != 0) {
            const auto origin = request.get_header_value("Origin");
            if (originCount != 1 || origin.empty() ||
                std::find(options_.allowedOrigins.begin(), options_.allowedOrigins.end(), origin) ==
                    options_.allowedOrigins.end())
                return refuse(403);
            if (queryCount == 1 && bearerCount == 0) candidate = request.get_param_value("token");
        } else if (queryCount == 0 && bearerCount == 1) {
            const auto header = request.get_header_value("Authorization");
            constexpr std::string_view bearer = "Bearer ";
            if (header.starts_with(bearer)) candidate = header.substr(bearer.size());
        }
        if (!secureEquals(candidate, options_.token)) {
            response.set_header("WWW-Authenticate", "Bearer");
            return refuse(401);
        }
        {
            std::lock_guard lock(sessionsMutex_);
            if (sessions_.size() >= kConnections) return refuse(503);
        }
        return Response::Unhandled;
    }

    void serve(LoopbackRoute route, httplib::ws::WebSocket& ws) {
        auto peer = std::make_shared<Peer>(readerSocket);
        SessionId session = 0;
        {
            std::lock_guard lock(sessionsMutex_);
            if (sessions_.size() < kConnections && !server_.interrupted()) {
                session = nextSession_++;
                sessions_.emplace(session, peer);
            }
        }
        if (session == 0) {
            ws.close(Close::PolicyViolation, "connection limit");
            return;
        }

        std::thread writer;
        try {
            ws.set_read_timeout(250ms);
            if (onSession_) onSession_(route, session, true);
            writer = std::thread([&] { peer->write(ws); });
            std::string frame;
            while (peer->acceptRead() && !server_.interrupted()) {
                const auto read = ws.read(frame);
                if (read == httplib::ws::Timeout) continue;
                if (read == httplib::ws::Fail) break;
                if (read != httplib::ws::Text) {
                    peer->end(Close::UnsupportedData);
                    break;
                }
                const auto ticket = peer->begin(frame);
                if (ticket == 0) {
                    peer->end(Close::PolicyViolation);
                    break;
                }
                auto cancel = onFrame_(route, session, frame, [weak = std::weak_ptr<Peer>(peer), ticket](auto reply) {
                    if (auto active = weak.lock()) active->reply(ticket, std::move(reply));
                });
                peer->setCancellation(ticket, std::move(cancel));
            }
        } catch (...) {
            peer->end(Close::InternalError);
        }
        peer->end();
        if (writer.joinable()) writer.join();
        peer->cancelRemaining();
        {
            std::lock_guard lock(sessionsMutex_);
            sessions_.erase(session);
        }
        if (onSession_) {
            try { onSession_(route, session, false); } catch (...) {}
        }
    }

    LoopbackOptions options_;
    FrameHandler onFrame_;
    SessionHandler onSession_;
    InterruptibleServer server_;
    std::thread thread_;
    std::atomic<std::uint16_t> port_{0};
    std::mutex sessionsMutex_;
    std::unordered_map<SessionId, std::shared_ptr<Peer>> sessions_;
    SessionId nextSession_ = 1;
};

LoopbackListener::LoopbackListener(LoopbackOptions options, FrameHandler frame, SessionHandler session)
    : impl_(std::make_unique<Impl>(std::move(options), std::move(frame), std::move(session))) {}

LoopbackListener::~LoopbackListener() { stop(); }

bool LoopbackListener::start(std::string& error) { return impl_->start(error); }
void LoopbackListener::stop() noexcept { impl_->stop(); }
std::uint16_t LoopbackListener::port() const noexcept { return impl_->port(); }
bool LoopbackListener::sendEvent(SessionId session, std::string frame) {
    return impl_->sendEvent(session, std::move(frame));
}

}  // namespace adi::control
