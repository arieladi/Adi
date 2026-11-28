// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include <chrono>
#include <cstddef>
#include <filesystem>
#include <memory>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

namespace adi::control {

struct AgentCommand {
    std::filesystem::path executable;
    std::vector<std::string> arguments; // UTF-8, each argument passed literally
};

/// Owner-worker transport for adi-agent. The host selects an absolute
/// executable and its fixed arguments. Only JSONL crosses stdin/stdout.
/// All methods are sequential. Another thread cancels a wait by requesting
/// its stop token. An I/O failure, timeout or cancellation reaps the child.
class AgentProcess {
public:
    static constexpr std::size_t kMaxFrameBytes = 1024 * 1024;

    AgentProcess();
    ~AgentProcess();
    AgentProcess(const AgentProcess&) = delete;
    AgentProcess& operator=(const AgentProcess&) = delete;

    [[nodiscard]] bool start(const AgentCommand&, std::string& error);
    [[nodiscard]] bool send(std::string_view frame, std::chrono::milliseconds timeout,
                            std::string& error, std::stop_token stop = {});
    [[nodiscard]] std::optional<std::string> read(std::chrono::milliseconds timeout,
                                                  std::string& error, std::stop_token stop = {});
    void stop() noexcept;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace adi::control
