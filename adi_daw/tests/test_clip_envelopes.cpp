// SPDX-License-Identifier: AGPL-3.0-or-later
#include "adi/engine/automation.hpp"
#include "adi/engine/curves.hpp"
#include "adi/engine/mixer.hpp"
#include "adi/engine/param_automation.hpp"
#include "adi/engine/snapshot.hpp"
#include "adi/audio/io_audit.hpp"
#include "adi/automation_edits.hpp"
#include "adi/history.hpp"
#include "adi/store.hpp"
#include "replace_operator_new.hpp"
#include "temp_directory.hpp"
#include <SQLiteCpp/SQLiteCpp.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <new>
#include <stdexcept>
namespace {std::atomic<unsigned> allocations{0};}
void adi::test::countReplacedNew() noexcept{if(adi::audio::onAudioThread)++allocations;}
namespace {
using namespace adi;using namespace adi::engine;
constexpr std::int64_t Q=5765760;
int checks=0,failures=0;
void check(bool ok,const char* label){++checks;if(!ok){++failures;std::printf("FAIL: %s\n",label);}}
bool near(double a,double b,double epsilon=1e-10){return std::abs(a-b)<epsilon;}
AutomationPoint pt(std::int64_t time,double value,std::uint8_t curve=1,float tension=0){AutomationPoint p{};p.time=time;p.value=value;p.curve=curve;p.tension=tension;p.point_id=static_cast<std::uint64_t>(time)+1;return p;}
rows::Model model(bool nanos=false,bool device=false) {
    rows::Model m;rows::AutomationLane lane;lane.id=1;lane.ownerKind=device?"device":"track";lane.ownerId=device?3:7;
    lane.paramRef=device?"7":"volume";lane.timeBase=nanos?1:0;lane.valueDomain=device?"normalized":"real";lane.defaultValue=0.25;m.automationLanes.push_back(lane);
    if(device){rows::DeviceChain chain;chain.id=2;chain.trackId=7;m.deviceChains.push_back(chain);rows::Device d;d.id=3;d.chainId=2;m.devices.push_back(d);}
    return m;
}
rows::Clip clip(std::int64_t id,std::int64_t begin,std::int64_t length) {
    rows::Clip c;c.id=id;c.trackId=7;c.kind="midi";c.posTicks=begin;c.lengthTicks=length;return c;
}
void data(rows::Model& m,std::optional<std::int64_t> c,const std::vector<AutomationPoint>& points,bool nanos=false) {
    rows::AutomationData d;d.laneId=1;d.clipId=c;d.blob=writeStream<AutomationPoint>(FourCC::Automation,points,1,
        StreamFlags::SortedByTime|(nanos?StreamFlags::TimeIsNanos:0u));m.automationData.push_back(std::move(d));
}
const AutomationLaneProgram& lane(const AutomationProgram& p){const auto* l=p.lane(1);if(!l)throw std::runtime_error("missing lane");return *l;}
bool problem(const AutomationProgram& p,const char* part){return std::any_of(p.problems().begin(),p.problems().end(),[&](const auto& s){return s.find(part)!=std::string::npos;});}
void placement() {
    for(double rate:{44100.0,48000.0,96000.0,192000.0,384000.0,768000.0}) {
        auto m=model();m.clips.push_back(clip(10,Q,4*Q));data(m,std::nullopt,{pt(0,0.25),pt(10*Q,0.75)});data(m,10,{pt(0,0),pt(2*Q,1)});
        AutomationProgram p(m,rate);const auto& l=lane(p);
        check(p.problems().empty() && l.clipRegions.size()==1,"musical envelope compiles across session rates");
        check(near(valueAt(l,0),0.25) && near(valueAt(l,static_cast<std::int64_t>(rate*0.5)),0) &&
            near(valueAt(l,static_cast<std::int64_t>(rate)),0.5) && near(valueAt(l,static_cast<std::int64_t>(rate*2.5)),0.5),"clip placement, relative phase and arrangement fallback");
    }
    auto m=model(true);auto c=clip(10,0,Q);c.timeBase=1;c.posNs=1000000000;c.lengthNs=2000000000;c.contentOffsetTicks=Q;m.clips.push_back(c);
    data(m,10,{pt(0,0),pt(1000000000,1)},true);AutomationProgram p(m,48000);
    check(near(valueAt(lane(p),48000),0.5) && near(valueAt(lane(p),72000),1) && valueAt(lane(p),144000)==0.25,"nanosecond envelope on linear placement with musical content offset");
    m.clips[0].muted=true;AutomationProgram muted(m,48000);check(lane(muted).clipRegions.empty() && valueAt(lane(muted),48000)==0.25,"muted clip falls back");
}
void signedPrefixAndNativeEnd() {
    auto m=model();auto c=clip(10,0,3*Q);c.contentOffsetTicks=-Q;m.clips.push_back(c);
    data(m,10,{pt(0,0),pt(Q,1)});AutomationProgram prefix(m,48000);
    check(prefix.problems().empty() && valueAt(lane(prefix),0)==0 &&
              valueAt(lane(prefix),24000)==0 && near(valueAt(lane(prefix),36000),0.5) && valueAt(lane(prefix),48000)==1,
          "clip envelope signed source prefix retains its anchored curve and holds before the first point");
    for(const int begin:{0,5}) {
        auto linear=model();auto row=clip(10,0,Q);row.timeBase=1;row.posTicks.reset();row.lengthTicks.reset();
        row.posNs=begin;row.lengthNs=35000000-begin;linear.clips.push_back(row);data(linear,10,{pt(0,0.75)});
        AutomationProgram program(linear,44100);
        check(program.problems().empty() && valueAt(lane(program),1543)==0.75 && valueAt(lane(program),1544)==0.25,
              "linear clip envelope retains its native absolute end through a left-edge change");
    }
}
void curvesAndTempo() {
    bool good=true;
    for(std::uint8_t shape=0;shape<=5;++shape)for(float tension:{-1.0f,0.0f,0.6f,1.0f}) {
        auto m=model();m.tempo={{0,120,0,0},{Q,60,0,0}};m.clips.push_back(clip(10,0,3*Q));data(m,10,{pt(0,-1,shape,tension),pt(2*Q,3)});
        AutomationProgram p(m,48000);const auto& l=lane(p);
        good=good && near(valueAt(l,24000),curveValue(-1,3,0.5,shape,tension)) && near(valueAt(l,48000),curveValue(-1,3,0.75,shape,tension));
    }
    check(good,"all six native curves preserve beat phase across step tempos");
    auto ramp = model();
    ramp.tempo = {{0,90,2,1},{4*Q,210,0,0}};
    ramp.clips.push_back(clip(10,0,4*Q));
    data(ramp,10,{pt(0,0),pt(4*Q,1)});
    AutomationProgram rampProgram(ramp,48000);
    check(rampProgram.problems().empty() && near(valueAt(lane(rampProgram),61907),0.5,2e-5),
          "the clip envelope keeps musical phase through the Bezier tempo midpoint");
    auto m=model();auto c=clip(10,Q,2*Q);c.contentOffsetTicks=Q;m.clips.push_back(c);data(m,10,{pt(0,0,4,0.6f),pt(4*Q,1)});
    AutomationProgram p(m,48000);
    check(near(valueAt(lane(p),24000),curveValue(0,1,0.25,4,0.6f)) && near(valueAt(lane(p),48000),curveValue(0,1,0.5,4,0.6f)),"trimming retains original nonlinear curve phase");
}
void loopsAndOverlap() {
    auto m=model();auto c=clip(10,0,4*Q);c.loopEnabled=true;c.loopStartTicks=Q;c.loopLenTicks=Q;c.contentOffsetTicks=Q+Q/2;m.clips.push_back(c);
    data(m,10,{pt(0,0),pt(Q,0.2),pt(2*Q,0.8),pt(3*Q,1)});AutomationProgram p(m,48000);const auto& l=lane(p);
    check(near(valueAt(l,0),0.5) && near(valueAt(l,12000),0.2) && near(valueAt(l,24000),0.5) && near(valueAt(l,36000),0.2),"offset into loop and repeated half-open wrap");
    check(automationBoundaryAfter(l,0)==12000,"loop wrap is an exact device event boundary");
    auto rounded=model();rounded.tempo={{0,137,0,0}};auto rc=clip(20,0,4*Q);rc.loopEnabled=true;rc.loopLenTicks=Q;rounded.clips.push_back(rc);
    data(rounded,20,{pt(0,0.2),pt(Q,0.8)});AutomationProgram rounding(rounded,48000);
    const auto wrap=static_cast<std::int64_t>(std::llround(60.0/137*48000));
    check(valueAt(lane(rounding),wrap)==0.2,"fractional-sample loop boundary chases the exact loop-start value");
    m.clips[0].contentOffsetTicks=0;AutomationProgram intro(m,48000);
    check(near(valueAt(lane(intro),0),0) && near(valueAt(lane(intro),24000),0.2) && near(valueAt(lane(intro),48000),0.2),"pre-loop intro plays once");
    auto o=model();o.clips={clip(10,0,4*Q),clip(11,Q,Q),clip(12,Q,Q)};
    data(o,10,{pt(0,0.1)});data(o,11,{pt(0,0.9)});data(o,12,{pt(0,0.7)});AutomationProgram overlap(o,48000);
    check(near(valueAt(lane(overlap),0),0.1) && near(valueAt(lane(overlap),24000),0.7) && near(valueAt(lane(overlap),48000),0.1) &&
        near(valueAt(lane(overlap),96000),0.25),"latest-start then clip-ID ownership and resumed underlying clip");
    std::reverse(o.clips.begin(),o.clips.end());std::reverse(o.automationData.begin(),o.automationData.end());AutomationProgram reordered(o,48000);
    check(near(valueAt(lane(reordered),24000),0.7),"overlap precedence is independent of row order");
}
void boundariesAndRealtime() {
    auto m=model(true,true);auto c=clip(10,0,Q);c.timeBase=1;c.posNs=2000000000;c.lengthNs=1000000000;m.clips.push_back(c);
    data(m,10,{pt(0,0.1,0),pt(100000,0.9)},true);auto program=compileAutomation(m,48000);
    const auto& l=lane(*program);
    check(near(valueAt(l,96004),0.1) && near(valueAt(l,96005),0.9),"non-integral source point rounds to its exact audible sample");
    Transport transport;transport.locate(96000);transport.play();auto binding=bindDeviceAutomation(program,{},transport,48000);
    check(binding->byDevice.contains(3),"clip-only device lane is bound");if(!binding->byDevice.contains(3))return;
    std::array<Event,16> events{};EventList output(events.data(),static_cast<std::int32_t>(events.size()));
    allocations=0;audio::onAudioThread=true;binding->byDevice.at(3)->emit(output,16);
    std::array<double,32> values{};fill(l,95996,1,values.size(),values.data());audio::onAudioThread=false;
    check(output.size()==2 && output.at(0).frame==0 && output.at(0).value==0.1 && output.at(1).frame==5 && output.at(1).value==0.9,"device emission includes a point between cadence ticks");
    check(allocations==0,"clip evaluation, fill and device emission allocate nothing on callbacks");
    for(const auto ns:{83400LL,93750LL}) {
        auto rounded=m;rounded.automationData.clear();data(rounded,10,{pt(0,0.1,0),pt(ns,0.9)},true);AutomationProgram p(rounded,48000);
        const auto at=96000+static_cast<std::int64_t>(std::llround(static_cast<double>(ns)*48000/1e9));
        check(valueAt(lane(p),at-1)==0.1 && valueAt(lane(p),at)==0.9,"step rounding down and exact half ties agree with emitted sample boundaries");
    }
    bool same=true;for(std::size_t i=0;i<values.size();++i)same=same && values[i]==valueAt(l,95996+static_cast<std::int64_t>(i));
    check(same,"batched fill agrees at clip boundaries");
    transport.locate(144000);output.clear();binding->byDevice.at(3)->emit(output,16);
    check(output.size()==1 && output.at(0).value==0.25,"seek beyond clip restores arrangement default");
    transport.locate(96000);transport.loop(96000,96008);output.clear();binding->byDevice.at(3)->emit(output,16);
    check(output.size()>=4 && output.at(0).value==0.1 && output.at(1).frame==5 && output.at(2).frame==8,"transport wraps chase clip envelopes inside a callback");
}
// Integrate one quarter independently in the quadratic Bezier parameter.
// x'(p) is musical progress per unit p, while bpm(p) gives elapsed seconds.
long double bezierQuarterSeconds(const rows::TempoEvent& left, const rows::TempoEvent& right, long double beat) {
    const auto tension = static_cast<long double>(left.tension);
    long double low = 0, high = 1;
    for (int i = 0; i < 48; ++i) {
        const auto p = (low + high) / 2;
        if (p * (1 + tension - tension * p) < beat) low = p;
        else high = p;
    }
    constexpr int panels = 512;
    const auto step = (low + high) / (2 * panels);
    long double sum = 0;
    for (int i = 0; i <= panels; ++i) {
        const auto p = i * step;
        const auto y = p * (1 - tension + tension * p);
        const auto bpm = left.bpm + (right.bpm - left.bpm) * y;
        const auto weight = i == 0 || i == panels ? 1 : i % 2 == 0 ? 2 : 4;
        sum += weight * 60 * (1 + tension - 2 * tension * p) / bpm;
    }
    return sum * step / 3;
}

bool matchesDeviceBoundary(const std::shared_ptr<const AutomationProgram>& program, double rate,
                           std::int64_t at, double before, double after) {
    const auto& l = lane(*program);
    Transport transport;
    transport.locate(at-2);
    transport.play();
    const auto binding = bindDeviceAutomation(program,{},transport,rate);
    const auto device = binding->byDevice.find(3);
    if (device == binding->byDevice.end()) return false;
    std::array<Event,8> storage{};
    EventList events(storage.data(),static_cast<std::int32_t>(storage.size()));
    std::array<double,4> filled{};
    {
        const audio::CallbackScope callback;
        device->second->emit(events,4);
        fill(l,at-2,1,filled.size(),filled.data());
    }
    const std::array<double,4> expected{before,before,after,after};
    return automationBoundaryAfter(l,at-1) == at && filled == expected &&
        events.size() == 2 && events.at(0).frame == 0 && events.at(0).value == before &&
        events.at(1).frame == 2 && events.at(1).value == after;
}

void loopBoundariesAfterBezierRamps() {
    constexpr int lead = 96, laps = 3;
    constexpr long double pointBeat = 3.0L / 8;
    std::array<rows::TempoEvent,lead+laps+1> tempo{};
    for (int i = 0; i <= lead+laps; ++i) {
        auto& row = tempo[static_cast<std::size_t>(i)];
        row = {i*Q, static_cast<double>(75 + (37*i)%115), 2, i%2 == 0 ? -0.5 : 0.75};
    }
    long double prefix = 0;
    std::array<long double,2*laps+1> times{};
    for (std::size_t i = 0; i+1 < tempo.size(); ++i) {
        if (i >= lead) {
            times[2*(i-lead)] = prefix;
            times[2*(i-lead)+1] = prefix + bezierQuarterSeconds(tempo[i],tempo[i+1],pointBeat);
        }
        prefix += bezierQuarterSeconds(tempo[i],tempo[i+1],1);
    }
    times.back() = prefix;
    const auto pointNs = std::llround((times[1] - times[0]) * 1e9L);
    constexpr double low = 0.125, high = 0.875, fallback = 0.25;
    const std::array<double,2*laps+2> values{fallback,low,high,low,high,low,high,fallback};
    for (const bool nanos : {false,true}) {
        auto m = model(nanos,true);
        m.tempo.assign(tempo.begin(),tempo.end());
        auto c = clip(10,lead*Q,laps*Q);
        c.loopEnabled = true;
        c.loopLenTicks = Q;
        m.clips.push_back(c);
        data(m,10,{pt(0,low,0),pt(nanos ? pointNs : 3*Q/8,high,0)},nanos);
        for (const double rate : {44100.0,48000.0,96000.0}) {
            const auto program = compileAutomation(m,rate);
            const auto& l = lane(*program);
            bool blocksMatch = program->problems().empty() && l.clipRegions.size() == 1;
            allocations = 0;
            for (std::size_t i = 0; i < times.size(); ++i) {
                const auto at = static_cast<std::int64_t>(std::llround(times[i] * rate));
                blocksMatch &= matchesDeviceBoundary(program,rate,at,values[i],values[i+1]);
            }
            std::printf("Bezier loop %s at %.0f Hz: %zu exact boundaries, allocations %u\n",
                        nanos ? "nanoseconds" : "ticks",rate,blocksMatch ? times.size() : 0u,allocations.load());
            check(blocksMatch && allocations == 0,
                  "Bezier lead-in preserves exact loop point and wrap samples in fill and device blocks");
        }
    }
}

void tempoRampPreservesPrefixTies() {
    // These tick positions land mathematically halfway between two samples.
    // Read the established rounded boundary through the original TempoMap
    // conversion because the widened sample product differs by platform.
    double rates[]{44100.0,48000.0};
    std::int64_t ticks[]{4576,3003};
    bool matches = true;
    allocations = 0;
    for (std::size_t i = 0; i < std::size(rates); ++i) {
        auto m = model(false,true);
        m.tempo = {{0,120,0,0},{Q,120,2,0.5},{2*Q,80,0,0}};
        m.clips.push_back(clip(10,0,2*Q));
        data(m,10,{pt(0,0.125,0),pt(ticks[i],0.875,0)});
        const auto canonical = TempoMap::fromRows(m.tempo);
        const auto expectedSeconds = canonical.secondsAtPosition(static_cast<long double>(ticks[i]));
        const auto at = static_cast<std::int64_t>(std::llround(expectedSeconds * static_cast<long double>(rates[i])));
        const auto program = compileAutomation(m,rates[i]);
        matches &= program->problems().empty() && matchesDeviceBoundary(program,rates[i],at,0.125,0.875);
    }
    check(matches && allocations == 0,"a later ramp preserves the canonical prefix's exact device boundary at a half-sample tie");
}

void rendersAndStore() {
    auto m=model();m.clips.push_back(clip(10,Q,2*Q));constexpr double db=-6.1234567890123;data(m,10,{pt(0,db)});
    const auto program=compileAutomation(m,48000);const auto binding=bindStripAutomation(program,{});
    check(binding->byTrack.contains(7),"clip-only volume lane binds to a real strip");
    if(!binding->byTrack.contains(7))return;
    for(bool wide:{false,true}) {
        Transport transport;transport.locate(24000);StripNode strip;StripConfig config;
        config.lanes=&binding->byTrack.at(7);config.keepAlive=binding;strip.configure(config,true,&transport);strip.prepare(48000,16);
        std::array<float,16> input{},output{};std::array<double,16> input64{},output64{};input.fill(1);input64.fill(1);
        const float* in[]={input.data()};float* out[]={output.data()};const double* in64[]={input64.data()};double* out64[]={output64.data()};
        NodeIo io;io.channels=1;io.frames=16;if(wide){io.in64=in64;io.out64=out64;}else{io.in=in;io.out=out;}
        allocations=0;audio::onAudioThread=true;strip.process(io);audio::onAudioThread=false;
        const double expected=faderGain(db);bool matches=true;
        for(std::size_t i=0;i<16;++i)matches=matches && near(wide?output64[i]:static_cast<double>(output[i]),expected,wide?1e-14:1e-7);
        check(matches && allocations==0,wide?"double strip renders full-precision clip gain without allocation":"float strip renders clip gain without allocation");
    }
    test::TempDirectory temp("automation","clip_store");StoreError error=StoreError::Ok;
    auto store=Store::createEmpty(temp.path()/"a.adi",error);if(!store)throw std::runtime_error("store");
    store->db().exec("INSERT INTO project(id,name) VALUES(1,'Clips');INSERT INTO tracks(id,kind,name) VALUES(7,'audio','Track');"
        "INSERT INTO clips(id,track_id,kind,time_base,pos_ticks,length_ticks) VALUES(10,7,'midi',0,5765760,11531520);"
        "INSERT INTO automation_lanes(id,owner_kind,owner_id,param_ref,value_domain,default_value) VALUES(1,'track',7,'volume','real',0.25)");
    const auto request=automation::writePoints(3,1,10,Payload::array({{{"id",UINT64_MAX},{"time",0},{"value",db},{"curve",1},{"tension",0}}}));
    check(OpJournal(*store).commit(request).ok,"native addressed clip-envelope edit commits");
    AutomationProgram before(rows::readModel(*store),48000);
    check(near(valueAt(lane(before),24000),db),"Store-to-model-to-engine plays the committed clip envelope");
    check(History(*store).undo().ok,"clip-envelope edit undo");
    AutomationProgram after(rows::readModel(*store),48000);
    check(valueAt(lane(after),24000)==0.25,"undo restores arrangement fallback through the model");
}
void refusals() {
    auto m=model();m.clips.push_back(clip(10,0,2*Q));data(m,10,{pt(0,0),pt(Q,1)});
    auto bad=m;bad.automationLanes[0].timeBase=2;AutomationProgram unknown(bad,48000);check(problem(unknown,"unknown lane time base") && lane(unknown).clipRegions.empty(),"unknown lane clock refuses overlay");
    bad=m;bad.clips[0].trackId=8;AutomationProgram wrong(bad,48000);check(problem(wrong,"owner's track") && lane(wrong).clipRegions.empty(),"cross-track envelope is refused");
    bad=m;bad.clips[0].aliasOf=99;AutomationProgram alias(bad,48000);check(problem(alias,"alias") && lane(alias).clipRegions.empty(),"alias is explicit unsupported rather than wrong material");
    bad=m;bad.clips[0].loopEnabled=true;bad.clips[0].loopLenTicks=0;AutomationProgram loop(bad,48000);check(problem(loop,"invalid clip loop"),"invalid loop refused");
    bad=m;bad.clips[0].loopEnabled=true;bad.clips[0].loopLenTicks=1;AutomationProgram shortLoop(bad,48000);check(problem(shortLoop,"shorter than one"),"sub-sample loop is refused explicitly");
    bad=m;bad.automationData.clear();data(bad,10,{pt(0,0),pt(Q,std::numeric_limits<double>::quiet_NaN())});AutomationProgram invalid(bad,48000);
    check(lane(invalid).clipRegions.empty() && problem(invalid,"whole clip envelope refused"),"bad late point refuses complete clip stream");
    bad=m;bad.automationData[0].blob=writeStream<AutomationPoint>(FourCC::Automation,std::vector<AutomationPoint>{pt(0,1)},1,StreamFlags::TimeIsNanos);AutomationProgram units(bad,48000);
    check(problem(units,"time unit") && lane(units).clipRegions.empty(),"lane and stream units must match");
    bad=m;bad.automationData.clear();data(bad,10,{});AutomationProgram empty(bad,48000);check(empty.problems().empty() && lane(empty).clipRegions.empty(),"empty clip data does not mask arrangement");
    bad=m;bad.clips[0].lengthTicks=INT64_MAX;bad.clips[0].posTicks=1;AutomationProgram overflow(bad,48000);check(problem(overflow,"musical placement"),"overflowing placement refused before arithmetic");
    bad=m;bad.clips[0].loopEnabled=true;bad.clips[0].loopLenTicks=Q;bad.clips[0].lengthTicks=Q*1100000;AutomationProgram huge(bad,48000);check(problem(huge,"repeat budget"),"excessive loop expansion refuses without unbounded allocation");
}
} // namespace
int main(){try{placement();signedPrefixAndNativeEnd();curvesAndTempo();loopsAndOverlap();boundariesAndRealtime();loopBoundariesAfterBezierRamps();tempoRampPreservesPrefixTies();rendersAndStore();refusals();}catch(const std::exception& e){++failures;std::printf("EXCEPTION: %s\n",e.what());}std::printf("%d checks, %d failures\n",checks,failures);return failures?1:0;}
