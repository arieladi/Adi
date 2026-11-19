// SPDX-License-Identifier: AGPL-3.0-or-later
#include "adi/row_ids.hpp"
#include "adi/audio/io_audit.hpp"
#include <SQLiteCpp/SQLiteCpp.h>
#include <algorithm>
#include <limits>
#include <stdexcept>
namespace adi {
namespace {
void owner() {
    if (audio::onAudioThread)
        throw std::runtime_error("persistent row identities belong off the audio callback");
}
std::string quote(std::string_view value) {
    if (value.empty() || value.find('\0') != std::string_view::npos)
        throw std::runtime_error("row identity table name is invalid");
    std::string result = "\"";
    for (const char ch : value) {
        result += ch;
        if (ch == '"')
            result += '"';
    }
    return result + '"';
}
void increment(std::int64_t &value) {
    if (value == std::numeric_limits<std::int64_t>::max())
        throw std::runtime_error("persistent row identities are exhausted");
    ++value;
}
} // namespace
RowIds::State &RowIds::state(std::string_view table) {
    owner();
    if (const auto found = states_.find(table); found != states_.end())
        return found->second;
    const auto name = quote(table);
    SQLite::Statement exists(db_, "SELECT 1 FROM sqlite_schema WHERE type='table' AND name=?");
    exists.bind(1, std::string(table));
    if (!exists.executeStep())
        throw std::runtime_error("row identity table is missing");
    SQLite::Statement columns(db_, "SELECT name,type,pk FROM pragma_table_info(?)");
    columns.bind(1, std::string(table));
    int keys = 0;
    bool id = false;
    while (columns.executeStep()) {
        const auto primary = columns.getColumn(2).getInt();
        if (primary) {
            ++keys;
            id = columns.getColumn(0).getString() == "id" &&
                 columns.getColumn(1).getString() == "INTEGER" && primary == 1;
        }
    }
    if (keys != 1 || !id)
        throw std::runtime_error("row identity needs one INTEGER PRIMARY KEY named id");
    SQLite::Statement maximum(db_, "SELECT COALESCE(MAX(\"id\"),0) FROM " + name);
    maximum.executeStep();
    return states_
        .emplace(std::string(table),
                 State{std::max<std::int64_t>(0, maximum.getColumn(0).getInt64()), {}})
        .first->second;
}
void RowIds::reserve(std::string_view table, std::int64_t id) {
    auto &group = state(table);
    group.high = std::max(group.high, id);
    if (id > 0)
        group.reserved.insert(id);
}
std::int64_t RowIds::next(std::string_view table, RowIdExhaustion exhaustion) {
    auto &group = state(table);
    if (group.high < std::numeric_limits<std::int64_t>::max()) {
        increment(group.high);
        group.reserved.insert(group.high);
        return group.high;
    }
    if (exhaustion != RowIdExhaustion::FirstPositiveGap)
        throw std::runtime_error("persistent row identities are exhausted");
    std::int64_t candidate = 1;
    auto skipReserved = [&]() {
        while (group.reserved.contains(candidate))
            increment(candidate);
    };
    SQLite::Statement ids(db_,
                          "SELECT \"id\" FROM " + quote(table) + " WHERE \"id\">0 ORDER BY \"id\"");
    while (ids.executeStep()) {
        skipReserved();
        const auto stored = ids.getColumn(0).getInt64();
        if (stored > candidate)
            break;
        if (stored == candidate)
            increment(candidate);
    }
    skipReserved();
    group.reserved.insert(candidate);
    return candidate;
}
} // namespace adi
