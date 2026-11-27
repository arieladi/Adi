// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include "adi/control/agent_process.hpp"
#include "adi/control/consent.hpp"
#include "adi/control/jobs.hpp"
#include "adi/control/service.hpp"

#include <condition_variable>
#include <deque>
#include <mutex>
#include <unordered_map>

namespace adi::control {

class AgentLog;

enum class AgentProviderKind { Local, Lan, Cloud, Faux };
struct AgentProvider {
    AgentProviderKind kind = AgentProviderKind::Local;
    std::string model;
    std::string endpoint;
    std::string vendor;
    std::string credentialAccount;
    friend bool operator==(const AgentProvider&, const AgentProvider&) = default;
};

struct AgentLaunch {
    AgentCommand command;
    AgentProvider provider;
};

enum class AgentReadiness { Unconfigured, ConsentRequired, Unavailable, Ready };
struct AgentCheck {
    AgentReadiness state = AgentReadiness::Unconfigured;
    std::string detail;
    [[nodiscard]] bool canAnswer() const { return state == AgentReadiness::Ready; }
};

struct AgentRunResult {
    std::string job;
    std::string answer;
    std::string reason;
    int steps = 0;
    int tokens = 0;
};

using AgentTool = std::function<Outcome(std::string_view method, const Json& args, const RequestMeta&,
                                        std::stop_token)>;

/// One instance per application launch. Probe and run are called on ordinary
/// workers. The injected tool dispatcher is responsible for the document hop.
class AgentRuntime {
public:
    AgentRuntime(ConsentLedger&, JobBook&, std::function<void(ConsentRequest)> ask = {}, AgentLog* = nullptr);

    [[nodiscard]] AgentCheck check(const AgentLaunch&, std::string projectUuid);
    [[nodiscard]] AgentCheck readiness(std::string_view projectUuid) const;
    [[nodiscard]] AgentRunResult run(const AgentLaunch&, std::string owner, std::string projectUuid,
                                     std::string projectHandle, std::string request, RunOrigin,
                                     const AgentTool&);

private:
    struct Checked {
        AgentLaunch launch;
        AgentCheck result;
    };
    void releaseLane(const std::string& project);

    ConsentLedger& ledger_;
    JobBook& jobs_;
    std::function<void(ConsentRequest)> ask_;
    AgentLog* log_;
    mutable std::mutex mutex_;
    std::condition_variable laneReady_;
    std::unordered_map<std::string, Checked> checked_;
    std::unordered_map<std::string, std::deque<std::string>> lanes_;
};

}  // namespace adi::control
