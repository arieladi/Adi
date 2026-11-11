// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include "adi/row_capture.hpp"
#include <SQLiteCpp/SQLiteCpp.h>
#include <stdexcept>
namespace adi::automation::range_rows {
// Document owner only. Preserve every SQLite cell, including future columns;
// record_index is storage identity and must follow its actual AAUT record.
inline Payload capture(SQLite::Database& db,std::int64_t dataId) {
    Payload result=Payload::array();
    SQLite::Statement query(db,"SELECT * FROM automation_segment_ranges WHERE data_id=? ORDER BY id");
    query.bind(1,dataId);
    while(query.executeStep())result.push_back(adi::detail::captureQueryRow(query));
    return result;
}
inline void replace(SQLite::Database& db,std::int64_t dataId,const Payload& clip,const Payload& rows) {
    if(!rows.is_array())throw std::runtime_error("automation: curve ranges must be an array");
    for(const auto& row:rows){
        if(!row.is_object() || adi::detail::signedInteger(row.at("data_id"))!=dataId || row.at("clip_id")!=clip)
            throw std::runtime_error("automation: curve range address mismatch");
        // A captured inverse must not ask SQLite to invent a replacement ID.
        (void)adi::detail::signedInteger(row.at("id"));
    }
    // Erase before reinsertion so ordinal swaps cannot collide. The enclosing
    // OpJournal transaction makes a later refusal restore both tables.
    SQLite::Statement erase(db,"DELETE FROM automation_segment_ranges WHERE data_id=?");
    erase.bind(1,dataId);erase.exec();
    for(const auto& row:rows)adi::detail::restoreRow(db,"automation_segment_ranges",row);
}
} // namespace adi::automation::range_rows
