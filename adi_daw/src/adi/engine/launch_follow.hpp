// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

namespace adi::engine {

enum class FollowAction : std::uint8_t {
    None, Stop, Again, Previous, Next, First, Last, Any, Other, Jump
};
enum class FollowResult : std::uint8_t { Unchanged, Stop, Launch, Unavailable, GroupBoundary };

struct FollowTarget {
    std::size_t slot = 0;
    std::int64_t clip = 0;
    bool operator==(const FollowTarget &) const = default;
};
struct FollowDecision {
    FollowResult result = FollowResult::Unavailable;
    std::optional<FollowTarget> target;
};
struct FollowChoice {
    /// Uniform rank from the owner's current pass. Any allows every group
    /// member. Other uses the same order with the current clip omitted.
    std::optional<std::size_t> rank;
    /// Explicit destination supplied by the owner after resolving Jump.
    std::optional<std::size_t> jump;
};
struct FollowGroup {
    std::size_t begin = 0, end = 0; ///< half-open slot positions
    bool operator==(const FollowGroup &) const = default;
};

/// Occupied runs on one track. Empty slots split the run even when their
/// Stop button is hidden.
[[nodiscard]] constexpr std::optional<FollowGroup>
followGroup(std::span<const std::optional<std::int64_t>> clips, std::size_t playing) noexcept {
    if (playing >= clips.size() || !clips[playing])
        return {};
    FollowGroup group{playing, playing + 1};
    while (group.begin > 0 && clips[group.begin - 1])
        --group.begin;
    while (group.end < clips.size() && clips[group.end])
        ++group.end;
    return group;
}

/// Pure target selection. GroupBoundary lets the driver apply its documented
/// Previous/Next boundary or single-member Other policy. Chance and exact
/// launch time stay with the driver.
[[nodiscard]] constexpr FollowDecision
resolveFollowTarget(std::span<const std::optional<std::int64_t>> clips, std::size_t playing,
                    FollowAction action, FollowChoice choice = {}) noexcept {
    const auto target = [&](std::size_t slot) -> FollowDecision {
        if (slot >= clips.size() || !clips[slot])
            return {};
        return {FollowResult::Launch, FollowTarget{slot, *clips[slot]}};
    };
    if (playing >= clips.size() || !clips[playing])
        return {};
    switch (action) {
    case FollowAction::None: return {FollowResult::Unchanged, {}};
    case FollowAction::Stop: return {FollowResult::Stop, {}};
    case FollowAction::Again: return target(playing);
    case FollowAction::Jump: return choice.jump ? target(*choice.jump) : FollowDecision{};
    default: break;
    }
    const auto group = followGroup(clips, playing);
    const auto begin = group->begin, end = group->end;
    const auto count = end - begin;
    const FollowDecision boundary{FollowResult::GroupBoundary, {}};
    switch (action) {
    case FollowAction::Previous: return playing > begin ? target(playing - 1) : boundary;
    case FollowAction::Next: return playing + 1 < end ? target(playing + 1) : boundary;
    case FollowAction::First: return target(begin);
    case FollowAction::Last: return target(end - 1);
    case FollowAction::Any:
        return choice.rank && *choice.rank < count ? target(begin + *choice.rank)
                                                    : FollowDecision{};
    case FollowAction::Other:
        if (count == 1)
            return boundary;
        if (choice.rank && *choice.rank < count - 1) {
            const auto selected = begin + *choice.rank;
            return target(selected < playing ? selected : selected + 1);
        }
        return {};
    default: return {};
    }
}
} // namespace adi::engine
