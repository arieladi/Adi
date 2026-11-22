// SPDX-License-Identifier: GPL-3.0-or-later
#include "adi/track_image.hpp"
#include "adi/row_capture.hpp"
#include <SQLiteCpp/SQLiteCpp.h>
#include <sqlite3.h>
#include <algorithm>
#include <cctype>
#include <limits>
#include <map>
#include <stdexcept>
#include <utility>
#include <vector>

namespace adi::detail {
namespace {
std::string quoted(const std::string& name) {
    std::string result="\"";for(char c:name){result+=c;if(c=='"')result+=c;}return result+'"';
}
std::string lower(std::string name) {
    for(char& c:name)c=static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return name;
}
void bind(SQLite::Statement& statement,int index,const Payload& value) {
    if(value.is_null())statement.bind(index);
    else if(value.is_number_integer())statement.bind(index,signedInteger(value));
    else if(value.is_number_float())statement.bind(index,value.get<double>());
    else if(value.is_string())statement.bind(index,value.get<std::string>());
    else if(value.is_binary()) {
        const auto& bytes=value.get_binary();static const char empty=0;
        if(bytes.size()>static_cast<std::size_t>(std::numeric_limits<int>::max()))throw std::runtime_error("captured blob too large");
        statement.bind(index,bytes.empty()?static_cast<const void*>(&empty):bytes.data(),static_cast<int>(bytes.size()));
    }else throw std::runtime_error("invalid captured SQLite cell");
}
Payload record(SQLite::Statement& query, std::size_t inverseLimit = 0) {
    // The raw TEXT/BLOB byte count is a lower bound on the inverse size.
    // Reject an oversized cell before copying it into a Payload.
    if (inverseLimit != 0) {
        auto remaining = inverseLimit;
        for (int i = 0; i < query.getColumnCount(); ++i) {
            const auto column = query.getColumn(i);
            const auto type = column.getType();
            if (type != SQLITE_TEXT && type != SQLITE_BLOB) continue;
            const auto bytes = static_cast<std::size_t>(column.getBytes());
            if (bytes > remaining) throw std::length_error(std::string(kRackUndoLimitMessage));
            remaining -= bytes;
        }
    }
    return captureQueryRow(query);
}

struct Table {std::string name;std::vector<std::string> columns,keys;bool generated=false,triggers=false;};
struct ForeignKey {std::string child,parent,action;std::vector<std::string> from,to;};
struct Catalog {
    std::map<std::string,Table> tables;
    std::map<std::string,std::vector<ForeignKey>> references;
    explicit Catalog(SQLite::Database& db) {
        SQLite::Statement names(db,"SELECT name FROM sqlite_master WHERE type='table' AND name NOT LIKE 'sqlite_%' ORDER BY name");
        while(names.executeStep()) {
            Table table;table.name=names.getColumn(0).getString();std::map<int,std::string> keys;
            SQLite::Statement columns(db,"SELECT name,pk,hidden FROM pragma_table_xinfo(?) ORDER BY cid");columns.bind(1,table.name);
            while(columns.executeStep()) {
                const auto name=columns.getColumn(0).getString();table.columns.push_back(name);
                const int order=columns.getColumn(1).getInt();if(order)keys.emplace(order,name);
                if(columns.getColumn(2).getInt()!=0)table.generated=true;
            }
            for(const auto& [order,name]:keys){(void)order;table.keys.push_back(name);}
            SQLite::Statement triggers(db,"SELECT EXISTS(SELECT 1 FROM sqlite_master WHERE type='trigger' AND tbl_name=?)");triggers.bind(1,table.name);
            table.triggers=triggers.executeStep() && triggers.getColumn(0).getInt()!=0;
            tables.emplace(lower(table.name),std::move(table));
        }
        for(const auto& [name,table]:tables) {
            std::map<int,ForeignKey> keys;
            SQLite::Statement foreign(db,"SELECT id,\"table\",\"from\",\"to\",on_delete FROM pragma_foreign_key_list(?) ORDER BY id,seq");foreign.bind(1,table.name);
            while(foreign.executeStep()) {
                auto& key=keys[foreign.getColumn(0).getInt()];key.child=name;key.parent=lower(foreign.getColumn(1).getString());key.action=foreign.getColumn(4).getString();
                key.from.push_back(foreign.getColumn(2).getString());key.to.push_back(foreign.getColumn(3).isNull()?"":foreign.getColumn(3).getString());
            }
            for(auto& [id,key]:keys) {
                (void)id;
                if(std::all_of(key.to.begin(),key.to.end(),[](const auto& column){return column.empty();}))key.to=get(key.parent).keys;
                if(key.from.size()!=key.to.size())throw std::runtime_error("invalid captured foreign-key arity");
                references[key.parent].push_back(std::move(key));
            }
        }
    }
    const Table& get(const std::string& name)const {
        const auto found=tables.find(lower(name));if(found==tables.end())throw std::runtime_error("missing captured table "+name);return found->second;
    }
};
void safeTable(const Table& table) {
    if(table.keys.empty() || table.generated || table.triggers)
        throw std::runtime_error("track delete cannot safely capture "+table.name+" (primary key, generated column or trigger)");
}
Payload keyOf(const Table& table,const Payload& row) {
    Payload key=Payload::object();for(const auto& column:table.keys) {
        const auto& value=row.at(column);if(value.is_null())throw std::runtime_error("nullable captured primary key");key[column]=value;
    }
    return key;
}
std::string predicate(const Payload& key) {
    std::string result;for(auto it=key.begin();it!=key.end();++it){if(!result.empty())result+=" AND ";result+=quoted(it.key())+" IS ?";}return result;
}
void bindKey(SQLite::Statement& query,const Payload& key,int index=0) {
    for(const auto& value:key)bind(query,++index,value);
}
Payload current(SQLite::Database& db,const Table& table,const Payload& key,std::size_t inverseLimit=0) {
    SQLite::Statement query(db,"SELECT * FROM "+quoted(table.name)+" WHERE "+predicate(key));bindKey(query,key);
    if(!query.executeStep())return nullptr;
    return record(query,inverseLimit);
}
using Identity=std::pair<std::string,std::string>;
struct Row {std::string table;Payload key,before,after;};
class Image {
public:
  Image(SQLite::Database &database, const Catalog &schema, bool source = false, std::size_t inverseLimit = 0)
      : db(database), catalog(schema), sourceRead(source), inverseLimit_(inverseLimit), remaining_(inverseLimit) {}
  bool ignored(const std::string &name) const {
      return sourceRead && (name == "frozen_renders" || name == "frozen_groups" || name == "device_panels" ||
                            name == "device_panel_params" || name == "controller_maps");
  }
    std::map<Identity,Row> rows;
    void remove(const std::string& name,const Payload& before) {
        if (ignored(lower(name)))
            return;
        const auto &table = catalog.get(name);
        if (!sourceRead)
            safeTable(table);
        if (sourceRead && table.keys.empty())
            throw std::runtime_error("freeze source table requires a primary key: " + table.name);
        auto key = keyOf(table, before);
        const Identity identity{lower(name), key.dump()};
        if (sourceRead && rows.size() >= 100000 && !rows.contains(identity))
            throw std::runtime_error("freeze source row count exceeds limit");
        const auto found=rows.find(identity);
        if(found==rows.end()){
            account(before);
            rows.emplace(identity,Row{lower(name),std::move(key),before,nullptr});
            pending.push_back(identity);
        }
        else if(!found->second.after.is_null()){found->second.after=nullptr;pending.push_back(identity);}
    }
    void nullReferences(const ForeignKey& foreign,const Payload& before) {
        const auto& table=catalog.get(foreign.child);safeTable(table);auto key=keyOf(table,before);const Identity identity{foreign.child,key.dump()};
        auto found = rows.find(identity);
        if (found == rows.end()) {
            account(before);
            found = rows.emplace(identity, Row{foreign.child,key,before,before}).first;
        }
        if(found->second.after.is_null())return;
        for(const auto& column:foreign.from)found->second.after[column]=nullptr;
        if(keyOf(table,found->second.after)!=key)throw std::runtime_error("track delete changes a surviving primary key");
    }
    void matching(const std::string& name,const std::string& where,const Payload& values) {
        if(!catalog.tables.contains(name))return;
        SQLite::Statement query(db,"SELECT * FROM "+quoted(catalog.get(name).name)+" WHERE "+where);int index=0;
        for(const auto& value:values)bind(query,++index,value);
        while(query.executeStep())remove(name,record(query,inverseLimit_));
    }
    void collect(const std::string& table, std::int64_t root) {
        const auto row=current(db,catalog.get(table),Payload{{"id",root}},inverseLimit_);
        if(row.is_null())throw std::runtime_error("no matching root row in "+table);
        const auto first = pending.size();
        remove(table,row);
        for(std::size_t next=first;next<pending.size();++next) {
            // Copy: discovering a later deletion may change an earlier row.
            const auto removed=rows.at(pending[next]);
            const auto references=catalog.references.find(removed.table);
            if(references!=catalog.references.end())for(const auto& foreign:references->second) {
                    if (ignored(foreign.child))
                        continue;
                    if (!sourceRead && foreign.action != "CASCADE" && foreign.action != "SET NULL" &&
                        foreign.action != "SET DEFAULT")
                        continue;
                    std::string where;
                    Payload values = Payload::array();
                    bool null = false;
                    for (std::size_t field = 0; field < foreign.from.size(); ++field) {
                        const auto &value = removed.before.at(foreign.to[field]);
                        if (value.is_null()) {
                            null = true;
                            break;
                        }
                        if (!where.empty())
                            where += " AND ";
                        where += quoted(foreign.from[field]) + "=?";
                        values.push_back(value);
                }
                if(null)continue;
                SQLite::Statement children(db,"SELECT * FROM "+quoted(catalog.get(foreign.child).name)+" WHERE "+where);int index=0;
                for(const auto& value:values)bind(children,++index,value);
                while(children.executeStep()) {
                    if (sourceRead || foreign.action == "CASCADE")
                        remove(foreign.child, record(children,inverseLimit_));
                    else if(foreign.action=="SET NULL")nullReferences(foreign,record(children,inverseLimit_));
                    else throw std::runtime_error("track delete cannot safely capture SET DEFAULT");
                }
            }
            std::string kind;std::string idColumn;
            if(removed.table=="tracks"){kind="track";idColumn="id";}
            else if(removed.table=="clips"){kind="clip";idColumn="id";}
            else if(removed.table=="devices"){kind="device";idColumn="id";}
            else if(removed.table=="routing"){kind="routing";idColumn="id";}
            if(kind.empty())continue;
            const auto& id=removed.before.at(idColumn);
            if(kind=="track" || kind=="device")matching("routing","(src_kind=? AND src_id=?) OR (dst_kind=? AND dst_id=?)",Payload::array({kind,id,kind,id}));
            matching("automation_lanes","owner_kind=? AND owner_id=?",Payload::array({kind,id}));
            matching("controller_maps","target_kind=? AND target_id=?",Payload::array({kind,id}));
        }
    }
    Payload encode(std::int64_t root)const {
        Payload result={{"version",1},{"root",root},{"rows",Payload::array()}};
        for(const auto& [identity,row]:rows){(void)identity;result["rows"].push_back({{"table",row.table},{"key",row.key},{"before",row.before},{"after",row.after}});}
        return result;
    }
private:
  void account(const Payload& row) {
      if (inverseLimit_ == 0) return;
      // Every distinct before row remains in the complete encoded inverse.
      const auto bytes = Payload::to_cbor(row).size();
      if (bytes > remaining_) throw std::length_error(std::string(kRackUndoLimitMessage));
      remaining_ -= bytes;
  }
  SQLite::Database &db;
  const Catalog &catalog;
  bool sourceRead = false;
  std::size_t inverseLimit_, remaining_;
  std::vector<Identity> pending;
};
void fullRow(const Table& table,const Payload& row) {
    if(!row.is_object() || row.size()!=table.columns.size())throw std::runtime_error("captured columns differ from table "+table.name);
    for(const auto& column:table.columns)if(!row.contains(column))throw std::runtime_error("missing captured column "+column);
}
Image decode(SQLite::Database& db,const Catalog& catalog,const std::string& rootTable,
             std::int64_t root,const Payload& packet) {
    if(!packet.is_object() || packet.size()!=3 || packet.at("version")!=1 || signedInteger(packet.at("root"))!=root || !packet.at("rows").is_array())
        throw std::runtime_error("invalid track contents packet");
    Image image(db,catalog);
    for(const auto& entry:packet.at("rows")) {
        if(!entry.is_object() || entry.size()!=4)throw std::runtime_error("invalid track contents row");
        const auto name=entry.at("table").get<std::string>();const auto& table=catalog.get(name);safeTable(table);
        const auto& key=entry.at("key");const auto& before=entry.at("before");const auto& after=entry.at("after");fullRow(table,before);
        if(key!=keyOf(table,before))throw std::runtime_error("captured row key differs");
        if(!after.is_null()){fullRow(table,after);if(keyOf(table,after)!=key)throw std::runtime_error("captured row changes key");}
        if(name!=lower(table.name) || !image.rows.emplace(Identity{name,key.dump()},Row{name,key,before,after}).second)throw std::runtime_error("duplicate captured row");
        if(current(db,table,key)!=after)throw std::runtime_error("track restore target changed: "+name);
    }
    const auto captured=image.rows.find({rootTable,Payload{{"id",root}}.dump()});
    if(captured==image.rows.end() || !captured->second.after.is_null())throw std::runtime_error("track contents lack their root");
    return image;
}
class Atomic {
public:
    explicit Atomic(SQLite::Database& database):db(database),deferred(db.execAndGet("PRAGMA defer_foreign_keys").getInt()!=0) {
        if(sqlite3_get_autocommit(db.getHandle())!=0 || db.execAndGet("PRAGMA foreign_keys").getInt()==0)throw std::runtime_error("track image needs a foreign-key journal transaction");
        db.exec("SAVEPOINT adi_track_image_change");db.exec("PRAGMA defer_foreign_keys=ON");
    }
    ~Atomic(){if(active){try{db.exec("ROLLBACK TO adi_track_image_change");db.exec("RELEASE adi_track_image_change");restoreFlag();}catch(const std::exception&){}}}
    void finish() {
        SQLite::Statement violations(db,"PRAGMA foreign_key_check");if(violations.executeStep())throw std::runtime_error("track image leaves a foreign-key violation");
        db.exec("RELEASE adi_track_image_change");active=false;restoreFlag();
    }
private:
    SQLite::Database& db;bool deferred=false,active=true;
    void restoreFlag(){if(!deferred)db.exec("PRAGMA defer_foreign_keys=OFF");}
};
void update(SQLite::Database& db,const Table& table,const Row& row) {
    std::string assignments;for(auto it=row.before.begin();it!=row.before.end();++it){if(!assignments.empty())assignments+=',';assignments+=quoted(it.key())+"=?";}
    SQLite::Statement statement(db,"UPDATE "+quoted(table.name)+" SET "+assignments+" WHERE "+predicate(row.key));int index=0;
    for(const auto& value:row.before)bind(statement,++index,value);
    bindKey(statement,row.key,index);
    if(statement.exec()!=1)throw std::runtime_error("captured update target missing");
}
void removeImage(SQLite::Database& db, const Catalog& catalog, const Image& image) {
    Atomic atomic(db);
    for (const auto& [identity, row] : image.rows) {
        (void)identity;
        if (!row.after.is_null()) continue;
        SQLite::Statement statement(db, "DELETE FROM " + quoted(catalog.get(row.table).name) +
                                         " WHERE " + predicate(row.key));
        bindKey(statement, row.key);
        statement.exec();
    }
    for (const auto& [identity, row] : image.rows) {
        (void)identity;
        if (current(db, catalog.get(row.table), row.key) != row.after)
            throw std::runtime_error("row deletion differs from captured image");
    }
    atomic.finish();
}
void restoreImage(SQLite::Database& db, const std::string& rootTable, std::int64_t root,
                  const Payload& packet, const Payload* expectedRoot = nullptr) {
    const Catalog catalog(db);
    const auto image = decode(db, catalog, rootTable, root, packet);
    if (expectedRoot) {
        const auto& captured = image.rows.at({rootTable, Payload{{"id", root}}.dump()});
        if (captured.before != *expectedRoot)
            throw std::runtime_error("captured routing contents belong to another connection");
    }
    Atomic atomic(db);
    for (const auto& [identity, row] : image.rows) {
        (void)identity;
        if (row.after.is_null()) restoreRow(db, catalog.get(row.table).name, row.before);
    }
    for (const auto& [identity, row] : image.rows) {
        (void)identity;
        if (!row.after.is_null()) update(db, catalog.get(row.table), row);
    }
    Image restored(db, catalog);
    restored.collect(rootTable, root);
    if (restored.encode(root) != packet)
        throw std::runtime_error("captured contents contain unrelated or incomplete effects");
    atomic.finish();
}
} // namespace
Payload captureTrack(SQLite::Database& db,std::int64_t root) {
    const Catalog catalog(db);Image image(db,catalog);image.collect("tracks",root);return image.encode(root);
}
Payload captureTrackSource(SQLite::Database &db, std::int64_t root) {
    const Catalog catalog(db);
    Image image(db, catalog, true);
    image.collect("tracks", root);
    return image.encode(root);
}
void deleteTrack(SQLite::Database& db,std::int64_t root) {
    const Catalog catalog(db);
    Image image(db, catalog);
    image.collect("tracks", root);
    removeImage(db, catalog, image);
}
void restoreTrack(SQLite::Database& db,std::int64_t root,const Payload& packet) {
    restoreImage(db, "tracks", root, packet);
}
std::vector<Payload> captureRouting(SQLite::Database& db, std::span<const std::int64_t> ids) {
    if (ids.empty()) return {};
    const Catalog catalog(db);
    std::vector<Payload> packets;
    packets.reserve(ids.size());
    for (const auto id : ids) {
        Image image(db, catalog);
        image.collect("routing", id);
        packets.push_back(image.encode(id));
    }
    return packets;
}
void deleteRouting(SQLite::Database& db, std::span<const std::int64_t> ids) {
    if (ids.empty()) return;
    const Catalog catalog(db);
    Image image(db, catalog);
    for (const auto id : ids) image.collect("routing", id);
    removeImage(db, catalog, image);
}
void restoreRouting(SQLite::Database& db, const Payload& route, const Payload& packet) {
    restoreImage(db, "routing", signedInteger(route.at("id")), packet, &route);
}
Payload captureDevice(SQLite::Database& db,std::int64_t root,std::size_t inverseLimit) {
    const Catalog catalog(db);
    Image image(db, catalog, false, inverseLimit);
    image.collect("devices", root);
    return image.encode(root);
}
void deleteDevice(SQLite::Database& db, std::int64_t root, const Payload* expected) {
    const Catalog catalog(db);
    Image image(db, catalog);
    image.collect("devices", root);
    if (expected && Payload::to_cbor(image.encode(root)) != Payload::to_cbor(*expected))
        throw std::runtime_error("device subtree changed since its recorded removal");
    removeImage(db, catalog, image);
}
void restoreDevice(SQLite::Database& db,std::int64_t root,const Payload& packet) {
    restoreImage(db, "devices", root, packet);
}
Payload captureChain(SQLite::Database& db, std::int64_t root, std::size_t inverseLimit) {
    const Catalog catalog(db);
    Image image(db, catalog, false, inverseLimit);
    image.collect("device_chains", root);
    return image.encode(root);
}
void deleteChain(SQLite::Database& db, std::int64_t root, const Payload* expected) {
    const Catalog catalog(db);
    Image image(db, catalog);
    image.collect("device_chains", root);
    if (expected && Payload::to_cbor(image.encode(root)) != Payload::to_cbor(*expected))
        throw std::runtime_error("chain subtree changed since its recorded removal");
    removeImage(db, catalog, image);
}
void restoreChain(SQLite::Database& db, std::int64_t root, const Payload& packet) {
    restoreImage(db, "device_chains", root, packet);
}
} // namespace adi::detail
