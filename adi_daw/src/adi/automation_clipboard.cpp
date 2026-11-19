// SPDX-License-Identifier: AGPL-3.0-or-later
#include "adi/automation_clipboard.hpp"
#include "adi/blob.hpp"
#include "adi/engine/curves.hpp"
#include "adi/row_capture.hpp"
#include "adi/store.hpp"
#include <SQLiteCpp/SQLiteCpp.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <map>
#include <set>
#include <stdexcept>

namespace adi::edit {
namespace {
struct Envelope {
    StreamHeader header{};
    std::vector<AutomationPoint> points;
    std::vector<std::uint8_t> trailer;
    std::map<std::size_t,Payload> ranges;
};
Envelope decode(const Payload& row,int base) {
    const auto& bytes=row.at("data").get_binary();
    StreamReader<AutomationPoint> reader({reinterpret_cast<const std::byte*>(bytes.data()),bytes.size()},FourCC::Automation);
    if(!reader.ok() || reader.header().rec_size!=sizeof(AutomationPoint) ||
       (reader.header().flags & ~(StreamFlags::SortedByTime|StreamFlags::TimeIsNanos))!=0 ||
       ((reader.header().flags & StreamFlags::TimeIsNanos)!=0)!=(base==1))
        throw std::runtime_error("time clipboard needs the current automation record layout and declared clock");
    Envelope result;result.header=reader.header();
    for(std::uint32_t index=0;index<reader.count();++index) {
        const auto point=*reader.at(index);
        if(point.time<0 || (!result.points.empty() && point.time<result.points.back().time) ||
           point.curve>5 || point.reserved || !std::isfinite(point.value) || !std::isfinite(point.tension))
            throw std::runtime_error("time clipboard needs valid sorted automation");
        result.points.push_back(point);
    }
    const auto end=sizeof(StreamHeader)+result.points.size()*sizeof(AutomationPoint);
    result.trailer.assign(bytes.begin()+static_cast<std::ptrdiff_t>(end),bytes.end());return result;
}
Payload encoded(const Envelope& envelope) {
    if(envelope.points.size()>std::numeric_limits<std::uint32_t>::max())throw std::runtime_error("automation record count exhausted");
    auto header=envelope.header;header.count=static_cast<std::uint32_t>(envelope.points.size());
    std::vector<std::uint8_t> bytes(sizeof header+envelope.points.size()*sizeof(AutomationPoint));
    std::memcpy(bytes.data(),&header,sizeof header);
    for(std::size_t index=0;index<envelope.points.size();++index)
        std::memcpy(bytes.data()+sizeof header+index*sizeof(AutomationPoint),&envelope.points[index],sizeof(AutomationPoint));
    bytes.insert(bytes.end(),envelope.trailer.begin(),envelope.trailer.end());return Payload::binary(std::move(bytes));
}
void addRange(Envelope& envelope,Payload row) {
    const auto index=detail::signedInteger(row.at("record_index"));
    const double lo=row.at("progress_begin"),hi=row.at("progress_end"),first=row.at("source_first"),last=row.at("source_last");
    if(index<0 || static_cast<std::uint64_t>(index)>=envelope.points.size() ||
       !row.at("clip_id").is_null() || !std::isfinite(lo) || !std::isfinite(hi) ||
       !std::isfinite(first) || !std::isfinite(last) || lo<0 || hi>1 || lo>=hi ||
       !envelope.ranges.emplace(static_cast<std::size_t>(index),std::move(row)).second)
        throw std::runtime_error("time clipboard has an invalid automation curve range");
}
engine::CurveSlice sliceAt(const Envelope& envelope,std::size_t index) {
    engine::CurveSlice slice{0,1,envelope.points[index].value,envelope.points[index+1].value};
    if(const auto found=envelope.ranges.find(index);found!=envelope.ranges.end()) {
        const auto& row=found->second;slice={row.at("progress_begin"),row.at("progress_end"),row.at("source_first"),row.at("source_last")};
    }
    return slice;
}
std::int64_t sampleAt(const TimeMap& time,int base,std::int64_t position,double rate) {
    const double value=(base==0?time.seconds(position):static_cast<double>(position)*1e-9)*rate;
    if(!std::isfinite(value) || value<0 || value>0x1p50)throw std::runtime_error("automation clipboard time exceeds sample precision");
    return static_cast<std::int64_t>(std::llround(value));
}
}
Payload copyAutomationTime(Store& store,const TimeMap& time,std::int64_t beginTicks,std::int64_t endTicks,
                           std::int64_t beginNanos,std::int64_t endNanos,double rate) {
    Payload result=Payload::array();auto& db=store.db();
    SQLite::Statement lanes(db,"SELECT id,time_base FROM automation_lanes ORDER BY id");
    while(lanes.executeStep()) {
        const auto laneId=lanes.getColumn(0).getInt64();const int base=lanes.getColumn(1).getInt();
        if(base!=0 && base!=1)throw std::runtime_error("automation clipboard has an unknown clock");
        const auto begin=base==0?beginTicks:beginNanos,end=base==0?endTicks:endNanos;
        const auto lane=*detail::captureRow(db,"automation_lanes","id",laneId);
        std::optional<Payload> row;Envelope original;
        SQLite::Statement data(db,"SELECT id FROM automation_data WHERE lane_id=? AND clip_id IS NULL");data.bind(1,laneId);
        if(data.executeStep()) {
            row=*detail::captureRow(db,"automation_data","id",data.getColumn(0).getInt64());original=decode(*row,base);
            SQLite::Statement ranges(db,"SELECT id FROM automation_segment_ranges WHERE data_id=? ORDER BY record_index");ranges.bind(1,data.getColumn(0).getInt64());
            while(ranges.executeStep())addRange(original,*detail::captureRow(db,"automation_segment_ranges","id",ranges.getColumn(0).getInt64()));
        } else {
            original.header.fourcc=static_cast<std::uint32_t>(FourCC::Automation);original.header.version=1;
            original.header.rec_size=sizeof(AutomationPoint);original.header.flags=StreamFlags::SortedByTime|(base==1?StreamFlags::TimeIsNanos:0);
        }
        Envelope copied;copied.header=original.header;copied.trailer=original.trailer;
        const auto& points=original.points;
        auto fraction=[&](std::size_t index,std::int64_t position) {
            const auto first=sampleAt(time,base,points[index].time,rate),last=sampleAt(time,base,points[index+1].time,rate),at=sampleAt(time,base,position,rate);
            if(last<=first || at<first || at>last)throw std::runtime_error("time clipboard boundary is below automation sample precision");
            return static_cast<double>(at-first)/static_cast<double>(last-first);
        };
        auto value=[&](std::int64_t position,bool incoming) {
            if(points.empty())return lane.at("default_value").get<double>();
            const auto found=incoming?std::lower_bound(points.begin(),points.end(),position,[](const auto& point,auto at){return point.time<at;}):
                std::upper_bound(points.begin(),points.end(),position,[](auto at,const auto& point){return at<point.time;});
            if(found==points.begin())return points.front().value;
            if(found==points.end())return points.back().value;
            const auto index=static_cast<std::size_t>(found-points.begin()-1);const auto slice=sliceAt(original,index);
            return engine::curveSliceValue(points[index].value,points[index+1].value,fraction(index,position),points[index].curve,points[index].tension,&slice);
        };
        // Build each retained outgoing segment from its original domain.
        auto append=[&](AutomationPoint point,std::optional<std::size_t> source,std::int64_t originalStart,std::int64_t originalEnd) {
            const auto index=copied.points.size();copied.points.push_back(point);
            if(!source || *source+1>=points.size() || originalEnd<=originalStart)return;
            const auto old=original.ranges.find(*source);const auto slice=sliceAt(original,*source);
            const auto lo=originalStart==points[*source].time?slice.begin:slice.begin+(slice.end-slice.begin)*fraction(*source,originalStart);
            const auto hi=originalEnd==points[*source+1].time?slice.end:slice.begin+(slice.end-slice.begin)*fraction(*source,originalEnd);
            if(old==original.ranges.end() && points[*source].curve<=1)return;
            if(!std::isfinite(lo) || !std::isfinite(hi) || lo>=hi)throw std::runtime_error("time clipboard boundary is below curve progress precision");
            auto range=old==original.ranges.end()?defaultRow(db,"automation_segment_ranges",{{"id",1},{"data_id",row->at("id")},{"record_index",0},{"progress_begin",lo},{"progress_end",hi},{"source_first",slice.sourceFirst},{"source_last",slice.sourceLast}}):old->second;
            range["record_index"]=static_cast<std::int64_t>(index);range["progress_begin"]=lo;range["progress_end"]=hi;copied.ranges[index]=std::move(range);
        };
        const auto first=static_cast<std::size_t>(std::lower_bound(points.begin(),points.end(),begin,[](const auto& point,auto at){return point.time<at;})-points.begin());
        const bool exact=first<points.size() && points[first].time==begin;
        if(!exact) {
            AutomationPoint point{};point.value=value(begin,false);point.curve=0;
            std::optional<std::size_t> source;
            if(first>0 && first<points.size()){source=first-1;point.curve=points[*source].curve;point.tension=points[*source].tension;}
            append(point,source,begin,first<points.size()?std::min(end,points[first].time):end);
        }
        for(std::size_t index=first;index<points.size() && points[index].time<end;++index) {
            auto point=points[index];point.time-=begin;
            append(point,index,points[index].time,index+1<points.size()?std::min(end,points[index+1].time):end);
        }
        AutomationPoint last{};last.time=end-begin;last.value=value(end,true);last.curve=0;copied.points.push_back(last);
        Payload ranges=Payload::array();for(const auto& [index,range]:copied.ranges){(void)index;ranges.push_back(range);}
        result.push_back({{"lane",lane},{"row",row.value_or(Payload{})},{"data",encoded(copied)},{"ranges",std::move(ranges)}});
    }
    return result;
}
void pasteAutomationTime(Store& store,RowEdits& plan,clip_edit::CopyIds& ids,const Payload& clipboard,
                        std::int64_t atTicks,std::int64_t ticks,std::int64_t atNanos,
                        std::int64_t nanos,double rate) {
    (void)rate;auto& db=store.db();
    for(const auto& item:clipboard) {
        const auto& capturedLane=item.at("lane");const auto laneId=detail::signedInteger(capturedLane.at("id"));
        const auto lane=detail::captureRow(db,"automation_lanes","id",laneId);
        if(!lane || *lane!=capturedLane)throw std::runtime_error("automation clipboard lane changed its owner or contract");
        const int base=static_cast<int>(detail::signedInteger(lane->at("time_base")));
        if(base!=0 && base!=1)throw std::runtime_error("automation clipboard has an unknown clock");
        const auto at=base==0?atTicks:atNanos,span=base==0?ticks:nanos,end=addTime(at,span);
        Payload copiedRow={{"data",item.at("data")}};Envelope copied=decode(copiedRow,base);
        if(copied.points.empty() || copied.points.front().time!=0 || copied.points.back().time!=span)
            throw std::runtime_error("automation clipboard does not cover its full time span");
        for(const auto& range:item.at("ranges"))addRange(copied,range);
        std::optional<Payload> current;std::int64_t dataId=0;
        SQLite::Statement stored(db,"SELECT id FROM automation_data WHERE lane_id=? AND clip_id IS NULL");stored.bind(1,laneId);
        if(stored.executeStep()){dataId=stored.getColumn(0).getInt64();current=plan.current("automation_data",dataId);}
        Envelope target;Payload row;
        if(current) {
            row=*current;target=decode(row,base);std::set<std::int64_t> rangeIds;
            SQLite::Statement ranges(db,"SELECT id FROM automation_segment_ranges WHERE data_id=?");ranges.bind(1,dataId);
            while(ranges.executeStep())rangeIds.insert(ranges.getColumn(0).getInt64());
            for(const auto& change:plan.changes())if(change.at("table")=="automation_segment_ranges") {
                const auto& before=change.at("before");const auto& after=change.at("after");
                if((!before.is_null() && before.at("data_id")==dataId) || (!after.is_null() && after.at("data_id")==dataId))
                    rangeIds.insert(detail::signedInteger(change.at("id")));
            }
            for(const auto key:rangeIds)if(auto range=plan.current("automation_segment_ranges",key);range && range->at("data_id")==dataId)addRange(target,*range);
        } else {
            dataId=ids.next("automation_data");
            if(!item.at("row").is_null()) {
                row=item.at("row");row["id"]=dataId;row["lane_id"]=laneId;row["clip_id"]=nullptr;
            } else row=defaultRow(db,"automation_data",{{"id",dataId},{"lane_id",laneId},{"data",item.at("data")}});
            target.header=copied.header;target.trailer=copied.trailer;
        }
        std::set<std::uint64_t> used;
        for(const auto& point:target.points)used.insert(point.point_id);
        std::uint64_t nextId=1;
        auto freshId=[&]() {
            while(used.contains(nextId)){if(nextId==std::numeric_limits<std::uint64_t>::max())throw std::runtime_error("automation point ids exhausted");++nextId;}
            const auto result=nextId;used.insert(result);return result;
        };
        Envelope output;output.header=target.header;output.trailer=target.trailer;
        auto append=[&](const Envelope& source,std::size_t index,AutomationPoint point,bool clone) {
            const auto destination=output.points.size();if(clone)point.point_id=freshId();output.points.push_back(point);
            if(const auto found=source.ranges.find(index);found!=source.ranges.end()) {
                auto range=found->second;
                if(clone)range["id"]=ids.next("automation_segment_ranges");
                range["data_id"]=dataId;range["record_index"]=static_cast<std::int64_t>(destination);range["clip_id"]=nullptr;
                output.ranges[destination]=std::move(range);
            }
        };
        for(std::size_t index=0;index<target.points.size() && target.points[index].time<=at;++index)
            append(target,index,target.points[index],false);
        const auto lastPrefix=output.points.size();
        if(target.points.empty()) {
            AutomationPoint guard{};guard.time=at;guard.value=lane->at("default_value");guard.point_id=freshId();output.points.push_back(guard);
        }
        for(std::size_t index=0;index<copied.points.size();++index) {
            auto point=copied.points[index];point.time=addTime(at,point.time);append(copied,index,point,true);
        }
        const auto suffix=static_cast<std::size_t>(std::lower_bound(target.points.begin(),target.points.end(),end,[](const auto& point,auto position){return point.time<position;})-target.points.begin());
        if(suffix==target.points.size() || target.points[suffix].time!=end) {
            // Insert Silence emits only one guard before the first or after
            // the last point. Restore that held target value at the far join.
            AutomationPoint guard{};guard.time=end;guard.curve=0;guard.point_id=freshId();
            guard.value=target.points.empty()?lane->at("default_value").get<double>():
                lastPrefix?target.points[lastPrefix-1].value:target.points.front().value;
            output.points.push_back(guard);
        }
        for(std::size_t index=suffix;index<target.points.size();++index)append(target,index,target.points[index],false);
        row["data"]=encoded(output);plan.set("automation_data",dataId,row);
        std::set<std::int64_t> retained;
        for(const auto& [index,range]:output.ranges){(void)index;const auto key=detail::signedInteger(range.at("id"));retained.insert(key);plan.set("automation_segment_ranges",key,range);}
        for(const auto& [index,range]:target.ranges){(void)index;const auto key=detail::signedInteger(range.at("id"));if(!retained.contains(key))plan.set("automation_segment_ranges",key,std::nullopt);}
    }
}

}
