// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <SQLiteCpp/SQLiteCpp.h>
#include <array>
#include <span>
#include <stdexcept>
namespace adi::scan::detail {
inline void schemaRequire(bool condition,const char* message){if(!condition)throw std::runtime_error(message);}
inline bool tableExists(SQLite::Database& db,const char* name,bool required){
    SQLite::Statement query(db,"SELECT type FROM sqlite_master WHERE name=?");query.bind(1,name);
    if(!query.executeStep()){schemaRequire(!required,"missing catalogue registry table");return false;}
    schemaRequire(query.getColumn(0).isText() && query.getColumn(0).getString()=="table",
        "invalid catalogue registry table type");return true;
}
struct LegacyColumn {const char* name;const char* type;int notNull,primaryKey;};
inline void legacyTable(SQLite::Database& db,const char* name,std::span<const LegacyColumn> columns,bool withoutRowId){
    tableExists(db,name,true);
    SQLite::Statement properties(db,"SELECT ncol,wr,strict FROM pragma_table_list WHERE schema='main' AND name=?");
    properties.bind(1,name);
    schemaRequire(properties.executeStep() && properties.getColumn(0).getInt64()==static_cast<std::int64_t>(columns.size()) &&
        properties.getColumn(1).getInt()==(withoutRowId?1:0) && properties.getColumn(2).getInt()==1,
        "unsupported legacy registry table layout");
    SQLite::Statement fields(db,"SELECT name,type,\"notnull\",pk,hidden FROM pragma_table_xinfo(?) ORDER BY cid");
    fields.bind(1,name);std::size_t index=0;
    while(fields.executeStep()) {
        schemaRequire(index<columns.size(),"unexpected legacy registry column");const auto& column=columns[index++];
        schemaRequire(fields.getColumn(0).getString()==column.name && fields.getColumn(1).getString()==column.type &&
            fields.getColumn(2).getInt()==column.notNull && fields.getColumn(3).getInt()==column.primaryKey &&
            fields.getColumn(4).getInt()==0,"invalid legacy registry columns");
    }
    schemaRequire(index==columns.size(),"missing legacy registry column");
}
inline void validateLegacy(SQLite::Database& db,int version){
    static constexpr std::array routes{LegacyColumn{"format","TEXT",1,1},LegacyColumn{"uid","TEXT",1,2},
        LegacyColumn{"route","TEXT",1,0},LegacyColumn{"updated_utc","INTEGER",1,0}};
    static constexpr std::array blocks{LegacyColumn{"id","INTEGER",0,1},LegacyColumn{"format","TEXT",1,0},
        LegacyColumn{"path","TEXT",1,0},LegacyColumn{"state","TEXT",1,0},LegacyColumn{"reason","TEXT",1,0},
        LegacyColumn{"updated_utc","INTEGER",1,0}};
    legacyTable(db,"plugin_routes",routes,true);
    schemaRequire(!tableExists(db,"plugin_scan_cache",false) && !tableExists(db,"plugin_scan_failures",false),
        "legacy registry contains unexpected cache schema");
    if(version==1)schemaRequire(!tableExists(db,"plugin_scan_blocks",false),"version-one registry has unexpected scan schema");
    else {
        legacyTable(db,"plugin_scan_blocks",blocks,false);
        schemaRequire(db.execAndGet("SELECT count(*) FROM pragma_index_list('plugin_scan_blocks') i WHERE i.\"unique\"=1 AND "
            "i.partial=0 AND (SELECT count(*) FROM pragma_index_info(i.name))=2 AND "
            "(SELECT name FROM pragma_index_info(i.name) WHERE seqno=0)='format' AND "
            "(SELECT name FROM pragma_index_info(i.name) WHERE seqno=1)='path'").getInt()==1,
            "legacy quarantine identity is not unique");
    }
    schemaRequire(db.execAndGet("SELECT count(*) FROM sqlite_master WHERE type='trigger' AND "
        "tbl_name IN ('plugin_routes','plugin_scan_blocks')").getInt()==0,"unexpected legacy registry trigger");
}
} // namespace adi::scan::detail
