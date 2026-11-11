// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include "adi/edit_rows.hpp"
#include "adi/row_ids.hpp"
namespace adi::clip_edit {
struct ChildTable {const char* table;const char* key;};
[[nodiscard]] std::span<const ChildTable> childTables();
[[nodiscard]] bool tablePresent(SQLite::Database&,const std::string&);
/// Capture complete known material without assuming the root will be deleted.
/// Actual row changes must still pass validateRows before publication.
[[nodiscard]] Payload captureImage(OpContext&,std::int64_t);
[[nodiscard]] Payload captureDeleted(OpContext&,std::int64_t);
void restoreDeleted(OpContext&,const Payload&);
void createImage(edit::RowEdits&,const Payload&);
void deleteImage(edit::RowEdits&,const Payload&);
class CopyIds {
public:
    explicit CopyIds(SQLite::Database& db):ids_(db){}
    [[nodiscard]] std::int64_t next(const std::string& table);
    void reserve(const std::string& table,std::int64_t);
private:
    RowIds ids_;
};
/// Remap row ids/clip links only. Note identities and blob bytes stay exact;
/// creating a copy never rewires aliases of the original.
[[nodiscard]] Payload cloneImage(const Payload&,std::int64_t,CopyIds&);
} // namespace adi::clip_edit
