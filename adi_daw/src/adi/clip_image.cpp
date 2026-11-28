// SPDX-License-Identifier: AGPL-3.0-or-later
#include "adi/clip_image.hpp"
#include "adi/row_capture.hpp"
#include <SQLiteCpp/SQLiteCpp.h>
#include <algorithm>
#include <array>
#include <limits>
#include <stdexcept>

namespace adi::clip_edit {
namespace {
constexpr char pitchExtractions[]="clip_pitch_extractions";
constexpr std::array<ChildTable,10> children={{
    {"event_streams","id"},{"note_expression","id"},{"audio_clips","clip_id"},
    {"automation_data","id"},{"automation_segment_ranges","id"},{"clip_properties","clip_id"},{"clip_playback_details","clip_id"},{"clip_warp_fade_domains","clip_id"},
    {"clip_pitch_edits","clip_id"},{pitchExtractions,"clip_id"}
}};
}
std::span<const ChildTable> childTables(){return children;}
bool tablePresent(SQLite::Database& db,const std::string& table) {
    SQLite::Statement st(db,"SELECT 1 FROM sqlite_master WHERE type IN ('table','view') AND name=? "
        "UNION ALL SELECT 1 FROM sqlite_temp_master WHERE type IN ('table','view') AND name=?");
    st.bind(1,table);st.bind(2,table);return st.executeStep();
}
void createImage(edit::RowEdits& plan,const Payload& image) {
    const auto id=detail::signedInteger(image.at("id"));
    if(detail::signedInteger(image.at("row").at("id"))!=id || plan.current("clips",id))
        throw std::runtime_error("new clip id is occupied or differs from its image");
    plan.set("clips",id,image.at("row"));
    for(const auto& child:children) {
        if(!image.at("children").contains(child.table))continue;
        for(const auto& row:image.at("children").at(child.table)) {
            if(detail::signedInteger(row.at("clip_id"))!=id)throw std::runtime_error("child belongs to a different clip");
            const auto key=detail::signedInteger(row.at(child.key));
            if(plan.current(child.table,key))throw std::runtime_error("new clip child id is occupied");
            plan.set(child.table,key,row);
        }
    }
    if(image.contains("alias_links"))
        for(const auto& link:image.at("alias_links")) {
            auto alias=plan.current("clips",detail::signedInteger(link));
            if(!alias)throw std::runtime_error("captured alias is missing");
            if(!alias->at("alias_of").is_null() && detail::signedInteger(alias->at("alias_of"))!=id)
                throw std::runtime_error("captured alias changed after deletion");
            (*alias)["alias_of"]=id;plan.set("clips",detail::signedInteger(link),alias);
        }
}
void deleteImage(edit::RowEdits& plan,const Payload& image) {
    const auto id=detail::signedInteger(image.at("id"));
    for(const auto& child:children) {
        if(!image.at("children").contains(child.table))continue;
        for(const auto& row:image.at("children").at(child.table))
            plan.set(child.table,detail::signedInteger(row.at(child.key)),std::nullopt);
    }
    if(image.contains("alias_links"))
        for(const auto& link:image.at("alias_links")) {
            const auto key=detail::signedInteger(link);if(key==id)continue;
            auto alias=plan.current("clips",key);if(!alias)throw std::runtime_error("captured alias is missing");
            (*alias)["alias_of"]=nullptr;plan.set("clips",key,alias);
        }
    plan.set("clips",id,std::nullopt);
}
Payload captureImage(OpContext& context,std::int64_t id) {
    const auto root=detail::captureRow(context.db,"clips","id",id);
    if(!root)throw std::runtime_error("no such clip");
    Payload result={{"id",id},{"row",*root},{"children",Payload::object()},{"alias_links",Payload::array()}};
    for(const auto& child:children) {
        const bool present=tablePresent(context.db,child.table);
        const std::string_view table=child.table;
        if(!present && (table=="clip_pitch_edits" || table==pitchExtractions))continue;
        auto& records=result["children"][child.table];records=Payload::array();
        if(!present)continue;
        SQLite::Statement st(context.db,"SELECT "+std::string(child.key)+" FROM "+child.table+" WHERE clip_id=? ORDER BY "+child.key);
        st.bind(1,id);
        while(st.executeStep())records.push_back(*detail::captureRow(context.db,child.table,child.key,st.getColumn(0).getInt64()));
    }
    SQLite::Statement aliases(context.db,"SELECT id FROM clips WHERE alias_of=? ORDER BY id");aliases.bind(1,id);
    while(aliases.executeStep())result["alias_links"].push_back(aliases.getColumn(0).getInt64());
    return result;
}
Payload captureDeleted(OpContext& context,std::int64_t id) {
    auto result=captureImage(context,id);
    if (tablePresent(context.db, "clip_slots")) {
        SQLite::Statement slot(context.db, "SELECT * FROM clip_slots WHERE clip_id=?");
        slot.bind(1, id);
        if (slot.executeStep()) {
            result["session_slot"] = detail::captureQueryRow(slot);
            if (const auto settings = detail::captureRow(context.db, "clip_launch", "clip_id", id))
                result["session_launch"] = *settings;
        }
    }
    // A slot owns its clip's launch rows. The delete handler explicitly
    // detaches those rows before applying the normal Arrangement row patch.
    if (!result.contains("session_slot")) {
        edit::RowEdits removal(context.db);deleteImage(removal,result);std::string error;
        if(!edit::validateRows(context,{{"changes",removal.changes()}},error))throw std::runtime_error(error);
    }
    return result;
}
void restoreDeleted(OpContext& context,const Payload& image) {
    edit::RowEdits insertion(context.db);createImage(insertion,image);std::string error;
    if(!edit::applyRows(context,{{"changes",insertion.changes()}},error))throw std::runtime_error(error);
    if (const auto slot = image.find("session_slot"); slot != image.end()) {
        const Payload key{{"track_id", slot->at("track_id")}, {"scene_id", slot->at("scene_id")}};
        const auto current = detail::captureRowKey(context.db, "clip_slots", key);
        auto expected = *slot;
        expected["clip_id"] = nullptr;
        if (current != expected || slot->at("clip_id") != image.at("id"))
            throw std::runtime_error("the Session slot changed after the clip was deleted");
        detail::updateRowKey(context.db, "clip_slots", *slot, key);
        if (const auto settings = image.find("session_launch"); settings != image.end()) {
            if (settings->at("clip_id") != image.at("id"))
                throw std::runtime_error("captured Session settings belong to a different clip");
            detail::restoreRow(context.db, "clip_launch", *settings);
        }
    }
}
void CopyIds::reserve(const std::string& table,std::int64_t id) {
    const auto& spec=edit::rowSpec(table);
    if(std::string(spec.key)!="id")throw std::runtime_error("child identity is its clip identity");
    ids_.reserve(table,id);
}
std::int64_t CopyIds::next(const std::string& table) {
    reserve(table,0);return ids_.next(table);
}

void detachPitchExtraction(Payload& image) {
    image.at("children").erase(pitchExtractions);
}
Payload cloneImage(const Payload& source,std::int64_t id,CopyIds& ids,ClonePurpose purpose) {
    if(id==detail::signedInteger(source.at("id")))throw std::runtime_error("a copy needs a new clip identity");
    ids.reserve("clips",id);
    auto result=source;result["id"]=id;result["row"]["id"]=id;result["alias_links"]=Payload::array();
    if(purpose==ClonePurpose::Independent)detachPitchExtraction(result);
    std::map<std::int64_t,std::int64_t> automationIds;
    for(const auto& child:children) {
        if(!result.at("children").contains(child.table))continue;
        for(auto& row:result["children"][child.table]) {
            row["clip_id"]=id;
            if(std::string(child.key)=="id") {
                const auto before=detail::signedInteger(row.at("id"));row["id"]=ids.next(child.table);
                if(std::string(child.table)=="automation_data")automationIds[before]=detail::signedInteger(row.at("id"));
            }
            if(std::string(child.table)=="automation_segment_ranges")
                row["data_id"]=automationIds.at(detail::signedInteger(row.at("data_id")));
        }
    }
    return result;
}
} // namespace adi::clip_edit
