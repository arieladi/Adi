// SPDX-License-Identifier: AGPL-3.0-or-later
#include "adi/clip_edit_ops.hpp"
#include "adi/edit_rows.hpp"
#include "adi/row_capture.hpp"
#include "adi/engine/snapshot.hpp"
#include <SQLiteCpp/SQLiteCpp.h>
#include <array>
#include <algorithm>
#include <cmath>
#include <limits>
#include <set>
#include <stdexcept>

namespace adi {
namespace {
constexpr std::array<const char*,2> sides={"in","out"};
constexpr std::array<const char*,2> tickColumns={"fade_in_ticks","fade_out_ticks"};
constexpr std::array<const char*,2> curveColumns={"fade_in_curve","fade_out_curve"};
constexpr std::array<const char*,2> tensionColumns={"in_tension","out_tension"};
struct FadeTarget {std::int64_t id=0,span=0;std::array<std::int64_t,2> ticks{},curves{};};
FadeTarget target(OpContext& context,const Payload& payload,bool needSpan) {
    FadeTarget result;result.id=detail::signedInteger(payload.at("id"));
    SQLite::Statement st(context.db,"SELECT kind,time_base,pos_ns,length_ticks,length_ns,fade_in_ticks,fade_out_ticks,fade_in_curve,fade_out_curve FROM clips WHERE id=?");
    st.bind(1,result.id);
    if(!st.executeStep() || st.getColumn(0).getString()!="audio")throw std::runtime_error("fade target must be an existing audio clip");
    result.ticks={st.getColumn(5).getInt64(),st.getColumn(6).getInt64()};
    result.curves={st.getColumn(7).getInt64(),st.getColumn(8).getInt64()};
    if(!needSpan)return result;
    if(st.getColumn(1).getInt64()==0) {
        if(st.getColumn(3).isNull())throw std::runtime_error("fade needs a finite clip length");
        result.span=st.getColumn(3).getInt64();
    } else {
        if(st.getColumn(2).isNull() || st.getColumn(4).isNull())throw std::runtime_error("fade needs a finite linear clip");
        const auto begin=st.getColumn(2).getInt64(),length=st.getColumn(4).getInt64();
        if(begin<0 || length<0 || begin>std::numeric_limits<std::int64_t>::max()-length)throw std::runtime_error("linear fade time is outside range");
        engine::TempoMap tempo;
        SQLite::Statement points(context.db,"SELECT pos_ticks,bpm,curve FROM tempo_map ORDER BY pos_ticks,id");
        double fastest=120;
        while(points.executeStep()) {
            const auto position=points.getColumn(0).getInt64();const auto bpm=points.getColumn(1).getDouble();
            if(position<0 || !std::isfinite(bpm) || bpm<=0)throw std::runtime_error("fade tempo map is invalid");
            fastest=std::max(fastest,bpm);
            tempo.events.push_back({position,bpm,points.getColumn(2).getInt()});
        }
        const long double bound=static_cast<long double>(begin+length)/1e9L*fastest*textproj::kPPQ/60;
        if(!std::isfinite(bound) || bound>static_cast<long double>(std::numeric_limits<std::int64_t>::max())-4096)
            throw std::runtime_error("linear fade exceeds supported tick range");
        if(tempo.events.empty() || tempo.events.front().posTicks!=0)tempo.events.insert(tempo.events.begin(),{0,120,0});
        result.span=tempo.secondsToTicks(static_cast<double>(begin+length)/1e9)-tempo.secondsToTicks(static_cast<double>(begin)/1e9);
    }
    if(result.span<0)throw std::runtime_error("fade needs a nonnegative clip length");
    return result;
}
const Payload& selected(const Payload& payload) {
    return payload.contains("_restore")?payload.at("_restore").at("sides"):payload;
}
bool fadeInverse(OpContext& context,const Payload& payload,Payload& inverse,std::string& error) {
    try {
        const auto clip=target(context,payload,false);const auto& edits=selected(payload);
        Payload before={{"sides",Payload::object()}};bool any=false,shape=true;
        for(std::size_t side=0;side<sides.size();++side)if(edits.contains(sides[side])) {
            any=true;before["sides"][sides[side]]={{"ticks",clip.ticks[side]},{"curve",clip.curves[side]}};
            shape=shape || edits.at(sides[side]).contains("tension");
        }
        if(payload.contains("_restore"))shape=payload.at("_restore").contains("shape");
        if(!any)throw std::runtime_error("fade edit needs an in or out side");
        if(shape) {
            const auto row=detail::captureRow(context.db,"clip_playback_details","clip_id",clip.id);
            before["shape"]=row?*row:Payload{};
            if(!payload.contains("_restore") || payload.at("_restore").contains("warpShape")) {
                const auto warp=detail::captureRow(context.db,"clip_warp_fade_domains","clip_id",clip.id);
                before["warpShape"]=warp?*warp:Payload{};
            }
        }
        inverse={{"id",clip.id},{"_restore",before}};return true;
    } catch(const std::exception& exception){error=exception.what();return false;}
}
bool fadeApply(OpContext& context,const Payload& payload,std::string& error) {
    try {
        const bool restoring=payload.contains("_restore");const auto clip=target(context,payload,!restoring);
        const auto& edits=selected(payload);bool any=false;
        for(std::size_t side=0;side<sides.size();++side)if(edits.contains(sides[side])) {
            const auto& edit=edits.at(sides[side]);
            if(!edit.is_object() || (!edit.contains("ticks") && !edit.contains("curve") && !edit.contains("tension")))
                throw std::runtime_error("fade side needs ticks, curve or tension");
            for(auto it=edit.begin();it!=edit.end();++it)
                if(it.key()!="ticks" && it.key()!="curve" && it.key()!="tension")throw std::runtime_error("unknown fade field");
            const auto ticks=edit.contains("ticks")?detail::signedInteger(edit.at("ticks")):clip.ticks[side];
            const auto curve=edit.contains("curve")?detail::signedInteger(edit.at("curve")):clip.curves[side];
            if(!restoring && (ticks<0 || ticks>clip.span || curve<0 || curve>5))throw std::runtime_error("fade length or curve is outside range");
            if(edit.contains("tension") && !edit.at("tension").is_null()) {
                const auto& value=edit.at("tension");
                if(!value.is_number() || !std::isfinite(value.get<double>()) || std::abs(value.get<double>())>1)
                    throw std::runtime_error("fade tension must be finite in -1..1 or null");
            }
            // All changes belong to the journal's existing transaction. A bad
            // second side rolls back the first, including an inserted shape row.
            SQLite::Statement update(context.db,std::string("UPDATE clips SET ")+tickColumns[side]+"=?,"+curveColumns[side]+"=? WHERE id=?");
            update.bind(1,ticks);update.bind(2,curve);update.bind(3,clip.id);update.exec();
            if(!restoring) {
                const std::string prefix=side==0?"in":"out";
                SQLite::Statement reset(context.db,"UPDATE clip_playback_details SET "+prefix+"_begin_ns=NULL,"+prefix+"_end_ns=NULL WHERE clip_id=?");
                reset.bind(1,clip.id);reset.exec();
                SQLite::Statement resetWarp(context.db,"UPDATE clip_warp_fade_domains SET "+prefix+"_begin_ticks=NULL,"+prefix+"_end_ticks=NULL WHERE clip_id=?");
                resetWarp.bind(1,clip.id);resetWarp.exec();
            }
            if(!restoring && edit.contains("tension")) {
                SQLite::Statement insert(context.db,"INSERT OR IGNORE INTO clip_playback_details(clip_id) VALUES(?)");insert.bind(1,clip.id);insert.exec();
                SQLite::Statement shape(context.db,std::string("UPDATE clip_playback_details SET ")+tensionColumns[side]+"=? WHERE clip_id=?");
                if(edit.at("tension").is_null())shape.bind(1);else shape.bind(1,edit.at("tension").get<double>());
                shape.bind(2,clip.id);shape.exec();
            }
            any=true;
        }
        if(!any)throw std::runtime_error("fade edit needs an in or out side");
        if(restoring && payload.at("_restore").contains("shape")) {
            const auto& shape=payload.at("_restore").at("shape");
            if(shape.is_null()) {
                SQLite::Statement erase(context.db,"DELETE FROM clip_playback_details WHERE clip_id=?");erase.bind(1,clip.id);erase.exec();
            } else {
                if(detail::signedInteger(shape.at("clip_id"))!=clip.id)throw std::runtime_error("captured fade belongs to another clip");
                const bool exists=detail::captureRow(context.db,"clip_playback_details","clip_id",clip.id).has_value();
                detail::restoreRow(context.db,"clip_playback_details",shape,exists?"clip_id":"");
            }
        }
        if(restoring && payload.at("_restore").contains("warpShape")) {
            const auto& shape=payload.at("_restore").at("warpShape");
            if(shape.is_null()) {
                SQLite::Statement erase(context.db,"DELETE FROM clip_warp_fade_domains WHERE clip_id=?");erase.bind(1,clip.id);erase.exec();
            } else {
                if(detail::signedInteger(shape.at("clip_id"))!=clip.id)throw std::runtime_error("captured Warp fade belongs to another clip");
                const bool exists=detail::captureRow(context.db,"clip_warp_fade_domains","clip_id",clip.id).has_value();
                detail::restoreRow(context.db,"clip_warp_fade_domains",shape,exists?"clip_id":"");
            }
        }
        return true;
    } catch(const std::exception& exception){error=exception.what();return false;}
}
const std::set<std::string> clipTables={"clips","event_streams","note_expression","audio_clips","automation_data","automation_segment_ranges","clip_properties","clip_playback_details","clip_warp_fade_domains"};
void validDuplicate(const Payload& payload) {
    const auto id=detail::signedInteger(payload.at("id"));std::size_t roots=0;
    for(const auto& change:payload.at("changes")) {
        const auto table=change.at("table").get<std::string>();
        if(!clipTables.contains(table) || !change.at("before").is_null() || !change.at("after").is_object())
            throw std::runtime_error("duplicate can only create captured clip rows");
        const auto& row=change.at("after");
        if(table=="clips"){if(detail::signedInteger(row.at("id"))!=id)throw std::runtime_error("duplicate clip id differs");++roots;}
        else if(detail::signedInteger(row.at("clip_id"))!=id)throw std::runtime_error("duplicate child belongs to another clip");
    }
    if(roots!=1)throw std::runtime_error("duplicate needs one new clip");
}
bool duplicateApply(OpContext& context,const Payload& payload,std::string& error) {
    try{validDuplicate(payload);return edit::applyRows(context,payload,error);}
    catch(const std::exception& exception){error=exception.what();return false;}
}
bool duplicateInverse(OpContext&,const Payload& payload,Payload& inverse,std::string& error) {
    try{validDuplicate(payload);inverse={{"id",payload.at("id")}};return true;}
    catch(const std::exception& exception){error=exception.what();return false;}
}
void validSplit(const Payload& payload,bool joining) {
    const auto id=detail::signedInteger(payload.at("id"));
    const Payload* sourceBefore=nullptr;const Payload* sourceAfter=nullptr;const Payload* right=nullptr;
    std::int64_t rightId=0;
    for(const auto& change:payload.at("changes"))if(change.at("table")=="clips") {
        const auto key=detail::signedInteger(change.at("id"));
        if(key==id) {
            if(sourceBefore || !change.at("before").is_object() || !change.at("after").is_object())throw std::runtime_error("split needs its original clip");
            sourceBefore=&change.at("before");sourceAfter=&change.at("after");
        } else {
            if(right)throw std::runtime_error("split needs one second clip");
            const auto& absent=change.at(joining?"after":"before");
            const auto& present=change.at(joining?"before":"after");
            if(!absent.is_null() || !present.is_object())throw std::runtime_error("split/join direction differs");
            right=&present;rightId=key;
        }
    }
    if(!sourceBefore || !sourceAfter || !right)throw std::runtime_error("split needs two clip identities");
    if(right->at("kind")!=sourceBefore->at("kind") || right->at("track_id")!=sourceBefore->at("track_id"))
        throw std::runtime_error("split must keep the track and kind");
    const std::set<std::string> changedCore={"length_ticks","length_ns","fade_in_ticks","fade_out_ticks"};
    for(auto it=sourceBefore->begin();it!=sourceBefore->end();++it)
        if(!changedCore.contains(it.key()) && sourceAfter->at(it.key())!=it.value())
            throw std::runtime_error("split cannot change unrelated clip fields");
    const std::set<std::string> clocks={"content_offset_ns","loop_begin_ns","loop_end_ns","source_end_ns","in_begin_ns","in_end_ns","out_begin_ns","out_end_ns","in_begin_ticks","in_end_ticks","out_begin_ticks","out_end_ticks"};
    for(const auto& change:payload.at("changes")) {
        const auto table=change.at("table").get<std::string>();
        if(!clipTables.contains(table))throw std::runtime_error("split cannot change unrelated tables");
        if(table=="clips")continue;
        for(const auto* row:{&change.at("before"),&change.at("after")})if(!row->is_null()) {
            const auto owner=detail::signedInteger(row->at("clip_id"));
            if(owner!=id && owner!=rightId)throw std::runtime_error("split child belongs to another clip");
            if(owner==id && table!="clip_playback_details" && table!="clip_warp_fade_domains")throw std::runtime_error("split cannot rewrite the original material");
        }
        const auto& before=change.at("before");const auto& after=change.at("after");
        if(before.is_object() && after.is_object() && detail::signedInteger(before.at("clip_id"))==id)
            for(auto it=before.begin();it!=before.end();++it)
                if(!clocks.contains(it.key()) && after.at(it.key())!=it.value())
                    throw std::runtime_error("split cannot rewrite unrelated playback details");
    }
}
template<bool Joining>
bool splitApply(OpContext& context,const Payload& payload,std::string& error) {
    try{validSplit(payload,Joining);return edit::applyRows(context,payload,error);}
    catch(const std::exception& exception){error=exception.what();return false;}
}
template<bool Joining>
bool splitInverse(OpContext& context,const Payload& payload,Payload& inverse,std::string& error) {
    try{validSplit(payload,Joining);return edit::inverseRows(context,payload,inverse,error);}
    catch(const std::exception& exception){error=exception.what();return false;}
}
bool arrangerApply(OpContext& context,const Payload& payload,std::string& error) {
    try {
        if(payload.contains("loop")) {
            const auto& change=payload.at("loop");
            SQLite::Statement read(context.db,"SELECT value FROM session_state WHERE key='transport.loop'");
            const Payload before=read.executeStep()?Payload(read.getColumn(0).getString()):Payload{};
            if(before!=change.at("before"))throw std::runtime_error("loop changed after edit preparation");
            const auto& after=change.at("after");
            if(!after.is_string())throw std::runtime_error("captured loop must retain its stored value");
            const auto value=Payload::parse(after.get<std::string>());
            const auto start=detail::signedInteger(value.at("start")),end=detail::signedInteger(value.at("end"));
            if(!value.at("on").is_boolean() || start<0 || end<=start)throw std::runtime_error("captured loop is invalid");
        }
        if(!payload.at("changes").empty() && !edit::applyRows(context,payload,error))return false;
        if(payload.at("changes").empty() && !payload.contains("loop"))throw std::runtime_error("arrangement edit is empty");
        if(payload.contains("loop")) {
            SQLite::Statement write(context.db,"UPDATE session_state SET value=? WHERE key='transport.loop'");
            write.bind(1,payload.at("loop").at("after").get<std::string>());write.exec();
        }
        return true;
    } catch(const std::exception& exception){error=exception.what();return false;}
}
bool arrangerInverse(OpContext& context,const Payload& payload,Payload& inverse,std::string& error) {
    if(payload.at("changes").empty())inverse=payload;
    else if(!edit::inverseRows(context,payload,inverse,error))return false;
    if(inverse.contains("loop"))std::swap(inverse["loop"]["before"],inverse["loop"]["after"]);
    return true;
}
constexpr std::array<Field,4> fields={{{"id",FieldType::Int},{"in",FieldType::Object,false},{"out",FieldType::Object,false},{"_restore",FieldType::Object,false}}};
constexpr std::array<Field,2> changeFields={{{"id",FieldType::Int},{"changes",FieldType::Array}}};
constexpr std::array<Field,3> arrangerFields={{{"id",FieldType::Int},{"changes",FieldType::Array},{"loop",FieldType::Object,false}}};
const std::array<OpDescriptor,5> descriptors={{
    {"arranger.splice","Apply a captured arrangement edit",Scope::Edit,EngineImpact::GraphRebuild,arrangerFields,false,false,arrangerApply,arrangerInverse,""},
    {"clip.split","Split a clip with captured source clocks",Scope::Edit,EngineImpact::GraphRebuild,changeFields,false,false,splitApply<false>,splitInverse<false>,"clip.join"},
    {"clip.join","Restore the two sides of a captured split",Scope::Edit,EngineImpact::GraphRebuild,changeFields,false,false,splitApply<true>,splitInverse<true>,"clip.split"},
    {"clip.duplicate","Paste a captured independent clip",Scope::Edit,EngineImpact::GraphRebuild,changeFields,false,false,duplicateApply,duplicateInverse,"clip.delete"},
    {"clip.setFades","Set audio clip fade lengths and continuous curvature",Scope::Edit,EngineImpact::Snapshot,fields,false,false,fadeApply,fadeInverse,""}
}};
}
std::span<const OpDescriptor> clipEditOps(){return descriptors;}
} // namespace adi
