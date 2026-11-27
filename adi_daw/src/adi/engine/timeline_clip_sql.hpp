// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include <SQLiteCpp/Database.h>
#include <string>
#include <string_view>

namespace adi::engine {

/// SQL Arrangement readers append this to their existing WHERE clause. The
/// qualified clip ID is a static SQL expression supplied by the caller. An
/// older file or raw backup without the Session table has no slots.
[[nodiscard]] inline std::string excludeSlotClips(const SQLite::Database& db, std::string_view clipId) {
    if (!db.tableExists("clip_slots")) return {};
    return " AND NOT EXISTS (SELECT 1 FROM main.clip_slots AS slot WHERE slot.clip_id=" +
        std::string(clipId) + ")";
}

} // namespace adi::engine
