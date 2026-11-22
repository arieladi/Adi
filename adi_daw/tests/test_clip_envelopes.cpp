// SPDX-License-Identifier: AGPL-3.0-or-later
#include "adi/engine/automation.hpp"
#include "adi/engine/curves.hpp"
#include "adi/engine/mixer.hpp"
#include "adi/engine/param_automation.hpp"
#include "adi/audio/io_audit.hpp"
#include "adi/automation_edits.hpp"
#include "adi/history.hpp"
#include "adi/store.hpp"
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
void* operator new(std::size_t n){if(adi::audio::onAudioThread)++allocations;if(void* p=std::malloc(n?n:1))return p;throw std::bad_alloc();}
void* operator new[](std::size_t n){return operator new(n);}
void operator delete(void* p) noexcept{std::free(p);}
void operator delete[](void* p) noexcept{std::free(p);}
void operator delete(void* p,std::size_t) noexcept{std::free(p);}
void operator delete[](void* p,std::size_t) noexcept{std::free(p);}
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
int main(){try{placement();signedPrefixAndNativeEnd();curvesAndTempo();loopsAndOverlap();boundariesAndRealtime();rendersAndStore();refusals();}catch(const std::exception& e){++failures;std::printf("EXCEPTION: %s\n",e.what());}std::printf("%d checks, %d failures\n",checks,failures);return failures?1:0;}
