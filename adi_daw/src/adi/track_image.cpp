// SPDX-License-Identifier: GPL-3.0-or-later
#include "adi/track_image.hpp"
#include "adi/session_schema.hpp"
#include "adi/engine/timeline_clip_sql.hpp"
#include "adi/row_capture.hpp"
#include "adi/row_ids.hpp"
#include <SQLiteCpp/SQLiteCpp.h>
#include <sqlite3.h>
#include <algorithm>
#include <cctype>
#include <limits>
#include <map>
#include <optional>
#include <set>
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
Payload record(SQLite::Statement &query) {
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
            SQLite::Statement triggers(db, "SELECT name,sql FROM sqlite_master WHERE type='trigger' AND tbl_name=?");
            triggers.bind(1,table.name);
            while (triggers.executeStep()) {
                const auto name = triggers.getColumn(0).getString();
                if ((table.name != "clips" && table.name != "clip_slots") ||
                    !isKnownSessionTrigger(name, triggers.getColumn(1).getString())) {
                    table.triggers = true;
                    break;
                }
            }
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
Payload current(SQLite::Database& db,const Table& table,const Payload& key) {
    SQLite::Statement query(db,"SELECT * FROM "+quoted(table.name)+" WHERE "+predicate(key));bindKey(query,key);
    if(!query.executeStep())return nullptr;
    return record(query);
}
using Identity=std::pair<std::string,std::string>;
struct Row {std::string table;Payload key,before,after;};
class Image {
public:
  Image(SQLite::Database &database, const Catalog &schema, bool source = false)
      : db(database), catalog(schema), sourceRead(source) {}
  bool ignored(const std::string &name) const {
      return sourceRead && (name == "frozen_renders" || name == "frozen_groups" || name == "device_panels" ||
                            name == "device_panel_params" || name == "controller_maps" ||
                            name == "clip_slots" || name == "clip_launch");
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
        if(found==rows.end()){rows.emplace(identity,Row{lower(name),std::move(key),before,nullptr});pending.push_back(identity);}
        else if(!found->second.after.is_null()){found->second.after=nullptr;pending.push_back(identity);}
    }
    void nullReferences(const ForeignKey& foreign,const Payload& before) {
        const auto& table=catalog.get(foreign.child);safeTable(table);auto key=keyOf(table,before);const Identity identity{foreign.child,key.dump()};
        auto [found,inserted]=rows.try_emplace(identity,Row{foreign.child,key,before,before});(void)inserted;
        if(found->second.after.is_null())return;
        for(const auto& column:foreign.from)found->second.after[column]=nullptr;
        if(keyOf(table,found->second.after)!=key)throw std::runtime_error("track delete changes a surviving primary key");
    }
    void matching(const std::string& name,const std::string& where,const Payload& values) {
        if(!catalog.tables.contains(name))return;
        SQLite::Statement query(db,"SELECT * FROM "+quoted(catalog.get(name).name)+" WHERE "+where);int index=0;
        for(const auto& value:values)bind(query,++index,value);
        while(query.executeStep())remove(name,record(query));
    }
    void collect(const std::string& table, std::int64_t root) {
        const auto row=current(db,catalog.get(table),Payload{{"id",root}});
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
                if (sourceRead && foreign.child == "clips")
                    where += engine::excludeSlotClips(db, "clips.id");
                SQLite::Statement children(db,"SELECT * FROM "+quoted(catalog.get(foreign.child).name)+" WHERE "+where);int index=0;
                for(const auto& value:values)bind(children,++index,value);
                while(children.executeStep()) {
                    if (sourceRead || foreign.action == "CASCADE")
                        remove(foreign.child, record(children));
                    else if(foreign.action=="SET NULL")nullReferences(foreign,record(children));
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
  SQLite::Database &db;
  const Catalog &catalog;
  bool sourceRead = false;
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
namespace {
Identity identityOf(const std::string &table, const Payload &id) {
    return {table, Payload{{"id", id}}.dump()};
}
std::string endpointTable(const Payload &kind) {
    if (kind == "track") return "tracks";
    if (kind == "device") return "devices";
    if (kind == "clip") return "clips";
    if (kind == "routing") return "routing";
    return {};
}
using CopyRows = std::map<Identity, Row>;
using CopyKeys = std::map<Identity, std::int64_t>;
using ForeignByChild = std::map<std::string, std::vector<const ForeignKey *>>;
// R12 will settle whether a copied track starts armed.
constexpr bool kCopyArmState = false;

bool pitchExtractionRow(const Payload &entry) {
    return entry.at("table") == "clip_pitch_extractions";
}

Payload pooledKeys(SQLite::Database &db, const Catalog &catalog, const Payload &capture) {
    Payload keys = Payload::object();
    for (const auto &[table, columns] : {std::pair{"media_files", "hash_blake3"},
                                        std::pair{"plugin_refs", "format,uid,shell_id"}}) {
        const auto references = catalog.references.find(table);
        if (references == catalog.references.end())
            continue;
        std::set<std::int64_t> ids;
        for (const auto &entry : capture.at("rows")) {
            if (!entry.at("after").is_null())
                continue;
            for (const auto &foreign : references->second) {
                if (entry.at("table") != foreign.child || foreign.to.size() != 1 || foreign.to.front() != "id")
                    continue;
                const auto &value = entry.at("before").at(foreign.from.front());
                if (!value.is_null())
                    ids.insert(signedInteger(value));
            }
        }
        for (const auto id : ids) {
            SQLite::Statement read(db, std::string("SELECT ") + columns + " FROM " + table + " WHERE id=?");
            read.bind(1, id);
            if (!read.executeStep())
                throw std::runtime_error("The track's original media or plug-in is missing from the project.");
            keys[table][std::to_string(id)] = record(read);
        }
    }
    return keys;
}
// Form the referenced row's original identity using the foreign key's column
// names. Pooled rows and connections to an existing external track keep their
// original identity because they have no row in the clipboard image.
std::optional<Identity> referencedRow(const ForeignKey &foreign, const Payload &source) {
    Payload key = Payload::object();
    for (std::size_t i = 0; i < foreign.from.size(); ++i) {
        const auto &value = source.at(foreign.from[i]);
        if (value.is_null())
            return std::nullopt;
        key[foreign.to[i]] = value;
    }
    return Identity{foreign.parent, key.dump()};
}
class TrackCopyImage {
  public:
    TrackCopyImage(SQLite::Database &db, const Catalog &catalog, const Payload &capture, std::int64_t retainedDeviceId)
        : originalRoot_(signedInteger(capture.at("root"))) {
        for (const auto &entry : capture.at("rows")) {
            if (!entry.at("after").is_null())
                continue; // The deletion image can contain changes to surviving tracks.
            if (pitchExtractionRow(entry))
                continue;
            const auto name = entry.at("table").get<std::string>();
            const auto &source = entry.at("before");
            const auto &table = catalog.get(name);
            fullRow(table, source);
            auto key = keyOf(table, source);
            const Identity identity{name, key.dump()};
            rows_.emplace(identity, Row{name, std::move(key), source, nullptr});
        }
        const auto root = rows_.find(identityOf("tracks", originalRoot_));
        if (root == rows_.end())
            throw std::runtime_error("The clipboard has no track.");
        for (const auto &entry : catalog.references)
            for (const auto &link : entry.second)
                links_[link.child].push_back(&link);

        std::set<Identity> omitted;
        const auto owned = [&](const std::string &table, const Payload &id) {
            return !table.empty() && rows_.contains(identityOf(table, id));
        };
        const auto endpointExists = [&](const std::string &table, const Payload &id) {
            return table.empty() || owned(table, id) || !current(db, catalog.get(table), Payload{{"id", id}}).is_null();
        };
        for (const auto &[identity, row] : rows_) {
            const auto &values = row.before;
            if (row.table == "tracks" && values.at("kind") == "master")
                throw std::runtime_error("The Main track cannot be copied.");
            if (row.table == "routing") {
                const auto source = endpointTable(values.at("src_kind")), destination = endpointTable(values.at("dst_kind"));
                const bool ownSource = owned(source, values.at("src_id"));
                const bool ownDestination = owned(destination, values.at("dst_id"));
                const bool sidechain = values.at("kind") == "sidechain";
                const bool copyRoute = sidechain ? ownDestination : ownSource;
                if (!copyRoute ||
                    !endpointExists(source, values.at("src_id")) || !endpointExists(destination, values.at("dst_id")))
                    omitted.insert(identity);
            } else if (row.table == "frozen_renders" || row.table == "frozen_groups") {
                // Cache fingerprints describe the original track identities.
                omitted.insert(identity);
            } else if (row.table == "automation_data" && (!owned("automation_lanes", values.at("lane_id")) ||
                       (!values.at("clip_id").is_null() && !owned("clips", values.at("clip_id"))))) {
                omitted.insert(identity);
            } else if (row.table == "macro_mappings" &&
                       (!owned("macros", values.at("macro_id")) || !owned("devices", values.at("target_device_id")))) {
                omitted.insert(identity);
            }
        }

        // Drop rows owned by an excluded route or dependency, including the
        // track-format's polymorphic automation and controller links.
        std::size_t previous;
        do {
            previous = omitted.size();
            for (const auto &[identity, row] : rows_) {
                if (omitted.contains(identity))
                    continue;
                if (const auto target = polymorphicOwner(row); target && omitted.contains(*target))
                    omitted.insert(identity);
                if (const auto found = links_.find(row.table); found != links_.end())
                    for (const auto *foreign : found->second)
                        if (foreign->action == "CASCADE")
                            if (const auto target = referencedRow(*foreign, row.before); target && omitted.contains(*target))
                                omitted.insert(identity);
            }
        } while (previous != omitted.size());
        for (const auto &identity : omitted)
            rows_.erase(identity);

        RowIds allocator(db);
        if (retainedDeviceId > 0)
            allocator.reserve("devices", retainedDeviceId);
        for (const auto &[identity, row] : rows_)
            if (row.key.size() == 1 && row.key.contains("id"))
                allocator.reserve(identity.first, signedInteger(row.key.at("id")));
        for (const auto &[identity, row] : rows_)
            if (row.key.size() == 1 && row.key.contains("id"))
                keys_.emplace(identity, allocator.next(identity.first));
    }

    Payload clone(SQLite::Database &db, const Catalog &catalog, std::int64_t parent, std::int64_t index) const {
        const auto rootKey = identityOf("tracks", originalRoot_);
        const auto id = keys_.at(rootKey);
        Image image(db, catalog);
        for (const auto &[identity, row] : rows_) {
            auto values = row.before;
            for (auto at = values.begin(); at != values.end(); ++at)
                at.value() = remapCell(identity, row, at.key(), 0);
            if (row.table == "tracks") {
                values["frozen"] = 0;
                values["freeze_media_id"] = nullptr;
                if (!kCopyArmState)
                    values["record_armed"] = 0;
                if (values.contains("role"))
                    values["role"] = nullptr; // A project has only one Reference track.
                if (identity == rootKey) {
                    values["parent_id"] = parent == 0 ? Payload(nullptr) : Payload(parent);
                    values["index_in_parent"] = index;
                }
            }

            auto key = keyOf(catalog.get(row.table), values);
            const Identity newIdentity{row.table, key.dump()};
            if (!image.rows.emplace(newIdentity,
                                    Row{row.table, std::move(key), std::move(values), nullptr}).second)
                throw std::runtime_error("The duplicate would reuse a row identity.");
        }
        return image.encode(id);
    }

  private:
    static std::optional<Identity> polymorphicOwner(const Row &row) {
        if (row.table == "automation_lanes")
            return identityOf(endpointTable(row.before.at("owner_kind")), row.before.at("owner_id"));
        if (row.table == "controller_maps")
            return identityOf(endpointTable(row.before.at("target_kind")), row.before.at("target_id"));
        return std::nullopt;
    }
    Payload remapCell(const Identity &identity, const Row &row, const std::string &column, std::size_t depth) const {
        if (depth > rows_.size())
            throw std::runtime_error("The track copy contains a circular foreign key.");
        if (column == "id")
            if (const auto id = keys_.find(identity); id != keys_.end())
                return id->second;
        if (const auto links = links_.find(row.table); links != links_.end())
            for (const auto *foreign : links->second)
                for (std::size_t i = 0; i < foreign->from.size(); ++i)
                    if (foreign->from[i] == column)
                        if (const auto target = referencedRow(*foreign, row.before))
                            if (const auto found = rows_.find(*target); found != rows_.end())
                                return remapCell(*target, found->second, foreign->to[i], depth + 1);
        std::optional<Identity> polymorphic;
        if (row.table == "routing" && (column == "src_id" || column == "dst_id"))
            polymorphic = identityOf(endpointTable(row.before.at(column == "src_id" ? "src_kind" : "dst_kind")),
                                     row.before.at(column));
        else if ((row.table == "automation_lanes" && column == "owner_id") ||
                 (row.table == "controller_maps" && column == "target_id"))
            polymorphic = polymorphicOwner(row);
        if (polymorphic)
            if (const auto id = keys_.find(*polymorphic); id != keys_.end())
                return id->second;
        return row.before.at(column);
    }
    CopyRows rows_;
    CopyKeys keys_;
    ForeignByChild links_;
    std::int64_t originalRoot_;
};
} // namespace
Payload captureTrackForCopy(SQLite::Database &db, std::int64_t root) {
    const Catalog catalog(db);
    Image image(db, catalog);
    image.collect("tracks", root);
    auto capture = image.encode(root);
    auto &rows = capture.at("rows");
    rows.erase(std::remove_if(rows.begin(), rows.end(), pitchExtractionRow), rows.end());
    capture["assets"] = pooledKeys(db, catalog, capture);
    return capture;
}
Payload copyTrackContents(SQLite::Database &db, const Payload &capture, std::int64_t parent, std::int64_t index,
                          std::int64_t retainedDeviceId) {
    const Catalog catalog(db);
    if (capture.at("assets") != pooledKeys(db, catalog, capture))
        throw std::runtime_error("The track's media or plug-in identities have changed since it was copied.");
    return TrackCopyImage(db, catalog, capture, retainedDeviceId).clone(db, catalog, parent, index);
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
    for (const auto& [key, row] : image.rows)
        if (key.first == "tracks" && row.after.is_null() && row.before.at("kind") == "master")
            throw std::runtime_error("The Main track cannot be deleted.");
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
Payload captureDevice(SQLite::Database& db,std::int64_t root) {
    const Catalog catalog(db);
    Image image(db, catalog);
    image.collect("devices", root);
    return image.encode(root);
}
void deleteDevice(SQLite::Database& db,std::int64_t root) {
    const Catalog catalog(db);
    Image image(db, catalog);
    image.collect("devices", root);
    removeImage(db, catalog, image);
}
void restoreDevice(SQLite::Database& db,std::int64_t root,const Payload& packet) {
    restoreImage(db, "devices", root, packet);
}
} // namespace adi::detail
