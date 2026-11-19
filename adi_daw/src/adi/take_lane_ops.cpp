// SPDX-License-Identifier: AGPL-3.0-or-later
#include "adi/take_lane_ops.hpp"
#include "adi/clip_image.hpp"
#include "adi/edit_rows.hpp"
#include "adi/row_capture.hpp"
#include "adi/store.hpp"
#include <SQLiteCpp/SQLiteCpp.h>
#include <algorithm>
#include <set>
#include <stdexcept>

namespace adi {
namespace {
std::int64_t positive(const Payload& value) {
    const auto id=detail::signedInteger(value);
    if(id<=0)throw std::runtime_error("take lanes require positive identities");
    return id;
}
Payload required(SQLite::Database& db,const char* table,std::int64_t id) {
    const auto row=detail::captureRow(db,table,"id",id);
    if(!row)throw std::runtime_error(std::string("missing ")+table+" row");
    return *row;
}
void track(SQLite::Database& db,std::int64_t id) {
    const auto row=required(db,"tracks",id);
    const auto& kind=row.at("kind");
    if(kind!="audio"&&kind!="midi"&&kind!="instrument")
        throw std::runtime_error("take lanes require an audio, MIDI or instrument track");
}
Payload lane(SQLite::Database& db,std::int64_t id) {
    auto row=required(db,"lanes",id);
    if(row.at("kind")!="take")throw std::runtime_error("operation requires a take lane");
    track(db,positive(row.at("track_id")));return row;
}
std::vector<std::int64_t> order(SQLite::Database& db,std::int64_t owner) {
    SQLite::Statement rows(db,"SELECT id FROM lanes WHERE track_id=? AND kind='take' ORDER BY ord,id");rows.bind(1,owner);
    std::vector<std::int64_t> result;while(rows.executeStep())result.push_back(rows.getColumn(0).getInt64());return result;
}
void orderRows(edit::RowEdits& edits,const std::vector<std::int64_t>& ids) {
    std::int64_t ordinal=0;
    for(const auto id:ids){auto row=edits.current("lanes",id);if(!row)throw std::runtime_error("missing reordered lane");(*row)["ord"]=ordinal++;edits.set("lanes",id,std::move(row));}
}
enum class Action {Create,Delete,Reorder,Rename,Mute,SetLane};
Payload plan(OpContext& context,const Payload& payload,Action action) {
    auto& db=context.db;edit::RowEdits edits(db);
    if(action==Action::Create) {
        const auto id=positive(payload.at("id")),owner=positive(payload.at("track"));track(db,owner);
        if(detail::captureRow(db,"lanes","id",id))throw std::runtime_error("new lane identity is occupied");
        auto ids=order(db,owner);ids.insert(ids.begin(),id);
        auto row=payload.at("row");
        if(row.at("id")!=id||row.at("track_id")!=owner||row.at("kind")!="take"||
           detail::signedInteger(row.at("ord"))!=0||detail::signedInteger(row.at("muted"))!=0||
           detail::signedInteger(row.at("is_comp_target"))!=0)
            throw std::runtime_error("new take lane requires matching identities and default take state");
        edits.set("lanes",id,std::move(row));orderRows(edits,ids);
    } else if(action==Action::Reorder) {
        const auto owner=positive(payload.at("track"));track(db,owner);const auto existing=order(db,owner);
        std::vector<std::int64_t> ids;std::set<std::int64_t> seen;
        for(const auto& value:payload.at("lanes")){const auto id=positive(value);if(!seen.insert(id).second)throw std::runtime_error("duplicate reordered lane");ids.push_back(id);}
        if(seen!=std::set<std::int64_t>(existing.begin(),existing.end()))throw std::runtime_error("reorder must name every take lane of exactly one track");
        orderRows(edits,ids);
    } else if(action==Action::SetLane) {
        const auto id=positive(payload.at("id"));auto row=required(db,"clips",id);
        const auto owner=positive(row.at("track_id"));track(db,owner);
        if(!row.at("lane_id").is_null()&&lane(db,positive(row.at("lane_id"))).at("track_id")!=row.at("track_id"))
            throw std::runtime_error("existing clip and take lane ownership differs");
        if(payload.contains("lane")&&!payload.at("lane").is_null()) {
            const auto destination=positive(payload.at("lane"));const auto target=lane(db,destination);
            if(target.at("track_id")!=row.at("track_id"))throw std::runtime_error("clip and take lane belong to different tracks");
            row["lane_id"]=destination;
        }else row["lane_id"]=nullptr;
        edits.set("clips",id,std::move(row));
    } else {
        const auto id=positive(payload.at("id"));auto row=lane(db,id);
        if(action==Action::Rename){row["name"]=payload.at("name");edits.set("lanes",id,std::move(row));}
        else if(action==Action::Mute){row["muted"]=payload.at("on").get<bool>()?1:0;edits.set("lanes",id,std::move(row));}
        else {
            const auto owner=positive(row.at("track_id"));
            std::vector<Payload> images;std::set<std::int64_t> removed;
            SQLite::Statement clips(db,"SELECT id FROM clips WHERE lane_id=? ORDER BY id");clips.bind(1,id);
            while(clips.executeStep()) {
                const auto clip=clips.getColumn(0).getInt64();auto image=clip_edit::captureImage(context,clip);
                if(image.at("row").at("track_id")!=owner)throw std::runtime_error("lane contains a clip belonging to another track");
                removed.insert(clip);images.push_back(std::move(image));
            }
            // Internal aliases disappear together. Keep their original fields in
            // the captures, but do not re-edit a root already marked for deletion.
            for(auto& image:images) {
                auto& links=image["alias_links"];
                links.erase(std::remove_if(links.begin(),links.end(),[&](const Payload& value){return removed.contains(detail::signedInteger(value));}),links.end());
                clip_edit::deleteImage(edits,image);
            }
            edits.set("lanes",id,std::nullopt);
            auto ids=order(db,owner);ids.erase(std::remove(ids.begin(),ids.end(),id),ids.end());orderRows(edits,ids);
        }
    }
    const auto changes=edits.changes();
    if(changes.empty())throw std::runtime_error("take-lane operation makes no change");
    return {{"changes",changes}};
}
void allowedState(OpContext& context,const Payload& payload) {
    const auto& changes=payload.at("changes");
    if(!changes.is_array())throw std::runtime_error("lane restoration needs captured row changes");
    auto state=[&](const char* table,std::int64_t id,const char* side)->std::optional<Payload> {
        for(const auto& change:changes)if(change.at("table")==table&&detail::signedInteger(change.at("id"))==id)
            return change.at(side).is_null()?std::nullopt:std::optional<Payload>(change.at(side));
        return detail::captureRow(context.db,table,"id",id);
    };
    for(const auto& change:changes) {
        const auto table=change.at("table").get<std::string>();
        const bool child=std::any_of(clip_edit::childTables().begin(),clip_edit::childTables().end(),[&](const auto& item){return table==item.table;});
        if(table!="lanes"&&table!="clips"&&!child)throw std::runtime_error("unsupported take-lane restore table");
        // Both directions must be admissible. A seemingly useful implicit
        // repair is refused if its captured inverse would recreate corruption.
        for(const auto* side:{"before","after"}) {
            const auto& row=change.at(side);if(row.is_null())continue;
            if(table=="lanes") {
                track(context.db,positive(row.at("track_id")));
                if(row.at("kind")!="take")throw std::runtime_error("restoration requires a take lane");
                const auto& other=change.at(std::string_view(side)=="before"?"after":"before");
                if(!other.is_null()&&other.at("track_id")!=row.at("track_id"))throw std::runtime_error("lane restoration cannot move a lane across tracks");
            }else if(table=="clips") {
                track(context.db,positive(row.at("track_id")));
                if(!row.at("lane_id").is_null()) {
                    const auto target=state("lanes",positive(row.at("lane_id")),side);
                    if(!target||target->at("track_id")!=row.at("track_id"))throw std::runtime_error("restored clip and lane ownership differs");
                    if(target->at("kind")!="take")throw std::runtime_error("restored clip requires a take lane or main");
                }
            }
        }
    }
}
bool restore(OpContext& context,const Payload& payload,std::string& error) {
    try {allowedState(context,payload);return edit::applyRows(context,payload,error);}
    catch(const std::exception& exception){error=exception.what();return false;}
}
template<Action action> bool apply(OpContext& context,const Payload& payload,std::string& error) {
    try {return restore(context,plan(context,payload,action),error);}
    catch(const std::exception& exception){error=exception.what();return false;}
}
template<Action action> bool capture(OpContext& context,const Payload& payload,Payload& inverse,std::string& error) {
    try {
        const auto forward=plan(context,payload,action);allowedState(context,forward);
        if(!edit::validateRows(context,forward,error))return false;
        return edit::inverseRows(context,forward,inverse,error);
    }catch(const std::exception& exception){error=exception.what();return false;}
}
constexpr Field idFields[]={{"id",FieldType::Int}};
constexpr Field createFields[]={{"id",FieldType::Int},{"track",FieldType::Int},{"row",FieldType::Object}};
constexpr Field orderFields[]={{"track",FieldType::Int},{"lanes",FieldType::Array}};
constexpr Field nameFields[]={{"id",FieldType::Int},{"name",FieldType::Text}};
constexpr Field muteFields[]={{"id",FieldType::Int},{"on",FieldType::Bool}};
constexpr Field clipFields[]={{"id",FieldType::Int},{"lane",FieldType::Int,false}};
constexpr Field restoreFields[]={{"changes",FieldType::Array}};
const OpDescriptor ops[]={
    {"lane.create","Insert a take lane at the top",Scope::Edit,EngineImpact::Snapshot,createFields,false,false,apply<Action::Create>,capture<Action::Create>,"lane.restore"},
    {"lane.delete","Delete a take lane and its material",Scope::Edit,EngineImpact::Snapshot,idFields,false,false,apply<Action::Delete>,capture<Action::Delete>,"lane.restore"},
    {"lane.reorder","Order every take lane of one track",Scope::Edit,EngineImpact::Snapshot,orderFields,false,false,apply<Action::Reorder>,capture<Action::Reorder>,"lane.restore"},
    {"lane.rename","Rename a take lane",Scope::Edit,EngineImpact::None,nameFields,false,false,apply<Action::Rename>,capture<Action::Rename>,"lane.restore"},
    {"lane.setMute","Set stored take-lane mute",Scope::Edit,EngineImpact::Snapshot,muteFields,false,false,apply<Action::Mute>,capture<Action::Mute>,"lane.restore"},
    {"clip.setLane","Attach a clip to its track's take lane or main",Scope::Edit,EngineImpact::Snapshot,clipFields,false,false,apply<Action::SetLane>,capture<Action::SetLane>,"lane.restore"},
    {"lane.restore","Restore captured take-lane material",Scope::Edit,EngineImpact::Snapshot,restoreFields,false,false,restore,edit::inverseRows,""},
};
} // namespace
std::span<const OpDescriptor> takeLaneOps(){return ops;}
OpRequest createTakeLane(Store& store,std::int64_t id,std::int64_t owner,std::string name) {
    OpRequest result;result.opType="lane.create";
    result.payload={{"id",id},{"track",owner},{"row",edit::defaultRow(store.db(),"lanes",
        {{"id",id},{"track_id",owner},{"ord",0},{"kind","take"},{"name",std::move(name)},{"muted",0},{"is_comp_target",0}})}};
    return result;
}
} // namespace adi
