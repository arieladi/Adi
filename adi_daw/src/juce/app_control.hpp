// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include "adi/control/agent_log.hpp"
#include "adi/control/methods.hpp"
#include "adi/control/session_discovery.hpp"

#include <memory>
#include <optional>
#include <span>
#include <vector>

namespace adi::ui {
class ProjectDocument;
struct PluginScan;
}
namespace adi::preset { struct Entry; }

namespace adi::control {

struct AgentLaunch;
struct AgentCheck;
struct AgentRunResult;

struct AppControlOptions {
    std::uint16_t port = 0;
    std::vector<std::string> allowedOrigins;
    bool writeDiscoveryFile = false;
    Tier tier = Tier::Propose;
    agent::Limits limits;
    std::size_t maxOpsPerMinute = 6000;
};

struct AgentStopReport {
    std::size_t rejectedProposals = 0;
    std::size_t cancelledJobs = 0;
    std::int64_t atUtcMicros = 0;
    Actor actor = Actor::User;
    std::string surface;
};

/// ADI owns this on its message thread and supplies MessageManager::callAsync.
/// The seams for token display, consent and proposal review are owner-only.
class AppControl {
public:
    AppControl(Executor, AppControlOptions);
    ~AppControl();
    AppControl(const AppControl&) = delete;
    AppControl& operator=(const AppControl&) = delete;

    [[nodiscard]] bool start(std::string& error);
    void stop();
    void configure(AppControlOptions);
    [[nodiscard]] std::optional<SessionDiscovery> session() const;
    void attach(ui::ProjectDocument*);
    void tick(ui::ProjectDocument*);
    void setCataloguePlugins(const ui::PluginScan&);
    void setCataloguePresets(std::span<const preset::Entry>);
    [[nodiscard]] std::vector<ConsentRequest> takeConsentQuestions();
    [[nodiscard]] bool answerConsent(const ConsentRequest&, ConsentTerm);
    [[nodiscard]] std::vector<QueuedChangeset> proposals() const;
    [[nodiscard]] agent::ApplyResult review(std::string_view id, bool approve);
    [[nodiscard]] JobBook& jobs();
    [[nodiscard]] AgentStopReport stopAgent(std::string surface);
    void rearmAgent();
    [[nodiscard]] bool agentStopped() const;
    /// Trusted worker seams. The application owns the worker lifetime.
    [[nodiscard]] AgentCheck checkAgent(const AgentLaunch&, std::string_view project);
    [[nodiscard]] AgentCheck agentReadiness(std::string_view project) const;
    [[nodiscard]] AgentRunResult runAgent(const AgentLaunch&, std::string_view project,
                                         std::string request, RunOrigin = RunOrigin::Direct);
    /// Local history seam. The project argument is a handle for an open project.
    [[nodiscard]] std::optional<std::vector<AgentLogEntry>> agentHistory(
        std::string_view project, std::int64_t afterSequence = 0, std::size_t maximum = 256) const;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace adi::control
