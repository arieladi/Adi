// SPDX-License-Identifier: AGPL-3.0-or-later
#include "adi/clip_commands.hpp"
#include "adi/audio/progressive_decode.hpp"
#include "adi/audio/warp.hpp"
#include "adi/audio/warp_map.hpp"
#include "adi/clip_image.hpp"
#include "adi/automation_time_edit.hpp"
#include "adi/automation_clipboard.hpp"
#include "adi/edit_time.hpp"
#include "adi/edit_plan.hpp"
#include "adi/media/media_ops.hpp"
#include "adi/note_stream_detail.hpp"
#include "adi/row_capture.hpp"
#include "adi/store.hpp"
#include <SQLiteCpp/SQLiteCpp.h>
#include <sqlite3.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <map>
#include <numeric>
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
    return static_cast<double>(edit::addTime(detail::signedInteger(row.at("pos_ns")),
        detail::signedInteger(row.at("length_ns"))))*1e-9;
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
    const auto span=last-origin;
    const auto in=detail::signedInteger(row.at("fade_in_ticks"));
    const auto out=std::min(span,detail::signedInteger(row.at("fade_out_ticks")));
    if(in<0 || out<0 || in>span)throw std::runtime_error("warped clip fades exceed its visible musical span");
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
            time.ticks(edit::addTime(detail::signedInteger(row.at("pos_ns")),detail::signedInteger(row.at("length_ns"))));
        const auto length=fadeOut>lastTick-origin?span:
            std::min(span,sample(time.seconds(lastTick)-time.seconds(lastTick-fadeOut),rate));
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
    bool rightBeforeEnd = true;
};
ImageCut calculateImageCut(Payload& image,TimePoint point,
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
    const auto splitTick=base==0?result.cut:time.ticks(result.cut);
    const auto firstTick=originTicks(old,time),lastTick=base==0?result.last:time.ticks(result.last);
    result.beforeTicks=splitTick-firstTick;result.afterTicks=lastTick-splitTick;
    if(result.beforeTicks<0 || result.afterTicks<0)throw std::runtime_error("split has an invalid musical span");
    result.fadeIn=detail::signedInteger(old.at("fade_in_ticks"));
    result.fadeOut=detail::signedInteger(old.at("fade_out_ticks"));
    if(result.fadeIn<0 || result.fadeOut<0)throw std::runtime_error("split needs valid existing fades");
    result.rightOffsetTicks=edit::addTime(detail::signedInteger(old.at("content_offset_ticks")),result.beforeTicks);
    const auto& children = image.at("children");
    std::optional<bool> beforePhysicalEnd;
    if(kind=="audio") {
        const auto* clock=playback(image);
        if(clock && !clock->at("content_offset_ns").is_null()) {
            const auto offset=sample(static_cast<double>(detail::signedInteger(clock->at("content_offset_ns")))*1e-9,referenceRate);
            const double cutSeconds=base==0?time.seconds(result.cut):static_cast<double>(result.cut)*1e-9;
            const auto passed=sample(cutSeconds,referenceRate)-sample(startSeconds(old,time),referenceRate);
            const auto rightSample=edit::addTime(offset,passed);
            result.rightOffsetNs=sampleNanos(rightSample,referenceRate);
            const auto& sources=children.at("audio_clips");
            if (old.at("loop_enabled")==0 && !sources.empty() && sources.front().at("warp_enabled")==0 &&
                !clock->at("source_end_ns").is_null())
                beforePhysicalEnd=rightSample < sample(
                    static_cast<double>(detail::signedInteger(clock->at("source_end_ns")))*1e-9,
                    referenceRate);
        }
    }
    if (old.at("loop_enabled")==0) {
        if (beforePhysicalEnd) {
            result.rightBeforeEnd=*beforePhysicalEnd;
        } else if (children.contains("clip_properties") && !children.at("clip_properties").empty()) {
            const auto& marker=children.at("clip_properties").front().at("end_marker_ticks");
            if (!marker.is_null()) result.rightBeforeEnd=result.rightOffsetTicks < detail::signedInteger(marker);
        }
    }
    return result;
}
ImageCut prepareImageCut(Store& store,Payload& image,TimePoint point,
                         const edit::TimeMap& time,double referenceRate) {
    freezeAudio(store,image,time,referenceRate);
    return calculateImageCut(image,point,time,referenceRate);
}
void keepLeft(Payload& image,const ImageCut& cut) {
    auto& row=image["row"];
    row[cut.length]=cut.cut-cut.first;row["fade_out_ticks"]=0;
    row["fade_in_ticks"]=std::min(cut.fadeIn,cut.beforeTicks);
}
void keepRight(Payload& image,const ImageCut& cut) {
    if (!cut.rightBeforeEnd) throw std::runtime_error("right clip must start before its source End");
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
using ClockRange=std::pair<std::int64_t,std::int64_t>;
struct ReverseWindow {std::int64_t start,end;bool loop;};
ReverseWindow reverseWindow(std::int64_t size,std::int64_t offset,std::int64_t span,
                            std::optional<std::int64_t> end,std::optional<ClockRange> loop) {
    if(offset<0 || span<=0)throw std::runtime_error("audio does not cover the selected range");
    const auto endOfSelection=edit::addTime(offset,span);
    if(loop) {
        const auto [first,last]=*loop;
        if(first<0 || last<=first || last>size)throw std::runtime_error("clip loop is outside its source");
        if(offset<first) {
            // The clip plays contiguous material until its first wrap. Reversed
            // repeats followed by an outro need more than one clip window.
            if(endOfSelection>last)throw std::runtime_error("Select either the intro or the repeating loop to reverse it.");
            return {size-endOfSelection,size-offset,false};
        }
        const auto final=first+(endOfSelection-1-first)%(last-first);
        const auto start=size-1-final;
        // A split on a later repeat can store an unrolled Start beyond the
        // file. Use the mirrored loop End when that Start cannot give an End.
        const auto mirroredEnd=size-offset;
        return {start,mirroredEnd>start?mirroredEnd:size-first,true};
    }
    const auto through=std::min({size,endOfSelection,end.value_or(size)});
    if(through<=offset)throw std::runtime_error("audio does not cover the selected range");
    return {size-through,size-offset,false};
}
void shiftFadeClocks(Payload& row,Payload& clocks,const char* in,const char* inEnd,const char* out,const char* outEnd,
                     std::int64_t delta,std::int64_t start) {
    for(const auto& pair:{std::pair{in,inEnd},std::pair{out,outEnd}}) {
        if(clocks.at(pair.first).is_null())continue;
        const auto begin=edit::addTime(detail::signedInteger(clocks.at(pair.first)),delta);
        const auto end=edit::addTime(detail::signedInteger(clocks.at(pair.second)),delta);
        if(begin<0) {
            // A split can retain a fade-in that already finished before this
            // piece began. It has no gain effect in the reversed placement.
            if(pair.first==in && end<=start) {
                clocks[pair.first]=nullptr;clocks[pair.second]=nullptr;
                row["fade_in_ticks"]=0;
                continue;
            }
            throw std::runtime_error("Reverse cannot retain this clip's fade clock");
        }
        clocks[pair.first]=begin;clocks[pair.second]=end;
    }
}
std::int64_t sourceSampleRate(Store& store,std::int64_t id) {
    SQLite::Statement row(store.db(),"SELECT sample_rate FROM media_files WHERE id=?");row.bind(1,id);
    if(!row.executeStep())throw std::runtime_error("clip media is missing");
    if(!row.getColumn(0).isNull() && row.getColumn(0).getInt64()>0)return row.getColumn(0).getInt64();
    std::string error;
    const auto path=media::resolveMedia(media::projectFolder(store),media::mediaRow(store.db(),id),error);
    if(path.empty())throw std::runtime_error(error.empty()?"clip media is missing":error);
    return audio::inspectPcm(path).sampleRate;
}
void reverseImage(Store& store,Payload& image,const edit::TimeMap& time,double rate) {
    auto& row=image.at("row");
    auto& children=image.at("children");
    auto& sources=children.at("audio_clips");
    if(row.at("kind")!="audio" || !row.at("alias_of").is_null() || sources.size()!=1)
        throw std::runtime_error("Reverse needs attached, independent audio clips");
    freezeAudio(store,image,time,rate);
    auto& source=sources[0];
    const auto frames=detail::signedInteger(source.at("src_len_frames"));
    if(frames<=0)throw std::runtime_error("audio source window is empty");
    const auto origin=originTicks(row,time);
    const auto offsetTicks=detail::signedInteger(row.at("content_offset_ticks"));
    auto& properties=children.at("clip_properties");
    auto* end=properties.empty()?nullptr:&properties[0].at("end_marker_ticks");
    const bool hasEnd=end && !end->is_null();
    const auto loopTicks=detail::signedInteger(row.at("loop_start_ticks"));
    const auto& loopLength=row.at("loop_len_ticks");
    const bool looping=row.at("loop_enabled")!=0;
    const bool hasLoop=!loopLength.is_null() && detail::signedInteger(loopLength)>0;
    if(looping && !hasLoop)throw std::runtime_error("clip has an invalid loop");
    if(source.at("warp_enabled")!=0) {
        if(!source.at("warp_markers").is_binary())throw std::runtime_error("Reverse needs the clip's Warp map");
        const auto& bytes=source.at("warp_markers").get_binary();
        const auto markers=audio::readWarpMap(std::as_bytes(std::span(bytes)),static_cast<std::uint64_t>(frames));
        if(!markers)throw std::runtime_error("Reverse needs a readable Warp map");
        const auto size=markers.markers.back().tick;
        const auto span=row.at("time_base")==0?detail::signedInteger(row.at("length_ticks")):
            time.ticks(edit::addTime(detail::signedInteger(row.at("pos_ns")),detail::signedInteger(row.at("length_ns"))))-origin;
        const auto lastLoop=hasLoop?edit::addTime(loopTicks,detail::signedInteger(loopLength)):0;
        const auto window=reverseWindow(size,offsetTicks,span,hasEnd?std::optional{detail::signedInteger(*end)}:std::nullopt,
            looping?std::optional{ClockRange{loopTicks,lastLoop}}:std::nullopt);
        row["content_offset_ticks"]=window.start;
        row["loop_enabled"]=window.loop?1:0;
        if(hasEnd)*end=window.end;
        if(hasLoop && loopTicks>=0 && lastLoop<=size)row["loop_start_ticks"]=size-lastLoop;
        for(auto& clocks:children.at("clip_warp_fade_domains"))
            shiftFadeClocks(row,clocks,"in_begin_ticks","in_end_ticks","out_begin_ticks","out_end_ticks",
                            window.start-offsetTicks,window.start);
    } else {
        auto& clocks=ensurePlayback(store,image);
        const auto clockSample=[&](const char* column) {
            return sample(static_cast<double>(detail::signedInteger(clocks.at(column)))*1e-9,rate);
        };
        const auto offset=clockSample("content_offset_ns");
        const auto physicalEnd=clocks.at("source_end_ns").is_null()?std::nullopt:std::optional{clockSample("source_end_ns")};
        const auto span=sample(endSeconds(row,time),rate)-sample(startSeconds(row,time),rate);
        const auto sourceRate=sourceSampleRate(store,detail::signedInteger(source.at("media_id")));
        if(sourceRate<=0)throw std::runtime_error("audio source sample rate is unavailable");
        const auto size=sample(static_cast<double>(frames)/static_cast<double>(sourceRate),rate);
        const auto authoredLoop=looping?std::optional{ClockRange{clockSample("loop_begin_ns"),clockSample("loop_end_ns")}}:std::nullopt;
        const auto window=reverseWindow(size,offset,span,physicalEnd,authoredLoop);
        const auto ticksAt=[&](std::int64_t at){return time.ticks(edit::addTime(time.nanos(origin),sampleNanos(at,rate)))-origin;};
        const auto startNs=sampleNanos(window.start,rate);
        shiftFadeClocks(row,clocks,"in_begin_ns","in_end_ns","out_begin_ns","out_end_ns",
                       startNs-detail::signedInteger(clocks.at("content_offset_ns")),startNs);
        row["content_offset_ticks"]=ticksAt(window.start);
        row["loop_enabled"]=window.loop?1:0;
        clocks["content_offset_ns"]=startNs;
        if(physicalEnd)clocks["source_end_ns"]=sampleNanos(window.end,rate);
        if(hasEnd)*end=ticksAt(window.end);
        if(authoredLoop) {
            const auto [first,last]=*authoredLoop;
            clocks["loop_begin_ns"]=sampleNanos(size-last,rate);
            clocks["loop_end_ns"]=sampleNanos(size-first,rate);
            row["loop_start_ticks"]=ticksAt(size-last);
            row["loop_len_ticks"]=std::max<std::int64_t>(1,ticksAt(size-first)-ticksAt(size-last));
        }
    }
    source["reverse"]=source.at("reverse")==0?1:0;
}
void validRange(const RangeSelection& range) {
    if(range.begin.base!=range.end.base || (range.begin.base!=0 && range.begin.base!=1) ||
       range.begin.value<0 || range.end.value<=range.begin.value || range.tracks.empty())
        throw std::runtime_error("range needs ordered endpoints in one clock and selected tracks");
    std::set<std::int64_t> tracks;
    for(const auto id:range.tracks)if(!tracks.insert(id).second)throw std::runtime_error("range has duplicate tracks");
}
enum class ClipLaneScope { MainLane, AllLanes, OneTake };
const char* clipRangeQuery(ClipLaneScope scope) {
    switch (scope) {
    case ClipLaneScope::MainLane:
        return "SELECT id FROM clips WHERE track_id=? AND lane_id IS NULL ORDER BY id";
    case ClipLaneScope::OneTake:
        return "SELECT id FROM clips WHERE track_id=? AND lane_id=? ORDER BY id";
    case ClipLaneScope::AllLanes:
        return "SELECT id FROM clips WHERE track_id=? ORDER BY id";
    }
    throw std::logic_error("unknown clip lane scope");
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
PreparedEdit prepareSetSource(Store& store,std::int64_t clipId,std::int64_t mediaId,
                              std::int64_t frames,std::int64_t sampleRate,OpRequest capturedMedia) {
    PreparedEdit result;
    try {
        if(mediaId<=0 || frames<=0 || sampleRate<=0 || sampleRate>768000)
            throw std::runtime_error("replacement must be a readable audio file");
        if(!capturedMedia.opType.empty() && ((capturedMedia.opType!="media.import" && capturedMedia.opType!="media.relink") ||
                                           detail::signedInteger(capturedMedia.payload.at("id"))!=mediaId))
            throw std::runtime_error("replacement media capture differs from its source");
        Read read(store.db());expectedHead(store,result);
        OpContext context{store,store.db()};
        const auto clip=captureDeleted(context,clipId);
        const auto& core=clip.at("row");
        const auto& audio=clip.at("children").at("audio_clips");
        if(core.at("kind")!="audio" || !core.at("alias_of").is_null() || audio.size()!=1)
            throw std::runtime_error("choose one audio clip with its own sample");
        auto source=audio[0];
        const auto oldId=detail::signedInteger(source.at("media_id"));
        if(mediaId!=oldId) {
            const auto oldStart=detail::signedInteger(source.at("src_start_frames"));
            const auto oldLength=detail::signedInteger(source.at("src_len_frames"));
            if(oldStart<0 || oldLength<=0)
                throw std::runtime_error("the original clip lacks a valid source window");
            SQLite::Statement oldMedia(store.db(),"SELECT frames,sample_rate FROM media_files WHERE id=?");
            oldMedia.bind(1,oldId);
            if(!oldMedia.executeStep())throw std::runtime_error("the original clip has no media row");
            const auto oldFrames=oldMedia.getColumn(0).getInt64(),oldRate=oldMedia.getColumn(1).getInt64();
            bool sameDuration=false;
            if(oldFrames>0 && oldRate>0) {
                const auto oldDivisor=std::gcd(oldFrames,oldRate),newDivisor=std::gcd(frames,sampleRate);
                sameDuration=oldFrames/oldDivisor==frames/newDivisor && oldRate/oldDivisor==sampleRate/newDivisor;
            }
            auto& stored=source.at("warp_markers");
            if(!sameDuration || oldRate!=sampleRate) {
                const auto rescale=[&](std::int64_t frame) {
                    return std::llround(static_cast<long double>(frame)*static_cast<long double>(sampleRate)/
                                        static_cast<long double>(oldRate));
                };
                const auto first=sameDuration?rescale(oldStart):0;
                const auto count=sameDuration?rescale(edit::addTime(oldStart,oldLength))-first:frames;
                source["src_start_frames"]=first;
                source["src_len_frames"]=count;
                if(source.at("warp_enabled")!=0 || (sameDuration && !stored.is_null())) {
                    if(!stored.is_binary())throw std::runtime_error("Warp replacement needs a readable source map");
                    const auto& bytes=stored.get_binary();
                    const std::span raw(reinterpret_cast<const std::byte*>(bytes.data()),bytes.size());
                    auto map=audio::readWarpMap(raw,static_cast<std::uint64_t>(oldLength));
                    if(!map || map.futureLayout)throw std::runtime_error("Warp replacement cannot rewrite this source map");
                    if(sameDuration) {
                        for(auto& marker:map.markers)marker.sourceFrame=rescale(marker.sourceFrame);
                        map.markers.back().sourceFrame=count;
                    } else {
                        // Keep the clip's musical source span and replace the hand-aligned points with a fresh linear map.
                        map.markers={{0,0},{count,map.markers.back().tick}};
                    }
                    const auto replacement=audio::writeWarpMap(map.markers,static_cast<std::uint64_t>(count));
                    if(!replacement)throw std::runtime_error("replacement cannot carry a valid Warp map");
                    const auto* begin=reinterpret_cast<const std::uint8_t*>(replacement.bytes.data());
                    stored=Payload::binary(std::vector<std::uint8_t>(begin,begin+replacement.bytes.size()));
                } else {
                    stored=nullptr;
                }
            }
            // A legacy Warp-off loop can wrap its offset or fall back to the
            // start of the file. The retained Start must be playable for a one-shot.
            if(source.at("warp_enabled")==0 && core.at("loop_enabled")==0) {
                const auto rate=audio::pitchedSampleRate(static_cast<double>(sampleRate),source.at("transpose_semis").get<double>());
                const auto& clocks=clip.at("children").at("clip_playback_details");
                long double retainedStart;
                if(!clocks.empty() && !clocks.front().at("content_offset_ns").is_null()) {
                    retainedStart=static_cast<long double>(detail::signedInteger(clocks.front().at("content_offset_ns")))/1e9L;
                } else {
                    const edit::TimeMap time(store.db());
                    retainedStart=time.durationSeconds(originTicks(core,time),detail::signedInteger(core.at("content_offset_ticks")));
                }
                const auto window=detail::signedInteger(source.at("src_len_frames"));
                if(rate>0 && retainedStart>=static_cast<long double>(window)/rate)
                    throw std::runtime_error("The replacement sample ends at or before the clip's Start at its current Pitch.");
            }
            source["media_id"]=mediaId;
        }
        if(!capturedMedia.opType.empty()) {
            capturedMedia.label="Replace Clip Sample";
            result.requests.push_back(std::move(capturedMedia));
        }
        edit::RowEdits rows(store.db());rows.set("audio_clips",clipId,std::move(source));
        packet(store,result,"audioClip.setSource","Replace Clip Sample",clipId,rows);
    } catch(const std::exception& exception){result.error=exception.what();}
    return result;
}
ClipCopy copyClip(Store& store,std::int64_t id,double referenceRate) {
    return copyClip(store,id,edit_plan::Options{referenceRate,{}});
}
ClipCopy copyClip(Store& store,std::int64_t id,const edit_plan::Options& options) {
    const auto referenceRate=options.referenceRate;
    ClipCopy result;
    try {
        validRate(referenceRate);Read read(store.db());OpContext context{store,store.db()};
        result.sourceFile=filename(store);result.image=captureDeleted(context,id);captureMediaKeys(store,result.image);
        edit::TimeMap time(store.db());freezeAudio(store,result.image,time,referenceRate);
        const auto& row=result.image.at("row");
        if(options.policy.automationFollowsClips && row.at("lane_id").is_null()) {
            const auto base=static_cast<int>(detail::signedInteger(row.at("time_base")));
            const auto first=detail::signedInteger(row.at(base==0?"pos_ticks":"pos_ns"));
            const auto& length=row.at(base==0?"length_ticks":"length_ns");
            if(!length.is_null() && detail::signedInteger(length)>0)
                result.envelopes=edit::copyAutomationMaterial(store,time,detail::signedInteger(row.at("track_id")),
                    {base,first},{base,edit::addTime(first,detail::signedInteger(length))},referenceRate);
        }
        result.ok=true;
    } catch(const std::exception& exception){result.ok=false;result.error=exception.what();}
    return result;
}
ClipCut prepareCutClip(Store& store,std::int64_t id,double referenceRate) {
    return prepareCutClip(store,id,edit_plan::Options{referenceRate,{}});
}
ClipCut prepareCutClip(Store& store,std::int64_t id,const edit_plan::Options& options) {
    ClipCut result;
    try {
        Read read(store.db());result.copy=copyClip(store,id,options);
        if(!result.copy.ok)throw std::runtime_error(result.copy.error);
        const std::array clips{id};
        result.edit=edit_plan::prepareDelete(store,clips,options);
        for(auto& request:result.edit.requests)request.label="Cut Clip";
    } catch(const std::exception& exception){result.edit.error=exception.what();}
    return result;
}
PreparedEdit preparePaste(Store& store,const ClipCopy& copy,std::int64_t newId,std::int64_t track,TimePoint point,double referenceRate) {
    return preparePaste(store,copy,newId,track,point,edit_plan::Options{referenceRate,{}});
}
PreparedEdit preparePaste(Store& store,const ClipCopy& copy,std::int64_t newId,std::int64_t track,TimePoint point,const edit_plan::Options& options) {
    PreparedEdit result;
    try {
        validRate(options.referenceRate);if(!copy.ok)throw std::runtime_error(copy.error);
        Read read(store.db());
        if(filename(store)!=copy.sourceFile)throw std::runtime_error("clip clipboard belongs to a different project");
        verifyMediaKeys(store,copy.image);
        CopyIds ids(store.db());auto image=cloneImage(copy.image,newId,ids);edit::TimeMap time(store.db());
        relocate(image["row"],track,point,time);
        image["row"]["lane_id"]=nullptr;
        const auto sourceTrack=detail::signedInteger(copy.image.at("row").at("track_id"));
        const auto& core=image.at("row");const int base=static_cast<int>(detail::signedInteger(core.at("time_base")));
        const auto first=detail::signedInteger(core.at(base==0?"pos_ticks":"pos_ns"));
        const auto last=edit::addTime(first,detail::signedInteger(core.at(base==0?"length_ticks":"length_ns")));
        const auto oldFirst=detail::signedInteger(copy.image.at("row").at(base==0?"pos_ticks":"pos_ns"));
        const auto oldLast=edit::addTime(oldFirst,detail::signedInteger(copy.image.at("row").at(base==0?"length_ticks":"length_ns")));
        std::vector<std::int64_t> tracks{track};
        if(track!=sourceTrack && detail::captureRow(store.db(),"tracks","id",sourceTrack))tracks.push_back(sourceTrack);
        auto end=std::max(last,oldLast);const auto begin=std::min(first,oldFirst);
        if(end==begin)end=edit::addTime(end,1);
        auto scope=edit_plan::materialScope(tracks,base==0?edit_plan::PlacementClock::Ticks:edit_plan::PlacementClock::Nanoseconds,{begin,end});
        edit_plan::detail::PlacementBatch batch;batch.incoming.push_back(std::move(image));batch.sourceTracks.push_back(sourceTrack);
        if(!copy.envelopes.is_null())batch.envelopes.push_back({track,{base,first},{base,last},copy.envelopes});
        result=edit_plan::detail::prepareCaptured(store,scope,std::move(batch),options,ids,"Paste Clip");
    } catch(const std::exception& exception){result.ok=false;result.error=exception.what();}
    return result;
}
PreparedEdit prepareDuplicate(Store& store,std::int64_t sourceId,std::int64_t newId,std::int64_t track,TimePoint point,double referenceRate) {
    return prepareDuplicate(store,sourceId,newId,track,point,edit_plan::Options{referenceRate,{}});
}
PreparedEdit prepareDuplicate(Store& store,std::int64_t sourceId,std::int64_t newId,std::int64_t track,TimePoint point,const edit_plan::Options& options) {
    // Both reads share one snapshot; no second writer can change material
    // between capturing the copy and capturing the expected undo head.
    try {
        Read read(store.db());auto copy=copyClip(store,sourceId,options);
        auto result=preparePaste(store,copy,newId,track,point,options);
        if(result.ok && !result.requests.empty())result.requests[0].label="Duplicate Clip";
        return result;
    } catch(const std::exception& exception){PreparedEdit result;result.error=exception.what();return result;}
}
PreparedEdit prepareDuplicates(Store& store,std::span<const ClipPlacement> places,const edit_plan::Options& options) {
    // preparePaste for several clips in one batch: each copy clears what it
    // lands on; the scope covers every source and destination, in nanoseconds.
    PreparedEdit result;
    try {
        validRate(options.referenceRate);if(places.empty())throw std::runtime_error("no clip to duplicate");
        Read read(store.db());edit::TimeMap time(store.db());CopyIds ids(store.db());
        for(const auto& place:places)ids.reserve("clips",place.newId);
        std::set<std::int64_t> tracks;auto begin=std::numeric_limits<std::int64_t>::max();std::int64_t end=0;
        edit_plan::detail::PlacementBatch batch;
        for(const auto& place:places) {
            const auto copy=copyClip(store,place.sourceId,options);
            if(!copy.ok)throw std::runtime_error(copy.error);
            verifyMediaKeys(store,copy.image);
            auto image=cloneImage(copy.image,place.newId,ids);
            relocate(image["row"],place.track,place.point,time);image["row"]["lane_id"]=nullptr;
            const std::array<const Payload*,2> spans{&copy.image.at("row"),&image.at("row")};
            for(const auto* row:spans) {
                const int base=static_cast<int>(detail::signedInteger(row->at("time_base")));
                const auto first=detail::signedInteger(row->at(base==0?"pos_ticks":"pos_ns"));
                const auto last=edit::addTime(first,detail::signedInteger(row->at(base==0?"length_ticks":"length_ns")));
                begin=std::min(begin,coordinate({base,first},1,time));end=std::max(end,coordinate({base,last},1,time));
            }
            const auto sourceTrack=detail::signedInteger(copy.image.at("row").at("track_id"));
            tracks.insert(place.track);
            if(sourceTrack!=place.track && detail::captureRow(store.db(),"tracks","id",sourceTrack))tracks.insert(sourceTrack);
            const auto& row=image.at("row");
            const auto base=static_cast<int>(detail::signedInteger(row.at("time_base")));
            const auto first=detail::signedInteger(row.at(base==0?"pos_ticks":"pos_ns"));
            const auto last=edit::addTime(first,detail::signedInteger(row.at(base==0?"length_ticks":"length_ns")));
            if(!copy.envelopes.is_null())batch.envelopes.push_back({place.track,{base,first},{base,last},copy.envelopes});
            batch.sourceTracks.push_back(sourceTrack);batch.incoming.push_back(std::move(image));
        }
        const std::vector<std::int64_t> literals(tracks.begin(),tracks.end());
        const auto scope=edit_plan::materialScope(literals,edit_plan::PlacementClock::Nanoseconds,
            {begin,std::max(end,edit::addTime(begin,1))});
        result=edit_plan::detail::prepareCaptured(store,scope,std::move(batch),options,ids,
            places.size()==1?"Duplicate Clip":"Duplicate Clips");
    } catch(const std::exception& exception){result.ok=false;result.error=exception.what();result.requests.clear();}
    return result;
}
namespace {
OpRequest captureSplit(Store& store,const ClipSplit& split,std::int64_t firstId,
                       std::optional<std::int64_t> secondId,const edit::TimeMap& time,
                       double rate,CopyIds& ids) {
    auto first=split.first;
    auto second=split.second;
    if(second) {
        if(split.clipId==firstId || split.clipId==*secondId || firstId==*secondId)
            throw std::runtime_error("split needs three distinct clip identities");
        if(first.base!=second->base || (first.base!=0 && first.base!=1) || first.value<0 || second->value<=first.value)
            throw std::runtime_error("split needs ordered endpoints in one clock");
    }
    OpContext context{store,store.db()};
    auto image=captureDeleted(context,split.clipId);
    if(second) {
        const int base=static_cast<int>(detail::signedInteger(image.at("row").at("time_base")));
        first={base,coordinate(first,base,time)};
        second=TimePoint{base,coordinate(*second,base,time)};
        if(second->value<=first.value)throw std::runtime_error("split endpoints coincide in the clip's clock");
    }
    auto next=splitImage(store,image,firstId,first,time,rate,ids);
    std::optional<Payload> tail;
    if(second)tail=splitImage(store,next,*secondId,*second,time,rate,ids);
    edit::RowEdits plan(store.db());
    plan.set("clips",split.clipId,image.at("row"));
    retainDetails(plan,image);
    createImage(plan,next);
    if(tail)createImage(plan,*tail);
    OpRequest request;
    request.opType="clip.split";
    request.payload={{"id",split.clipId},{"changes",plan.changes()}};
    request.label="Split Clip";
    request.targetKind="clip";request.targetId=split.clipId;
    return request;
}

PreparedEdit prepareOneSplit(Store& store,const ClipSplit& split,std::int64_t firstId,
                             std::optional<std::int64_t> secondId,double referenceRate) {
    PreparedEdit result;
    try {
        validRate(referenceRate);Read read(store.db());expectedHead(store,result);
        result.sourceFile=filename(store);
        const edit::TimeMap time(store.db());
        CopyIds ids(store.db());
        result.requests.push_back(captureSplit(store,split,firstId,secondId,time,referenceRate,ids));
        result.ok=true;
    } catch(const std::exception& exception){result.error=exception.what();}
    return result;
}
}
PreparedEdit prepareSplit(Store& store,std::int64_t sourceId,std::int64_t rightId,TimePoint point,double referenceRate) {
    return prepareOneSplit(store,{sourceId,point,{}},rightId,{},referenceRate);
}
PreparedEdit prepareSplit(Store& store,std::int64_t sourceId,std::int64_t middleId,std::int64_t rightId,
                          TimePoint begin,TimePoint end,double referenceRate) {
    return prepareOneSplit(store,{sourceId,begin,end},middleId,rightId,referenceRate);
}
PreparedSplits prepareSplits(Store& store,std::span<const ClipSplit> splits,double referenceRate) {
    PreparedSplits result;
    try {
        validRate(referenceRate);Read read(store.db());expectedHead(store,result.edit);
        result.edit.sourceFile=filename(store);
        const edit::TimeMap time(store.db());
        std::optional<CopyIds> ids(std::in_place,store.db());
        std::set<std::int64_t> seen;
        for(const auto& split:splits) {
            try {
                if(!seen.insert(split.clipId).second)throw std::runtime_error("clip appears twice in the split selection");
                const auto firstId=ids->next("clips");
                std::optional<std::int64_t> secondId;
                if(split.second)secondId=ids->next("clips");
                auto request=captureSplit(store,split,firstId,secondId,time,referenceRate,*ids);
                result.edit.requests.push_back(std::move(request));
            } catch(const std::exception& exception){
                result.refused.push_back({split.clipId,exception.what()});
                // A refused split may have reserved the last available ID.
                // Carry only the actual insertions from accepted requests.
                ids.emplace(store.db());
                for(const auto& accepted:result.edit.requests)for(const auto& change:accepted.payload.at("changes")) {
                    const auto& table=change.at("table").get_ref<const std::string&>();
                    if(change.at("before").is_null() && !change.at("after").is_null() &&
                       std::string_view(edit::rowSpec(table).key)=="id")
                        ids->reserve(table,detail::signedInteger(change.at("id")));
                }
            }
        }
        if(result.edit.requests.empty()) {
            result.edit.error=result.refused.empty()?"split selection is empty":result.refused.front().error;
            return result;
        }
        if(result.edit.requests.size()>1)for(auto& request:result.edit.requests)request.label="Split Clips";
        result.edit.ok=true;
    } catch(const std::exception& exception){result.edit.error=exception.what();}
    return result;
}
PreparedEdit prepareReverse(Store& store,std::span<const std::int64_t> clipIds,
    std::optional<std::pair<std::int64_t,std::int64_t>> range,double referenceRate) {
    PreparedEdit result;
    try {
        validRate(referenceRate);
        if(clipIds.empty() || (range && (range->first<0 || range->second<=range->first)))
            throw std::runtime_error("Select audio clips or an audio time range to reverse");
        Read read(store.db());expectedHead(store,result);
        const edit::TimeMap time(store.db());edit::RowEdits plan(store.db());CopyIds ids(store.db());
        OpContext context{store,store.db()};std::set<std::int64_t> seen;
        for(const auto id:clipIds) {
            if(!seen.insert(id).second)continue;
            auto original=captureDeleted(context,id);
            Payload* selected=&original;
            std::optional<Payload> middle,tail;
            if(range) {
                const auto& row=original.at("row");
                const int base=static_cast<int>(detail::signedInteger(row.at("time_base")));
                const auto from=coordinate({0,range->first},base,time),to=coordinate({0,range->second},base,time);
                const auto begin=detail::signedInteger(row.at(base==0?"pos_ticks":"pos_ns"));
                const auto end=edit::addTime(begin,detail::signedInteger(row.at(base==0?"length_ticks":"length_ns")));
                if(from>=end || to<=begin || from>=to)continue;
                if(from>begin) {
                    middle=splitImage(store,original,ids.next("clips"),{base,from},time,referenceRate,ids);
                    selected=&*middle;
                }
                if(to<end)tail=splitImage(store,*selected,ids.next("clips"),{base,to},time,referenceRate,ids);
            }
            reverseImage(store,*selected,time,referenceRate);
            plan.set("clips",id,original.at("row"));
            retainDetails(plan,original);
            if(middle)createImage(plan,*middle);
            else {
                plan.set("audio_clips",id,original.at("children").at("audio_clips")[0]);
                const auto& properties=original.at("children").at("clip_properties");
                if(!properties.empty())plan.set("clip_properties",id,properties[0]);
            }
            if(tail)createImage(plan,*tail);
        }
        packet(store,result,"arranger.splice","Reverse Clip(s)",clipIds.front(),plan);
        if(result.requests.empty()) {result.ok=false;result.error="No audio covers the selected range";}
    } catch(const std::exception& exception){result.error=exception.what();}
    return result;
}
namespace {
PreparedEdit prepareInsertion(Store& store,std::int64_t atTicks,std::int64_t ticks,double referenceRate,
                              std::optional<std::int64_t> exactNanos,bool keepMeterAtStart=false) {
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
                const auto position=detail::signedInteger(point.at("pos_ticks"));
                if(keepMeterAtStart && position==atTicks && std::string_view(table)=="time_signature_map")continue;
                point["pos_ticks"]=edit::addTime(position,ticks);plan.set(table,id,point);
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
PreparedEdit prepareCompleteFragmentaryBar(Store& store,std::int64_t atTicks,std::int64_t missingTicks,double referenceRate) {
    return prepareInsertion(store,atTicks,missingTicks,referenceRate,std::nullopt,true);
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
RangeCopy copyRangeImpl(Store& store,const RangeSelection& selection,double referenceRate,ClipLaneScope scope,
                        std::optional<std::int64_t> lane={},std::span<const std::int64_t> selectedClips={},
                        bool copyClipEnvelopes=false,bool rejectLinked=false) {
    RangeCopy result;result.selection=selection;result.clearsRange=selectedClips.empty();
    try {
        validRange(selection);validRate(referenceRate);Read read(store.db());
        result.sourceFile=filename(store);edit::TimeMap time(store.db());CopyIds ids(store.db());OpContext context{store,store.db()};
        for(std::size_t track=0;track<selection.tracks.size();++track) {
            SQLite::Statement clips(store.db(),clipRangeQuery(scope));clips.bind(1,selection.tracks[track]);
            if (lane)
                clips.bind(2, *lane);
            while(clips.executeStep()) {
                const auto id=clips.getColumn(0).getInt64();
                if(!result.clearsRange && std::find(selectedClips.begin(),selectedClips.end(),id)==selectedClips.end())
                    continue;
                const auto root=*edit::row(store.db(),"clips",id);
                const int base=static_cast<int>(detail::signedInteger(root.at("time_base")));
                const char* position=base==0?"pos_ticks":"pos_ns";const char* length=base==0?"length_ticks":"length_ns";
                const auto begin=coordinate(selection.begin,base,time),end=coordinate(selection.end,base,time);
                const auto first=detail::signedInteger(root.at(position));
                if(result.clearsRange && first>=end)continue;
                if(root.at(length).is_null())throw std::runtime_error("copy range needs finite clip extents");
                const auto last=edit::addTime(first,detail::signedInteger(root.at(length)));
                if(result.clearsRange && last<=begin)continue;
                auto image=captureDeleted(context,id);
                if(rejectLinked && (!root.at("alias_of").is_null() || !image.at("alias_links").empty()))
                    throw std::runtime_error("Moving a time selection with linked clips is not supported.");
                captureMediaKeys(store,image);
                freezeAudio(store,image,time,referenceRate);
                if(result.clearsRange && first<begin)image=splitImage(store,image,ids.next("clips"),{base,begin},time,referenceRate,ids);
                if(result.clearsRange && last>end)(void)splitImage(store,image,ids.next("clips"),{base,end},time,referenceRate,ids);
                image["row"][position]=detail::signedInteger(image.at("row").at(position))-begin;
                if(copyClipEnvelopes)
                    result.clipEnvelopes.push_back(edit::copyAutomationMaterial(store,time,selection.tracks[track],
                        {base,first},{base,last},referenceRate));
                image["trackIndex"]=track;result.images.push_back(std::move(image));
            }
        }
        if(!result.clearsRange && result.images.size()!=selectedClips.size())
            throw std::runtime_error("A selected clip is no longer in the Arrangement.");
        result.ok=true;
    } catch(const std::exception& exception){result.ok=false;result.error=exception.what();}
    return result;
}
void captureRangeEnvelopes(Store& store,RangeCopy& copy,const edit::TimeMap& time,double referenceRate) {
    for(const auto track:copy.selection.tracks)
        copy.envelopes.push_back(edit::copyAutomationMaterial(store,time,track,
            copy.selection.begin,copy.selection.end,referenceRate));
}
} // namespace
RangeCopy copyRange(Store& store,const RangeSelection& selection,double referenceRate) {
    return copyRange(store,selection,edit_plan::Options{referenceRate,{}});
}
RangeCopy copyRange(Store& store,const RangeSelection& selection,const edit_plan::Options& options) {
    RangeCopy result;
    try {
        Read read(store.db());
        result=copyRangeImpl(store,selection,options.referenceRate,ClipLaneScope::MainLane);
        if(result.ok && options.policy.automationFollowsClips) {
            const edit::TimeMap time(store.db());
            captureRangeEnvelopes(store,result,time,options.referenceRate);
        }
    } catch(const std::exception& exception){result.ok=false;result.error=exception.what();}
    return result;
}
RangeCopy copySelectedClips(Store& store,const RangeSelection& selection,std::span<const std::int64_t> clips,
                            const edit_plan::Options& options) {
    if(clips.empty()) {
        RangeCopy result;result.error="Select clips to copy.";return result;
    }
    return copyRangeImpl(store,selection,options.referenceRate,ClipLaneScope::MainLane,{},clips,
                         options.policy.automationFollowsClips);
}
RangeCut prepareCutClips(Store& store,const RangeSelection& selection,std::span<const std::int64_t> clips,
                         const edit_plan::Options& options) {
    RangeCut result;
    try {
        Read read(store.db());result.copy=copySelectedClips(store,selection,clips,options);
        if(!result.copy.ok)throw std::runtime_error(result.copy.error);
        result.edit=edit_plan::prepareDelete(store,clips,options);
        for(auto& request:result.edit.requests)request.label="Cut Clips";
    } catch(const std::exception& exception){result.edit.error=exception.what();}
    return result;
}
RangeCut prepareCutRange(Store& store,const RangeSelection& selection,double referenceRate) {
    return prepareCutRange(store,selection,edit_plan::Options{referenceRate,{}});
}
RangeCut prepareCutRange(Store& store,const RangeSelection& selection,const edit_plan::Options& options) {
    RangeCut result;
    try {
        Read read(store.db());result.copy=copyRange(store,selection,options);
        if(!result.copy.ok)throw std::runtime_error(result.copy.error);
        result.edit=prepareDeleteRange(store,selection,options);
        for(auto& request:result.edit.requests)request.label="Cut Material";
    } catch(const std::exception& exception){result.edit.ok=false;result.edit.error=exception.what();}
    return result;
}
PreparedEdit prepareDeleteRange(Store& store,const RangeSelection& selection,double referenceRate,std::int64_t reserveClipId) {
    return prepareDeleteRange(store,selection,edit_plan::Options{referenceRate,{}},reserveClipId);
}
PreparedEdit prepareDeleteRange(Store& store,const RangeSelection& selection,const edit_plan::Options& options,std::int64_t reserveClipId) {
    PreparedEdit result;
    try {
        validRange(selection);Read read(store.db());CopyIds ids(store.db());ids.reserve("clips",reserveClipId);
        const auto clock=selection.begin.base==0?edit_plan::PlacementClock::Ticks:edit_plan::PlacementClock::Nanoseconds;
        const auto scope=edit_plan::materialScope(selection.tracks,clock,{selection.begin.value,selection.end.value});
        edit_plan::detail::PlacementBatch batch;
        for(const auto& lane:scope.targets) {
            batch.clear.push_back({lane,scope.bounds});
            if(options.policy.automationFollowsClips)
                batch.envelopes.push_back({lane.track,selection.begin,selection.end,Payload::array()});
        }
        result=edit_plan::detail::prepareCaptured(store,scope,std::move(batch),options,ids,"Delete Material");
    } catch(const std::exception& exception){result.ok=false;result.error=exception.what();}
    return result;
}
MidiConsolidation prepareMidiConsolidation(Store& store, const RangeSelection& selection,
    std::span<const std::int64_t> picked, const edit_plan::Options& options) {
    MidiConsolidation result;
    try {
        validRange(selection);
        constexpr std::int64_t tickLimit = std::int64_t{1} << 50;
        if (selection.tracks.size() != 1 || selection.begin.base != 0 || selection.end.value > tickLimit)
            throw std::runtime_error("Consolidate currently needs a musical time range on one MIDI track.");
        const auto trackId = selection.tracks.front();
        auto& db = store.db();
        Read read(db);
        SQLite::Statement track(db, "SELECT kind FROM tracks WHERE id=?");
        track.bind(1, trackId);
        if (!track.executeStep() || (track.getColumn(0).getString() != "midi" &&
                                     track.getColumn(0).getString() != "instrument"))
            throw std::runtime_error("Consolidate currently supports MIDI tracks only.");
        CopyIds ids(db);
        const auto output = ids.next("clips");
        struct PositionedNote {
            notes::Record record;
            notes::NoteValues values;
        };
        struct CapturedClip {
            std::vector<PositionedNote> notes;
            Payload expression;
        };
        std::vector<CapturedClip> sources;
        std::set<std::uint64_t> reservedNoteIds;
        std::string name;
        OpContext context{store,db};
        edit::TimeMap time(db);
        SQLite::Statement clips(db, "SELECT id FROM clips WHERE track_id=? AND lane_id IS NULL ORDER BY pos_ticks,id");
        clips.bind(1, trackId);
        while (clips.executeStep()) {
            const auto id = clips.getColumn(0).getInt64();
            const auto row = *edit::row(db, "clips", id);
            const int base = static_cast<int>(detail::signedInteger(row.at("time_base")));
            const auto first = detail::signedInteger(row.at(base == 0 ? "pos_ticks" : "pos_ns"));
            const auto end = coordinate(selection.end, base, time);
            if (first >= end)
                continue;
            const auto length = detail::signedInteger(row.at(base == 0 ? "length_ticks" : "length_ns"));
            const auto last = edit::addTime(first, length);
            const auto begin = coordinate(selection.begin, base, time);
            if (last <= begin)
                continue;
            if (!picked.empty() && std::find(picked.begin(), picked.end(), id) == picked.end())
                throw std::runtime_error("Consolidate would cover an unselected clip.");
            if (base != 0 || length <= 0 || first < begin || last > end || row.at("kind") != "midi")
                throw std::runtime_error("Consolidate currently needs whole MIDI clips on the musical timeline.");
            const auto offset = detail::signedInteger(row.at("content_offset_ticks"));
            if (offset < -tickLimit || offset > tickLimit)
                throw std::runtime_error("MIDI clip source offset is outside the supported timeline.");
            const auto finish = edit::addTime(offset, length);
            if (detail::signedInteger(row.at("loop_enabled")) != 0) {
                const auto loopStart = detail::signedInteger(row.at("loop_start_ticks"));
                const auto& loopLength = row.at("loop_len_ticks");
                if (loopLength.is_null() || loopStart < 0 || loopStart > offset ||
                    finish > edit::addTime(loopStart, detail::signedInteger(loopLength)))
                    throw std::runtime_error("Consolidate cannot yet assign note IDs to repeated MIDI notes.");
            }
            auto image = captureDeleted(context, id);
            auto& sourceChildren = image.at("children");
            if (detail::signedInteger(row.at("muted")) != 0 || !row.at("alias_of").is_null() ||
                !image.at("alias_links").empty() || !sourceChildren.at("automation_data").empty() ||
                !sourceChildren.at("automation_segment_ranges").empty() || !sourceChildren.at("audio_clips").empty() ||
                !sourceChildren.at("clip_properties").empty() || !sourceChildren.at("clip_playback_details").empty() ||
                !sourceChildren.at("clip_warp_fade_domains").empty())
                throw std::runtime_error("Consolidate cannot yet combine muted, linked or specially configured clips.");
            if (sources.empty())
                name = row.at("name").get<std::string>();
            auto& captured = sources.emplace_back();
            const auto& streams = sourceChildren.at("event_streams");
            if (streams.size() > 1 || (streams.size() == 1 && (streams[0].at("stream_kind") != "notes" ||
                !streams[0].at("channel").is_null() || !streams[0].at("controller").is_null())))
                throw std::runtime_error("Consolidate cannot yet merge MIDI controller or other event streams.");
            std::set<std::uint64_t> sourceIds;
            if (!streams.empty()) {
                const auto& raw = streams[0].at("data").get_binary();
                std::string error;
                auto source = notes::detail::decodeNotes(std::as_bytes(std::span(raw.data(), raw.size())), error);
                if (!source)
                    throw std::runtime_error(error);
                if (notes::detail::streamVersion(*source) != 1 || !source->trailer.empty() ||
                    notes::detail::recordStride(*source) != notes::detail::notePrefixBytes)
                    throw std::runtime_error("Consolidate cannot merge extended MIDI note streams without losing data.");
                captured.notes.reserve(source->records.size());
                for (auto& record : source->records) {
                    auto note = notes::values(record, error);
                    if (!note)
                        throw std::runtime_error(error);
                    if (note->start < offset || note->start >= finish || note->duration > finish - note->start)
                        throw std::runtime_error("Consolidate cannot yet trim notes at a source boundary.");
                    sourceIds.insert(note->id);
                    reservedNoteIds.insert(note->id);
                    note->start = (first - begin) + (note->start - offset);
                    captured.notes.push_back({std::move(record), *note});
                }
            }
            for (const auto& expression : sourceChildren.at("note_expression"))
                if (!sourceIds.contains(notes::detail::expressionNoteId(detail::signedInteger(expression.at("note_id")))))
                    throw std::runtime_error("Consolidate found expression without its source note.");
            captured.expression = std::move(sourceChildren.at("note_expression"));
        }
        if (sources.size() < 2 || (!picked.empty() && sources.size() != picked.size()))
            throw std::runtime_error("Select at least two whole MIDI clips on the same track.");

        auto children = Payload::object();
        auto expressions = Payload::array();
        notes::Diff noteDiff;
        // Reserve every source ID before assigning any replacement. A note in
        // a later clip keeps its ID even if it is an early allocation candidate.
        std::set<std::uint64_t> seenNoteIds;
        std::uint64_t nextNoteId = 1;
        for (auto& source : sources) {
            std::map<std::uint64_t, std::uint64_t> remapped;
            for (auto& note : source.notes) {
                if (!seenNoteIds.insert(note.values.id).second) {
                    while (!reservedNoteIds.insert(nextNoteId).second) {
                        if (nextNoteId == std::numeric_limits<std::uint64_t>::max())
                            throw std::runtime_error("Consolidate ran out of MIDI note IDs.");
                        ++nextNoteId;
                    }
                    remapped.emplace(note.values.id, nextNoteId);
                    note.values.id = nextNoteId;
                }
                std::string error;
                auto placed = notes::withValues(note.record, note.values, error);
                if (!placed)
                    throw std::runtime_error(error);
                noteDiff.added.push_back(std::move(*placed));
            }
            for (auto& expression : source.expression) {
                const auto original = notes::detail::expressionNoteId(detail::signedInteger(expression.at("note_id")));
                if (const auto found = remapped.find(original); found != remapped.end())
                    expression["note_id"] = notes::detail::expressionSqlId(found->second);
                expression["id"] = ids.next("note_expression");
                expression["clip_id"] = output;
                expressions.push_back(std::move(expression));
            }
        }
        if (!expressions.empty())
            children["note_expression"] = std::move(expressions);
        if (!noteDiff.added.empty()) {
            const auto combined = notes::detail::planRecords(notes::detail::emptyNotes(), noteDiff);
            if (!combined.ok || !combined.changed)
                throw std::runtime_error(combined.error);
            std::vector<std::uint8_t> bytes;
            bytes.reserve(combined.bytes.size());
            for (const auto byte : combined.bytes)
                bytes.push_back(std::to_integer<std::uint8_t>(byte));
            children["event_streams"] = Payload::array({edit::defaultRow(db, "event_streams", {
                {"id",ids.next("event_streams")},{"clip_id",output},{"stream_kind","notes"},
                {"data",Payload::binary(std::move(bytes))},
            })});
        }
        const auto begin = selection.begin.value, end = selection.end.value;
        auto row = edit::defaultRow(db, "clips", {{"id",output},{"track_id",trackId},{"kind","midi"},
            {"name",name.empty() ? std::string("Consolidated MIDI") : std::move(name)},
            {"time_base",0},{"pos_ticks",begin},{"length_ticks",end-begin}});
        edit_plan::detail::PlacementBatch batch;
        batch.incoming.push_back({{"id",output},{"row",std::move(row)},{"children",std::move(children)},
                                  {"alias_links",Payload::array()}});
        const auto scope = edit_plan::materialScope(selection.tracks, edit_plan::PlacementClock::Ticks, {begin,end});
        result.edit = edit_plan::detail::prepareCaptured(store, scope, std::move(batch), options, ids, "Consolidate");
        if (result.edit.ok)
            result.clip = output;
    } catch (const std::exception& error) {
        result.edit.error = error.what();
    }
    return result;
}
PreparedEdit prepareAudioDropReplacement(Store& store,std::int64_t track,TimePoint begin,TimePoint end,
                                         const edit_plan::Options& options,std::int64_t lastIncomingId) {
    PreparedEdit result;
    try {
        if(options.policy.deleteOverlaps && begin.value!=end.value) {
            const RangeSelection range{{track},begin,end};
            validRange(range);
            Read read(store.db());
            const edit::TimeMap time(store.db());
            const auto first=coordinate(begin,1,time),last=coordinate(end,1,time);
            SQLite::Statement clips(store.db(),
                "SELECT time_base,COALESCE(pos_ticks,pos_ns),COALESCE(length_ticks,length_ns) "
                "FROM clips WHERE track_id=? AND lane_id IS NULL");
            clips.bind(1,track);
            while(clips.executeStep()) {
                const auto base=clips.getColumn(0).getInt();
                const auto start=clips.getColumn(1).getInt64();
                const auto physical=[&](std::int64_t value){
                    return base==1?static_cast<long double>(value):std::round(static_cast<long double>(time.seconds(value))*1e9L);
                };
                if(physical(start)>=static_cast<long double>(last))continue;
                const auto length=clips.getColumn(2);
                if(length.isNull() || (length.getInt64()>0 &&
                    physical(edit::addTime(start,length.getInt64()))>static_cast<long double>(first))) {
                    // Only an actual overlap invokes the planner. Other
                    // imports keep their usual policy and admission path.
                    return prepareDeleteRange(store,range,options,lastIncomingId);
                }
            }
        }
        result.ok=true;
    } catch(const std::exception& error) {result.error=error.what();}
    return result;
}
PreparedEdit preparePasteRange(Store& store,const RangeCopy& copy,const std::vector<std::int64_t>& tracks,
                                TimePoint at,double referenceRate,bool insertTime) {
    if(!insertTime)return preparePasteRange(store,copy,tracks,at,edit_plan::Options{referenceRate,{}});
    PreparedEdit result;
    try {
        if(!copy.ok)throw std::runtime_error(copy.error);
        if(!copy.clearsRange)throw std::runtime_error("Paste Time requires a time selection, not picked clips");
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
namespace {
PreparedEdit prepareRangePlacement(Store& store,const RangeCopy& copy,const std::vector<std::int64_t>& tracks,
                                   TimePoint at,const edit_plan::Options& options,bool move) {
    PreparedEdit result;
    try {
        if(!copy.ok)throw std::runtime_error(copy.error);
        validRange(copy.selection);validRate(options.referenceRate);Read read(store.db());
        if(copy.sourceFile!=filename(store))throw std::runtime_error("range clipboard belongs to a different project");
        if(tracks.size()!=copy.selection.tracks.size())throw std::runtime_error("paste needs one destination for each copied track");
        edit::TimeMap time(store.db());CopyIds ids(store.db());
        const auto base=copy.selection.begin.base;
        const auto begin=coordinate(at,base,time),end=edit::addTime(begin,copy.selection.end.value-copy.selection.begin.value);
        RangeSelection destination{tracks,{base,begin},{base,end}};validRange(destination);
        std::set<std::int64_t> scopeTracks;
        edit_plan::detail::PlacementBatch batch;
        if(copy.clearsRange) {
            scopeTracks.insert(tracks.begin(),tracks.end());
            batch.sourceTracks=copy.selection.tracks;
        }
        const auto beginNs=coordinate({base,begin},1,time),endNs=coordinate({base,end},1,time);
        const auto sourceBeginNs=coordinate(copy.selection.begin,1,time),sourceEndNs=coordinate(copy.selection.end,1,time);
        if(endNs<=beginNs || sourceEndNs<=sourceBeginNs)throw std::runtime_error("range paste span collapses in physical time");
        // A linear image can extend past the musical range at a faster tempo.
        // Admit each complete image. Only a time selection clears the gaps.
        auto scopeBegin=std::min(beginNs,sourceBeginNs),scopeEnd=std::max(endNs,sourceEndNs);
        // Clear source envelopes before placing destinations, which may overlap them.
        if(move)
            for(const auto track:copy.selection.tracks) {
                batch.clear.push_back({{track,std::nullopt},{sourceBeginNs,sourceEndNs}});
                if(options.policy.automationFollowsClips)
                    batch.envelopes.push_back({track,copy.selection.begin,copy.selection.end,Payload::array()});
            }
        if(copy.clearsRange) {
            for(std::size_t index=0;index<tracks.size();++index) {
                batch.clear.push_back({{tracks[index],std::nullopt},{beginNs,endNs}});
                if(index<copy.envelopes.size())
                    batch.envelopes.push_back({tracks[index],{base,begin},{base,end},copy.envelopes[index]});
            }
        }
        for(const auto& image:copy.images)ids.reserve("clips",detail::signedInteger(image.at("id")));
        for(std::size_t imageIndex=0;imageIndex<copy.images.size();++imageIndex) {
            const auto& original=copy.images[imageIndex];
            verifyMediaKeys(store,original);auto image=cloneImage(original,ids.next("clips"),ids);
            const auto index=original.at("trackIndex").get<std::size_t>();
            if(index>=tracks.size())throw std::runtime_error("clipboard track offset is invalid");
            auto& row=image["row"];const auto native=static_cast<int>(detail::signedInteger(row.at("time_base")));
            const char* position=native==0?"pos_ticks":"pos_ns";
            row[position]=edit::addTime(coordinate(at,native,time),detail::signedInteger(row.at(position)));
            const auto first=detail::signedInteger(row.at(position));
            const auto last=edit::addTime(first,detail::signedInteger(row.at(native==0?"length_ticks":"length_ns")));
            scopeBegin=std::min(scopeBegin,coordinate({native,first},1,time));
            scopeEnd=std::max(scopeEnd,coordinate({native,last},1,time));
            if(!copy.clearsRange) {
                scopeTracks.insert(tracks[index]);
                batch.sourceTracks.push_back(copy.selection.tracks[index]);
                if(imageIndex<copy.clipEnvelopes.size())
                    batch.envelopes.push_back({tracks[index],{native,first},{native,last},copy.clipEnvelopes[imageIndex]});
            }
            row["track_id"]=tracks[index];row["lane_id"]=nullptr;batch.incoming.push_back(std::move(image));
        }
        for(const auto track:batch.sourceTracks)
            if(detail::captureRow(store.db(),"tracks","id",track))scopeTracks.insert(track);
        const std::vector<std::int64_t> literals(scopeTracks.begin(),scopeTracks.end());
        const auto scope=edit_plan::materialScope(literals,edit_plan::PlacementClock::Nanoseconds,{scopeBegin,scopeEnd});
        result=edit_plan::detail::prepareCaptured(store,scope,std::move(batch),options,ids,move?"Move Material":"Paste Material");
    } catch(const std::exception& exception){result.ok=false;result.error=exception.what();}
    return result;
}
} // namespace
PreparedEdit preparePasteRange(Store& store,const RangeCopy& copy,const std::vector<std::int64_t>& tracks,
                                TimePoint at,const edit_plan::Options& options) {
    return prepareRangePlacement(store,copy,tracks,at,options,false);
}
PreparedEdit prepareMoveRange(Store& store,const RangeSelection& selection,TimePoint at,const edit_plan::Options& options) {
    PreparedEdit result;
    try {
        validRange(selection);validRate(options.referenceRate);Read read(store.db());
        const edit::TimeMap time(store.db());
        if(coordinate(at,selection.begin.base,time)!=selection.begin.value) {
            auto copy=copyRangeImpl(store,selection,options.referenceRate,ClipLaneScope::MainLane,{}, {},
                                    false,/*rejectLinked=*/true);
            if(!copy.ok)throw std::runtime_error(copy.error);
            if(!copy.images.empty()) {
                if(options.policy.automationFollowsClips)
                    captureRangeEnvelopes(store,copy,time,options.referenceRate);
                return prepareRangePlacement(store,copy,selection.tracks,at,options,true);
            }
        }
        result.sourceFile=filename(store);result.ok=true;
    } catch(const std::exception& exception){result.error=exception.what();}
    return result;
}
PreparedEdit prepareTakeToMain(Store& store, std::int64_t track, std::int64_t lane,
                                   TimePoint begin, TimePoint end, double referenceRate) {
    try {
        Read read(store.db());
        const auto row = edit::row(store.db(), "lanes", lane);
        if (!row || row->at("kind") != "take" || row->at("track_id") != track)
            throw std::runtime_error("the take lane is no longer on this track");
        const RangeSelection selection{{track}, begin, end};
        const auto copy = copyRangeImpl(store, selection, referenceRate, ClipLaneScope::OneTake, lane);
        if (!copy.ok)
            throw std::runtime_error(copy.error);
        if (copy.images.empty())
            throw std::runtime_error("the selected take range has no clips");
        edit_plan::Options options{referenceRate, {}};
        options.policy.automationFollowsClips = false;
        auto prepared = preparePasteRange(store, copy, selection.tracks, begin, options);
        for (auto& request : prepared.requests)
            request.label = "Copy Take to Main Lane";
        return prepared;
    } catch (const std::exception& error) {
        PreparedEdit refused;
        refused.error = error.what();
        return refused;
    }
}
PreparedEdit prepareDuplicateRange(Store& store,const RangeSelection& selection,const edit_plan::Options& options) {
    try {
        Read read(store.db());const auto copy=copyRange(store,selection,options);
        auto result=preparePasteRange(store,copy,selection.tracks,selection.end,options);
        if(result.ok && !result.requests.empty())result.requests[0].label="Duplicate Material";
        return result;
    } catch(const std::exception& exception){PreparedEdit result;result.error=exception.what();return result;}
}
PreparedEdit prepareDuplicateRange(Store& store,const RangeSelection& selection,double referenceRate,bool insertTime) {
    if(!insertTime)return prepareDuplicateRange(store,selection,edit_plan::Options{referenceRate,{}});
    try {
        Read read(store.db());const auto copy=copyRangeImpl(store,selection,referenceRate,ClipLaneScope::MainLane);
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

namespace adi::edit_plan::detail {
void freezeImage(Store& store,Payload& image,const edit::TimeMap& time,double rate) {
    // An alias without local audio follows its source's captured playback.
    // Splitting/resize still requires explicit alias resolution.
    if(!image.at("row").at("alias_of").is_null() && image.at("children").at("audio_clips").empty())return;
    clip_edit::freezeAudio(store,image,time,rate);
}
void retainImageDetails(edit::RowEdits& plan,const Payload& image) {clip_edit::retainDetails(plan,image);}
void trimFrozenImage(Payload& image,std::int64_t begin,std::int64_t end,const edit::TimeMap& time,double rate) {
    const auto base=static_cast<int>(adi::detail::signedInteger(image.at("row").at("time_base")));
    const char* position=base==0?"pos_ticks":"pos_ns";
    const char* length=base==0?"length_ticks":"length_ns";
    const auto first=adi::detail::signedInteger(image.at("row").at(position));
    const auto last=edit::addTime(first,adi::detail::signedInteger(image.at("row").at(length)));
    if(begin<first || end>last || begin>=end)throw std::runtime_error("placement cut collapsed in the native clock");
    // Cut the right edge first so the left-cut offset always starts from the
    // original captured source. Neither call freezes the changed visible span.
    if(end<last)clip_edit::keepLeft(image,clip_edit::calculateImageCut(image,{base,end},time,rate));
    if(begin>first)clip_edit::keepRight(image,clip_edit::calculateImageCut(image,{base,begin},time,rate));
}
void resizeFrozenEnd(Payload& image,std::int64_t end,const edit::TimeMap& time,double rate) {
    auto& row=image["row"];
    if(!row.at("alias_of").is_null())throw std::runtime_error("end resize needs alias resolution");
    if(row.at("kind")!="audio" && row.at("kind")!="midi")throw std::runtime_error("this clip kind has no end-resize playback");
    const auto base=static_cast<int>(adi::detail::signedInteger(row.at("time_base")));
    const char* position=base==0?"pos_ticks":"pos_ns";
    const char* length=base==0?"length_ticks":"length_ns";
    const auto first=adi::detail::signedInteger(row.at(position));
    if(end<=first)throw std::runtime_error("end resize must follow the anchored start");
    const auto origin=clip_edit::originTicks(row,time),lastTick=base==0?end:time.ticks(end);
    if(lastTick<origin)throw std::runtime_error("end resize has an invalid musical span");
    const auto ticks=lastTick-origin;
    const bool activeIn=adi::detail::signedInteger(row.at("fade_in_ticks"))>0;
    const bool activeOut=adi::detail::signedInteger(row.at("fade_out_ticks"))>0;
    for(const char* fade:{"fade_in_ticks","fade_out_ticks"}) {
        const auto value=adi::detail::signedInteger(row.at(fade));
        if(value<0)throw std::runtime_error("end resize needs valid fades");
        row[fade]=std::min(value,ticks);
    }
    row[length]=end-first;
    if(auto* clock=clip_edit::playback(image);clock && !clock->at("content_offset_ns").is_null()) {
        const auto offset=clip_edit::sample(static_cast<double>(adi::detail::signedInteger(clock->at("content_offset_ns")))*1e-9,rate);
        const auto samples=clip_edit::sample(clip_edit::endSeconds(row,time),rate)-clip_edit::sample(clip_edit::startSeconds(row,time),rate);
        if(samples<0)throw std::runtime_error("end resize has an invalid source span");
        for(const auto& pair:{std::pair{"in_begin_ns","in_end_ns"},std::pair{"out_begin_ns","out_end_ns"}}) {
            const bool outgoing=std::string_view(pair.first)=="out_begin_ns";
            // Split survivors retain source domains with zero visible handles.
            // An anchored resize must not turn those into newly authored fades.
            if(outgoing?!activeOut:!activeIn)continue;
            if(clock->at(pair.first).is_null() || clock->at(pair.second).is_null())continue;
            const auto a=clip_edit::sample(static_cast<double>(adi::detail::signedInteger(clock->at(pair.first)))*1e-9,rate);
            const auto b=clip_edit::sample(static_cast<double>(adi::detail::signedInteger(clock->at(pair.second)))*1e-9,rate);
            if(b<a)throw std::runtime_error("end resize needs ordered fade domains");
            const auto available=outgoing?samples:std::max<std::int64_t>(0,edit::addTime(offset,samples)-a);
            const auto count=std::min(b-a,available);
            const auto finish=outgoing?edit::addTime(offset,samples):edit::addTime(a,count);
            if(count==0){(*clock)[pair.first]=nullptr;(*clock)[pair.second]=nullptr;continue;}
            (*clock)[pair.first]=clip_edit::sampleNanos(finish-count,rate);
            (*clock)[pair.second]=clip_edit::sampleNanos(finish,rate);
        }
    }
    auto& warped=image["children"]["clip_warp_fade_domains"];
    if(!warped.empty()) {
        auto& domain=warped[0];const auto offset=adi::detail::signedInteger(row.at("content_offset_ticks"));
        for(const auto& pair:{std::pair{"in_begin_ticks","in_end_ticks"},std::pair{"out_begin_ticks","out_end_ticks"}}) {
            const bool outgoing=std::string_view(pair.first)=="out_begin_ticks";
            if(outgoing?!activeOut:!activeIn)continue;
            if(domain.at(pair.first).is_null() || domain.at(pair.second).is_null())continue;
            const auto a=adi::detail::signedInteger(domain.at(pair.first)),b=adi::detail::signedInteger(domain.at(pair.second));
            if(b<a)throw std::runtime_error("end resize needs ordered warped fade domains");
            const auto available=outgoing?ticks:std::max<std::int64_t>(0,edit::addTime(offset,ticks)-a);
            const auto count=std::min(b-a,available);
            const auto finish=outgoing?edit::addTime(offset,ticks):edit::addTime(a,count);
            if(count==0){domain[pair.first]=nullptr;domain[pair.second]=nullptr;continue;}
            domain[pair.first]=finish-count;domain[pair.second]=finish;
        }
    }
}
void trimFrozenStart(Payload& image,std::int64_t begin,const edit::TimeMap& time,double rate) {
    auto& row=image["row"];
    if(!row.at("alias_of").is_null())throw std::runtime_error("left trim needs alias resolution");
    const bool audio=row.at("kind")=="audio";
    if(!audio && row.at("kind")!="midi")throw std::runtime_error("this clip kind has no left-trim playback");
    const auto base=static_cast<int>(adi::detail::signedInteger(row.at("time_base")));
    const char* position=base==0?"pos_ticks":"pos_ns";
    const char* length=base==0?"length_ticks":"length_ns";
    const auto first=adi::detail::signedInteger(row.at(position));
    const auto end=edit::addTime(first,adi::detail::signedInteger(row.at(length)));
    if(begin<0 || begin>=end)throw std::runtime_error("left trim must not pass the anchored end");
    const auto oldOrigin=clip_edit::originTicks(row,time);
    const auto oldOffset=adi::detail::signedInteger(row.at("content_offset_ticks"));
    constexpr std::int64_t limit=std::int64_t{1}<<50;
    if(oldOffset < -limit || oldOffset > limit)throw std::runtime_error("left trim source offset outside supported range");
    auto tickAt=[&](std::int64_t value){return base==0?value:time.ticks(value);};
    auto sampleAt=[&](std::int64_t value){return clip_edit::sample(base==0?time.seconds(value):static_cast<double>(value)*1e-9,rate);};
    auto* clock=clip_edit::playback(image);
    const auto& sources=image.at("children").at("audio_clips");
    const bool warpSource=audio && !sources.empty() && sources.front().at("warp_enabled")!=0;
    const bool physical=audio && !warpSource && clock && !clock->at("content_offset_ns").is_null();
    std::int64_t oldSample=0,sourceSample=0;
    if(physical) {
        oldSample=sampleAt(first);
        sourceSample=clip_edit::sample(static_cast<double>(adi::detail::signedInteger(clock->at("content_offset_ns")))*1e-9,rate);
    } else if(audio && oldOffset<0)throw std::runtime_error("audio left trim needs a nonnegative source clock");
    // A clip already at source zero has no earlier material: keep its begin,
    // so prepareTrim restores the original image as a true no-op.
    if(physical && sourceSample==0 && begin<first)begin=first;
    if(audio && begin<first) {
        // Find the first representable native position at or after source zero.
        // Binary search keeps directed conversion independent of tempo and of
        // tick/ns rounding at a sample boundary. The old begin is admissible.
        auto sourceAt=[&](std::int64_t value){return physical?(base==0?time.nanos(value):value):tickAt(value);};
        const auto lower=physical?std::min(sourceAt(first),clip_edit::sampleNanos(std::max<std::int64_t>(0,oldSample-sourceSample),rate)):oldOrigin-oldOffset;
        if(sourceAt(begin)<lower) {
            auto high=first;
            while(begin<high) {
                const auto middle=begin+(high-begin)/2;
                if(sourceAt(middle)<lower)begin=middle+1;else high=middle;
            }
        }
    }
    const auto origin=tickAt(begin),last=tickAt(end);
    if(last<origin)throw std::runtime_error("left trim has an invalid musical span");
    const auto offset=edit::addTime(oldOffset,origin-oldOrigin);
    if(offset < -limit || offset > limit || (audio && !physical && offset<0))
        throw std::runtime_error("left trim source offset outside supported range");
    // An unlooped MIDI or warped clip must keep Start before its End marker.
    // Unwarped audio plays from its physical pins instead.
    const auto& children=image.at("children");
    if((!audio || warpSource) && row.at("loop_enabled")==0 && children.contains("clip_properties") &&
       !children.at("clip_properties").empty() && !children.at("clip_properties")[0].at("end_marker_ticks").is_null() &&
       offset>=adi::detail::signedInteger(children.at("clip_properties")[0].at("end_marker_ticks")))
        throw std::runtime_error("left trim must not pass the clip End marker");
    const auto ticks=last-origin;
    const bool activeIn=adi::detail::signedInteger(row.at("fade_in_ticks"))>0;
    const bool activeOut=adi::detail::signedInteger(row.at("fade_out_ticks"))>0;
    for(const char* fade:{"fade_in_ticks","fade_out_ticks"}) {
        const auto value=adi::detail::signedInteger(row.at(fade));
        if(value<0)throw std::runtime_error("left trim needs valid fades");
        row[fade]=std::min(value,ticks);
    }
    row[position]=begin;row[length]=end-begin;row["content_offset_ticks"]=offset;
    auto retarget=[&](Payload& domain,const char* firstKey,const char* lastKey,
                      std::int64_t sourceBegin,std::int64_t sourceEnd,bool incoming) {
        if(incoming?!activeIn:!activeOut)return;
        if(domain.at(firstKey).is_null() || domain.at(lastKey).is_null())return;
        const auto a=adi::detail::signedInteger(domain.at(firstKey));
        const auto b=adi::detail::signedInteger(domain.at(lastKey));
        if(a<0 || b<a)throw std::runtime_error("left trim needs ordered fade domains");
        const auto start=incoming?sourceBegin:std::max(a,sourceBegin);
        const auto finish=incoming?edit::addTime(start,std::min(b-a,sourceEnd-sourceBegin)):std::min(b,sourceEnd);
        if(finish<=start){domain[firstKey]=nullptr;domain[lastKey]=nullptr;return;}
        domain[firstKey]=start;domain[lastKey]=finish;
    };
    if(physical) {
        const auto next=edit::addTime(sourceSample,sampleAt(begin)-oldSample);
        const auto finish=edit::addTime(next,sampleAt(end)-sampleAt(begin));
        if(next<0 || finish<next)throw std::runtime_error("audio left trim precedes source zero");
        // Operate in samples, then pin representable ns once per boundary.
        Payload domains=Payload::object();
        for(const char* key:{"in_begin_ns","in_end_ns","out_begin_ns","out_end_ns"}) {
            if(std::string_view(key).starts_with("in_")?!activeIn:!activeOut)continue;
            domains[key]=clock->at(key).is_null()?Payload(nullptr):Payload(clip_edit::sample(static_cast<double>(adi::detail::signedInteger(clock->at(key)))*1e-9,rate));
        }
        retarget(domains,"in_begin_ns","in_end_ns",next,finish,true);
        retarget(domains,"out_begin_ns","out_end_ns",next,finish,false);
        for(const char* key:{"in_begin_ns","in_end_ns","out_begin_ns","out_end_ns"}) {
            if(!domains.contains(key))continue;
            (*clock)[key]=domains.at(key).is_null()?Payload(nullptr):Payload(clip_edit::sampleNanos(adi::detail::signedInteger(domains.at(key)),rate));
        }
        (*clock)["content_offset_ns"]=clip_edit::sampleNanos(next,rate);
    }
    auto& warped=image["children"]["clip_warp_fade_domains"];
    if(warpSource && !warped.empty()) {
        const auto finish=edit::addTime(offset,ticks);
        retarget(warped[0],"in_begin_ticks","in_end_ticks",offset,finish,true);
        retarget(warped[0],"out_begin_ticks","out_end_ticks",offset,finish,false);
    }
}
} // namespace adi::edit_plan::detail
