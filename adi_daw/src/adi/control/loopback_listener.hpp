// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace adi::control {

enum class LoopbackRoute { Rpc, Mcp };

struct LoopbackOptions {
    std::uint16_t port = 0;
    std::string token;
    std::vector<std::string> allowedOrigins;
};

/// Owns the loopback WebSocket listener and every connection thread. Start
/// and stop are called by its owner, never by a handler or the audio thread.
class LoopbackListener {
public:
    using SessionId = std::uint64_t;
    using Reply = std::function<void(std::optional<std::string>)>;
    /// Abandon a pending call. Returns true if the owner has not taken it.
    using Cancel = std::function<bool()>;
    /// Runs on the connection's reader thread; always returns promptly. The
    /// callback can reply on any thread. nullopt completes a notification.
    using FrameHandler = std::function<Cancel(LoopbackRoute, SessionId, std::string_view, Reply)>;
    using SessionHandler = std::function<void(LoopbackRoute, SessionId, bool connected)>;

    LoopbackListener(LoopbackOptions, FrameHandler, SessionHandler = {});
    ~LoopbackListener();
    LoopbackListener(const LoopbackListener&) = delete;
    LoopbackListener& operator=(const LoopbackListener&) = delete;

    [[nodiscard]] bool start(std::string& error);
    void stop() noexcept;
    [[nodiscard]] std::uint16_t port() const noexcept;

    /// Events have an independent, bounded queue. False means the event
    /// could not be queued. This function never waits for the network.
    [[nodiscard]] bool sendEvent(SessionId, std::string frame);

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace adi::control
