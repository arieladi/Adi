// SPDX-License-Identifier: AGPL-3.0-or-later
#include "adi/automation_shapes.hpp"
#include "adi/automation_edits.hpp"
#include "adi/engine/curves.hpp"
#include "adi/history.hpp"
#include "adi/store.hpp"
#include <SQLiteCpp/SQLiteCpp.h>
#include <sqlite3.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <numbers>
#include <set>
#include <stdexcept>

namespace adi::automation {
namespace {
std::int64_t fraction(std::int64_t span,std::uint32_t n,std::uint32_t denominator) {
    return (span/static_cast<std::int64_t>(denominator))*static_cast<std::int64_t>(n)+
        ((span%static_cast<std::int64_t>(denominator))*static_cast<std::int64_t>(n))/static_cast<std::int64_t>(denominator);
}
double value(Shape shape,double x,const ShapeSpec& s) {
    switch(shape) {
        case Shape::Sine:return 0.5-0.5*std::cos(2*std::numbers::pi*x);
        case Shape::Triangle:return x<=0.5?2*x:2*(1-x);
        case Shape::Sawtooth:return x;
        case Shape::InverseSawtooth:return 1-x;
        case Shape::Square:return x<0.5?1:0;
        case Shape::ExponentialRise:return engine::curveShape(2,x,1);
        case Shape::ExponentialFall:return 1-engine::curveShape(2,x,1);
        case Shape::LogarithmicRise:return engine::curveShape(3,x,1);
        case Shape::LogarithmicFall:return 1-engine::curveShape(3,x,1);
        case Shape::ADSR: {
            const double a=static_cast<double>(s.attack)/10000, d=static_cast<double>(s.decay)/10000;
            const double releaseAt=1-static_cast<double>(s.release)/10000;
            if (a>0 && x<a)return x/a;
            if (d>0 && x<a+d)return 1+(s.sustain-1)*(x-a)/d;
            if (x<releaseAt)return s.sustain;
            return releaseAt<1?s.sustain*(1-x)/(1-releaseAt):s.sustain;
        }
    }
    throw std::runtime_error("automation: unknown shape");
}
} // namespace
std::vector<AutomationPoint> generateShape(const ShapeSpec& s) {
    if (s.start<0 || s.end<=s.start || !std::isfinite(s.low) || !std::isfinite(s.high) || s.low>s.high ||
        !std::isfinite(s.high-s.low) || !s.cycles || s.cycles>128 || s.stepsPerCycle<2 || s.stepsPerCycle>2048 ||
        s.attack>10000 || s.decay>10000 || s.release>10000 || s.attack+s.decay+s.release>10000 ||
        !std::isfinite(s.sustain) || s.sustain<0 || s.sustain>1)
        throw std::runtime_error("automation: invalid shape range, resolution or envelope");
    // Validate the enum even when a later early return might avoid evaluation.
    (void)value(s.shape,0,s);
    const auto span=s.end-s.start;
    const auto subdivisions=s.shape==Shape::Sine?s.stepsPerCycle:
        (s.shape==Shape::Triangle || s.shape==Shape::Square?2u:1u);
    if (span<static_cast<std::int64_t>(s.cycles)*subdivisions)
        throw std::runtime_error("automation: shape resolution exceeds available time units");
    std::vector<AutomationPoint> points;
    for (std::uint32_t cycle=0;cycle<s.cycles;++cycle) {
        const auto begin=s.start+fraction(span,cycle,s.cycles);
        const auto finish=s.start+fraction(span,cycle+1,s.cycles);
        auto add=[&](std::int64_t time,double normalized,std::uint8_t curve) {
            AutomationPoint p{};p.time=time;p.value=s.low+(s.high-s.low)*normalized;p.curve=curve;points.push_back(p);
        };
        if (s.shape==Shape::ADSR) {
            add(begin,0,1);
            add(begin+fraction(finish-begin,s.attack,10000),1,1);
            add(begin+fraction(finish-begin,s.attack+s.decay,10000),s.sustain,1);
            add(begin+fraction(finish-begin,10000-s.release,10000),s.sustain,1);
            add(finish,s.release?0:s.sustain,1);
        } else if (s.shape==Shape::Square) {
            add(begin,1,0);add(begin+(finish-begin)/2,0,0);add(finish,0,0);
        } else if (s.shape==Shape::Triangle) {
            add(begin,0,1);add(begin+(finish-begin)/2,1,1);add(finish,0,1);
        } else if (s.shape==Shape::Sawtooth || s.shape==Shape::InverseSawtooth) {
            const bool up=s.shape==Shape::Sawtooth;
            add(begin,up?0:1,1);add(finish,up?1:0,1);
        } else if (s.shape==Shape::ExponentialRise || s.shape==Shape::ExponentialFall ||
                   s.shape==Shape::LogarithmicRise || s.shape==Shape::LogarithmicFall) {
            const bool up=s.shape==Shape::ExponentialRise || s.shape==Shape::LogarithmicRise;
            const std::uint8_t curve=(s.shape==Shape::ExponentialRise || s.shape==Shape::ExponentialFall)?2:3;
            add(begin,up?0:1,curve);points.back().tension=1;add(finish,up?1:0,1);
        } else {
            for(std::uint32_t n=0;n<=s.stepsPerCycle;++n)
                add(begin+fraction(finish-begin,n,s.stepsPerCycle),value(s.shape,static_cast<double>(n)/s.stepsPerCycle,s),1);
        }
    }
    return points;
}
std::optional<PreparedShape> prepareAutomationWrite(Store& store,std::int64_t data,std::int64_t lane,
    std::optional<std::int64_t> clip,std::span<const AutomationPoint> source,std::string& error) {
    try {
        if(data<=0 || lane<=0 || (clip && *clip<=0)) throw std::runtime_error("automation: positive shape addresses required");
        if(source.empty() || source.size()>(1u<<23))throw std::runtime_error("automation: invalid recording point count");
        double low=source.front().value, high=low;std::int64_t previous=source.front().time;
        for(const auto& p:source) {
            if(p.point_id || p.flags || p.reserved || p.time<0 || p.time<previous || !std::isfinite(p.value) ||
                !std::isfinite(p.tension) || p.tension < -1 || p.tension > 1 || p.curve>5)
                throw std::runtime_error("automation: invalid explicit recording point");
            previous=p.time;low=std::min(low,p.value);high=std::max(high,p.value);
        }
        std::vector<AutomationPoint> generated(source.begin(),source.end());
        std::optional<SQLite::Transaction> txn;
        if(sqlite3_get_autocommit(store.db().getHandle())!=0)txn.emplace(store.db(),SQLite::TransactionBehavior::DEFERRED);
        PreparedShape result;result.owner=&store;result.client=store.clientId();result.head=History(store).head();
        SQLite::Statement metadata(store.db(),"SELECT min_value,max_value,time_base FROM automation_lanes WHERE id=?");metadata.bind(1,lane);
        if(!metadata.executeStep())throw std::runtime_error("automation: missing shape lane");
        if((!metadata.getColumn(0).isNull() && low<metadata.getColumn(0).getDouble()) ||
            (!metadata.getColumn(1).isNull() && high>metadata.getColumn(1).getDouble()))
            throw std::runtime_error("automation: shape exceeds lane bounds");
        if(clip) {
            SQLite::Statement q(store.db(),"SELECT 1 FROM clips WHERE id=?");q.bind(1,*clip);
            if(!q.executeStep())throw std::runtime_error("automation: missing shape clip");
        }
        SQLite::Statement q(store.db(),"SELECT id,clip_id,data FROM automation_data WHERE lane_id=? AND clip_id IS ?");q.bind(1,lane);
        if(clip)q.bind(2,*clip);else q.bind(2);
        std::set<std::uint64_t> used;Payload remove=Payload::array();
        if(q.executeStep()) {
            if(q.getColumn(0).getInt64()!=data)throw std::runtime_error("automation: shape data address mismatch");
            const auto col=q.getColumn(2);const auto* b=static_cast<const std::byte*>(col.getBlob());
            const StreamReader<AutomationPoint> reader(std::span<const std::byte>(b,static_cast<std::size_t>(col.getBytes())),FourCC::Automation);
            if(!reader.ok() || reader.header().version!=1 || reader.header().rec_size!=sizeof(AutomationPoint) ||
                (reader.header().flags&~(StreamFlags::SortedByTime|StreamFlags::TimeIsNanos))!=0)
                throw std::runtime_error("automation: shape needs a writable known AAUT layout");
            if (((reader.header().flags&StreamFlags::TimeIsNanos)!=0)!=(metadata.getColumn(2).getInt()==1))
                throw std::runtime_error("automation: stream time base disagrees with lane");
            for(std::uint32_t n=0;n<reader.count();++n) {
                const auto p=reader.at(n);
                if(!p || !p->point_id || !used.insert(p->point_id).second)throw std::runtime_error("automation: ambiguous shape point IDs");
                if(p->time>=source.front().time && p->time<=source.back().time)remove.push_back(p->point_id);
            }
        } else {
            SQLite::Statement occupied(store.db(),"SELECT 1 FROM automation_data WHERE id=?");occupied.bind(1,data);
            if(occupied.executeStep())throw std::runtime_error("automation: shape data ID already used");
        }
        const Payload clipValue=clip?Payload(*clip):Payload(nullptr);
        if(!remove.empty())result.requests.push_back(deletePoints(data,lane,clipValue,std::move(remove)));
        Payload write=Payload::array();std::uint64_t candidate=1;
        for(auto& p:generated) {
            while(used.contains(candidate)) {
                if(candidate==UINT64_MAX)throw std::runtime_error("automation: no shape point identity available");
                ++candidate;
            }
            p.point_id=candidate;used.insert(candidate);
            write.push_back({{"id",p.point_id},{"time",p.time},{"value",p.value},{"curve",p.curve},{"tension",p.tension}});
        }
        result.requests.push_back(writePoints(data,lane,clipValue,std::move(write)));
        for(auto& request:result.requests)request.label="Record Automation";
        if(txn)txn->commit();
        error.clear();return result;
    } catch(const std::exception& e) { error=e.what();return std::nullopt; }
}
std::optional<PreparedShape> prepareShapeInsertion(Store& store,std::int64_t data,std::int64_t lane,
    std::optional<std::int64_t> clip,const ShapeSpec& spec,std::string& error) {
    try {
        const auto points=generateShape(spec);
        auto prepared=prepareAutomationWrite(store,data,lane,clip,points,error);
        if(prepared)for(auto& request:prepared->requests)request.label="Insert Automation Shape";
        return prepared;
    }catch(const std::exception& e){error=e.what();return std::nullopt;}
}
CommitResult commitShapeInsertion(Store& store,const PreparedShape& p) {
    if(p.owner!=&store || p.client!=store.clientId() || p.requests.empty()) {
        CommitResult r;r.error="automation: shape belongs to another open document or is empty";return r;
    }
    CommitOptions options;options.expectHead.emplace(p.head);
    return OpJournal(store).commit(p.requests,options);
}
} // namespace adi::automation
