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

/// One proposal as the AI mode shows it: a card in the conversation, and the
/// full list for the floating window (ADR-0262 d2). Owner-thread data only.
struct ProposalCard {
    std::string id;
    std::string request;          ///< what the user asked
    std::string client;           ///< who proposed it: "ADI agent" or the client's own label
    bool fromAdiAgent = false;    ///< only these offer Ask again
    RunOrigin origin = RunOrigin::Direct;
    /// Why it waits: "tier" when the agent may only propose, else the
    /// always-confirm rules that held it ("marker", "bulk", "egress", "remark").
    std::vector<std::string> whyWaits;
    std::size_t changeCount = 0;
    /// Every change, before and after, as changeset.propose showed it (at most
    /// 200; `changesTruncated` past that). The card's count is `changeCount`.
    Json changes = Json::array();
    bool changesTruncated = false;
    std::string diff;             ///< the text projection's unified diff, as proposed
    /// ADR-0261 d7: how many edits came after, and whether Undo or Redo makes
    /// it current. Apply is offered only when `freshness.state` is Current.
    ProposalFreshness freshness;
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
    /// The open project's handle, as reviewCards and agentReadiness take it;
    /// empty while no project is attached.
    [[nodiscard]] std::string liveProject() const;
    [[nodiscard]] agent::ApplyResult review(std::string_view id, bool approve);
    /// The owner thread: the cards waiting in this open project, oldest first.
    /// Reads the history tree once per card; call it when the queue or the
    /// head changes, never from a paint.
    [[nodiscard]] std::vector<ProposalCard> reviewCards(std::string_view project) const;
    /// A worker, as runAgent: "Ask again" on a card whose song moved on. The
    /// old proposal is rejected by the user (source "ask again") and the same
    /// request, with its origin, runs again on a fresh read; never the same
    /// ops on a moved head (ADR-0148 d4). Refused, with nothing changed, as
    /// reason "not_waiting", "not_adi_agent", "project_unavailable" or
    /// "current" (Apply it instead).
    [[nodiscard]] AgentRunResult askAgain(const AgentLaunch&, std::string_view id);
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
