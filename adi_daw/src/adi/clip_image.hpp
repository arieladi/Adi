// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include "adi/edit_rows.hpp"
#include <map>
namespace adi::clip_edit {
struct ChildTable {const char* table;const char* key;};
[[nodiscard]] std::span<const ChildTable> childTables();
[[nodiscard]] bool tablePresent(SQLite::Database&,const std::string&);
[[nodiscard]] Payload captureDeleted(OpContext&,std::int64_t);
void restoreDeleted(OpContext&,const Payload&);
void createImage(edit::RowEdits&,const Payload&);
void deleteImage(edit::RowEdits&,const Payload&);
class CopyIds {
public:
    explicit CopyIds(SQLite::Database& db):db_(db){}
    [[nodiscard]] std::int64_t next(const std::string& table);
    void reserve(const std::string& table,std::int64_t);
private:
    SQLite::Database& db_;
    std::map<std::string,std::int64_t> used_;
};
/// Remap row ids/clip links only. Note identities and blob bytes stay exact;
/// creating a copy never rewires aliases of the original.
[[nodiscard]] Payload cloneImage(const Payload&,std::int64_t,CopyIds&);
} // namespace adi::clip_edit
