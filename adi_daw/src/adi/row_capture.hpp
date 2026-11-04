// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "adi/ops.hpp"

namespace adi::detail {
// Owner-thread undo capture. Table/key names come from engine code, never a
// payload. All columns and SQLite storage types survive, including extensions.
std::optional<Payload> captureRow(SQLite::Database&, const std::string& table,
                                const std::string& key, std::int64_t id);
std::optional<Payload> captureRowKey(SQLite::Database&,const std::string& table,const Payload& key);
void updateRowKey(SQLite::Database&,const std::string& table,const Payload& row,const Payload& key);
int eraseRowKey(SQLite::Database&,const std::string& table,const Payload& key);
void restoreRow(SQLite::Database&, const std::string& table, const Payload& row,
                const std::string& updateKey = {});
std::int64_t signedInteger(const Payload&);
} // namespace adi::detail
