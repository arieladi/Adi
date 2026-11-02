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
constexpr std::array<RowSpec,15> specs={{
    {"clips","id"},{"event_streams","id"},{"note_expression","id"},{"audio_clips","clip_id"},
    {"automation_data","id"},{"automation_segment_ranges","id"},{"clip_playback_details","clip_id"},{"clip_properties","clip_id"},
    {"tempo_map","id"},{"time_signature_map","id"},{"key_map","id"},{"markers","id"},
    {"marker_details","marker_id"},{"project","id"},{"arranger_sections","id"}
}};
std::string quoted(const std::string& value) {
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
using Key=std::pair<std::string,std::int64_t>;
struct Change {const RowSpec* spec=nullptr;std::int64_t id=0;Payload before,after;};
std::vector<Change> decode(SQLite::Database& db,const Payload& changes) {
    if(!changes.is_array() || changes.empty())throw std::runtime_error("edit needs captured row changes");
    std::set<Key> seen;std::vector<Change> result;result.reserve(changes.size());
    for(const auto& item:changes) {
        const auto table=item.at("table").get<std::string>();
        const auto& spec=rowSpec(table);const auto id=detail::signedInteger(item.at("id"));
        if(!seen.insert({spec.table,id}).second)throw std::runtime_error("duplicate captured row");
        if(std::string(spec.table)=="project" && id!=1)throw std::runtime_error("time edit can only address the project singleton");
        const auto& before=item.at("before");const auto& after=item.at("after");
        if(before.is_null() && after.is_null())throw std::runtime_error("captured row has no state");
        for(const auto* value:{&before,&after})
            if(!value->is_null() && (!value->is_object() || detail::signedInteger(value->at(spec.key))!=id))
                throw std::runtime_error("captured row identity differs");
        storedRow(db,spec,before);storedRow(db,spec,after);
        const auto current=row(db,spec.table,id);
        if((!current && !before.is_null()) || (current && *current!=before))
            throw std::runtime_error(std::string(spec.table)+" changed after edit preparation");
        if(std::string(spec.table)=="project") {
            if(before.is_null() || after.is_null() || before.size()!=after.size())
                throw std::runtime_error("time edit must retain the project row");
            for(auto it=before.begin();it!=before.end();++it)
                if(it.key()!="length_ticks" && after.at(it.key())!=it.value())
                    throw std::runtime_error("time edit can only change project length");
        }
        result.push_back({&spec,id,before,after});
    }
    return result;
}
void guardReferences(SQLite::Database& db,const std::vector<Change>& changes) {
    std::map<Key,const Change*> byKey;
    for(const auto& change:changes)byKey[{change.spec->table,change.id}]=&change;
    std::vector<std::string> tables;
    SQLite::Statement list(db,"SELECT name FROM sqlite_master WHERE type='table' AND name NOT LIKE 'sqlite_%' ORDER BY name");
    while(list.executeStep())tables.push_back(list.getColumn(0).getString());
    for(const auto& removed:changes) {
        if(!removed.after.is_null())continue;
        for(const auto& table:tables) {
            SQLite::Statement keys(db,"SELECT \"table\",\"from\",\"to\",on_delete FROM pragma_foreign_key_list(?)");
            keys.bind(1,table);
            while(keys.executeStep()) {
                if(lower(keys.getColumn(0).getString())!=removed.spec->table)continue;
                const auto action=keys.getColumn(3).getString();
                if(action!="CASCADE" && action!="SET NULL" && action!="SET DEFAULT")continue;
                const auto from=keys.getColumn(1).getString();
                auto to=keys.getColumn(2).getString();if(to.empty())to=removed.spec->key;
                const auto& parentValue=removed.before.at(to);
                SQLite::Statement references(db,"SELECT count(*) FROM "+quoted(table)+" WHERE "+quoted(from)+"=?");
                bind(references,1,parentValue);
                if(!references.executeStep() || references.getColumn(0).getInt64()==0)continue;
                const auto normalized=lower(table);
                const auto& child=rowSpec(normalized); // unknown child refuses before a cascade
                SQLite::Statement ids(db,"SELECT "+quoted(child.key)+" FROM "+quoted(table)+" WHERE "+quoted(from)+"=?");
                bind(ids,1,parentValue);
                while(ids.executeStep()) {
                    const auto found=byKey.find({child.table,ids.getColumn(0).getInt64()});
                    if(found==byKey.end() || (!found->second->after.is_null() && found->second->after.at(from)==parentValue))
                        throw std::runtime_error(table+" has an uncaptured foreign-key effect");
                }
            }
        }
    }
}
void erase(SQLite::Database& db,const Change& change) {
    SQLite::Statement st(db,"DELETE FROM "+quoted(change.spec->table)+" WHERE "+quoted(change.spec->key)+"=?");
    st.bind(1,change.id);st.exec();
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
    const auto& spec=rowSpec(table);return detail::captureRow(db,spec.table,spec.key,id);
}
Payload defaultRow(SQLite::Database& db,const std::string& table,const Payload& values) {
    const auto& spec=rowSpec(table);
    if(!values.is_object() || !values.contains(spec.key))throw std::runtime_error("new row needs its captured identity");
    SQLite::Statement schema(db,"SELECT sql FROM sqlite_master WHERE type='table' AND name=? "
        "UNION ALL SELECT sql FROM sqlite_temp_master WHERE type='table' AND name=?");
    schema.bind(1,table);schema.bind(2,table);
    if(!schema.executeStep())throw std::runtime_error("new row needs a native table definition");
    SQLite::Database scratch(":memory:",SQLite::OPEN_READWRITE|SQLite::OPEN_CREATE);
    scratch.exec("PRAGMA foreign_keys=OFF");scratch.exec(schema.getColumn(0).getString());
    detail::restoreRow(scratch,table,values);
    auto result=*detail::captureRow(scratch,table,spec.key,detail::signedInteger(values.at(spec.key)));
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
        for(const auto* change:updates)detail::restoreRow(context.db,change->spec->table,change->after,change->spec->key);
        auto root=[](const Change& change) {
            const std::string table=change.spec->table;
            return table=="markers" || table=="tempo_map" || table=="time_signature_map" || table=="key_map" || table=="automation_data";
        };
        for(const auto& change:changes)
            if(change.before.is_null() && root(change))detail::restoreRow(context.db,change.spec->table,change.after);
        for(const auto& change:changes)if(change.before.is_null()) {
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
    const auto found=rows_.find({table,id});return found==rows_.end()?row(db_,table,id):found->second.after;
}
void RowEdits::set(const std::string& table,std::int64_t id,std::optional<Payload> value) {
    const auto key=std::make_pair(table,id);auto found=rows_.find(key);
    if(found==rows_.end()) {
        auto before=row(db_,table,id);rows_.emplace(key,Change{before,std::move(value)});
    } else found->second.after=std::move(value);
}
Payload RowEdits::changes()const {
    Payload result=Payload::array();
    for(const auto& [key,value]:rows_)if(value.before!=value.after)
        result.push_back({{"table",key.first},{"id",key.second},
            {"before",value.before.value_or(Payload{})},{"after",value.after.value_or(Payload{})}});
    return result;
}
} // namespace adi::edit
