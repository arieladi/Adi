// SPDX-License-Identifier: AGPL-3.0-or-later
#include "adi/clip_commands.hpp"
#include "adi/clip_image.hpp"
#include "adi/automation_time_edit.hpp"
#include "adi/automation_clipboard.hpp"
#include "adi/edit_time.hpp"
#include "adi/row_capture.hpp"
#include "adi/store.hpp"
#include <SQLiteCpp/SQLiteCpp.h>
#include <sqlite3.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <set>

namespace adi::clip_edit {
namespace {
struct Read {
    std::optional<SQLite::Transaction> transaction;
    explicit Read(SQLite::Database& db) {
        if(sqlite3_get_autocommit(db.getHandle())!=0)transaction.emplace(db,SQLite::TransactionBehavior::DEFERRED);
    }
};
std::string filename(Store& store) {
    SQLite::Statement st(store.db(),"PRAGMA database_list");
    while(st.executeStep())if(st.getColumn(1).getString()=="main")return st.getColumn(2).getString();
    throw std::runtime_error("edit needs a project connection");
}
Payload mediaKey(SQLite::Database& db,std::int64_t id) {
    SQLite::Statement row(db,"SELECT hash_blake3,orig_name,rel_path,abs_path_hint,sample_rate,channels,frames,format FROM media_files WHERE id=?");
    row.bind(1,id);
    if(!row.executeStep())throw std::runtime_error("copied clip media reference is missing");
    Payload key={{"id",id},{"hash",nullptr}};
    const auto hash=row.getColumn(0).isNull()?std::string{}:row.getColumn(0).getString();
    if(!row.getColumn(0).isNull())key["hash"]=hash;
    if(hash.empty()) {
        // Legacy un-hashed rows have no content identity yet. Preserve their
        // locator/dimensions without copying potentially huge peaks BLOBs.
        constexpr std::array<const char*,7> names={"name","relative","absolute","rate","channels","frames","format"};
        auto& legacy=key["legacy"];legacy=Payload::object();
        for(int column=1;column<=7;++column) {
            auto& cell=legacy[names[static_cast<std::size_t>(column-1)]];
            if(row.getColumn(column).isNull())cell=nullptr;
            else if(column>=4 && column<=6)cell=row.getColumn(column).getInt64();
            else cell=row.getColumn(column).getString();
        }
    }
    return key;
}
void captureMediaKeys(Store& store,Payload& image) {
    auto& keys=image["mediaKeys"];keys=Payload::array();
    for(const auto& source:image.at("children").at("audio_clips"))
        keys.push_back(mediaKey(store.db(),detail::signedInteger(source.at("media_id"))));
}
void verifyMediaKeys(Store& store,const Payload& image) {
    const auto& sources=image.at("children").at("audio_clips");
    if(sources.empty())return;
    if(!image.contains("mediaKeys") || !image.at("mediaKeys").is_array() || image.at("mediaKeys").size()!=sources.size())
        throw std::runtime_error("copied media identity is not captured");
    for(const auto& source:sources) {
        const auto id=detail::signedInteger(source.at("media_id"));
        const auto& keys=image.at("mediaKeys");
        const auto found=std::find_if(keys.begin(),keys.end(),[&](const Payload& key){return detail::signedInteger(key.at("id"))==id;});
        if(found==keys.end() || *found!=mediaKey(store.db(),id))
            throw std::runtime_error("media identity changed after clip copy");
    }
}
void validRate(double rate) {
    if(!std::isfinite(rate) || rate<=0 || rate>768000)throw std::runtime_error("invalid reference render rate");
}
std::int64_t sample(double seconds,double rate) {
    const double value=seconds*rate;
    if(!std::isfinite(value) || value<0 || value>0x1p50)throw std::runtime_error("source time is outside supported sample range");
    return static_cast<std::int64_t>(std::llround(value));
}
std::int64_t sampleNanos(std::int64_t value,double rate) {
    if(value<0)throw std::runtime_error("negative source sample position");
    const auto nanos=edit::nanosFromSeconds(static_cast<double>(value)/rate);
    if(sample(static_cast<double>(nanos)*1e-9,rate)!=value)throw std::runtime_error("source clock cannot preserve this sample boundary");
    return nanos;
}
double startSeconds(const Payload& row,const edit::TimeMap& time) {
    return row.at("time_base")==0?time.seconds(detail::signedInteger(row.at("pos_ticks"))):
        static_cast<double>(detail::signedInteger(row.at("pos_ns")))*1e-9;
}
double endSeconds(const Payload& row,const edit::TimeMap& time) {
    if(row.at("time_base")==0)
        return time.seconds(edit::addTime(detail::signedInteger(row.at("pos_ticks")),detail::signedInteger(row.at("length_ticks"))));
    return startSeconds(row,time)+static_cast<double>(detail::signedInteger(row.at("length_ns")))*1e-9;
}
std::int64_t originTicks(const Payload& row,const edit::TimeMap& time) {
    return row.at("time_base")==0?detail::signedInteger(row.at("pos_ticks")):time.ticks(detail::signedInteger(row.at("pos_ns")));
}
Payload* playback(Payload& image) {
    auto& records=image["children"]["clip_playback_details"];
    return records.empty()?nullptr:&records[0];
}
Payload& ensurePlayback(Store& store,Payload& image) {
    if(auto* existing=playback(image))return *existing;
    auto row=edit::defaultRow(store.db(),"clip_playback_details",{{"clip_id",image.at("id")}});
    image["children"]["clip_playback_details"].push_back(std::move(row));
    return image["children"]["clip_playback_details"][0];
}
void retainDetails(edit::RowEdits& plan,const Payload& image) {
    const auto id=detail::signedInteger(image.at("id"));
    for(const char* table:{"clip_playback_details","clip_warp_fade_domains"})
        if(image.at("children").contains(table))
            for(const auto& row:image.at("children").at(table))plan.set(table,id,row);
}
void freezeWarpFades(Store& store,Payload& image,const edit::TimeMap& time) {
    const auto& row=image.at("row");
    const auto offset=detail::signedInteger(row.at("content_offset_ticks"));
    const auto origin=originTicks(row,time);
    const auto last=row.at("time_base")==0?edit::addTime(origin,detail::signedInteger(row.at("length_ticks"))):
        time.ticks(edit::addTime(detail::signedInteger(row.at("pos_ns")),detail::signedInteger(row.at("length_ns"))));
    if(offset<0 || last<origin)throw std::runtime_error("warped clip needs a valid source clock");
    const auto in=detail::signedInteger(row.at("fade_in_ticks")),out=detail::signedInteger(row.at("fade_out_ticks"));
    const auto span=last-origin;
    if(in<0 || out<0 || in>span || out>span)throw std::runtime_error("warped clip fades exceed its visible musical span");
    if(in==0 && out==0)return;
    auto& records=image["children"]["clip_warp_fade_domains"];
    if(records.empty())records.push_back(edit::defaultRow(store.db(),"clip_warp_fade_domains",{{"clip_id",image.at("id")}}));
    auto& domain=records[0];
    if(in>0 && domain.at("in_begin_ticks").is_null()) {
        domain["in_begin_ticks"]=offset;domain["in_end_ticks"]=edit::addTime(offset,in);
    }
    if(out>0 && domain.at("out_begin_ticks").is_null()) {
        const auto end=edit::addTime(offset,span);
        domain["out_begin_ticks"]=end-out;domain["out_end_ticks"]=end;
    }
}
void freezeAudio(Store& store,Payload& image,const edit::TimeMap& time,double rate) {
    const auto& row=image.at("row");
    if(row.at("kind")!="audio")return;
    if(!row.at("alias_of").is_null())throw std::runtime_error("audio alias editing needs alias resolution");
    const auto& sources=image.at("children").at("audio_clips");
    if(sources.empty())return; // keep unattached/missing metadata editable
    if(sources[0].at("warp_enabled")!=0){freezeWarpFades(store,image,time);return;}
    const auto origin=originTicks(row,time);
    auto& clock=ensurePlayback(store,image);
    auto readSample=[&](const char* column,std::int64_t ticks) {
        return clock.at(column).is_null()?sample(time.durationSeconds(origin,ticks),rate):
            sample(static_cast<double>(detail::signedInteger(clock.at(column)))*1e-9,rate);
    };
    const auto offset=readSample("content_offset_ns",detail::signedInteger(row.at("content_offset_ticks")));
    clock["content_offset_ns"]=sampleNanos(offset,rate);
    std::optional<std::int64_t> marker;
    if(image.at("children").contains("clip_properties")) {
        const auto& properties=image.at("children").at("clip_properties");
        if(!properties.empty() && !properties[0].at("end_marker_ticks").is_null())
            marker=detail::signedInteger(properties[0].at("end_marker_ticks"));
    }
    const auto span=sample(endSeconds(row,time),rate)-sample(startSeconds(row,time),rate);
    if(span<0)throw std::runtime_error("clip duration is negative");
    if(clock.at("source_end_ns").is_null() && marker)
        clock["source_end_ns"]=sampleNanos(sample(time.durationSeconds(origin,*marker),rate),rate);
    const auto fadeIn=detail::signedInteger(row.at("fade_in_ticks")),fadeOut=detail::signedInteger(row.at("fade_out_ticks"));
    if(fadeIn<0 || fadeOut<0)throw std::runtime_error("clip has invalid fades");
    if(fadeIn>0 && clock.at("in_begin_ns").is_null()) {
        const auto length=sample(time.durationSeconds(origin,fadeIn),rate);
        if(length>0) {clock["in_begin_ns"]=sampleNanos(offset,rate);clock["in_end_ns"]=sampleNanos(edit::addTime(offset,length),rate);}
    }
    if(fadeOut>0 && clock.at("out_begin_ns").is_null()) {
        const auto lastTick=row.at("time_base")==0?edit::addTime(origin,detail::signedInteger(row.at("length_ticks"))):
            time.ticks(detail::signedInteger(row.at("pos_ns"))+detail::signedInteger(row.at("length_ns")));
        if(lastTick<fadeOut)throw std::runtime_error("clip fade precedes project origin");
        const auto length=sample(time.seconds(lastTick)-time.seconds(lastTick-fadeOut),rate);
        if(length>0) {
            clock["out_begin_ns"]=sampleNanos(edit::addTime(offset,span)-length,rate);
            clock["out_end_ns"]=sampleNanos(edit::addTime(offset,span),rate);
        }
    }
    if(row.at("loop_enabled")!=0) {
        const auto begin=detail::signedInteger(row.at("loop_start_ticks"));
        const auto length=detail::signedInteger(row.at("loop_len_ticks"));
        const auto first=readSample("loop_begin_ns",begin),last=readSample("loop_end_ns",edit::addTime(begin,length));
        if(length<=0 || last<=first)throw std::runtime_error("clip loop is invalid at this render rate");
        clock["loop_begin_ns"]=sampleNanos(first,rate);clock["loop_end_ns"]=sampleNanos(last,rate);
    }
}
std::int64_t coordinate(TimePoint point,int base,const edit::TimeMap& time) {
    if((point.base!=0 && point.base!=1) || point.value<0)throw std::runtime_error("invalid edit coordinate");
    if(point.base==base)return point.value;
    return base==0?time.ticks(point.value):time.nanos(point.value);
}
void relocate(Payload& row,std::int64_t track,TimePoint point,const edit::TimeMap& time) {
    const int base=static_cast<int>(detail::signedInteger(row.at("time_base")));
    row[base==0?"pos_ticks":"pos_ns"]=coordinate(point,base,time);
    if(detail::signedInteger(row.at("track_id"))!=track)row["lane_id"]=nullptr;
    row["track_id"]=track;
}
void packet(Store& store,PreparedEdit& result,const char* name,const char* label,std::int64_t id,edit::RowEdits& plan) {
    const auto changes=plan.changes();
    if(!changes.empty()) {
        OpRequest request;request.opType=name;request.payload={{"id",id},{"changes",changes}};
        request.label=label;request.targetKind="clip";request.targetId=id;result.requests.push_back(std::move(request));
    }
    result.sourceFile=filename(store);result.ok=true;
}
struct ImageCut {
    const char* position=nullptr;
    const char* length=nullptr;
    std::int64_t first=0,last=0,cut=0,beforeTicks=0,afterTicks=0;
    std::int64_t fadeIn=0,fadeOut=0,rightOffsetTicks=0;
    std::optional<std::int64_t> rightOffsetNs;
};
ImageCut prepareImageCut(Store& store,Payload& image,TimePoint point,
                         const edit::TimeMap& time,double referenceRate) {
    const auto kind=image.at("row").at("kind").get<std::string>();
    if(kind!="audio" && kind!="midi")throw std::runtime_error("this clip kind has no split playback");
    if(!image.at("row").at("alias_of").is_null())throw std::runtime_error("split needs alias resolution");
    const auto old=image.at("row");
    const int base=static_cast<int>(detail::signedInteger(old.at("time_base")));
    ImageCut result;
    result.position=base==0?"pos_ticks":"pos_ns";result.length=base==0?"length_ticks":"length_ns";
    result.first=detail::signedInteger(old.at(result.position));
    const auto extent=detail::signedInteger(old.at(result.length));
    result.last=edit::addTime(result.first,extent);result.cut=coordinate(point,base,time);
    if(extent<=0 || result.cut<=result.first || result.cut>=result.last)
        throw std::runtime_error("split must lie strictly inside the clip");
    // Freeze the original source domains before changing either visible span.
    // A single surviving side uses the same math without cloning its children.
    freezeAudio(store,image,time,referenceRate);
    const auto splitTick=base==0?result.cut:time.ticks(result.cut);
    const auto firstTick=originTicks(old,time),lastTick=base==0?result.last:time.ticks(result.last);
    result.beforeTicks=splitTick-firstTick;result.afterTicks=lastTick-splitTick;
    if(result.beforeTicks<0 || result.afterTicks<0)throw std::runtime_error("split has an invalid musical span");
    result.fadeIn=detail::signedInteger(old.at("fade_in_ticks"));
    result.fadeOut=detail::signedInteger(old.at("fade_out_ticks"));
    if(result.fadeIn<0 || result.fadeOut<0)throw std::runtime_error("split needs valid existing fades");
    result.rightOffsetTicks=edit::addTime(detail::signedInteger(old.at("content_offset_ticks")),result.beforeTicks);
    if(kind=="audio") {
        const auto* clock=playback(image);
        if(clock && !clock->at("content_offset_ns").is_null()) {
            const auto offset=sample(static_cast<double>(detail::signedInteger(clock->at("content_offset_ns")))*1e-9,referenceRate);
            const double cutSeconds=base==0?time.seconds(result.cut):static_cast<double>(result.cut)*1e-9;
            const auto passed=sample(cutSeconds,referenceRate)-sample(startSeconds(old,time),referenceRate);
            result.rightOffsetNs=sampleNanos(edit::addTime(offset,passed),referenceRate);
        }
    }
    return result;
}
void keepLeft(Payload& image,const ImageCut& cut) {
    auto& row=image["row"];
    row[cut.length]=cut.cut-cut.first;row["fade_out_ticks"]=0;
    row["fade_in_ticks"]=std::min(cut.fadeIn,cut.beforeTicks);
}
void keepRight(Payload& image,const ImageCut& cut) {
    auto& row=image["row"];
    row[cut.position]=cut.cut;row[cut.length]=cut.last-cut.cut;row["fade_in_ticks"]=0;
    row["fade_out_ticks"]=std::min(cut.fadeOut,cut.afterTicks);
    row["content_offset_ticks"]=cut.rightOffsetTicks;
    if(cut.rightOffsetNs) {
        auto* clock=playback(image);
        if(!clock)throw std::runtime_error("right clip lost its captured source clock");
        (*clock)["content_offset_ns"]=*cut.rightOffsetNs;
    }
}
Payload splitImage(Store& store,Payload& image,std::int64_t rightId,TimePoint point,
                   const edit::TimeMap& time,double referenceRate,CopyIds& ids) {
    const auto cut=prepareImageCut(store,image,point,time,referenceRate);
    auto right=cloneImage(image,rightId,ids);
    keepLeft(image,cut);keepRight(right,cut);
    return right;
}
void validRange(const RangeSelection& range) {
    if(range.begin.base!=range.end.base || (range.begin.base!=0 && range.begin.base!=1) ||
       range.begin.value<0 || range.end.value<=range.begin.value || range.tracks.empty())
        throw std::runtime_error("range needs ordered endpoints in one clock and selected tracks");
    std::set<std::int64_t> tracks;
    for(const auto id:range.tracks)if(!tracks.insert(id).second)throw std::runtime_error("range has duplicate tracks");
}
enum class ClipLaneScope { MainLane, AllLanes };
const char* clipRangeQuery(ClipLaneScope scope) {
    return scope==ClipLaneScope::MainLane?
        "SELECT id FROM clips WHERE track_id=? AND lane_id IS NULL ORDER BY id":
        "SELECT id FROM clips WHERE track_id=? ORDER BY id";
}
void eraseRange(Store& store,edit::RowEdits& plan,CopyIds& ids,const RangeSelection& range,
                const edit::TimeMap& time,double rate,ClipLaneScope scope) {
    OpContext context{store,store.db()};
    for(const auto track:range.tracks) {
        SQLite::Statement clips(store.db(),clipRangeQuery(scope));clips.bind(1,track);
        while(clips.executeStep()) {
            const auto id=clips.getColumn(0).getInt64();auto image=captureDeleted(context,id);
            const auto& core=image.at("row");const int base=static_cast<int>(detail::signedInteger(core.at("time_base")));
            const char* position=base==0?"pos_ticks":"pos_ns";const char* length=base==0?"length_ticks":"length_ns";
            if(core.at(length).is_null())throw std::runtime_error("range editing needs finite clip extents");
            const auto first=detail::signedInteger(core.at(position)),last=edit::addTime(first,detail::signedInteger(core.at(length)));
            const auto begin=coordinate(range.begin,base,time),end=coordinate(range.end,base,time);
            if(last<=begin || first>=end)continue;
            if(first>=begin && last<=end){deleteImage(plan,image);continue;}
            if(first<begin && last>end) {
                auto tail=splitImage(store,image,ids.next("clips"),{base,begin},time,rate,ids);
                const auto cut=prepareImageCut(store,tail,{base,end},time,rate);
                keepRight(tail,cut);
                plan.set("clips",id,image.at("row"));retainDetails(plan,image);
                createImage(plan,tail);
            } else if(first<begin) {
                const auto cut=prepareImageCut(store,image,{base,begin},time,rate);
                keepLeft(image,cut);
                plan.set("clips",id,image.at("row"));retainDetails(plan,image);
            } else {
                const auto cut=prepareImageCut(store,image,{base,end},time,rate);
                keepRight(image,cut);
                plan.set("clips",id,image.at("row"));retainDetails(plan,image);
            }
        }
    }
}
void expectedHead(Store& store,PreparedEdit& result) {result.options.expectHead.emplace(OpJournal(store).headSeq());}
}
ClipCopy copyClip(Store& store,std::int64_t id,double referenceRate) {
    ClipCopy result;
    try {
        validRate(referenceRate);Read read(store.db());OpContext context{store,store.db()};
        result.sourceFile=filename(store);result.image=captureDeleted(context,id);captureMediaKeys(store,result.image);
        edit::TimeMap time(store.db());freezeAudio(store,result.image,time,referenceRate);result.ok=true;
    } catch(const std::exception& exception){result.ok=false;result.error=exception.what();}
    return result;
}
ClipCut prepareCutClip(Store& store,std::int64_t id,double referenceRate) {
    ClipCut result;
    try {
        Read read(store.db());expectedHead(store,result.edit);result.copy=copyClip(store,id,referenceRate);
        if(!result.copy.ok)throw std::runtime_error(result.copy.error);
        result.edit.sourceFile=result.copy.sourceFile;
        OpRequest request;request.opType="clip.delete";request.payload={{"id",id}};request.label="Cut Clip";
        request.targetKind="clip";request.targetId=id;result.edit.requests.push_back(std::move(request));result.edit.ok=true;
    } catch(const std::exception& exception){result.edit.error=exception.what();}
    return result;
}
PreparedEdit preparePaste(Store& store,const ClipCopy& copy,std::int64_t newId,std::int64_t track,TimePoint point,double referenceRate) {
    PreparedEdit result;
    try {
        validRate(referenceRate);if(!copy.ok)throw std::runtime_error(copy.error);
        Read read(store.db());expectedHead(store,result);
        if(filename(store)!=copy.sourceFile)throw std::runtime_error("clip clipboard belongs to a different project");
        verifyMediaKeys(store,copy.image);
        CopyIds ids(store.db());auto image=cloneImage(copy.image,newId,ids);edit::TimeMap time(store.db());
        relocate(image["row"],track,point,time);
        edit::RowEdits plan(store.db());createImage(plan,image);packet(store,result,"clip.duplicate","Paste Clip",newId,plan);
    } catch(const std::exception& exception){result.ok=false;result.error=exception.what();}
    return result;
}
PreparedEdit prepareDuplicate(Store& store,std::int64_t sourceId,std::int64_t newId,std::int64_t track,TimePoint point,double referenceRate) {
    // Both reads share one snapshot; no second writer can change material
    // between capturing the copy and capturing the expected undo head.
    try {
        Read read(store.db());auto copy=copyClip(store,sourceId,referenceRate);
        auto result=preparePaste(store,copy,newId,track,point,referenceRate);
        if(result.ok && !result.requests.empty())result.requests[0].label="Duplicate Clip";
        return result;
    } catch(const std::exception& exception){PreparedEdit result;result.error=exception.what();return result;}
}
PreparedEdit prepareSplit(Store& store,std::int64_t sourceId,std::int64_t rightId,TimePoint point,double referenceRate) {
    PreparedEdit result;
    try {
        validRate(referenceRate);Read read(store.db());expectedHead(store,result);OpContext context{store,store.db()};
        auto image=captureDeleted(context,sourceId);edit::TimeMap time(store.db());
        CopyIds ids(store.db());auto right=splitImage(store,image,rightId,point,time,referenceRate,ids);
        edit::RowEdits plan(store.db());plan.set("clips",sourceId,image.at("row"));
        retainDetails(plan,image);
        createImage(plan,right);packet(store,result,"clip.split","Split Clip",sourceId,plan);
    } catch(const std::exception& exception){result.ok=false;result.error=exception.what();}
    return result;
}
namespace {
PreparedEdit prepareInsertion(Store& store,std::int64_t atTicks,std::int64_t ticks,double referenceRate,std::optional<std::int64_t> exactNanos) {
    PreparedEdit result;
    try {
        validRate(referenceRate);
        if(atTicks<0 || ticks<=0)throw std::runtime_error("Insert Silence needs a nonnegative start and positive length");
        (void)edit::addTime(atTicks,ticks);
        Read read(store.db());expectedHead(store,result);auto& db=store.db();
        edit::TimeMap time(db);edit::RowEdits plan(db);CopyIds ids(db);OpContext context{store,db};
        const auto atNs=time.nanos(atTicks);
        const auto gapNs=exactNanos?*exactNanos:edit::nanosFromSeconds(static_cast<double>(ticks)/textproj::kPPQ*60/time.bpmBefore(atTicks));
        if(gapNs<=0)throw std::runtime_error("Insert Silence is shorter than a nanosecond");
        SQLite::Statement clips(db,"SELECT id FROM clips ORDER BY id");
        while(clips.executeStep()) {
            const auto id=clips.getColumn(0).getInt64();auto core=*plan.current("clips",id);
            const int base=static_cast<int>(detail::signedInteger(core.at("time_base")));
            const char* position=base==0?"pos_ticks":"pos_ns";const char* length=base==0?"length_ticks":"length_ns";
            const auto at=base==0?atTicks:atNs,gap=base==0?ticks:gapNs;
            const auto first=detail::signedInteger(core.at(position));
            if(core.at(length).is_null())throw std::runtime_error("Insert Silence needs finite clip extents");
            const auto extent=detail::signedInteger(core.at(length));
            if(first<0 || extent<0)throw std::runtime_error("Insert Silence needs valid clip placement");
            const auto last=edit::addTime(first,extent);
            if(last<=at && first<at)continue;
            auto image=captureDeleted(context,id);
            if(first>=at) {
                freezeAudio(store,image,time,referenceRate);image["row"][position]=edit::addTime(first,gap);
            } else {
                auto right=splitImage(store,image,ids.next("clips"),{base,at},time,referenceRate,ids);
                right["row"][position]=edit::addTime(at,gap);createImage(plan,right);
            }
            plan.set("clips",id,image.at("row"));
            retainDetails(plan,image);
        }
        for(const char* table:{"tempo_map","time_signature_map","key_map"}) {
            SQLite::Statement points(db,"SELECT id FROM "+std::string(table)+" WHERE pos_ticks>=? AND pos_ticks<>0 ORDER BY pos_ticks");
            points.bind(1,atTicks);
            while(points.executeStep()) {
                const auto id=points.getColumn(0).getInt64();auto point=*plan.current(table,id);
                point["pos_ticks"]=edit::addTime(detail::signedInteger(point.at("pos_ticks")),ticks);plan.set(table,id,point);
            }
        }
        SQLite::Statement markers(db,"SELECT id FROM markers ORDER BY id");
        while(markers.executeStep()) {
            const auto id=markers.getColumn(0).getInt64();auto row=*plan.current("markers",id);
            const bool musical=row.at("time_base")==0;
            const char* position=musical?"pos_ticks":"pos_ns";const char* length=musical?"length_ticks":"length_ns";
            const auto at=musical?atTicks:atNs,gap=musical?ticks:gapNs,first=detail::signedInteger(row.at(position));
            if(first>=at)row[position]=edit::addTime(first,gap);
            else if(!row.at(length).is_null() && edit::addTime(first,detail::signedInteger(row.at(length)))>at)
                row[length]=edit::addTime(detail::signedInteger(row.at(length)),gap);
            plan.set("markers",id,row);
        }
        SQLite::Statement sections(db,"SELECT id FROM arranger_sections ORDER BY id");
        while(sections.executeStep()) {
            const auto id=sections.getColumn(0).getInt64();auto row=*plan.current("arranger_sections",id);
            for(const char* endpoint:{"start_ticks","end_ticks"})
                if(detail::signedInteger(row.at(endpoint))>=atTicks)
                    row[endpoint]=edit::addTime(detail::signedInteger(row.at(endpoint)),ticks);
            plan.set("arranger_sections",id,row);
        }
        edit::insertAutomationTime(store,plan,ids,time,atTicks,ticks,atNs,gapNs,referenceRate);
        auto project=plan.current("project",1);
        if(project && detail::signedInteger(project->at("length_ticks"))>0 &&
           detail::signedInteger(project->at("length_ticks"))>=atTicks) {
            (*project)["length_ticks"]=edit::addTime(detail::signedInteger(project->at("length_ticks")),ticks);plan.set("project",1,project);
        }
        packet(store,result,"arranger.splice","Insert Silence",0,plan);
        SQLite::Statement loop(db,"SELECT value FROM session_state WHERE key='transport.loop'");
        if(loop.executeStep()) {
            const auto before=loop.getColumn(0).getString();auto value=Payload::parse(before);
            const auto first=detail::signedInteger(value.at("start")),last=detail::signedInteger(value.at("end"));
            if(!value.at("on").is_boolean() || first<0 || last<=first)throw std::runtime_error("stored loop region is invalid");
            if(first>=atTicks)value["start"]=edit::addTime(first,ticks);
            if(last>atTicks || first>=atTicks)value["end"]=edit::addTime(last,ticks);
            if(value!=Payload::parse(before)) {
                if(result.requests.empty()) {
                    OpRequest request;request.opType="arranger.splice";request.payload={{"id",0},{"changes",Payload::array()}};
                    request.label="Insert Silence";result.requests.push_back(std::move(request));
                }
                result.requests[0].payload["loop"]={{"before",before},{"after",value.dump()}};
            }
        }
    } catch(const std::exception& exception){result.ok=false;result.error=exception.what();}
    return result;
}
} // namespace
PreparedEdit prepareInsertSilence(Store& store,std::int64_t atTicks,std::int64_t ticks,double referenceRate) {
    return prepareInsertion(store,atTicks,ticks,referenceRate,std::nullopt);
}

PreparedEdit prepareDeleteTime(Store& store,std::int64_t beginTicks,std::int64_t endTicks,double referenceRate) {
    PreparedEdit result;
    try {
        validRate(referenceRate);
        if(beginTicks<0 || endTicks<=beginTicks)throw std::runtime_error("Delete Time needs ordered nonnegative endpoints");
        Read read(store.db());expectedHead(store,result);auto& db=store.db();
        edit::TimeMap time(db);edit::RowEdits plan(db);CopyIds ids(db);
        const auto beginNs=time.nanos(beginTicks),endNs=time.nanos(endTicks);
        if(endNs<=beginNs)throw std::runtime_error("Delete Time is shorter than a nanosecond");
        auto collapse=[](std::int64_t value,std::int64_t begin,std::int64_t end) {
            if(value<0)throw std::runtime_error("Delete Time needs nonnegative placement");
            return value<begin?value:value>=end?value-(end-begin):begin;
        };
        std::vector<std::int64_t> tracks;
        SQLite::Statement trackRows(db,"SELECT id FROM tracks ORDER BY id");
        while(trackRows.executeStep())tracks.push_back(trackRows.getColumn(0).getInt64());
        // The same allocator spans trimming and relocation, including newly
        // created right pieces. No intermediate edit is committed.
        if(!tracks.empty())eraseRange(store,plan,ids,{tracks,{0,beginTicks},{0,endTicks}},time,referenceRate,ClipLaneScope::AllLanes);
        std::set<std::int64_t> candidates;
        SQLite::Statement clips(db,"SELECT id FROM clips ORDER BY id");
        while(clips.executeStep())candidates.insert(clips.getColumn(0).getInt64());
        for(const auto& change:plan.changes())if(change.at("table")=="clips" && !change.at("after").is_null())
            candidates.insert(detail::signedInteger(change.at("id")));
        const bool properties=db.tableExists("clip_properties");OpContext context{store,db};
        for(const auto id:candidates) {
            auto core=plan.current("clips",id);if(!core)continue;
            const int base=static_cast<int>(detail::signedInteger(core->at("time_base")));
            if(base!=0 && base!=1)throw std::runtime_error("Delete Time found an unknown clip clock");
            const char* position=base==0?"pos_ticks":"pos_ns";
            const char* length=base==0?"length_ticks":"length_ns";
            if(core->at(length).is_null() || detail::signedInteger(core->at(length))<0)
                throw std::runtime_error("Delete Time needs finite nonnegative clip extents");
            const auto first=detail::signedInteger(core->at(position)),begin=base==0?beginTicks:beginNs,end=base==0?endTicks:endNs;
            if(first<0)throw std::runtime_error("Delete Time needs nonnegative clip placement");
            if(first>=begin && first<end && detail::signedInteger(core->at(length))==0) {
                deleteImage(plan,captureDeleted(context,id));continue;
            }
            if(first<end)continue;
            Payload image={{"id",id},{"row",*core},{"children",Payload::object()}};
            for(const char* child:{"audio_clips","clip_playback_details","clip_warp_fade_domains","clip_properties"}) {
                image["children"][child]=Payload::array();
                if(std::string(child)=="clip_properties" && !properties)continue;
                if(const auto value=plan.current(child,id))image["children"][child].push_back(*value);
            }
            // Resolve the old source clock before changing its timeline origin.
            freezeAudio(store,image,time,referenceRate);
            image["row"][position]=first-(end-begin);plan.set("clips",id,image.at("row"));
            retainDetails(plan,image);
        }
        for(const char* table:{"tempo_map","time_signature_map","key_map"}) {
            std::vector<Payload> points;
            SQLite::Statement rows(db,"SELECT id FROM "+std::string(table)+" ORDER BY pos_ticks,id");
            while(rows.executeStep())points.push_back(*plan.current(table,rows.getColumn(0).getInt64()));
            std::optional<std::int64_t> active;
            for(const auto& point:points)if(detail::signedInteger(point.at("pos_ticks"))<=endTicks)
                active=detail::signedInteger(point.at("id"));
            for(auto point:points) {
                const auto id=detail::signedInteger(point.at("id")),position=detail::signedInteger(point.at("pos_ticks"));
                if(position>=beginTicks && position<endTicks && active!=id) {
                    if(std::string(table)=="key_map") {
                        SQLite::Statement degrees(db,"SELECT degree FROM key_map_degrees WHERE key_map_id=?");degrees.bind(1,id);
                        while(degrees.executeStep())plan.setKey("key_map_degrees",{{"key_map_id",id},{"degree",degrees.getColumn(0).getInt64()}},std::nullopt);
                        if(plan.current("key_map_tunings",id))plan.set("key_map_tunings",id,std::nullopt);
                    }
                    plan.set(table,id,std::nullopt);
                } else if(position>=beginTicks) {
                    point["pos_ticks"]=collapse(position,beginTicks,endTicks);plan.set(table,id,point);
                }
            }
        }
        SQLite::Statement markers(db,"SELECT id FROM markers ORDER BY id");
        while(markers.executeStep()) {
            const auto id=markers.getColumn(0).getInt64();auto row=*plan.current("markers",id);
            const auto base=detail::signedInteger(row.at("time_base"));
            if(base!=0 && base!=1)throw std::runtime_error("Delete Time found an unknown marker clock");
            const char* position=base==0?"pos_ticks":"pos_ns";const char* length=base==0?"length_ticks":"length_ns";
            const auto begin=base==0?beginTicks:beginNs,end=base==0?endTicks:endNs,first=detail::signedInteger(row.at(position));
            bool remove=false;
            if(row.at(length).is_null())remove=first>=begin && first<end;
            else {
                const auto extent=detail::signedInteger(row.at(length));
                if(extent<0)throw std::runtime_error("Delete Time found a negative marker span");
                const auto last=edit::addTime(first,extent),mapped=collapse(last,begin,end)-collapse(first,begin,end);
                remove=mapped==0 && first>=begin && last<=end;
                row[length]=mapped;
            }
            if(remove) {
                if(db.tableExists("marker_details") && plan.current("marker_details",id))plan.set("marker_details",id,std::nullopt);
                plan.set("markers",id,std::nullopt);
            } else {row[position]=collapse(first,begin,end);plan.set("markers",id,row);}
        }
        SQLite::Statement sections(db,"SELECT id FROM arranger_sections ORDER BY id");
        while(sections.executeStep()) {
            const auto id=sections.getColumn(0).getInt64();auto row=*plan.current("arranger_sections",id);
            const auto first=collapse(detail::signedInteger(row.at("start_ticks")),beginTicks,endTicks);
            const auto last=collapse(detail::signedInteger(row.at("end_ticks")),beginTicks,endTicks);
            if(last<=first) {
                SQLite::Statement chain(db,"SELECT id FROM arranger_chain WHERE section_id=?");chain.bind(1,id);
                while(chain.executeStep())plan.set("arranger_chain",chain.getColumn(0).getInt64(),std::nullopt);
                plan.set("arranger_sections",id,std::nullopt);
            } else {row["start_ticks"]=first;row["end_ticks"]=last;plan.set("arranger_sections",id,row);}
        }
        edit::deleteAutomationTime(store,plan,ids,time,beginTicks,endTicks,beginNs,endNs,referenceRate);
        if(auto project=plan.current("project",1);project && detail::signedInteger(project->at("length_ticks"))>0) {
            (*project)["length_ticks"]=collapse(detail::signedInteger(project->at("length_ticks")),beginTicks,endTicks);plan.set("project",1,project);
        }
        packet(store,result,"arranger.splice","Delete Time",0,plan);
        SQLite::Statement loop(db,"SELECT value FROM session_state WHERE key='transport.loop'");
        if(loop.executeStep()) {
            const auto before=loop.getColumn(0).getString();auto value=Payload::parse(before);
            const auto first=detail::signedInteger(value.at("start")),last=detail::signedInteger(value.at("end"));
            if(!value.at("on").is_boolean() || first<0 || last<=first)throw std::runtime_error("stored loop region is invalid");
            const auto mappedFirst=collapse(first,beginTicks,endTicks),mappedLast=collapse(last,beginTicks,endTicks);
            value["start"]=mappedFirst;value["end"]=mappedLast;
            if(mappedLast<=mappedFirst) {
                // Until measured against Live: disable a fully removed loop,
                // retaining a positive bracket so it can be enabled again.
                value["on"]=false;value["end"]=edit::addTime(mappedFirst,last-first);
            }
            if(value!=Payload::parse(before)) {
                if(result.requests.empty()) {
                    OpRequest request;request.opType="arranger.splice";request.payload={{"id",0},{"changes",Payload::array()}};
                    request.label="Delete Time";result.requests.push_back(std::move(request));
                }
                result.requests[0].payload["loop"]={{"before",before},{"after",value.dump()}};
            }
        }
    } catch(const std::exception& exception){result.ok=false;result.error=exception.what();}
    return result;
}

namespace {
RangeCopy copyRangeImpl(Store& store,const RangeSelection& selection,double referenceRate,ClipLaneScope scope) {
    RangeCopy result;result.selection=selection;
    try {
        validRange(selection);validRate(referenceRate);Read read(store.db());
        result.sourceFile=filename(store);edit::TimeMap time(store.db());CopyIds ids(store.db());OpContext context{store,store.db()};
        for(std::size_t track=0;track<selection.tracks.size();++track) {
            SQLite::Statement clips(store.db(),clipRangeQuery(scope));clips.bind(1,selection.tracks[track]);
            while(clips.executeStep()) {
                auto image=captureDeleted(context,clips.getColumn(0).getInt64());captureMediaKeys(store,image);
                const int base=static_cast<int>(detail::signedInteger(image.at("row").at("time_base")));
                const char* position=base==0?"pos_ticks":"pos_ns";const char* length=base==0?"length_ticks":"length_ns";
                const auto begin=coordinate(selection.begin,base,time),end=coordinate(selection.end,base,time);
                const auto first=detail::signedInteger(image.at("row").at(position));
                if(image.at("row").at(length).is_null())throw std::runtime_error("copy range needs finite clip extents");
                const auto last=edit::addTime(first,detail::signedInteger(image.at("row").at(length)));
                if(last<=begin || first>=end)continue;
                freezeAudio(store,image,time,referenceRate);
                if(first<begin)image=splitImage(store,image,ids.next("clips"),{base,begin},time,referenceRate,ids);
                if(last>end)(void)splitImage(store,image,ids.next("clips"),{base,end},time,referenceRate,ids);
                image["row"][position]=detail::signedInteger(image.at("row").at(position))-begin;
                image["trackIndex"]=track;result.images.push_back(std::move(image));
            }
        }
        result.ok=true;
    } catch(const std::exception& exception){result.ok=false;result.error=exception.what();}
    return result;
}
} // namespace
RangeCopy copyRange(Store& store,const RangeSelection& selection,double referenceRate) {
    return copyRangeImpl(store,selection,referenceRate,ClipLaneScope::MainLane);
}
RangeCut prepareCutRange(Store& store,const RangeSelection& selection,double referenceRate) {
    RangeCut result;
    try {
        Read read(store.db());result.copy=copyRange(store,selection,referenceRate);
        if(!result.copy.ok)throw std::runtime_error(result.copy.error);
        result.edit=prepareDeleteRange(store,selection,referenceRate);
        for(auto& request:result.edit.requests)request.label="Cut Material";
    } catch(const std::exception& exception){result.edit.ok=false;result.edit.error=exception.what();}
    return result;
}
PreparedEdit prepareDeleteRange(Store& store,const RangeSelection& selection,double referenceRate,std::int64_t reserveClipId) {
    PreparedEdit result;
    try {
        validRange(selection);validRate(referenceRate);Read read(store.db());expectedHead(store,result);
        edit::TimeMap time(store.db());CopyIds ids(store.db());ids.reserve("clips",reserveClipId);edit::RowEdits plan(store.db());
        eraseRange(store,plan,ids,selection,time,referenceRate,ClipLaneScope::MainLane);packet(store,result,"arranger.splice","Delete Material",0,plan);
    } catch(const std::exception& exception){result.ok=false;result.error=exception.what();}
    return result;
}
PreparedEdit preparePasteRange(Store& store,const RangeCopy& copy,const std::vector<std::int64_t>& tracks,
                                TimePoint at,double referenceRate,bool insertTime) {
    PreparedEdit result;
    try {
        if(!copy.ok)throw std::runtime_error(copy.error);
        validRange(copy.selection);validRate(referenceRate);Read read(store.db());expectedHead(store,result);
        if(copy.sourceFile!=filename(store))throw std::runtime_error("range clipboard belongs to a different project");
        if(tracks.size()!=copy.selection.tracks.size())throw std::runtime_error("paste needs one destination for each copied track");
        edit::TimeMap time(store.db());edit::RowEdits plan(store.db());CopyIds ids(store.db());
        std::optional<Payload> insertedLoop;
        const auto begin=coordinate(at,copy.selection.begin.base,time);
        const auto span=copy.selection.end.value-copy.selection.begin.value;
        RangeSelection destination{tracks,{copy.selection.begin.base,begin},{copy.selection.begin.base,edit::addTime(begin,span)}};
        validRange(destination);
        if(insertTime) {
            if(copy.selection.begin.base!=0)throw std::runtime_error("Paste Time requires a musical time selection");
            const auto insertion=prepareInsertSilence(store,begin,span,referenceRate);
            if(!insertion.ok)throw std::runtime_error(insertion.error);
            for(const auto& request:insertion.requests)if(request.payload.contains("loop"))insertedLoop=request.payload.at("loop");
            for(const auto& request:insertion.requests)for(const auto& change:request.payload.at("changes")) {
                const auto table=change.at("table").get<std::string>();const auto id=detail::signedInteger(change.at("id"));
                plan.set(table,id,change.at("after").is_null()?std::optional<Payload>{}:std::optional<Payload>{change.at("after")});
                if(!change.at("after").is_null() && std::string(edit::rowSpec(table).key)=="id")ids.reserve(table,id);
            }
        } else eraseRange(store,plan,ids,destination,time,referenceRate,ClipLaneScope::MainLane);
        // Range clipping can assign an identity above every stored clip.
        // Reserve those clipboard identities as well as live/insertion ids.
        for(const auto& image:copy.images)ids.reserve("clips",detail::signedInteger(image.at("id")));
        for(const auto& original:copy.images) {
            verifyMediaKeys(store,original);
            auto image=cloneImage(original,ids.next("clips"),ids);
            const auto index=original.at("trackIndex").get<std::size_t>();
            if(index>=tracks.size())throw std::runtime_error("clipboard track offset is invalid");
            auto& core=image["row"];const int base=static_cast<int>(detail::signedInteger(core.at("time_base")));
            const char* position=base==0?"pos_ticks":"pos_ns";
            core[position]=edit::addTime(coordinate(at,base,time),detail::signedInteger(core.at(position)));
            core["track_id"]=tracks[index];core["lane_id"]=nullptr; // source comp lanes do not belong to a destination track
            createImage(plan,image);
        }
        packet(store,result,"arranger.splice",insertTime?"Paste Time":"Paste Material",0,plan);
        if(insertedLoop && !result.requests.empty())result.requests[0].payload["loop"]=*insertedLoop;
    } catch(const std::exception& exception){result.ok=false;result.error=exception.what();}
    return result;
}
PreparedEdit prepareDuplicateRange(Store& store,const RangeSelection& selection,double referenceRate,bool insertTime) {
    try {
        Read read(store.db());const auto copy=copyRange(store,selection,referenceRate);
        auto result=preparePasteRange(store,copy,selection.tracks,selection.end,referenceRate,insertTime);
        if(result.ok && !result.requests.empty())result.requests[0].label=insertTime?"Duplicate Time":"Duplicate Material";
        return result;
    } catch(const std::exception& exception){PreparedEdit result;result.error=exception.what();return result;}
}
namespace {
Payload mapImage(Store& store,const char* table,const Payload& row,bool exists=true) {
    Payload result={{"table",table},{"row",row},{"tuning",nullptr},{"degrees",Payload::array()}};
    if(std::string(table)!="key_map" || !exists)return result;
    const auto id=detail::signedInteger(row.at("id"));
    if(auto tuning=edit::row(store.db(),"key_map_tunings",id)) {
        result["tuning"]=*tuning;
        SQLite::Statement degrees(store.db(),"SELECT degree FROM key_map_degrees WHERE key_map_id=? ORDER BY degree");degrees.bind(1,id);
        while(degrees.executeStep())result["degrees"].push_back(*edit::rowKey(store.db(),"key_map_degrees",{{"key_map_id",id},{"degree",degrees.getColumn(0).getInt64()}}));
    }
    return result;
}
Payload mapContext(Store& store,const char* table,std::int64_t position) {
    SQLite::Statement context(store.db(),"SELECT id FROM "+std::string(table)+" WHERE pos_ticks<=? ORDER BY pos_ticks DESC,id DESC LIMIT 1");context.bind(1,position);
    if(context.executeStep())return mapImage(store,table,*edit::row(store.db(),table,context.getColumn(0).getInt64()));
    Payload defaults={{"id",1},{"pos_ticks",0}};
    if(std::string(table)=="tempo_map")defaults["bpm"]=120.0;
    else if(std::string(table)=="time_signature_map"){defaults["numerator"]=4;defaults["denominator"]=4;}
    else defaults["root"]=0;
    return mapImage(store,table,edit::defaultRow(store.db(),table,defaults),false);
}
Payload tuningKey(Store& store,std::int64_t id) {
    auto row=detail::captureRow(store.db(),"tuning_systems","id",id);
    if(!row)throw std::runtime_error("copied time has a missing tuning");
    Payload key={{"row",*row},{"degrees",Payload::array()}};
    SQLite::Statement degrees(store.db(),"SELECT degree FROM tuning_degrees WHERE tuning_id=? ORDER BY degree");degrees.bind(1,id);
    while(degrees.executeStep())key["degrees"].push_back(*detail::captureRowKey(store.db(),"tuning_degrees",{{"tuning_id",id},{"degree",degrees.getColumn(0).getInt64()}}));
    return key;
}
void clearKeyBinding(Store& store,edit::RowEdits& plan,std::int64_t id) {
    SQLite::Statement degrees(store.db(),"SELECT degree FROM key_map_degrees WHERE key_map_id=?");degrees.bind(1,id);
    while(degrees.executeStep())plan.setKey("key_map_degrees",{{"key_map_id",id},{"degree",degrees.getColumn(0).getInt64()}},std::nullopt);
    if(plan.current("key_map_tunings",id))plan.set("key_map_tunings",id,std::nullopt);
}
void placeMap(edit::RowEdits& plan,const Payload& image,std::int64_t id,std::int64_t position) {
    const auto table=image.at("table").get<std::string>();auto row=image.at("row");row["id"]=id;row["pos_ticks"]=position;
    plan.set(table,id,row);
    if(table=="key_map" && !image.at("tuning").is_null()) {
        auto binding=image.at("tuning");binding["key_map_id"]=id;plan.set("key_map_tunings",id,binding);
        for(auto degree:image.at("degrees")) {
            degree["key_map_id"]=id;plan.setKey("key_map_degrees",{{"key_map_id",id},{"degree",degree.at("degree")}},degree);
        }
    }
}
void mergePrepared(edit::RowEdits& plan,CopyIds& ids,const PreparedEdit& prepared,std::optional<Payload>& loop) {
    if(!prepared.ok)throw std::runtime_error(prepared.error);
    for(const auto& request:prepared.requests) {
        if(request.payload.contains("loop"))loop=request.payload.at("loop");
        for(const auto& change:request.payload.at("changes")) {
            const auto table=change.at("table").get<std::string>();const auto& key=change.at("id");const auto& value=change.at("after");
            const std::optional<Payload> after=value.is_null()?std::optional<Payload>{}:std::optional<Payload>{value};
            if(key.is_object())plan.setKey(table,key,after);else plan.set(table,detail::signedInteger(key),after);
            if(after && std::string(edit::rowSpec(table).key)=="id")ids.reserve(table,detail::signedInteger(after->at("id")));
        }
    }
}
}
TimeCopy copyTime(Store& store,std::int64_t beginTicks,std::int64_t endTicks,double referenceRate) {
    TimeCopy result;
    try {
        validRate(referenceRate);if(beginTicks<0 || endTicks<=beginTicks)throw std::runtime_error("Copy Time needs ordered nonnegative endpoints");
        Read read(store.db());auto& db=store.db();edit::TimeMap time(db);
        const auto beginNs=time.nanos(beginTicks),endNs=time.nanos(endTicks);
        result.ticks=endTicks-beginTicks;result.nanos=endNs-beginNs;result.referenceRate=referenceRate;
        if(result.nanos<=0)throw std::runtime_error("Copy Time is shorter than a nanosecond");
        std::vector<std::int64_t> tracks;
        SQLite::Statement rows(db,"SELECT id FROM tracks ORDER BY id");while(rows.executeStep())tracks.push_back(rows.getColumn(0).getInt64());
        if(!tracks.empty())result.material=copyRangeImpl(store,{tracks,{0,beginTicks},{0,endTicks}},referenceRate,ClipLaneScope::AllLanes);
        else {result.material.ok=true;result.material.sourceFile=filename(store);result.material.selection={{},{0,beginTicks},{0,endTicks}};}
        if(!result.material.ok)throw std::runtime_error(result.material.error);
        auto& timeline=result.timeline;timeline={{"maps",Payload::array()},{"tuningKeys",Payload::array()},{"markers",Payload::array()},{"sections",Payload::array()},{"tracks",Payload::array()}};
        for(const auto id:tracks) {
            const auto row=detail::captureRow(db,"tracks","id",id);timeline["tracks"].push_back({{"id",id},{"kind",row->at("kind")}});
        }
        std::set<std::int64_t> tunings;
        auto appendMap=[&](Payload image,std::int64_t position) {
            image["row"]["pos_ticks"]=position;
            if(!image.at("tuning").is_null())tunings.insert(detail::signedInteger(image.at("tuning").at("tuning_id")));
            timeline["maps"].push_back(std::move(image));
        };
        for(const char* table:{"tempo_map","time_signature_map","key_map"}) {
            appendMap(mapContext(store,table,beginTicks),0);
            SQLite::Statement points(db,"SELECT id FROM "+std::string(table)+" WHERE pos_ticks>? AND pos_ticks<? ORDER BY pos_ticks,id");points.bind(1,beginTicks);points.bind(2,endTicks);
            while(points.executeStep()) {
                auto image=mapImage(store,table,*edit::row(db,table,points.getColumn(0).getInt64()));
                const auto position=detail::signedInteger(image.at("row").at("pos_ticks"));appendMap(std::move(image),position-beginTicks);
            }
        }
        for(const auto id:tunings)timeline["tuningKeys"].push_back(tuningKey(store,id));
        SQLite::Statement markers(db,"SELECT id FROM markers ORDER BY id");
        while(markers.executeStep()) {
            const auto id=markers.getColumn(0).getInt64();auto row=*edit::row(db,"markers",id);
            const auto base=detail::signedInteger(row.at("time_base"));if(base!=0 && base!=1)throw std::runtime_error("copied marker has an unknown clock");
            const char* position=base==0?"pos_ticks":"pos_ns";const char* length=base==0?"length_ticks":"length_ns";
            const auto begin=base==0?beginTicks:beginNs,end=base==0?endTicks:endNs,first=detail::signedInteger(row.at(position));
            if(first<0)throw std::runtime_error("copied marker needs nonnegative placement");
            if(row.at(length).is_null()){if(first<begin || first>=end)continue;row[position]=first-begin;}
            else {
                const auto extent=detail::signedInteger(row.at(length));if(extent<0)throw std::runtime_error("copied marker needs a nonnegative span");
                const auto last=edit::addTime(first,extent);if(last<=begin || first>=end)continue;
                const auto clipped=std::max(first,begin);row[position]=clipped-begin;row[length]=std::min(last,end)-clipped;
            }
            std::optional<Payload> details;if(db.tableExists("marker_details"))details=edit::row(db,"marker_details",id);
            timeline["markers"].push_back({{"row",row},{"details",details.value_or(Payload{})}});
        }
        SQLite::Statement sections(db,"SELECT id FROM arranger_sections WHERE start_ticks<? AND end_ticks>? ORDER BY start_ticks,id");sections.bind(1,endTicks);sections.bind(2,beginTicks);
        while(sections.executeStep()) {
            auto row=*edit::row(db,"arranger_sections",sections.getColumn(0).getInt64());
            row["start_ticks"]=std::max(beginTicks,detail::signedInteger(row.at("start_ticks")))-beginTicks;
            row["end_ticks"]=std::min(endTicks,detail::signedInteger(row.at("end_ticks")))-beginTicks;timeline["sections"].push_back(std::move(row));
        }
        timeline["automation"]=edit::copyAutomationTime(store,time,beginTicks,endTicks,beginNs,endNs,referenceRate);
        result.ok=true;
    } catch(const std::exception& exception){result.ok=false;result.error=exception.what();}
    return result;
}
TimeCut prepareCutTime(Store& store,std::int64_t beginTicks,std::int64_t endTicks,double referenceRate) {
    TimeCut result;
    try {
        Read read(store.db());result.copy=copyTime(store,beginTicks,endTicks,referenceRate);
        if(!result.copy.ok)throw std::runtime_error(result.copy.error);
        result.edit=prepareDeleteTime(store,beginTicks,endTicks,referenceRate);
        for(auto& request:result.edit.requests)request.label="Cut Time";
    } catch(const std::exception& exception){result.edit.ok=false;result.edit.error=exception.what();}
    return result;
}
PreparedEdit preparePasteTime(Store& store,const TimeCopy& copy,std::int64_t atTicks,double referenceRate) {
    PreparedEdit result;
    try {
        if(!copy.ok)throw std::runtime_error(copy.error);
        validRate(referenceRate);
        if(atTicks<0 || copy.ticks<=0 || copy.nanos<=0 || referenceRate!=copy.referenceRate)
            throw std::runtime_error("Paste Time needs a valid span and the clipboard's reference render rate");
        const auto endTicks=edit::addTime(atTicks,copy.ticks);
        Read read(store.db());expectedHead(store,result);auto& db=store.db();
        if(copy.material.sourceFile!=filename(store))throw std::runtime_error("time clipboard belongs to a different project");
        for(const auto& track:copy.timeline.at("tracks")) {
            const auto current=detail::captureRow(db,"tracks","id",detail::signedInteger(track.at("id")));
            if(!current || current->at("kind")!=track.at("kind"))throw std::runtime_error("time clipboard track no longer has its captured kind");
        }
        for(const auto& key:copy.timeline.at("tuningKeys"))
            if(tuningKey(store,detail::signedInteger(key.at("row").at("id")))!=key)throw std::runtime_error("time clipboard tuning identity changed");
        edit::TimeMap time(db);const auto atNs=time.nanos(atTicks);(void)edit::addTime(atNs,copy.nanos);
        edit::RowEdits plan(db);CopyIds ids(db);std::optional<Payload> loop;
        mergePrepared(plan,ids,prepareInsertion(store,atTicks,copy.ticks,referenceRate,copy.nanos),loop);
        for(const auto& image:copy.material.images)ids.reserve("clips",detail::signedInteger(image.at("id")));
        for(const auto& original:copy.material.images) {
            verifyMediaKeys(store,original);
            const auto track=original.at("trackIndex").get<std::size_t>();if(track>=copy.material.selection.tracks.size())throw std::runtime_error("clipboard track index differs");
            auto image=cloneImage(original,ids.next("clips"),ids);auto& core=image["row"];
            const auto base=detail::signedInteger(core.at("time_base"));if(base!=0 && base!=1)throw std::runtime_error("copied clip has an unknown clock");
            const char* position=base==0?"pos_ticks":"pos_ns";
            core[position]=edit::addTime(base==0?atTicks:atNs,detail::signedInteger(core.at(position)));
            core["track_id"]=copy.material.selection.tracks[track];core["lane_id"]=nullptr;createImage(plan,image);
        }
        for(const char* table:{"tempo_map","time_signature_map","key_map"}) {
            const auto target=mapContext(store,table,atTicks);bool restored=false;
            // A real boundary event (except the protected root) moved to the
            // far join. Its original binding already restores the destination.
            SQLite::Statement boundary(db,"SELECT id FROM "+std::string(table)+" WHERE pos_ticks=? AND pos_ticks<>0");boundary.bind(1,atTicks);restored=boundary.executeStep();
            for(const auto& image:copy.timeline.at("maps"))if(image.at("table")==table) {
                const auto relative=detail::signedInteger(image.at("row").at("pos_ticks"));
                if(relative<0 || relative>=copy.ticks)throw std::runtime_error("copied map point exceeds the clipboard span");
                std::optional<std::int64_t> id;
                if(atTicks==0 && relative==0) {
                    SQLite::Statement root(db,"SELECT id FROM "+std::string(table)+" WHERE pos_ticks=0");
                    if(root.executeStep())id=root.getColumn(0).getInt64();
                }
                if(!id)id=ids.next(table);else if(std::string(table)=="key_map")clearKeyBinding(store,plan,*id);
                placeMap(plan,image,*id,edit::addTime(atTicks,relative));
            }
            if(!restored)placeMap(plan,target,ids.next(table),endTicks);
        }
        for(const auto& marker:copy.timeline.at("markers")) {
            const auto id=ids.next("markers");auto row=marker.at("row");
            const auto base=detail::signedInteger(row.at("time_base"));
            const char* position=base==0?"pos_ticks":"pos_ns";row["id"]=id;
            row[position]=edit::addTime(base==0?atTicks:atNs,detail::signedInteger(row.at(position)));plan.set("markers",id,row);
            if(!marker.at("details").is_null()){auto details=marker.at("details");details["marker_id"]=id;plan.set("marker_details",id,details);}
        }
        for(auto row:copy.timeline.at("sections")) {
            const auto id=ids.next("arranger_sections");row["id"]=id;
            row["start_ticks"]=edit::addTime(atTicks,detail::signedInteger(row.at("start_ticks")));
            row["end_ticks"]=edit::addTime(atTicks,detail::signedInteger(row.at("end_ticks")));plan.set("arranger_sections",id,row);
        }
        edit::pasteAutomationTime(store,plan,ids,copy.timeline.at("automation"),atTicks,copy.ticks,atNs,copy.nanos,referenceRate);
        if(auto project=plan.current("project",1);project && detail::signedInteger(project->at("length_ticks"))>0 &&
           detail::signedInteger(project->at("length_ticks"))<endTicks) {
            (*project)["length_ticks"]=endTicks;plan.set("project",1,project);
        }
        packet(store,result,"arranger.splice","Paste Time",0,plan);
        if(loop && !result.requests.empty())result.requests[0].payload["loop"]=*loop;
    } catch(const std::exception& exception){result.ok=false;result.error=exception.what();}
    return result;
}
PreparedEdit prepareDuplicateTime(Store& store,std::int64_t beginTicks,std::int64_t endTicks,double referenceRate) {
    try {
        Read read(store.db());const auto copy=copyTime(store,beginTicks,endTicks,referenceRate);
        auto result=preparePasteTime(store,copy,endTicks,referenceRate);
        for(auto& request:result.requests)request.label="Duplicate Time";
        return result;
    } catch(const std::exception& exception){PreparedEdit result;result.error=exception.what();return result;}
}

CommitResult commitPrepared(Store& store,const PreparedEdit& prepared,Actor actor) {
    CommitResult result;
    if(!prepared.ok){result.error=prepared.error;return result;}
    if(filename(store)!=prepared.sourceFile){result.error="project changed after edit preparation";result.stale=true;return result;}
    if(prepared.requests.empty()){result.ok=true;return result;}
    auto requests=prepared.requests;for(auto& request:requests)request.actor=actor;
    return OpJournal(store).commit(requests,prepared.options);
}
} // namespace adi::clip_edit
