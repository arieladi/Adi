// SPDX-License-Identifier: AGPL-3.0-or-later
#include "adi/automation_time_edit.hpp"
#include "adi/blob.hpp"
#include "adi/row_capture.hpp"
#include "adi/engine/automation.hpp"
#include "adi/store.hpp"
#include <SQLiteCpp/SQLiteCpp.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <set>
#include <stdexcept>

namespace adi::edit {
namespace {
std::int64_t sample(double seconds,double rate) {
    const double n=seconds*rate;
    if(!std::isfinite(n) || n<0 || n>0x1p50)throw std::runtime_error("automation time outside sample range");
    return static_cast<std::int64_t>(std::llround(n));
}
}
void insertAutomationTime(Store& store,RowEdits& plan,clip_edit::CopyIds& ids,const TimeMap& time,
                          std::int64_t atTicks,std::int64_t ticks,std::int64_t atNanos,std::int64_t nanos,double rate) {
    auto& db=store.db();
    SQLite::Statement data(db,"SELECT d.id,l.time_base FROM automation_data d JOIN automation_lanes l ON l.id=d.lane_id WHERE d.clip_id IS NULL ORDER BY d.id");
    while(data.executeStep()) {
        const auto id=data.getColumn(0).getInt64();const int base=data.getColumn(1).getInt();
        if(base!=0 && base!=1)throw std::runtime_error("automation has an unknown clock");
        const auto at=base==0?atTicks:atNanos,gap=base==0?ticks:nanos;
        auto row=*plan.current("automation_data",id);
        const auto& binary=row.at("data").get_binary();
        const std::span<const std::byte> bytes(reinterpret_cast<const std::byte*>(binary.data()),binary.size());
        StreamReader<AutomationPoint> reader(bytes,FourCC::Automation);
        if(!reader.ok())throw std::runtime_error("Insert Silence needs readable global automation");
        if((reader.header().flags & ~(StreamFlags::SortedByTime|StreamFlags::TimeIsNanos))!=0 ||
           ((reader.header().flags & StreamFlags::TimeIsNanos)!=0)!=(base==1))
            throw std::runtime_error("automation clock or header flags differ");
        std::vector<AutomationPoint> points;std::set<std::uint64_t> used;
        for(std::uint32_t i=0;i<reader.count();++i) {
            const auto p=*reader.at(i);
            if(p.time<0 || (!points.empty() && p.time<points.back().time) || p.curve>5 ||
               p.reserved || !std::isfinite(p.value) || !std::isfinite(p.tension))
                throw std::runtime_error("Insert Silence needs valid sorted automation");
            points.push_back(p);used.insert(p.point_id);
        }
        if(points.empty())continue;
        const auto first=static_cast<std::size_t>(std::lower_bound(points.begin(),points.end(),at,
            [](const AutomationPoint& p,std::int64_t pos){return p.time<pos;})-points.begin());
        if(first==points.size())continue; // already holds its last value
        const bool exact=points[first].time==at;
        const std::size_t extra=exact || first==0?1:2;
        if(reader.header().rec_size!=sizeof(AutomationPoint))
            throw std::runtime_error("Insert Silence cannot synthesize a future automation record");
        if(points.size()>std::numeric_limits<std::uint32_t>::max()-extra)
            throw std::runtime_error("automation record count exhausted");
        std::uint64_t next=1;
        auto freshId=[&]() {
            while(used.contains(next)) {
                if(next==std::numeric_limits<std::uint64_t>::max())throw std::runtime_error("automation point ids exhausted");
                ++next;
            }
            const auto result=next;used.insert(result);return result;
        };
        std::map<std::size_t,Payload> ranges;
        SQLite::Statement metadata(db,"SELECT id FROM automation_segment_ranges WHERE data_id=? ORDER BY record_index");
        metadata.bind(1,id);
        while(metadata.executeStep()) {
            auto range=*plan.current("automation_segment_ranges",metadata.getColumn(0).getInt64());
            const auto index=detail::signedInteger(range.at("record_index"));
            if(index<0 || static_cast<std::uint64_t>(index)>=points.size() || !range.at("clip_id").is_null())
                throw std::runtime_error("automation curve range has an invalid owner or record");
            const double lo=range.at("progress_begin"),hi=range.at("progress_end");
            const double sourceFirst=range.at("source_first"),sourceLast=range.at("source_last");
            if(!std::isfinite(lo) || !std::isfinite(hi) || !std::isfinite(sourceFirst) || !std::isfinite(sourceLast) ||
               lo<0 || hi>1 || lo>=hi)throw std::runtime_error("automation curve range is invalid");
            ranges.emplace(static_cast<std::size_t>(index),std::move(range));
        }
        auto sampleAt=[&](std::int64_t pos){return sample(base==0?time.seconds(pos):static_cast<double>(pos)*1e-9,rate);};
        const auto cutSample=sampleAt(at);
        double held=points[first].value;
        std::optional<Payload> leftRange,rightRange;
        if(first>0) {
            const auto& a=points[first-1];const auto& b=points[first];
            engine::CurveSlice slice{0,1,a.value,b.value};
            const auto previous=ranges.find(first-1);
            if(previous!=ranges.end()) {
                const auto& r=previous->second;
                slice={r.at("progress_begin"),r.at("progress_end"),r.at("source_first"),r.at("source_last")};
            }
            if(exact)held=a.curve==0?a.value:b.value;
            else {
                const auto begin=sampleAt(a.time),end=sampleAt(b.time);
                const double fraction=end>begin?
                    std::clamp(static_cast<double>(cutSample-begin)/static_cast<double>(end-begin),0.0,1.0):
                    static_cast<double>(at-a.time)/static_cast<double>(b.time-a.time);
                held=engine::curveSliceValue(a.value,b.value,fraction,a.curve,a.tension,&slice);
                // Even a linear slice may already carry an original nonlinear
                // domain; retain the original kind and domain through repeated edits.
                if(a.curve>1 || previous!=ranges.end()) {
                    const double split=slice.begin+(slice.end-slice.begin)*fraction;
                    if(!std::isfinite(split) || split<=slice.begin || split>=slice.end)
                        throw std::runtime_error("automation split is below curve progress precision");
                    auto makeRange=[&](std::int64_t key,std::int64_t index,double lo,double hi) {
                        return defaultRow(db,"automation_segment_ranges",{{"id",key},{"data_id",id},{"record_index",index},
                            {"progress_begin",lo},{"progress_end",hi},{"source_first",slice.sourceFirst},{"source_last",slice.sourceLast}});
                    };
                    const auto leftId=previous==ranges.end()?ids.next("automation_segment_ranges"):
                        detail::signedInteger(previous->second.at("id"));
                    leftRange=previous==ranges.end()?makeRange(leftId,static_cast<std::int64_t>(first-1),slice.begin,split):previous->second;
                    (*leftRange)["progress_end"]=split;
                    rightRange=makeRange(ids.next("automation_segment_ranges"),static_cast<std::int64_t>(first+1),split,slice.end);
                }
            }
        }
        std::vector<AutomationPoint> inserted;
        AutomationPoint hold{};hold.time=at;hold.value=held;hold.curve=0;hold.point_id=freshId();inserted.push_back(hold);
        if(extra==2) {
            auto after=points[first-1];after.time=addTime(at,gap);after.value=held;after.flags=0;after.point_id=freshId();
            inserted.push_back(after);
        }
        auto header=reader.header();header.count+=static_cast<std::uint32_t>(extra);
        std::vector<std::uint8_t> output(sizeof(StreamHeader)+static_cast<std::size_t>(header.count)*sizeof(AutomationPoint));
        std::memcpy(output.data(),&header,sizeof header);
        std::size_t out=sizeof header;
        auto append=[&](const AutomationPoint& p){std::memcpy(output.data()+out,&p,sizeof p);out+=sizeof p;};
        for(std::size_t i=0;i<first;++i)append(points[i]);
        for(const auto& p:inserted)append(p);
        for(std::size_t i=first;i<points.size();++i){auto p=points[i];p.time=addTime(p.time,gap);append(p);}
        const auto oldEnd=sizeof(StreamHeader)+points.size()*sizeof(AutomationPoint);
        output.insert(output.end(),binary.begin()+static_cast<std::ptrdiff_t>(oldEnd),binary.end());
        row["data"]=Payload::binary(std::move(output));plan.set("automation_data",id,row);
        for(auto& [index,range]:ranges) {
            if(index>=first)range["record_index"]=static_cast<std::int64_t>(index+extra);
            plan.set("automation_segment_ranges",detail::signedInteger(range.at("id")),range);
        }
        for(const auto* range:{&leftRange,&rightRange})if(*range)
            plan.set("automation_segment_ranges",detail::signedInteger(range->value().at("id")),range->value());
    }
}
} // namespace adi::edit
