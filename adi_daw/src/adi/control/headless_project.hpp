// SPDX-License-Identifier: AGPL-3.0-or-later
//
// adi-mcp --project: a project file with no ADI running (CTL-08's headless
// backend; docs/CONTROL-MCP.md §2).
//
// It holds NO writable handle to the user's file, and no handle at all
// between requests:
//   * every read opens a read-only connection, reads the rows and the head in
//     one read transaction, and closes it again -- so an idle bridge holds no
//     lock that could stop ADI leaving WAL mode at close (SPEC §3.3), and a
//     file upgraded in place meanwhile is read as it now is;
//   * a preview runs on a private copy: the SQLite backup API takes a
//     consistent snapshot of the committed file into a scratch folder, which
//     is opened writable and thrown away (ADR-0233 d5; ADR-0148 d1's
//     rolled-back preview, run on the copy). The copy is taken again for
//     every preview, so a preview runs on the project as it is now, even
//     when something changed the file without moving its head.
//
// The scratch folder is named for this process; one whose process is gone (a
// crash, a kill) is swept the next time a bridge starts, so no copy of
// somebody's unreleased project accumulates in the temp folder.
//
// Applying is the user's act: `adi_tool propose ... --apply` with the
// changeset that changeset_propose (or changeset_status) returns.

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
    /// Checks that `file` opens read-only. Null, with `error`, when it does not.
    [[nodiscard]] static std::shared_ptr<HeadlessProject> open(const std::filesystem::path& file,
                                                               std::string& error);
    ~HeadlessProject() override;
    HeadlessProject(const HeadlessProject&) = delete;
    HeadlessProject& operator=(const HeadlessProject&) = delete;

    [[nodiscard]] const std::string& handle() const override { return handle_; }
    [[nodiscard]] std::string label() const override;
    [[nodiscard]] ReadSnapshot read(std::string& error) override;
    [[nodiscard]] std::optional<std::int64_t> head(std::string& error) override;
    [[nodiscard]] std::string fileSchema() override;
    [[nodiscard]] agent::Preview preview(const agent::Changeset&, const agent::Limits&) override;
    [[nodiscard]] bool wasApplied(const std::string& actorDetail) override;
    [[nodiscard]] bool live() const override { return false; }
    [[nodiscard]] std::shared_ptr<const TelemetryFrame> telemetry() const override { return nullptr; }
    [[nodiscard]] std::string applyHint() const override;

    /// Where the snapshot lives: a private folder, never beside the file.
    /// Empty until the first preview.
    [[nodiscard]] const std::filesystem::path& scratchFolder() const { return scratch_; }

    /// Removes `adi-mcp-<pid>-<16 lowercase hex>` folders in the temp folder
    /// whose process is gone: real folders only, never a link, and on POSIX
    /// only those this user owns. Returns how many it removed. Called once
    /// per bridge start.
    static std::size_t sweepAbandonedScratch();

private:
    HeadlessProject();
    [[nodiscard]] std::unique_ptr<Store> openReader(std::string& error) const;
    bool refreshSnapshotLocked(std::string& error);
    void dropSnapshotLocked();

    std::filesystem::path file_;
    std::filesystem::path scratch_;
    std::unique_ptr<Store> snapshot_;
    std::string handle_;
    std::string label_;
    mutable std::mutex mutex_;
};

}  // namespace adi::control
