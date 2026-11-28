// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "adi/control/project_access.hpp"
#include "adi/control/telemetry.hpp"

#include <atomic>
#include <filesystem>

namespace adi::ui {
class ProjectDocument;
}

namespace adi::control {

class HeadlessProject;

/// CTL-09's document adapter. Reads use short independent connections, and
/// previews and commits use the document's Store and view on its owner thread.
class LiveProject final : public ProjectAccess {
public:
    [[nodiscard]] static std::shared_ptr<LiveProject> open(ui::ProjectDocument&, std::string& error);

    /// Owner only. A Save As changes the Store's path without changing the
    /// document, so the host binds a fresh adapter.
    [[nodiscard]] bool matches(ui::ProjectDocument&) const;
    /// Owner only, before the document is replaced or destroyed.
    void detach();
    /// Owner only, at the view's tick. Network callers read the last frame.
    void publishTelemetry(std::int64_t nowMs);

    [[nodiscard]] const std::string& handle() const override;
    [[nodiscard]] std::string projectUuid() const override;
    [[nodiscard]] std::string label() const override;
    [[nodiscard]] ReadSnapshot read(std::string& error) override;
    [[nodiscard]] std::optional<std::int64_t> head(std::string& error) override;
    [[nodiscard]] std::string fileSchema() override;
    [[nodiscard]] agent::Preview preview(const agent::Changeset&, const agent::Limits&) override;
    [[nodiscard]] std::optional<Tier> agentTierOverride() override;
    [[nodiscard]] bool allowsAutomaticApply() const override;
    [[nodiscard]] agent::ApplyResult applyChangeset(const agent::Changeset&, const agent::Limits&) override;
    [[nodiscard]] bool wasApplied(const std::string& actorDetail) override;
    [[nodiscard]] bool live() const override;
    [[nodiscard]] std::shared_ptr<const TelemetryFrame> telemetry() const override;
    [[nodiscard]] std::string applyHint() const override;

private:
    LiveProject(ui::ProjectDocument&, std::shared_ptr<HeadlessProject>);
    [[nodiscard]] ui::ProjectDocument* ownerDocument() const;

    const std::filesystem::path path_;
    const std::shared_ptr<HeadlessProject> reader_;
    const bool writable_;
    ui::ProjectDocument* document_;
    std::atomic<bool> attached_{true};
    TelemetryBoard board_;
    std::uint64_t sequence_ = 0;
};

}  // namespace adi::control
