// SPDX-License-Identifier: AGPL-3.0-or-later
//
// adi-mcp --project: a project file with no ADI running (CTL-08's headless
// backend; docs/CONTROL-MCP.md §2).
//
// It holds NO writable handle to the user's file. Reads go through a
// read-only connection, and previews through a private copy: the SQLite backup
// API takes a consistent snapshot of the committed file into a scratch folder,
// which is opened writable and thrown away. So the file is never written,
// never upgraded, never locked for writing against a running ADI, and a
// preview of an older minor works without touching the original. Applying is
// the user's act: Apply in the app, or `adi_tool propose ... --apply`.
//
// The snapshot is taken again whenever the file's head has moved, so a
// preview always runs on the project as it is.

#pragma once

#include "adi/control/project_access.hpp"

#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <string>

namespace adi {
class Store;
}

namespace adi::control {

class HeadlessProject final : public ProjectAccess {
public:
    /// Opens `file` read-only. Null, with `error`, when it cannot be read.
    [[nodiscard]] static std::shared_ptr<HeadlessProject> open(const std::filesystem::path& file,
                                                               std::string& error);
    ~HeadlessProject() override;
    HeadlessProject(const HeadlessProject&) = delete;
    HeadlessProject& operator=(const HeadlessProject&) = delete;

    [[nodiscard]] const std::string& handle() const override { return handle_; }
    [[nodiscard]] std::string label() const override;
    [[nodiscard]] rows::Model readModel(std::string& error) override;
    [[nodiscard]] std::optional<std::int64_t> head(std::string& error) override;
    [[nodiscard]] std::string fileSchema() override;
    [[nodiscard]] agent::Preview preview(const agent::Changeset&, const agent::Limits&) override;
    [[nodiscard]] bool wasApplied(const std::string& actorDetail) override;
    [[nodiscard]] bool live() const override { return false; }
    [[nodiscard]] std::shared_ptr<const TelemetryFrame> telemetry() const override { return nullptr; }
    [[nodiscard]] std::string applyHint() const override;

    /// Where the snapshot lives: a private folder, never beside the file.
    [[nodiscard]] const std::filesystem::path& scratchFolder() const { return scratch_; }

private:
    HeadlessProject();
    bool refreshSnapshotLocked(std::string& error);
    void dropSnapshotLocked();

    std::filesystem::path file_;
    std::filesystem::path scratch_;
    std::unique_ptr<Store> reader_;
    std::unique_ptr<Store> snapshot_;
    std::optional<std::int64_t> snapshotHead_;
    std::string handle_;
    std::string label_;
    mutable std::mutex mutex_;
};

}  // namespace adi::control
