// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include "adi/control/methods.hpp"
#include "adi/control/session_discovery.hpp"

#include <memory>
#include <optional>
#include <vector>

namespace adi::ui {
class ProjectDocument;
}

namespace adi::control {

struct AppControlOptions {
    std::uint16_t port = 0;
    std::vector<std::string> allowedOrigins;
    bool writeDiscoveryFile = false;
    Tier tier = Tier::Propose;
    agent::Limits limits;
    std::size_t maxOpsPerMinute = 6000;
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
    [[nodiscard]] std::vector<ConsentRequest> takeConsentQuestions();
    [[nodiscard]] bool answerConsent(const ConsentRequest&, ConsentTerm);
    [[nodiscard]] std::vector<QueuedChangeset> proposals() const;
    [[nodiscard]] agent::ApplyResult review(std::string_view id, bool approve);
    [[nodiscard]] JobBook& jobs();

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace adi::control
