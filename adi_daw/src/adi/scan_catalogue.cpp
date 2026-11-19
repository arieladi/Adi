// SPDX-License-Identifier: GPL-3.0-or-later
#include "adi/scan_catalogue.hpp"
#include "adi/audio/io_audit.hpp"
#include <SQLiteCpp/SQLiteCpp.h>
#include <algorithm>
#include <array>
#include <span>
#include <stdexcept>

namespace adi::scan {
namespace {
void require(bool condition,const char* message){if(!condition)throw std::runtime_error(message);}
std::string utf8(const std::filesystem::path& path){const auto value=path.u8string();return {value.begin(),value.end()};}
bool validStamp(const std::string& stamp){
    return stamp.size()==64 && std::all_of(stamp.begin(),stamp.end(),[](char ch){
        return (ch>='0' && ch<='9') || (ch>='a' && ch<='f');
    });
}
bool before(const CatalogueModule& module,std::string_view format,std::string_view path){
    return module.format<format || (module.format==format && module.canonicalPath<path);
}
bool tableExists(SQLite::Database& db,const char* name,bool required){
    SQLite::Statement query(db,"SELECT type FROM sqlite_master WHERE name=?");query.bind(1,name);
    if(!query.executeStep()){require(!required,"missing catalogue registry table");return false;}
    require(query.getColumn(0).isText() && query.getColumn(0).getString()=="table",
        "invalid catalogue registry table type");return true;
}
struct LegacyColumn {const char* name;const char* type;int notNull,primaryKey;};
void legacyTable(SQLite::Database& db,const char* name,std::span<const LegacyColumn> columns,bool withoutRowId){
    tableExists(db,name,true);
    SQLite::Statement properties(db,"SELECT ncol,wr,strict FROM pragma_table_list WHERE schema='main' AND name=?");
    properties.bind(1,name);
    require(properties.executeStep() && properties.getColumn(0).getInt64()==static_cast<std::int64_t>(columns.size()) &&
        properties.getColumn(1).getInt()==(withoutRowId?1:0) && properties.getColumn(2).getInt()==1,
        "unsupported legacy registry table layout");
    SQLite::Statement fields(db,"SELECT name,type,\"notnull\",pk,hidden FROM pragma_table_xinfo(?) ORDER BY cid");
    fields.bind(1,name);std::size_t index=0;
    while(fields.executeStep()) {
        require(index<columns.size(),"unexpected legacy registry column");const auto& column=columns[index++];
        require(fields.getColumn(0).getString()==column.name && fields.getColumn(1).getString()==column.type &&
            fields.getColumn(2).getInt()==column.notNull && fields.getColumn(3).getInt()==column.primaryKey &&
            fields.getColumn(4).getInt()==0,"invalid legacy registry columns");
    }
    require(index==columns.size(),"missing legacy registry column");
}
void validateLegacy(SQLite::Database& db,int version){
    static constexpr std::array routes{LegacyColumn{"format","TEXT",1,1},LegacyColumn{"uid","TEXT",1,2},
        LegacyColumn{"route","TEXT",1,0},LegacyColumn{"updated_utc","INTEGER",1,0}};
    static constexpr std::array blocks{LegacyColumn{"id","INTEGER",0,1},LegacyColumn{"format","TEXT",1,0},
        LegacyColumn{"path","TEXT",1,0},LegacyColumn{"state","TEXT",1,0},LegacyColumn{"reason","TEXT",1,0},
        LegacyColumn{"updated_utc","INTEGER",1,0}};
    legacyTable(db,"plugin_routes",routes,true);
    require(!tableExists(db,"plugin_scan_cache",false) && !tableExists(db,"plugin_scan_failures",false),
        "legacy registry contains unexpected cache schema");
    if(version==1)require(!tableExists(db,"plugin_scan_blocks",false),"version-one registry has unexpected scan schema");
    else {
        legacyTable(db,"plugin_scan_blocks",blocks,false);
        require(db.execAndGet("SELECT count(*) FROM pragma_index_list('plugin_scan_blocks') i WHERE i.\"unique\"=1 AND "
            "i.partial=0 AND (SELECT count(*) FROM pragma_index_info(i.name))=2 AND "
            "(SELECT name FROM pragma_index_info(i.name) WHERE seqno=0)='format' AND "
            "(SELECT name FROM pragma_index_info(i.name) WHERE seqno=1)='path'").getInt()==1,
            "legacy quarantine identity is not unique");
    }
    require(db.execAndGet("SELECT count(*) FROM sqlite_master WHERE type='trigger' AND "
        "tbl_name IN ('plugin_routes','plugin_scan_blocks')").getInt()==0,"unexpected legacy registry trigger");
}
} // namespace

const CatalogueModule* Catalogue::find(std::string_view format,std::string_view canonicalPath) const noexcept {
    const auto found=std::lower_bound(modules.begin(),modules.end(),canonicalPath,
        [format](const CatalogueModule& module,std::string_view path){return before(module,format,path);});
    return found!=modules.end() && found->format==format && found->canonicalPath==canonicalPath?&*found:nullptr;
}

std::optional<Catalogue> readCatalogue(const std::filesystem::path& registry,std::string& error,
    const CatalogueLimits& limits,CatalogueReadStatus* status) {
    if(audio::onAudioThread)return std::nullopt;
    error.clear();if(status)*status=CatalogueReadStatus::Invalid;
    try {
        require(registry.is_absolute(),"catalogue requires an absolute registry path");
        require(limits.maximumModules>0 && limits.maximumModules<=65536 && limits.maximumPlugins>0 &&
            limits.maximumPlugins<=kMaximumPlugins && limits.maximumTextBytes>0 &&
            limits.maximumTextBytes<=64U*1024U*1024U,"invalid catalogue read bounds");
        std::error_code filesystemError;const bool exists=std::filesystem::exists(registry,filesystemError);
        require(!filesystemError,"catalogue registry path unavailable");
        if(!exists){if(status)*status=CatalogueReadStatus::Missing;error="plug-in registry does not exist";return std::nullopt;}
        SQLite::Database db(utf8(registry),SQLite::OPEN_READONLY);
        db.setBusyTimeout(2000);
        SQLite::Transaction transaction(db);
        require(db.execAndGet("PRAGMA application_id").getInt()==PluginRegistry::kApplicationId,
            "file is not a plug-in registry");
        const auto version=db.execAndGet("PRAGMA user_version").getInt();
        if(version==1 || version==2){
            validateLegacy(db,version);transaction.commit();
            if(status)*status=CatalogueReadStatus::Legacy;
            error="validated pre-cache registry requires owner discovery/migration";return std::nullopt;
        }
        require(version==PluginRegistry::kVersion,
            "catalogue requires supported version-3 registry; no migration is performed");
        tableExists(db,"plugin_scan_cache",true);tableExists(db,"plugin_scan_blocks",true);
        const bool failures=tableExists(db,"plugin_scan_failures",false);
        const std::string sql="SELECT c.format,c.path,c.stamp,c.packet,c.updated_utc,"
            "length(CAST(c.format AS BLOB)),length(CAST(c.path AS BLOB)),"
            "length(CAST(c.stamp AS BLOB)),length(CAST(c.packet AS BLOB)) "
            "FROM plugin_scan_cache c WHERE NOT EXISTS "
            "(SELECT 1 FROM plugin_scan_blocks b WHERE b.format=c.format AND b.path=c.path) "+
            std::string(failures?"AND NOT EXISTS (SELECT 1 FROM plugin_scan_failures f WHERE f.format=c.format AND f.path=c.path) ":"")+
            "ORDER BY c.format,c.path LIMIT ?";
        SQLite::Statement query(db,sql);query.bind(1,static_cast<std::int64_t>(limits.maximumModules)+1);
        Catalogue result;std::size_t modules=0,textBytes=0;
        while(query.executeStep()) {
            require(++modules<=limits.maximumModules,"catalogue exceeds module bound");
            require(query.getColumn(0).isText() && query.getColumn(1).isText() && query.getColumn(2).isText() &&
                query.getColumn(3).isText() && query.getColumn(4).isInteger() && query.getColumn(4).getInt64()>=0,
                "invalid stored catalogue column types/time");
            const auto formatBytes=query.getColumn(5).getInt64(),pathBytes=query.getColumn(6).getInt64();
            const auto stampBytes=query.getColumn(7).getInt64(),packetBytes=query.getColumn(8).getInt64();
            require(formatBytes==4 && pathBytes>0 && pathBytes<=65536 && stampBytes==64 &&
                packetBytes>0 && packetBytes<=static_cast<std::int64_t>(kMaximumPacket),
                "stored catalogue row exceeds text bounds");
            const auto rowBytes=static_cast<std::size_t>(formatBytes+pathBytes+stampBytes+packetBytes);
            require(rowBytes<=limits.maximumTextBytes-textBytes,"catalogue exceeds aggregate text bound");
            textBytes+=rowBytes;
            CatalogueModule module;module.format=query.getColumn(0).getString();
            module.canonicalPath=query.getColumn(1).getString();module.stamp=query.getColumn(2).getString();
            module.updatedUtc=query.getColumn(4).getInt64();
            require(validStamp(module.stamp),"invalid stored catalogue stamp");
            const auto packet=query.getColumn(3).getString();
            auto plugins=decodeMetadata(packet,module.format,module.canonicalPath,error);
            require(plugins.has_value(),"invalid stored catalogue metadata or identity");
            // Older success-only v3 writers could store a valid empty factory.
            // That is not an installed positive and does not become Ready here.
            if(plugins->empty())continue;
            require(plugins->size()<=limits.maximumPlugins-result.pluginCount,"catalogue exceeds total plugin bound");
            require(result.modules.empty() || before(result.modules.back(),module.format,module.canonicalPath),
                "duplicate or unsorted catalogue identity");
            result.pluginCount+=plugins->size();module.plugins=std::move(*plugins);result.modules.push_back(std::move(module));
        }
        transaction.commit();if(status)*status=CatalogueReadStatus::Ready;return result;
    } catch(const std::exception& failure){error=failure.what();return std::nullopt;}
}
} // namespace adi::scan
