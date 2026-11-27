// SPDX-License-Identifier: AGPL-3.0-or-later
//
// The control methods (docs/CONTROL-MCP.md §5.1). Every transport reaches the
// same nine: the MCP projection maps its four tools and its resources onto
// them, and the loopback WebSocket (CTL-04) will expose them as they are.
//
//   system.describe    project.list       project.read      project.manifest
//   project.graph      project.telemetry  ops.describe      changeset.propose
//   changeset.status
//
// Only changeset.propose can write project edits. The project changes in one
// transaction, under an Apply grant or when the user approves (ADR-0148).

#pragma once

#include "adi/changeset.hpp"
#include "adi/control/apply_rate_limit.hpp"
#include "adi/control/changeset_queue.hpp"
#include "adi/control/consent.hpp"
#include "adi/control/project_access.hpp"
#include "adi/control/service.hpp"

#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <unordered_set>

namespace adi::control {

struct ControlContext {
    ProjectSet& projects;
    ChangesetQueue& queue;
    /// The per-request cap: agent.maxOpsPerRequest (ADR-0203 d20: 6000).
    agent::Limits limits;
    /// Wall-clock microseconds for a changeset's createdUtc. Injected.
    std::function<std::int64_t()> nowUtcMicros;
    /// The steady clock the telemetry sampler stamps frames with.
    std::function<std::int64_t()> steadyMs;
    std::string serverName = "adi";
    std::string serverVersion;
    /// How the caller reached ADI, for actor_detail: "adi-mcp", "rpc", ...
    std::string transport = "adi-mcp";
    /// Read the owner's settings. The host supplies thread-safe readers.
    std::function<Tier()> appTier = {};
    std::function<std::size_t()> maxOpsPerMinute = {};
    ApplyRateLimit applyRateLimit{};
    /// A client with no host-owned run stays tainted after receiving remarks.
    std::mutex remarkMutex{};
    std::unordered_set<std::string> clientsThatReadRemarks{};
    ConsentLedger* consentLedger = nullptr;
    /// The app can post this question to its owner thread for the local user.
    std::function<void(ConsentRequest)> askConsent = {};
};

/// Registers the nine methods. `context` must outlive `service`.
void registerControlMethods(Service& service, ControlContext& context);

}  // namespace adi::control
