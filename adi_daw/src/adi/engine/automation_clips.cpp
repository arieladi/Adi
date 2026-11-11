// SPDX-License-Identifier: AGPL-3.0-or-later
#include "adi/engine/automation_clips.hpp"
#include "adi/engine/curves.hpp"
#include <algorithm>
#include <cmath>
#include <limits>
#include <set>
#include <stdexcept>
#include <tuple>

namespace adi::engine {
struct AutomationClock {
    struct Knot { long double tick=0, seconds=0, ticksPerSecond=0; };
    double rate=0;
    std::vector<Knot> knots;
    long double secondsAt(long double tick) const noexcept {
        auto i=std::upper_bound(knots.begin(),knots.end(),tick,[](long double t,const Knot& k){return t<k.tick;});
        if(i!=knots.begin())--i;
        return i->seconds+(tick-i->tick)/i->ticksPerSecond;
    }
    long double ticksAt(long double seconds) const noexcept {
        auto i=std::upper_bound(knots.begin(),knots.end(),seconds,[](long double t,const Knot& k){return t<k.seconds;});
        if(i!=knots.begin())--i;
        return i->tick+(seconds-i->seconds)*i->ticksPerSecond;
    }
};
namespace {
constexpr std::size_t budget=1u<<20;
constexpr long double maxTime=static_cast<long double>(std::int64_t{1}<<50);
std::int64_t sampleAt(long double seconds,double rate) {
    const long double n=seconds*static_cast<long double>(rate);
    if(!std::isfinite(n) || n<0 || n>maxTime)throw std::runtime_error("clip time outside supported range");
    return static_cast<std::int64_t>(std::llround(n));
}
std::shared_ptr<const AutomationClock> clockFor(const rows::Model& model,double rate) {
    auto clock=std::make_shared<AutomationClock>();clock->rate=rate;
    clock->knots.push_back({0,0,5765760.0L*2});
    for(const auto& row:model.tempo) {
        if(row.posTicks<0 || !std::isfinite(row.bpm) || row.bpm<=0 ||
            static_cast<long double>(row.posTicks)<clock->knots.back().tick)
            throw std::runtime_error("invalid step tempo map");
        const auto t=static_cast<long double>(row.posTicks);
        const auto seconds=clock->secondsAt(t);
        AutomationClock::Knot knot{t,seconds,5765760.0L*static_cast<long double>(row.bpm)/60};
        if(!std::isfinite(seconds) || !std::isfinite(knot.ticksPerSecond) || knot.ticksPerSecond<=0)throw std::runtime_error("invalid step tempo map");
        if(t==clock->knots.back().tick)clock->knots.back()=knot;else clock->knots.push_back(knot);
    }
    return clock;
}
std::optional<std::int64_t> deviceTrack(const rows::Model& model,std::int64_t device) {
    std::set<std::int64_t> visited;
    while(visited.insert(device).second) {
        const auto d=std::find_if(model.devices.begin(),model.devices.end(),[&](const auto& v){return v.id==device;});
        if(d==model.devices.end())return {};
        const auto c=std::find_if(model.deviceChains.begin(),model.deviceChains.end(),[&](const auto& v){return v.id==d->chainId;});
        if(c==model.deviceChains.end())return {};
        if(c->trackId)return c->trackId;
        if(!c->parentDeviceId)return {};
        device=*c->parentDeviceId;
    }
    return {};
}
std::int64_t roundedSample(long double seconds,double rate) noexcept {
    const auto value=seconds*static_cast<long double>(rate);
    if(value>=static_cast<long double>(INT64_MAX))return INT64_MAX;
    if(value<=static_cast<long double>(INT64_MIN))return INT64_MIN;
    if(!std::isfinite(value))return INT64_MAX;
    return static_cast<std::int64_t>(std::llround(value));
}
long double cycleAt(const ClipEnvelope& e,const AutomationClock& clock,std::int64_t sample) noexcept {
    if(!e.loop)return 0;
    const auto tick=clock.ticksAt(static_cast<long double>(sample)/static_cast<long double>(clock.rate))-e.originTick+e.offsetTicks;
    auto cycle=tick>=e.loopStart?std::floor((tick-e.loopStart)/e.loopLength):0;
    const auto next=clock.secondsAt(e.originTick+e.loopStart+(cycle+1)*e.loopLength-e.offsetTicks);
    if(roundedSample(next,clock.rate)<=sample)++cycle;
    else if(cycle>0 && roundedSample(clock.secondsAt(e.originTick+e.loopStart+cycle*e.loopLength-e.offsetTicks),clock.rate)>sample)--cycle;
    return cycle;
}
bool wrapBoundary(const ClipEnvelope& e,const AutomationClock& clock,std::int64_t sample,long double cycle) noexcept {
    return e.loop && cycle>0 && roundedSample(clock.secondsAt(e.originTick+e.loopStart+cycle*e.loopLength-e.offsetTicks),clock.rate)==sample;
}
long double sourceTime(const ClipEnvelope& e,const AutomationClock& clock,std::int64_t sample,long double cycle,bool wrap) noexcept {
    const auto seconds=sample==e.begin?e.originSeconds:static_cast<long double>(sample)/static_cast<long double>(clock.rate);
    auto ticks=sample==e.begin?e.offsetTicks:clock.ticksAt(seconds)-e.originTick+e.offsetTicks;
    if(e.loop) {
        ticks-=cycle*e.loopLength;
        if(cycle>0 || e.offsetTicks>=e.loopStart)ticks=std::clamp(ticks,e.loopStart,e.loopStart+e.loopLength);
        if(wrap)ticks=e.loopStart;
    }
    if(!e.nanos)return ticks;
    if(!e.loop)return (seconds-e.originSeconds+clock.secondsAt(e.originTick+e.offsetTicks)-e.originSeconds)*1e9L;
    return (clock.secondsAt(e.originTick+ticks)-e.originSeconds)*1e9L;
}
std::int64_t pointSample(const AutomationPoint& p,const ClipEnvelope& e,const AutomationClock& clock,long double cycle) noexcept {
    if(e.nanos && !e.loop)return roundedSample(e.originSeconds+static_cast<long double>(p.time)*1e-9L-
        (clock.secondsAt(e.originTick+e.offsetTicks)-e.originSeconds),clock.rate);
    const auto tick=e.nanos?clock.ticksAt(e.originSeconds+static_cast<long double>(p.time)*1e-9L)-e.originTick:
        static_cast<long double>(p.time);
    return roundedSample(clock.secondsAt(e.originTick+tick-e.offsetTicks+cycle*e.loopLength),clock.rate);
}
void changesFor(const ClipEnvelope& e,const AutomationClock& clock,std::vector<std::int64_t>& out) {
    auto add=[&](long double seconds) {
        const auto n=seconds*static_cast<long double>(clock.rate);
        if(n<static_cast<long double>(e.begin)-0.5L || n>static_cast<long double>(e.end)+0.5L)return;
        const auto at=sampleAt(seconds,clock.rate);
        if(at>=e.begin && at<=e.end) {
            if(out.size()>=budget)throw std::runtime_error("clip-envelope boundary budget exceeded");
            out.push_back(at);
        }
    };
    out.push_back(e.begin);out.push_back(e.end);
    const auto elapsedEnd=clock.ticksAt(static_cast<long double>(e.end)/clock.rate)-e.originTick;
    const auto loopEnd=e.loopStart+e.loopLength;
    if(e.loop && elapsedEnd/e.loopLength+2>static_cast<long double>(budget))
        throw std::runtime_error("clip-envelope repeat budget exceeded");
    for(const auto& p:e.points) {
        const long double tick=e.nanos?clock.ticksAt(e.originSeconds+static_cast<long double>(p.time)*1e-9L)-e.originTick:
            static_cast<long double>(p.time);
        if(!e.loop) {
            if(e.nanos)add(e.originSeconds+static_cast<long double>(p.time)*1e-9L-
                (clock.secondsAt(e.originTick+e.offsetTicks)-e.originSeconds));
            else if(tick>=e.offsetTicks)add(clock.secondsAt(e.originTick+tick-e.offsetTicks));
            continue;
        }
        if(e.offsetTicks<e.loopStart && tick>=e.offsetTicks && tick<loopEnd)
            add(clock.secondsAt(e.originTick+tick-e.offsetTicks));
        if(tick<e.loopStart || tick>=loopEnd)continue;
        const auto first=e.offsetTicks<e.loopStart?loopEnd-e.offsetTicks:
            -std::fmod(e.offsetTicks-e.loopStart,e.loopLength);
        for(auto base=first;base<elapsedEnd;base+=e.loopLength) {
            const auto local=base+tick-e.loopStart;
            if(local>=0)add(clock.secondsAt(e.originTick+local));
        }
    }
    if(e.loop) {
        const auto first=e.offsetTicks<e.loopStart?loopEnd-e.offsetTicks:
            e.loopLength-std::fmod(e.offsetTicks-e.loopStart,e.loopLength);
        for(auto local=first;local<elapsedEnd;local+=e.loopLength)
            add(clock.secondsAt(e.originTick+local));
    }
}
} // namespace
std::optional<double> clipEnvelopeValue(const AutomationLaneProgram& lane,std::int64_t sample) noexcept {
    if(!lane.clipClock || lane.clipRegions.empty())return {};
    const auto& regions=lane.clipRegions;
    auto r=std::upper_bound(regions.begin(),regions.end(),sample,[](auto at,const auto& v){return at<v.begin;});
    if(r==regions.begin())return {};
    --r;if(sample>=r->end)return {};
    const auto& e=lane.clipEnvelopes[r->envelope];const auto& points=e.points;
    const auto cycle=cycleAt(e,*lane.clipClock,sample);
    const auto wrap=wrapBoundary(e,*lane.clipClock,sample,cycle);
    const auto time=sourceTime(e,*lane.clipClock,sample,cycle,wrap);
    auto p=std::upper_bound(points.begin(),points.end(),sample,[&](std::int64_t at,const auto& v){return at<pointSample(v,e,*lane.clipClock,cycle);});
    if(p==points.begin())return p->value;
    if(p==points.end())return points.back().value;
    const auto& a=*(p-1);const auto& b=*p;
    if(!wrap && pointSample(a,e,*lane.clipClock,cycle)==sample)return a.value;
    const auto x=static_cast<double>((time-static_cast<long double>(a.time))/
        (static_cast<long double>(b.time)-static_cast<long double>(a.time)));
    return curveValue(a.value,b.value,x,a.curve,a.tension);
}
void compileClipEnvelopes(AutomationLaneProgram& lane,const rows::AutomationLane& row,
    const rows::Model& model,double rate,std::vector<std::string>& problems) {
    const auto prefix="automation_lanes#"+std::to_string(row.id)+": ";
    if(std::none_of(model.automationData.begin(),model.automationData.end(),[&](const auto& d){return d.laneId==row.id && d.clipId.has_value();}))return;
    if(lane.points.size()>budget){problems.push_back(prefix+"clip-envelope boundary budget exceeded");return;}
    if(row.timeBase!=0 && row.timeBase!=1){problems.push_back(prefix+"clip envelope: unknown lane time base");return;}
    std::size_t compiledPoints=0;
    for(const auto& p:lane.points)lane.changes.push_back(p.sample);
    for(const auto& data:model.automationData) {
        if(data.laneId!=row.id || !data.clipId)continue;
        const auto name=prefix+"clip envelope (clips#"+std::to_string(*data.clipId)+"): ";
        try {
            const auto c=std::find_if(model.clips.begin(),model.clips.end(),[&](const auto& v){return v.id==*data.clipId;});
            if(c==model.clips.end())throw std::runtime_error("missing clip");
            if(c->muted)continue;
            if(c->aliasOf)throw std::runtime_error("alias envelope unsupported");
            auto track=row.ownerKind=="track"?std::optional<std::int64_t>(row.ownerId):deviceTrack(model,row.ownerId);
            if(row.ownerKind=="routing") {
                track.reset();
                const auto route=std::find_if(model.routing.begin(),model.routing.end(),[&](const auto& r){return r.id==row.ownerId;});
                if(route!=model.routing.end() && route->kind=="send" && route->srcKind=="track")track=route->srcId;
            }
            if(!track || *track!=c->trackId)throw std::runtime_error("clip does not belong to lane owner's track");
            if(!lane.clipClock)lane.clipClock=clockFor(model,rate);
            const auto& clock=*lane.clipClock;ClipEnvelope e;e.clipId=c->id;e.nanos=row.timeBase==1;e.loop=c->loopEnabled;
            if(c->timeBase==0) {
                if(!c->posTicks || !c->lengthTicks || *c->posTicks<0 || *c->lengthTicks<=0 ||
                    *c->posTicks>INT64_MAX-*c->lengthTicks)throw std::runtime_error("invalid musical placement");
                e.originTick=static_cast<long double>(*c->posTicks);e.originSeconds=clock.secondsAt(e.originTick);
                e.begin=sampleAt(e.originSeconds,rate);e.end=sampleAt(clock.secondsAt(e.originTick+static_cast<long double>(*c->lengthTicks)),rate);
            } else if(c->timeBase==1) {
                if(!c->posNs || !c->lengthNs || *c->posNs<0 || *c->lengthNs<=0 || *c->posNs>INT64_MAX-*c->lengthNs)
                    throw std::runtime_error("invalid linear placement");
                e.originSeconds=static_cast<long double>(*c->posNs)*1e-9L;e.originTick=clock.ticksAt(e.originSeconds);
                e.begin=sampleAt(e.originSeconds,rate);e.end=sampleAt(static_cast<long double>(*c->posNs+*c->lengthNs)*1e-9L,rate);
            } else throw std::runtime_error("unknown clip time base");
            if(e.end<=e.begin || static_cast<long double>(c->contentOffsetTicks)<-maxTime || c->loopStartTicks<0)throw std::runtime_error("invalid clip interval or offset");
            e.offsetTicks=static_cast<long double>(c->contentOffsetTicks);e.loopStart=static_cast<long double>(c->loopStartTicks);
            e.loopLength=static_cast<long double>(c->loopLenTicks.value_or(0));
            if(e.offsetTicks>maxTime || e.loopStart>maxTime || e.loopLength>maxTime ||
                (e.loop && (e.loopLength<=0 || c->loopStartTicks>INT64_MAX-c->loopLenTicks.value_or(0))))
                throw std::runtime_error("invalid clip loop");
            if(e.loop)for(const auto& knot:clock.knots)
                if(e.loopLength/knot.ticksPerSecond*static_cast<long double>(rate)<1)
                    throw std::runtime_error("clip loop shorter than one session sample");
            if(std::any_of(lane.clipEnvelopes.begin(),lane.clipEnvelopes.end(),[&](const auto& old){return old.clipId==e.clipId;}))
                throw std::runtime_error("duplicate clip-envelope address");
            const StreamReader<AutomationPoint> reader(data.blob,FourCC::Automation);
            if(!reader.ok() || reader.count()>budget-compiledPoints || (reader.header().flags&~(StreamFlags::SortedByTime|StreamFlags::TimeIsNanos))!=0 ||
                (((reader.header().flags&StreamFlags::TimeIsNanos)!=0)!=e.nanos))throw std::runtime_error("invalid stream or time unit");
            for(std::uint32_t n=0;n<reader.count();++n) {
                const auto p=reader.at(n);
                if(!p || p->time<0 || p->reserved || p->curve>kMaxCurve || !std::isfinite(p->value) || !std::isfinite(p->tension) ||
                    (row.minValue && p->value<*row.minValue) || (row.maxValue && p->value>*row.maxValue) ||
                    (!e.points.empty() && p->time<e.points.back().time))throw std::runtime_error("invalid point; whole clip envelope refused");
                e.points.push_back(*p);
            }
            if(e.points.empty())continue; // empty clips do not mask arrangement automation
            if(lane.clipEnvelopes.size()>=16384)throw std::runtime_error("clip-envelope placement budget exceeded");
            std::vector<std::int64_t> boundaries;changesFor(e,clock,boundaries);
            if(lane.changes.size()+boundaries.size()>budget)throw std::runtime_error("lane boundary budget exceeded");
            compiledPoints+=e.points.size();lane.changes.insert(lane.changes.end(),boundaries.begin(),boundaries.end());lane.clipEnvelopes.push_back(std::move(e));
        }catch(const std::exception& ex){problems.push_back(name+ex.what());}
    }
    struct Event {std::int64_t at;bool begin;std::size_t index;};std::vector<Event> events;
    for(std::size_t i=0;i<lane.clipEnvelopes.size();++i) {const auto& e=lane.clipEnvelopes[i];events.push_back({e.begin,true,i});events.push_back({e.end,false,i});}
    std::sort(events.begin(),events.end(),[](const Event& a,const Event& b){return std::tie(a.at,a.begin,a.index)<std::tie(b.at,b.begin,b.index);});
    std::set<std::tuple<std::int64_t,std::int64_t,std::size_t>> active;
    for(std::size_t i=0;i<events.size();) {
        const auto at=events[i].at;
        while(i<events.size() && events[i].at==at) {
            const auto& event=events[i++];const auto& e=lane.clipEnvelopes[event.index];const auto key=std::make_tuple(e.begin,e.clipId,event.index);
            if(event.begin)active.insert(key);else active.erase(key);
        }
        if(i==events.size() || active.empty())continue;
        const auto index=std::get<2>(*active.rbegin());const auto end=events[i].at;
        if(!lane.clipRegions.empty() && lane.clipRegions.back().end==at && lane.clipRegions.back().envelope==index)lane.clipRegions.back().end=end;
        else lane.clipRegions.push_back({at,end,index});
    }
    std::sort(lane.changes.begin(),lane.changes.end());lane.changes.erase(std::unique(lane.changes.begin(),lane.changes.end()),lane.changes.end());
}
} // namespace adi::engine
