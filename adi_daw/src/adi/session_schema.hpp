// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include "adi/session_schema_sql.hpp"

#include <array>
#include <string_view>

namespace adi::detail {

/// Compare against the shipped definition before allowing a trigger in an
/// operation that otherwise refuses unmodelled database side effects.
[[nodiscard]] inline bool isKnownSessionTrigger(std::string_view name, std::string_view sql) noexcept {
    constexpr std::array<std::string_view, 5> names{
        "clip_slots_validate_insert", "clip_slots_validate_update",
        "clips_validate_session_insert", "clips_validate_session_update", "scenes_delete_clips"
    };
    for (const auto allowed : names) {
        if (name != allowed)
            continue;
        constexpr std::string_view prefix = "CREATE TRIGGER ";
        const auto startName = kSessionSchemaSql.find(name);
        if (startName < prefix.size() || startName == std::string_view::npos)
            return false;
        const auto start = startName - prefix.size();
        const auto end = kSessionSchemaSql.find("END;", startName);
        return end != std::string_view::npos &&
            kSessionSchemaSql.substr(start, prefix.size()) == prefix &&
            sql == kSessionSchemaSql.substr(start, end + 3 - start);
    }
    return false;
}

} // namespace adi::detail
