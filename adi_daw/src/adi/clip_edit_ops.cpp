// SPDX-License-Identifier: AGPL-3.0-or-later
#include "adi/clip_edit_ops.hpp"
#include "adi/edit_rows.hpp"
#include "adi/edit_time.hpp"
#include "adi/row_capture.hpp"
#include "adi/engine/snapshot.hpp"
#include <SQLiteCpp/SQLiteCpp.h>
#include <array>
#include <algorithm>
#include <cmath>
#include <limits>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>

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
        result.span=tempo.storedNanosecondsToTicks(begin+length)-tempo.storedNanosecondsToTicks(begin);
    }
    if(result.span<0)throw std::runtime_error("fade needs a nonnegative clip length");
    return result;
}
const Payload& selected(const Payload& payload) {
    return payload.contains("_restore")?payload.at("_restore").at("sides"):payload;
}
void pinFadeDomain(OpContext& context,std::int64_t id,std::size_t side,std::int64_t peak,std::int64_t zero) {
    SQLite::Statement clip(context.db,
        "SELECT c.time_base,c.pos_ticks,c.length_ticks,c.pos_ns,c.length_ns,c.content_offset_ticks,"
        "COALESCE(a.warp_enabled,0),p.content_offset_ns FROM clips c "
        "LEFT JOIN audio_clips a ON a.clip_id=c.id LEFT JOIN clip_playback_details p ON p.clip_id=c.id WHERE c.id=?");
    clip.bind(1,id);
    if(!clip.executeStep())throw std::runtime_error("fade target is missing");
    const edit::TimeMap time(context.db);
    const bool musical=clip.getColumn(0).getInt()==0;
    const auto beginNs=musical?time.nanos(clip.getColumn(1).getInt64()):clip.getColumn(3).getInt64();
    const auto origin=musical?clip.getColumn(1).getInt64():time.ticks(beginNs);
    const auto endNs=musical?time.nanos(edit::addTime(origin,clip.getColumn(2).getInt64())):
        edit::addTime(beginNs,clip.getColumn(4).getInt64());
    const auto endTick=musical?edit::addTime(origin,clip.getColumn(2).getInt64()):time.ticks(endNs);
    const auto span=endTick-origin;
    const auto offset=clip.getColumn(5).getInt64();
    const bool warp=clip.getColumn(6).getInt()!=0;
    const auto sourceTime=[&](std::int64_t fromLeft) {
        if(warp)return edit::addTime(offset,fromLeft);
        // Linear placement edges need their stored nanoseconds, including
        // the sub-tick remainder lost by the display's musical projection.
        const auto at=fromLeft==0?beginNs:fromLeft==span?endNs:
            std::clamp(time.nanos(edit::addTime(origin,fromLeft)),beginNs,endNs);
        const auto sourceOffset=clip.getColumn(7).isNull()?time.duration(origin,offset):clip.getColumn(7).getInt64();
        return edit::addTime(sourceOffset,at-beginNs);
    };
    const auto first=sourceTime(side==0?zero:span-peak);
    const auto last=sourceTime(side==0?peak:span-zero);
    if(first<0 || last<=first)throw std::runtime_error("fade handles have no extent in the source clock");
    const std::string table=warp?"clip_warp_fade_domains":"clip_playback_details";
    const std::string suffix=warp?"_ticks":"_ns";
    const std::string prefix=sides[side];
    const std::string begin=prefix+"_begin"+suffix,end=prefix+"_end"+suffix;
    SQLite::Statement update(context.db,"INSERT INTO "+table+"(clip_id,"+begin+","+end+") VALUES(?,?,?) "
        "ON CONFLICT(clip_id) DO UPDATE SET "+begin+"=excluded."+begin+","+end+"=excluded."+end);
    update.bind(1,id);update.bind(2,first);update.bind(3,last);update.exec();
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
            if(!edit.is_object() || (!edit.contains("ticks") && !edit.contains("startTicks") && !edit.contains("curve") && !edit.contains("tension")))
                throw std::runtime_error("fade side needs ticks, startTicks, curve or tension");
            for(auto it=edit.begin();it!=edit.end();++it)
                if(it.key()!="ticks" && it.key()!="startTicks" && it.key()!="curve" && it.key()!="tension")throw std::runtime_error("unknown fade field");
            const auto ticks=edit.contains("ticks")?detail::signedInteger(edit.at("ticks")):clip.ticks[side];
            const auto start=edit.contains("startTicks")?detail::signedInteger(edit.at("startTicks")):0;
            const auto curve=edit.contains("curve")?detail::signedInteger(edit.at("curve")):clip.curves[side];
            if(!restoring && (ticks<0 || ticks>clip.span || curve<0 || curve>5))throw std::runtime_error("fade length or curve is outside range");
            if(start<0 || (start>0 && start>=ticks))throw std::runtime_error("fade start must be before the fade peak");
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
                if(start>0)pinFadeDomain(context,clip.id,side,ticks,start);
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
    const Payload* sourceBefore=nullptr;const Payload* sourceAfter=nullptr;
    std::array<const Payload*,2> pieces{};
    std::array<std::int64_t,2> createdIds{};
    std::size_t created=0;
    auto createdOwner=[&](std::int64_t owner) {
        for(std::size_t i=0;i<created;++i) {
            if(createdIds[i]==owner)return true;
        }
        return false;
    };
    for(const auto& change:payload.at("changes"))if(change.at("table")=="clips") {
        const auto key=detail::signedInteger(change.at("id"));
        if(key==id) {
            if(sourceBefore || !change.at("before").is_object() || !change.at("after").is_object())throw std::runtime_error("split needs its original clip");
            sourceBefore=&change.at("before");sourceAfter=&change.at("after");
        } else {
            if(created==pieces.size() || createdOwner(key))
                throw std::runtime_error("split needs one or two distinct new clip identities");
            const auto& absent=change.at(joining?"after":"before");
            const auto& present=change.at(joining?"before":"after");
            if(!absent.is_null() || !present.is_object())throw std::runtime_error("split/join direction differs");
            pieces[created]=&present;createdIds[created]=key;++created;
        }
    }
    if(!sourceBefore || !sourceAfter || created==0)throw std::runtime_error("split needs its source and new pieces");
    for(std::size_t i=0;i<created;++i) {
        if(pieces[i]->at("kind")!=sourceBefore->at("kind") || pieces[i]->at("track_id")!=sourceBefore->at("track_id") ||
           pieces[i]->at("time_base")!=sourceBefore->at("time_base"))
            throw std::runtime_error("split must keep the track, kind and time base");
    }
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
            if(owner!=id && !createdOwner(owner))throw std::runtime_error("split child belongs to another clip");
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
// The Loop switch and the loop brace are transport commands: they write the
// saved loop and leave no Undo step. When history replays a time edit and the
// loop is no longer as that edit left it, the replay must not refuse, or Undo
// stops there for good. A loop whose brace still stands where the edit left it
// takes the edit's brace and keeps its own switch. A brace placed since stays.
std::optional<std::string> replayedLoop(const Payload& stored,const Payload& expected,const Payload& after) {
    try {
        if(!stored.is_string() || !expected.is_string())return std::nullopt;
        const auto now=Payload::parse(stored.get_ref<const std::string&>());
        const auto left=Payload::parse(expected.get_ref<const std::string&>());
        if(!now.is_object() || !left.is_object() || !now.at("on").is_boolean() ||
           detail::signedInteger(now.at("start"))!=detail::signedInteger(left.at("start")) ||
           detail::signedInteger(now.at("end"))!=detail::signedInteger(left.at("end")))
            return std::nullopt;
        auto value=after;value["on"]=now.at("on");
        return value.dump();
    } catch(const std::exception&) {return std::nullopt;}
}
bool arrangerApply(OpContext& context,const Payload& payload,std::string& error) {
    try {
        std::optional<std::string> loop;
        if(payload.contains("loop")) {
            const auto& change=payload.at("loop");
            SQLite::Statement read(context.db,"SELECT value FROM session_state WHERE key='transport.loop'");
            const Payload before=read.executeStep()?Payload(read.getColumn(0).getString()):Payload{};
            const auto& after=change.at("after");
            if(!after.is_string())throw std::runtime_error("captured loop must retain its stored value");
            const auto value=Payload::parse(after.get<std::string>());
            const auto start=detail::signedInteger(value.at("start")),end=detail::signedInteger(value.at("end"));
            if(!value.at("on").is_boolean() || start<0 || end<=start)throw std::runtime_error("captured loop is invalid");
            if(before==change.at("before"))loop=after.get<std::string>();
            else if(!context.replay)throw std::runtime_error("loop changed after edit preparation");
            else loop=replayedLoop(before,change.at("before"),value);
        }
        if(!payload.at("changes").empty() && !edit::applyRows(context,payload,error))return false;
        if(payload.at("changes").empty() && !payload.contains("loop"))throw std::runtime_error("arrangement edit is empty");
        if(loop) {
            SQLite::Statement write(context.db,"UPDATE session_state SET value=? WHERE key='transport.loop'");
            write.bind(1,*loop);write.exec();
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
void validSourceReplacement(const Payload& payload) {
    const auto& changes=payload.at("changes");
    const auto id=detail::signedInteger(payload.at("id"));
    if(changes.size()!=1 || changes[0].at("table")!="audio_clips" || detail::signedInteger(changes[0].at("id"))!=id)
        throw std::runtime_error("sample replacement edits exactly one audio source");
    const auto& before=changes[0].at("before");
    const auto& after=changes[0].at("after");
    if(!before.is_object() || !after.is_object() || detail::signedInteger(before.at("clip_id"))!=id ||
       detail::signedInteger(after.at("clip_id"))!=id)
        throw std::runtime_error("sample replacement requires an existing audio clip");
    for(auto field=before.begin();field!=before.end();++field) {
        const auto& name=field.key();
        if(name!="media_id" && name!="src_start_frames" && name!="src_len_frames" && name!="warp_markers" && after.at(name)!=field.value())
            throw std::runtime_error("sample replacement cannot change other clip settings");
    }
}
bool sourceReplacementApply(OpContext& context,const Payload& payload,std::string& error) {
    try {
        validSourceReplacement(payload);
        if(!context.replay) {
            const auto& source=payload.at("changes")[0].at("after");
            const auto start=detail::signedInteger(source.at("src_start_frames"));
            const auto count=detail::signedInteger(source.at("src_len_frames"));
            SQLite::Statement media(context.db,"SELECT frames,embedded,missing FROM media_files WHERE id=?");
            media.bind(1,detail::signedInteger(source.at("media_id")));
            if(!media.executeStep() || media.getColumn(1).getInt()!=0 || media.getColumn(2).getInt()!=0 || start<0 || count<=0)
                throw std::runtime_error("sample replacement needs a playable source window");
            if(!media.getColumn(0).isNull()) {
                const auto frames=media.getColumn(0).getInt64();
                if(start>frames || count>frames-start)throw std::runtime_error("replacement source window exceeds the sample");
            }
        }
        return edit::applyRows(context,payload,error);
    }
    catch(const std::exception& exception){error=exception.what();return false;}
}
bool sourceReplacementInverse(OpContext& context,const Payload& payload,Payload& inverse,std::string& error) {
    try{validSourceReplacement(payload);return edit::inverseRows(context,payload,inverse,error);}
    catch(const std::exception& exception){error=exception.what();return false;}
}
constexpr std::array<Field,4> fields={{{"id",FieldType::Int},{"in",FieldType::Object,false},{"out",FieldType::Object,false},{"_restore",FieldType::Object,false}}};
constexpr std::array<Field,2> changeFields={{{"id",FieldType::Int},{"changes",FieldType::Array}}};
constexpr std::array<Field,3> arrangerFields={{{"id",FieldType::Int},{"changes",FieldType::Array},{"loop",FieldType::Object,false}}};
const std::array<OpDescriptor,6> descriptors={{
    {"arranger.splice","Apply a captured arrangement edit",Scope::Edit,EngineImpact::GraphRebuild,arrangerFields,false,false,arrangerApply,arrangerInverse,""},
    {"clip.split","Split a clip with captured source clocks",Scope::Edit,EngineImpact::GraphRebuild,changeFields,false,false,splitApply<false>,splitInverse<false>,"clip.join"},
    {"clip.join","Restore the pieces of a captured split",Scope::Edit,EngineImpact::GraphRebuild,changeFields,false,false,splitApply<true>,splitInverse<true>,"clip.split"},
    {"clip.duplicate","Paste a captured independent clip",Scope::Edit,EngineImpact::GraphRebuild,changeFields,false,false,duplicateApply,duplicateInverse,"clip.delete"},
    {"audioClip.setSource","Replace one audio clip's sample",Scope::Edit,EngineImpact::GraphRebuild,changeFields,false,false,sourceReplacementApply,sourceReplacementInverse,""},
    {"clip.setFades","Set audio clip fade handles and continuous curvature",Scope::Edit,EngineImpact::Snapshot,fields,false,false,fadeApply,fadeInverse,""}
}};
}
std::span<const OpDescriptor> clipEditOps(){return descriptors;}
} // namespace adi
