// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include "adi/ops.hpp"
#include <map>

namespace adi::edit {
struct RowSpec {const char* table;const char* key;const char* key2=nullptr;};
[[nodiscard]] const RowSpec& rowSpec(const std::string&);
[[nodiscard]] std::optional<Payload> row(SQLite::Database&,const std::string&,std::int64_t);
[[nodiscard]] std::optional<Payload> rowKey(SQLite::Database&,const std::string&,const Payload& key);
/// Evaluate defaults in an isolated in-memory copy of this table, then capture
/// the full typed result. No project write; replay never evaluates defaults.
[[nodiscard]] Payload defaultRow(SQLite::Database&,const std::string&,const Payload&);
/// Captured values only. Every row has table,id,before,after; id is an integer
/// or the full key object for a composite primary key. Existing rows
/// update in place; creations/deletions are explicit; silent FK effects that
/// are not captured refuse. The caller owns the journal transaction.
bool validateRows(OpContext&,const Payload&,std::string&);
bool applyRows(OpContext&,const Payload&,std::string&);
bool inverseRows(OpContext&,const Payload&,Payload&,std::string&);

/// Owner-thread preparation; call while holding a read transaction if a second
/// connection can write. Allocation and SQLite stay outside the audio callback.
class RowEdits {
public:
    explicit RowEdits(SQLite::Database& db):db_(db){}
    [[nodiscard]] std::optional<Payload> current(const std::string&,std::int64_t);
    [[nodiscard]] std::optional<Payload> currentKey(const std::string&,const Payload& key);
    void set(const std::string&,std::int64_t,std::optional<Payload>);
    void setKey(const std::string&,const Payload& key,std::optional<Payload>);
    [[nodiscard]] Payload changes()const;
private:
    struct Change {Payload key;std::optional<Payload> before,after;};
    SQLite::Database& db_;
    std::map<std::pair<std::string,std::string>,Change> rows_;
};
} // namespace adi::edit
