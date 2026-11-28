// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace adi::control {

enum class AgentLogKind { Request, Answer, Proposal, ProposalOutcome, ViewChange, StopPress };

struct AgentLogEvent {
    AgentLogKind kind;
    std::string id = {};     ///< run/job or proposal correlation
    std::string text = {};
    std::string state = {};
    std::string actor = {};
    std::string source = {};
};

struct AgentLogEntry {
    std::int64_t sequence;   ///< strictly increasing within this project
    std::int64_t utcMicros;   ///< local recording time in Unix microseconds
    AgentLogEvent event;
};

/// ADR-0261 d7. One log per project identity in ADI's private local data.
/// Only trusted app code supplies its events. Call off the audio thread.
class AgentLog {
public:
    AgentLog();
    explicit AgentLog(std::filesystem::path directory);

    [[nodiscard]] bool append(std::string_view projectUuid, const AgentLogEvent&) const;

    /// Chronological page, excluding afterSequence. A missing log gives an
    /// empty page; invalid storage or an unavailable lock gives nullopt.
    /// Pages are bounded by entry count and bytes. Use the last sequence for
    /// the next page, even if the previous page contained fewer than maximum.
    [[nodiscard]] std::optional<std::vector<AgentLogEntry>> read(
        std::string_view projectUuid, std::int64_t afterSequence = 0, std::size_t maximum = 256) const;

private:
    [[nodiscard]] std::filesystem::path fileFor(std::string_view projectUuid) const;
    std::filesystem::path directory_;
};

}  // namespace adi::control
