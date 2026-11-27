// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include "adi/ops.hpp"

namespace adi {
class AppliedBatch;
namespace engine {
class Session;

[[nodiscard]] std::span<const OpDescriptor> launchOperationDescriptors() noexcept;
[[nodiscard]] bool isSessionLaunchOperation(std::string_view) noexcept;

enum class LaunchOperationStatus {
    Queued,
    NoLaunchOperations,
    AudioThread,
    OversizeGesture,
    UnknownOperation,
    InvalidPayload,
    UnrelatedProject,
    MailboxUnavailable,
};

struct LaunchOperationResult {
    LaunchOperationStatus status = LaunchOperationStatus::NoLaunchOperations;
    std::size_t operation = 0;
    std::vector<ValidationIssue> issues;
};

/// Owner only. Validate and queue one complete gesture to the live Session
/// without a journal. The driver checks target identity and incarnation for
/// the whole gesture against the generation that actually renders it.
[[nodiscard]] LaunchOperationResult dispatchLaunchOperations(Session&, std::span<const OpRequest>);

/// Owner only. Submit the launch operations in a committed, same-project audit
/// batch as one gesture. The observer calls this once per delivered Commit.
/// History notices and transactions with no launch operations are ignored.
[[nodiscard]] LaunchOperationResult dispatchLaunchOperations(Session&, const AppliedBatch&);

} // namespace engine
} // namespace adi
