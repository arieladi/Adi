// SPDX-License-Identifier: AGPL-3.0-or-later
//
// One open project, as the control core reaches it (docs/CONTROL-MCP.md §3, §4).
//
// Two implementations, one interface:
//   * HeadlessProject (adi-mcp --project): a read-only connection for reads,
//     and a private snapshot copy for previews. It holds NO writable handle to
//     the user's file (AI-AGENT §6 rule 1), so applying stays the user's act.
//   * the app's (CTL-09, the window's src/juce/app_control.*): reads on a
//     second read-only connection, previews on the writer, on the message
//     thread; telemetry from the running engine.
//
// Every method that touches a project names it (ADR-0232 d2), by `handle()`.

#pragma once

#include "adi/changeset.hpp"
#include "adi/control/jsonrpc.hpp"
#include "adi/store_rows.hpp"

#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace adi::control {

struct TelemetryFrame;

/// The rows and the undo head, read under ONE read transaction: a read never
/// pairs one state's rows with a later head, so a changeset built on what was
/// read and stamped with this head is refused as stale if anything moved.
struct ReadSnapshot {
    rows::Model model;
    std::optional<std::int64_t> head;
};

class ProjectAccess {
public:
    virtual ~ProjectAccess() = default;

    /// The project's name on the wire: `project_uuid` when the file has one,
    /// otherwise a stable opaque handle. Never a path.
    [[nodiscard]] virtual const std::string& handle() const = 0;
    /// What a person calls it: the project row's name, else the file's stem.
    [[nodiscard]] virtual std::string label() const = 0;

    /// Any thread: the rows and the head, in one read transaction on a
    /// read-only connection. `error` set when the file cannot be read.
    [[nodiscard]] virtual ReadSnapshot read(std::string& error) = 0;
    /// Any thread: the undo head alone (ADR-0153). `error` set, and nullopt,
    /// when it cannot be read -- never confused with the root.
    [[nodiscard]] virtual std::optional<std::int64_t> head(std::string& error) = 0;
    /// The file's schema, as `major.minor` (user_version).
    [[nodiscard]] virtual std::string fileSchema() = 0;

    /// The owner thread: ADR-0148's preview -- every op run and rolled back,
    /// the projection diffed. Headless, on a snapshot of the current head.
    [[nodiscard]] virtual agent::Preview preview(const agent::Changeset&,
                                                 const agent::Limits&) = 0;
    /// The owner thread: whether a changeset carrying this actor detail was
    /// applied (its agent_requests row exists).
    [[nodiscard]] virtual bool wasApplied(const std::string& actorDetail) = 0;

    /// True when an audio engine runs for this project (the app).
    [[nodiscard]] virtual bool live() const = 0;
    /// The latest telemetry frame; null when none (and always, headless).
    [[nodiscard]] virtual std::shared_ptr<const TelemetryFrame> telemetry() const = 0;
    /// How the user applies a queued changeset here (docs/CONTROL-MCP.md §2).
    [[nodiscard]] virtual std::string applyHint() const = 0;
};

/// The projects a control session is bound to.
class ProjectSet {
public:
    void add(std::shared_ptr<ProjectAccess> project);
    [[nodiscard]] std::shared_ptr<ProjectAccess> find(std::string_view handle) const;
    [[nodiscard]] std::vector<std::shared_ptr<ProjectAccess>> all() const;

    /// ADR-0232 d2: `params.project` names a bound project. A name that is
    /// not bound is refused (−32602) and touches nothing (AC-17). Omitting it
    /// is allowed only when the host opted in with allowImplicitSingle() and
    /// exactly one project is bound -- adi-mcp, which serves one file.
    [[nodiscard]] std::shared_ptr<ProjectAccess> resolve(const Json& params, RpcError& error) const;
    /// For a host bound to one project for its whole life (adi-mcp --project).
    /// Never for the app, where projects open and close (ADR-0068).
    void allowImplicitSingle(bool allow);

private:
    mutable std::mutex mutex_;
    std::vector<std::shared_ptr<ProjectAccess>> projects_;
    bool implicitSingle_ = false;
};

}  // namespace adi::control
