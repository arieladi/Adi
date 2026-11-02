// SPDX-License-Identifier: GPL-3.0-or-later
#include "adi/row_capture.hpp"

#include <SQLiteCpp/SQLiteCpp.h>
#include <sqlite3.h>
#include <algorithm>
#include <limits>
#include <stdexcept>

namespace adi::detail {
namespace {
std::string quoted(const std::string& identifier) {
    std::string out = "\"";
    for (char c : identifier) {
        out += c;
        if (c == '"') out += c;
    }
    return out + '"';
}
} // namespace

std::int64_t signedInteger(const Payload& value) {
    if (!value.is_number_integer() ||
        (value.is_number_unsigned() && value.get<std::uint64_t>() >
             static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())))
        throw std::runtime_error("integer is outside signed 64-bit range");
    return value.get<std::int64_t>();
}

std::optional<Payload> captureRow(SQLite::Database& db, const std::string& table,
                                const std::string& key, std::int64_t id) {
    SQLite::Statement query(db, "SELECT * FROM " + quoted(table) + " WHERE " + quoted(key) + "=?");
    query.bind(1, id);
    if (!query.executeStep()) return std::nullopt;
    Payload row = Payload::object();
    for (int i = 0; i < query.getColumnCount(); ++i) {
        const auto column = query.getColumn(i);
        const auto name = query.getColumnName(i);
        switch (column.getType()) {
            case SQLITE_NULL:    row[name] = nullptr; break;
            case SQLITE_INTEGER: row[name] = column.getInt64(); break;
            case SQLITE_FLOAT:   row[name] = column.getDouble(); break;
            case SQLITE_TEXT:    row[name] = column.getString(); break;
            case SQLITE_BLOB: {
                std::vector<std::uint8_t> bytes(static_cast<std::size_t>(column.getBytes()));
                if (!bytes.empty())
                    std::copy_n(static_cast<const std::uint8_t*>(column.getBlob()), bytes.size(), bytes.data());
                row[name] = Payload::binary(std::move(bytes));
                break;
            }
            default: throw std::runtime_error("unknown SQLite column type");
        }
    }
    return row;
}

void restoreRow(SQLite::Database& db, const std::string& table, const Payload& row,
                const std::string& updateKey) {
    if (!row.is_object() || row.empty() ||
        row.size() > static_cast<std::size_t>(std::numeric_limits<int>::max()))
        throw std::runtime_error("invalid captured row");
    if (!updateKey.empty() && !row.contains(updateKey))
        throw std::runtime_error("captured update key is missing");
    std::string columns, slots, assignments;
    for (auto it = row.begin(); it != row.end(); ++it) {
        if (!columns.empty()) { columns += ','; slots += ','; }
        columns += quoted(it.key());
        slots += '?';
        if (it.key() != updateKey) {
            if (!assignments.empty()) assignments += ',';
            assignments += quoted(it.key()) + "=?";
        }
    }
    const auto sql = updateKey.empty()
        ? "INSERT INTO " + quoted(table) + "(" + columns + ") VALUES(" + slots + ")"
        : "UPDATE " + quoted(table) + " SET " +
              (assignments.empty() ? quoted(updateKey) + "=" + quoted(updateKey) : assignments) +
              " WHERE " + quoted(updateKey) + "=?";
    SQLite::Statement statement(db, sql);
    auto bind = [&](int index, const Payload& value) {
        if (value.is_null()) statement.bind(index);
        else if (value.is_number_integer()) statement.bind(index, signedInteger(value));
        else if (value.is_number_float()) statement.bind(index, value.get<double>());
        else if (value.is_string()) statement.bind(index, value.get<std::string>());
        else if (value.is_binary()) {
            const auto& bytes = value.get_binary();
            // sqlite3_bind_blob(nullptr,0) means NULL, not an empty BLOB.
            static const char empty = 0;
            if (bytes.size() > static_cast<std::size_t>(std::numeric_limits<int>::max()))
                throw std::runtime_error("captured blob is too large");
            statement.bind(index, bytes.empty() ? static_cast<const void*>(&empty)
                                               : static_cast<const void*>(bytes.data()),
                           static_cast<int>(bytes.size()));
        } else throw std::runtime_error("invalid captured SQLite value");
    };
    int index = 0;
    for (auto it = row.begin(); it != row.end(); ++it)
        if (updateKey.empty() || it.key() != updateKey) bind(++index, it.value());
    if (!updateKey.empty()) bind(++index, row.at(updateKey));
    if (statement.exec() == 0 && !updateKey.empty())
        throw std::runtime_error("captured update target is missing");
}
} // namespace adi::detail
