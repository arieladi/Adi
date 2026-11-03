// SPDX-License-Identifier: AGPL-3.0-or-later
#include "adi/edit_rows.hpp"
#include "adi/row_capture.hpp"
#include <SQLiteCpp/SQLiteCpp.h>
#include <algorithm>
#include <array>
#include <cctype>
#include <limits>
#include <set>
#include <vector>
#include <stdexcept>

namespace adi::edit {
namespace {
constexpr std::array<RowSpec,19> specs={{
    {"clips","id"},{"event_streams","id"},{"note_expression","id"},{"audio_clips","clip_id"},
    {"automation_data","id"},{"automation_segment_ranges","id"},{"clip_playback_details","clip_id"},{"clip_warp_fade_domains","clip_id"},{"clip_properties","clip_id"},
    {"tempo_map","id"},{"time_signature_map","id"},{"key_map","id"},{"markers","id"},
    {"marker_details","marker_id"},{"project","id"},{"arranger_sections","id"},
    {"key_map_tunings","key_map_id"},{"key_map_degrees","key_map_id","degree"},{"arranger_chain","id"}
}};
std::string sqlName(const std::string& value) {
    std::string result="\"";for(char c:value){result+=c;if(c=='"')result+=c;}return result+'"';
}
std::string lower(std::string value) {
    std::transform(value.begin(),value.end(),value.begin(),[](unsigned char c){return static_cast<char>(std::tolower(c));});return value;
}
void bind(SQLite::Statement& st,int index,const Payload& value) {
    if(value.is_null())st.bind(index);
    else if(value.is_number_integer())st.bind(index,detail::signedInteger(value));
    else if(value.is_number_float())st.bind(index,value.get<double>());
    else if(value.is_string())st.bind(index,value.get<std::string>());
    else if(value.is_binary()) {
        const auto& bytes=value.get_binary();static const char empty=0;
        if(bytes.size()>static_cast<std::size_t>(std::numeric_limits<int>::max()))throw std::runtime_error("captured key is too large");
        st.bind(index,bytes.empty()?static_cast<const void*>(&empty):static_cast<const void*>(bytes.data()),static_cast<int>(bytes.size()));
    } else throw std::runtime_error("invalid captured SQLite key");
}
void storedRow(SQLite::Database& db,const RowSpec& spec,const Payload& value) {
    if(value.is_null())return;
    std::size_t count=0;
    SQLite::Statement columns(db,"SELECT name,type,hidden FROM pragma_table_xinfo(?)");
    columns.bind(1,spec.table);
    while(columns.executeStep()) {
        const auto name=columns.getColumn(0).getString(),type=lower(columns.getColumn(1).getString());
        if(columns.getColumn(2).getInt()!=0)throw std::runtime_error("captured edits do not support generated columns in "+std::string(spec.table));
        ++count;const auto& cell=value.at(name);
        if(cell.is_null())continue;
        const bool valid=type=="integer"?cell.is_number_integer():
            type=="real"?cell.is_number():type=="text"?cell.is_string():
            type=="blob"?cell.is_binary():
            (cell.is_number() || cell.is_string() || cell.is_binary());
        if(!valid)throw std::runtime_error("captured SQLite type differs for "+std::string(spec.table)+"."+name);
        if(cell.is_number_integer())(void)detail::signedInteger(cell);
    }
    if(value.size()!=count)throw std::runtime_error("captured row columns differ from "+std::string(spec.table));
}
Payload checkedKey(const RowSpec& spec,const Payload& key) {
    if(!key.is_object() || key.size()!=(spec.key2?2u:1u) || !key.contains(spec.key) || (spec.key2 && !key.contains(spec.key2)))throw std::runtime_error("captured row key fields differ");
    Payload result={{spec.key,detail::signedInteger(key.at(spec.key))}};
    if(spec.key2)result[spec.key2]=detail::signedInteger(key.at(spec.key2));
    return result;
}
Payload keyOf(const RowSpec& spec,const Payload& row) {
    Payload key={{spec.key,row.at(spec.key)}};if(spec.key2)key[spec.key2]=row.at(spec.key2);return checkedKey(spec,key);
}
using Key=std::pair<std::string,std::string>;
struct Change {const RowSpec* spec=nullptr;std::int64_t id=0;Payload key,before,after;};
std::vector<Change> decode(SQLite::Database& db,const Payload& changes) {
    if(!changes.is_array() || changes.empty())throw std::runtime_error("edit needs captured row changes");
    std::set<Key> seen;std::vector<Change> result;result.reserve(changes.size());
    for(const auto& item:changes) {
        const auto table=item.at("table").get<std::string>();
        const auto& spec=rowSpec(table);
        const auto key=spec.key2?checkedKey(spec,item.at("id")):checkedKey(spec,Payload{{spec.key,item.at("id")}});
        const auto id=detail::signedInteger(key.at(spec.key));
        if(!seen.insert({spec.table,key.dump()}).second)throw std::runtime_error("duplicate captured row");
        if(std::string(spec.table)=="project" && id!=1)throw std::runtime_error("time edit can only address the project singleton");
        const auto& before=item.at("before");const auto& after=item.at("after");
        if(before.is_null() && after.is_null())throw std::runtime_error("captured row has no state");
        for(const auto* value:{&before,&after})
            if(!value->is_null() && (!value->is_object() || keyOf(spec,*value)!=key))
                throw std::runtime_error("captured row identity differs");
        storedRow(db,spec,before);storedRow(db,spec,after);
        const auto current=rowKey(db,spec.table,key);
        if((!current && !before.is_null()) || (current && *current!=before))
            throw std::runtime_error(std::string(spec.table)+" changed after edit preparation");
        if(std::string(spec.table)=="project") {
            if(before.is_null() || after.is_null() || before.size()!=after.size())
                throw std::runtime_error("time edit must retain the project row");
            for(auto it=before.begin();it!=before.end();++it)
                if(it.key()!="length_ticks" && after.at(it.key())!=it.value())
                    throw std::runtime_error("time edit can only change project length");
        }
        result.push_back({&spec,id,key,before,after});
    }
    return result;
}
void guardReferences(SQLite::Database& db,const std::vector<Change>& changes) {
    std::map<Key,const Change*> byKey;
    for(const auto& change:changes)byKey[{change.spec->table,change.key.dump()}]=&change;
    std::vector<std::string> tables;
    SQLite::Statement list(db,"SELECT name FROM sqlite_master WHERE type='table' AND name NOT LIKE 'sqlite_%' ORDER BY name");
    while(list.executeStep())tables.push_back(list.getColumn(0).getString());
    struct Foreign {std::string parent,action;std::vector<std::string> from,to;};
    for(const auto& removed:changes) {
        if(!removed.after.is_null())continue;
        for(const auto& table:tables) {
            std::map<int,Foreign> foreign;
            SQLite::Statement keys(db,"SELECT id,\"table\",\"from\",\"to\",on_delete FROM pragma_foreign_key_list(?) ORDER BY id,seq");keys.bind(1,table);
            while(keys.executeStep()) {
                auto& key=foreign[keys.getColumn(0).getInt()];key.parent=lower(keys.getColumn(1).getString());key.action=keys.getColumn(4).getString();
                key.from.push_back(keys.getColumn(2).getString());key.to.push_back(keys.getColumn(3).isNull()?"":keys.getColumn(3).getString());
            }
            for(auto& [number,key]:foreign) {
                (void)number;if(key.parent!=removed.spec->table || (key.action!="CASCADE" && key.action!="SET NULL" && key.action!="SET DEFAULT"))continue;
                if(std::all_of(key.to.begin(),key.to.end(),[](const auto& column){return column.empty();})) {
                    key.to={removed.spec->key};if(removed.spec->key2)key.to.push_back(removed.spec->key2);
                }
                if(key.from.size()!=key.to.size())throw std::runtime_error("foreign key arity differs from captured parent");
                std::string where;Payload values=Payload::array();bool nullable=false;
                for(std::size_t field=0;field<key.from.size();++field) {
                    const auto& value=removed.before.at(key.to[field]);if(value.is_null()){nullable=true;break;}
                    if(!where.empty())where+=" AND ";
                    where+=sqlName(key.from[field])+"=?";values.push_back(value);
                }
                if(nullable)continue;
                SQLite::Statement references(db,"SELECT count(*) FROM "+sqlName(table)+" WHERE "+where);int index=0;for(const auto& value:values)bind(references,++index,value);
                if(!references.executeStep() || references.getColumn(0).getInt64()==0)continue;
                const auto normalized=lower(table);const auto& child=rowSpec(normalized); // Unknown effects refuse before a cascade.
                const auto columns=sqlName(child.key)+(child.key2?","+sqlName(child.key2):"");
                SQLite::Statement ids(db,"SELECT "+columns+" FROM "+sqlName(table)+" WHERE "+where);index=0;for(const auto& value:values)bind(ids,++index,value);
                while(ids.executeStep()) {
                    Payload childKey={{child.key,ids.getColumn(0).getInt64()}};if(child.key2)childKey[child.key2]=ids.getColumn(1).getInt64();
                    const auto found=byKey.find({child.table,childKey.dump()});bool stillReferences=true;
                    if(found!=byKey.end() && !found->second->after.is_null())for(std::size_t field=0;field<key.from.size();++field)
                        if(found->second->after.at(key.from[field])!=values[field])stillReferences=false;
                    if(found==byKey.end() || (!found->second->after.is_null() && stillReferences))throw std::runtime_error(table+" has an uncaptured foreign-key effect");
                }
            }
        }
    }
}
void erase(SQLite::Database& db,const Change& change) {
    (void)detail::eraseRowKey(db,change.spec->table,change.key);
    // A captured parent may already have cascaded this captured child. Every
    // before image existed at validation, and guardReferences allowed only
    // effects explicitly included as deletions in this same packet.
}
}
const RowSpec& rowSpec(const std::string& table) {
    for(const auto& spec:specs)if(table==spec.table)return spec;
    throw std::runtime_error("unsupported time-edit table '"+table+"'");
}
std::optional<Payload> row(SQLite::Database& db,const std::string& table,std::int64_t id) {
    const auto& spec=rowSpec(table);if(spec.key2)throw std::runtime_error("composite row needs both key fields");return rowKey(db,table,Payload{{spec.key,id}});
}
std::optional<Payload> rowKey(SQLite::Database& db,const std::string& table,const Payload& key) {
    const auto& spec=rowSpec(table);return detail::captureRowKey(db,spec.table,checkedKey(spec,key));
}
Payload defaultRow(SQLite::Database& db,const std::string& table,const Payload& values) {
    const auto& spec=rowSpec(table);
    if(!values.is_object() || !values.contains(spec.key) || (spec.key2 && !values.contains(spec.key2)))throw std::runtime_error("new row needs its captured identity");
    SQLite::Statement schema(db,"SELECT sql FROM sqlite_master WHERE type='table' AND name=? "
        "UNION ALL SELECT sql FROM sqlite_temp_master WHERE type='table' AND name=?");
    schema.bind(1,table);schema.bind(2,table);
    if(!schema.executeStep())throw std::runtime_error("new row needs a native table definition");
    SQLite::Database scratch(":memory:",SQLite::OPEN_READWRITE|SQLite::OPEN_CREATE);
    scratch.exec("PRAGMA foreign_keys=OFF");scratch.exec(schema.getColumn(0).getString());
    detail::restoreRow(scratch,table,values);
    auto result=*rowKey(scratch,table,keyOf(spec,values));
    storedRow(db,spec,result);return result;
}
bool validateRows(OpContext& context,const Payload& payload,std::string& error) {
    try {const auto changes=decode(context.db,payload.at("changes"));guardReferences(context.db,changes);return true;}
    catch(const std::exception& exception){error=exception.what();return false;}
}
bool applyRows(OpContext& context,const Payload& payload,std::string& error) {
    try {
        const auto changes=decode(context.db,payload.at("changes"));guardReferences(context.db,changes);
        // Insert every root with links temporarily NULL, so a captured alias
        // forest (even a legacy cycle) need not depend on insertion order.
        for(const auto& change:changes)if(change.before.is_null() && std::string(change.spec->table)=="clips") {
            auto root=change.after;root["alias_of"]=nullptr;detail::restoreRow(context.db,"clips",root);
        }
        auto timeMap=[](const Change& change) {
            const std::string table=change.spec->table;
            return table=="tempo_map" || table=="time_signature_map" || table=="key_map" || table=="automation_segment_ranges";
        };
        auto coordinate=[](const Change& change) {
            return std::string(change.spec->table)=="automation_segment_ranges"?"record_index":"pos_ticks";
        };
        auto partition=[](const Change& change) {
            return std::string(change.spec->table)=="automation_segment_ranges"?
                std::string(change.spec->table)+":"+std::to_string(detail::signedInteger(change.before.at("data_id"))):
                std::string(change.spec->table);
        };
        // Free removed map positions before moving surviving UNIQUE positions.
        for(const auto& change:changes)if(change.after.is_null() && timeMap(change))erase(context.db,change);
        std::vector<const Change*> updates;
        std::map<std::string,int> direction;
        for(const auto& change:changes)if(!change.after.is_null() && !change.before.is_null()) {
            updates.push_back(&change);
            if(timeMap(change)) {
                const auto before=detail::signedInteger(change.before.at(coordinate(change)));
                const auto after=detail::signedInteger(change.after.at(coordinate(change)));
                const int sign=after>before?1:(after<before?-1:0);
                if(sign) {
                    auto& old=direction[partition(change)];
                    if(old && old!=sign)throw std::runtime_error("time-map moves must preserve their order");
                    old=sign;
                }
            }
        }
        std::sort(updates.begin(),updates.end(),[&](const Change* a,const Change* b) {
            const std::string table=a->spec->table,other=b->spec->table;
            if(table!=other)return table<other;
            if(timeMap(*a)) {
                const auto group=partition(*a),otherGroup=partition(*b);
                if(group!=otherGroup)return group<otherGroup;
                const auto left=detail::signedInteger(a->before.at(coordinate(*a)));
                const auto right=detail::signedInteger(b->before.at(coordinate(*b)));
                return direction[group]>0?left>right:left<right;
            }
            return a->id<b->id;
        });
        for(const auto* change:updates)detail::updateRowKey(context.db,change->spec->table,change->after,change->key);
        auto root=[](const Change& change) {
            const std::string table=change.spec->table;
            return table=="arranger_sections" || table=="markers" || table=="tempo_map" || table=="time_signature_map" || table=="key_map" || table=="automation_data";
        };
        for(const auto& change:changes)
            if(change.before.is_null() && root(change))detail::restoreRow(context.db,change.spec->table,change.after);
        // A tuning binding is a parent of its composite-key degree overrides.
        for(const auto& change:changes)if(change.before.is_null() && std::string(change.spec->table)=="key_map_tunings")detail::restoreRow(context.db,change.spec->table,change.after);
        for(const auto& change:changes)if(change.before.is_null()) {
            if(std::string(change.spec->table)=="key_map_tunings")continue;
            if(std::string(change.spec->table)=="clips")detail::restoreRow(context.db,"clips",change.after,"id");
            else if(!root(change))detail::restoreRow(context.db,change.spec->table,change.after);
        }
        // Children first. Every cascading effect was checked against the same
        // captured transaction state; no REPLACE can destroy a child implicitly.
        for(const auto& change:changes)
            if(change.after.is_null() && !timeMap(change) && std::string(change.spec->table)!="clips")erase(context.db,change);
        for(const auto& change:changes)if(change.after.is_null() && std::string(change.spec->table)=="clips")erase(context.db,change);
        return true;
    } catch(const std::exception& exception){error=exception.what();return false;}
}
bool inverseRows(OpContext&,const Payload& payload,Payload& inverse,std::string& error) {
    try {
        inverse=payload;
        auto& changes=inverse.at("changes");
        if(!changes.is_array() || changes.empty())throw std::runtime_error("edit needs row changes");
        std::reverse(changes.begin(),changes.end());
        for(auto& change:changes)std::swap(change.at("before"),change.at("after"));
        return true;
    } catch(const std::exception& exception){error=exception.what();return false;}
}
std::optional<Payload> RowEdits::current(const std::string& table,std::int64_t id) {
    const auto& spec=rowSpec(table);if(spec.key2)throw std::runtime_error("composite row needs both key fields");return currentKey(table,Payload{{spec.key,id}});
}
std::optional<Payload> RowEdits::currentKey(const std::string& table,const Payload& key) {
    const auto canonical=checkedKey(rowSpec(table),key);const auto found=rows_.find({table,canonical.dump()});return found==rows_.end()?rowKey(db_,table,canonical):found->second.after;
}
void RowEdits::set(const std::string& table,std::int64_t id,std::optional<Payload> value) {
    const auto& spec=rowSpec(table);if(spec.key2)throw std::runtime_error("composite row needs both key fields");setKey(table,Payload{{spec.key,id}},std::move(value));
}
void RowEdits::setKey(const std::string& table,const Payload& key,std::optional<Payload> value) {
    const auto canonical=checkedKey(rowSpec(table),key);const auto identity=std::make_pair(table,canonical.dump());auto found=rows_.find(identity);
    if(found==rows_.end()) {
        auto before=rowKey(db_,table,canonical);rows_.emplace(identity,Change{canonical,before,std::move(value)});
    }else found->second.after=std::move(value);
}
Payload RowEdits::changes()const {
    Payload result=Payload::array();
    for(const auto& [identity,value]:rows_)if(value.before!=value.after) {
        const auto& spec=rowSpec(identity.first);const auto id=spec.key2?value.key:value.key.at(spec.key);
        result.push_back({{"table",identity.first},{"id",id},{"before",value.before.value_or(Payload{})},{"after",value.after.value_or(Payload{})}});
    }
    return result;
}
} // namespace adi::edit
