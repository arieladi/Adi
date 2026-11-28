// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <algorithm>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace adi::ui {

struct PlaybackProblems {
    std::vector<std::string> reasons;
    std::vector<std::int64_t> clipIds;
    std::vector<std::int64_t> deviceIds;
};

namespace playback_problem_detail {

inline std::optional<std::int64_t> taggedId(std::string_view suffix, bool allowsDeviceName) {
    if (suffix.empty()) return std::nullopt;
    std::int64_t id{};
    const auto parsed = std::from_chars(suffix.data(), suffix.data() + suffix.size(), id);
    if (parsed.ec != std::errc{}) return std::nullopt;
    // The engine formats IDs as signed decimal, without a plus sign or leading zeroes.
    if ((suffix.front() == '0' && parsed.ptr != suffix.data() + 1) || suffix.starts_with("-0"))
        return std::nullopt;
    suffix.remove_prefix(static_cast<std::size_t>(parsed.ptr - suffix.data()));
    if (suffix.starts_with(':') ||
        (allowsDeviceName && suffix.starts_with(" (") && suffix.find("):", 2) != suffix.npos))
        return id;
    return std::nullopt;
}

}  // namespace playback_problem_detail

// Retain every reason in engine order. Only leading engine tags mark a clip or device.
[[nodiscard]] inline PlaybackProblems projectPlaybackProblems(std::span<const std::string> problems) {
    constexpr std::string_view clipTag = "clips#";
    constexpr std::string_view deviceTag = "devices#";
    PlaybackProblems result;
    result.reasons.assign(problems.begin(), problems.end());
    for (std::string_view reason : problems) {
        if (reason.starts_with(clipTag)) {
            if (auto id = playback_problem_detail::taggedId(reason.substr(clipTag.size()), false))
                result.clipIds.push_back(*id);
        } else if (reason.starts_with(deviceTag)) {
            if (auto id = playback_problem_detail::taggedId(reason.substr(deviceTag.size()), true))
                result.deviceIds.push_back(*id);
        }
    }
    for (auto* ids : {&result.clipIds, &result.deviceIds}) {
        std::sort(ids->begin(), ids->end());
        ids->erase(std::unique(ids->begin(), ids->end()), ids->end());
    }
    return result;
}

}  // namespace adi::ui
