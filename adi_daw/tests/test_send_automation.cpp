// SPDX-License-Identifier: AGPL-3.0-or-later
#include "adi/engine/session.hpp"
#include "adi/engine/automation_recording.hpp"
#include "adi/audio/io_audit.hpp"
#include "adi/history.hpp"
#include "adi/store.hpp"
#include "temp_directory.hpp"
#include <SQLiteCpp/SQLiteCpp.h>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <new>
#include <stdexcept>
namespace { std::atomic<unsigned> allocations{0}; }
void* operator new(std::size_t n) { if(adi::audio::onAudioThread)++allocations; if(void* p=std::malloc(n?n:1))return p; throw std::bad_alloc(); }
void* operator new[](std::size_t n) { return operator new(n); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p,std::size_t) noexcept { std::free(p); }
void operator delete[](void* p,std::size_t) noexcept { std::free(p); }
namespace {
using namespace adi; using namespace adi::engine;
int checks=0,failures=0;
void check(bool ok,const char* label) { ++checks; if(!ok){++failures;std::printf("FAIL: %s\n",label);} }
bool near(double a,double b,double eps=2e-6) { return std::abs(a-b)<eps; }
std::vector<std::byte> stream(double value) {
    AutomationPoint p{};p.point_id=1;p.curve=1;p.value=value;
    return writeStream<AutomationPoint>(FourCC::Automation,std::span<const AutomationPoint>(&p,1),1,
        StreamFlags::SortedByTime|StreamFlags::TimeIsNanos);
}
class Tone final : public Node {
public:
    void prepare(double,std::int32_t) override {}
    bool processesDouble() const noexcept override { return true; }
    void process(const NodeIo& io) noexcept override {
        for(std::int32_t c=0;c<io.channels;++c)for(std::int32_t f=0;f<io.frames;++f) {
            if(io.wide())io.out64[c][io.blockOffset+f]=1;
            else io.out[c][io.blockOffset+f]=1;
        }
    }
};
struct Fixture {
    test::TempDirectory temp{"automation","sends"};
    StoreError error=StoreError::Ok;
    std::unique_ptr<Store> store=Store::createEmpty(temp.path()/"a.adi",error);
    Tone tone;
    Session session;
    bool wide;
    explicit Fixture(bool useDouble):wide(useDouble) {
        if(!store)throw std::runtime_error("store");
        store->db().exec("INSERT INTO project(id,name) VALUES(1,'Sends');"
            "INSERT INTO tracks(id,kind,name) VALUES(1,'audio','Source'),(2,'return','Return'),(9,'master','Main');"
            "INSERT INTO mixer_strip(track_id) VALUES(1),(2);"
            "INSERT INTO routing(id,src_kind,src_id,dst_kind,dst_id,kind,gain_db) VALUES(1,'track',1,'track',2,'send',-150);"
            "INSERT INTO automation_lanes(id,owner_kind,owner_id,param_ref,time_base,value_domain,default_value) VALUES(2,'routing',1,'gain_db',1,'real',-150)");
        if(!store->putAutomationData(2,std::nullopt,stream(-12)))throw std::runtime_error("stream");
        session.setSourcesFor([this](std::int64_t track){return track==1?std::vector<Node*>{&tone}:std::vector<Node*>{};});
        SessionSpec spec;spec.maxFrames=64;spec.precision=wide?Precision::Float64:Precision::Float32;
        if(!session.load(*store,{},spec))throw std::runtime_error(session.error());
        session.graph().setFadeFrames(0);
    }
    double render(std::int64_t sample=0) {
        session.transport().locate(sample);
        std::array<float,64> l{},r{};std::array<double,64> dl{},dr{};
        float* out[]={l.data(),r.data()};double* out64[]={dl.data(),dr.data()};
        AudioIo io;io.out=out;io.out64=out64;io.numOut=2;io.frames=64;
        {audio::CallbackScope callback;for(int n=0;n<8;++n)session.process(io);}
        return wide?dl.back():static_cast<double>(l.back());
    }
    bool op(const char* type,nlohmann::json payload) {
        OpRequest request;request.opType=type;request.payload=std::move(payload);
        return OpJournal(*store).commit(request).ok && session.refresh(*store);
    }
};
void playback(bool wide) {
    Fixture f(wide);
    check(near(f.render(),1+faderGain(-12)),"send automation raises a stored silent send at both precisions");
    check(f.op("routing.setEnabled",{{"id",1},{"enabled",false}}) && near(f.render(),1),"disabled send gates its automation");
    check(f.op("routing.setEnabled",{{"id",1},{"enabled",true}}) && near(f.render(),1+faderGain(-12)),"send re-enable retains automation");
    check(f.op("mixer.setVolume",{{"id",1},{"db",-6.0}}) && near(f.render(),faderGain(-6)*(1+faderGain(-12))),"post send applies source fader before automated gain");
    f.store->db().exec("UPDATE routing SET pre_fader=1 WHERE id=1");
    check(f.session.refresh(*f.store) && near(f.render(),faderGain(-6)+faderGain(-12)),"pre send leaves source fader out of automated gain");
    f.store->db().exec("UPDATE tracks SET muted=1 WHERE id=1");
    check(f.session.refresh(*f.store) && near(f.render(),0),"source mute still gates automated pre send");
    f.store->db().exec("UPDATE tracks SET muted=0 WHERE id=1;UPDATE routing SET pre_fader=0 WHERE id=1");
    check(f.session.refresh(*f.store),"restore source");
    check(f.store->putAutomationData(2,std::nullopt,stream(60)) && f.session.refresh(*f.store) &&
        near(f.render(),faderGain(-6)*(1+faderGain(kSendTopDb)),2e-5),"automated gain retains the existing +24 dB send ceiling");
    check(f.store->putAutomationData(2,std::nullopt,stream(-12)) && f.session.refresh(*f.store),"restore send curve");
    f.store->db().exec("INSERT INTO clips(id,track_id,kind,time_base,pos_ns,length_ns) VALUES(10,1,'automation',1,1000000000,1000000000)");
    check(f.store->putAutomationData(2,10,stream(-3)) && f.session.refresh(*f.store) &&
        near(f.render(48000),faderGain(-6)*(1+faderGain(-3))) && near(f.render(96000),faderGain(-6)*(1+faderGain(-12))),
        "send clip envelope follows source placement and half-open arrangement fallback on seek");
}
void writePolicy() {
    Fixture f(false);
    f.store->db().exec("INSERT INTO automation_lanes(id,owner_kind,owner_id,param_ref,time_base,value_domain) VALUES(4,'track',1,'volume',1,'real')");
    check(f.store->putAutomationData(4,std::nullopt,stream(-6)) && f.session.refresh(*f.store),"track and send share a numeric owner ID");
    check(f.op("routing.setGain",{{"id",1},{"db",-6.0}}) && f.session.overriddenLanes().contains(2) &&
        !f.session.overriddenLanes().contains(4) && near(f.render(),faderGain(-6)*(1+faderGain(-6))),"manual send override never collides with track automation");
    check(f.session.reenableAutomation(2) && near(f.render(),faderGain(-6)*(1+faderGain(-12))),"send Re-Enable resumes stored automation");
    check(!f.session.beginAutomationWrite(2),"record-off rejects send write");
    f.session.setAutomationWriteState(true,true);
    check(f.session.beginAutomationWrite(2) && f.session.writingAutomationLanes().contains(2),"send lane participates in armed write suspension");
    check(f.op("routing.setGain",{{"id",1},{"db",-3.0}}) && !f.session.automationOverridden() &&
        near(f.render(),faderGain(-6)*(1+faderGain(-3))),"recorded send movement remains manual while suspended without override");
    const auto id=f.store->db().execAndGet("SELECT id FROM automation_data WHERE lane_id=2 AND clip_id IS NULL").getInt64();
    AutomationRecorder recorder({id,2,std::nullopt},8);recorder.setRecording(true,0);
    recorder.push(AutomationGesture::Begin,0,-9);recorder.push(AutomationGesture::End,1000000000,-9);
    std::vector<RecordedAutomation> recorded;recorder.drain(recorded);
    check(recorded.size()==1,"timestamped send gesture captured");if(recorded.empty())return;
    f.session.setAutomationWriteState(true,false);std::string why;
    const auto prepared=prepareRecordedAutomation(*f.store,recorded.front(),why);
    check(prepared && automation::commitShapeInsertion(*f.store,*prepared).ok && f.session.refresh(*f.store) &&
        f.session.finishAutomationWrite(2) && near(f.render(),faderGain(-6)*(1+faderGain(-9))),"captured send gesture commits and resumes through real Session");
    check(f.op("routing.setEnabled",{{"id",1},{"enabled",false}}) && !f.session.automationOverridden(),"send enable is a gate rather than a gain override");
    f.store->db().exec("DELETE FROM automation_lanes WHERE id=2");
    check(f.session.refresh(*f.store) && !f.session.overriddenLanes().contains(2),"deleted send lane prunes override state");
}
rows::Model model() {
    rows::Model m;rows::Track a;a.id=1;a.kind="audio";a.soloed=true;m.tracks.push_back(a);
    a.id=2;a.soloed=false;m.tracks.push_back(a);a.id=9;a.kind="master";m.tracks.push_back(a);
    rows::Routing r;r.id=1;r.srcKind="track";r.srcId=1;r.dstKind="track";r.dstId=2;r.kind="send";r.gainDb=-150;m.routing.push_back(r);
    rows::AutomationLane l;l.id=2;l.ownerKind="routing";l.ownerId=1;l.paramRef="gain_db";l.valueDomain="real";l.defaultValue=-12;m.automationLanes.push_back(l);
    return m;
}
void validationAndSolo() {
    auto m=model();auto p=compileAutomation(m,48000);auto b=bindStripAutomation(p,{});
    check(p->problems().empty() && b->bySend.contains(1) && b->sendLaneFor.at(1)==2 && b->byTrack.empty(),"routing gain binds in a separate send namespace");
    MixerStrips strips;Transport transport;strips.sync(m,&transport,b);
    check(strips.stripFor(2)!=nullptr,"solo fixture has destination strip");
    // Destination is not a return: solo reachability must include this send.
    // Its source strip is audible, while destination strip's gate is tested by rendering it.
    std::array<float,64> samples;samples.fill(1);const float* in[]={samples.data(),samples.data()};
    std::array<float,64> left{},right{};float* out[]={left.data(),right.data()};NodeIo io;io.in=in;io.out=out;io.channels=2;io.frames=64;
    auto* destination=strips.stripFor(2);destination->prepare(48000,64);destination->process(io);
    check(left.back()>0.99f,"automated non-silent send opens solo reachability despite silent stored knob");
    m.automationLanes[0].defaultValue=-150;b=bindStripAutomation(compileAutomation(m,48000),{});strips.sync(m,&transport,b);
    for(int n=0;n<8;++n)destination->process(io);
    check(left.back()==0,"wholly silent automation does not open solo reachability");
    m.automationLanes[0].paramRef="pan";b=bindStripAutomation(compileAutomation(m,48000),{});
    check(b->bySend.empty() && !b->problems.empty(),"unsupported routing parameter is named and not played");
    m.automationLanes[0].paramRef="gain_db";m.automationLanes[0].valueDomain="normalized";b=bindStripAutomation(compileAutomation(m,48000),{});
    check(b->bySend.empty() && !b->problems.empty(),"send dB domain is explicit");
    m.automationLanes[0].valueDomain="real";m.automationLanes[0].enabled=false;b=bindStripAutomation(compileAutomation(m,48000),{});
    check(b->bySend.empty() && b->sendLaneFor.empty(),"disabled send lane is not bound");
    m.automationLanes[0].enabled=true;m.automationLanes.push_back(m.automationLanes[0]);m.automationLanes.back().id=3;
    b=bindStripAutomation(compileAutomation(m,48000),{});check(b->sendLaneFor.at(1)==2 && !b->problems.empty(),"duplicate send lane reports first-wins ownership");
    m.automationLanes.resize(1);m.routing[0].kind="sidechain";p=compileAutomation(m,48000);
    check(p->lane(2)==nullptr && !p->problems().empty(),"sidechain routing is not treated as a send");
    m.routing.clear();p=compileAutomation(m,48000);check(p->lane(2)==nullptr && !p->problems().empty(),"missing send is named");
    m=model();rows::Clip clip;clip.id=10;clip.trackId=2;clip.kind="automation";clip.posTicks=0;clip.lengthTicks=5765760;m.clips.push_back(clip);
    rows::AutomationData d;d.laneId=2;d.clipId=10;d.blob=stream(-3);m.automationData.push_back(d);m.automationLanes[0].timeBase=1;
    p=compileAutomation(m,48000);check(p->lane(2)->clipRegions.empty() && !p->problems().empty(),"send clip envelope must belong to its source track");
}
void lifetimeAndPdc() {
    SendNode send;Transport transport;auto a=std::make_shared<int>(1),b=std::make_shared<int>(2);
    std::weak_ptr<int> wa=a,wb=b;StripConfig source;source.keepAlive=a;source.delaySamples=37;
    StripLanes sourceLanes,lanes;source.lanes=&sourceLanes;
    send.setSource(source,&transport);send.setAutomation(&lanes,b);send.prepare(48000,64);
    check(send.latencySamples()==-37,"automating send gain adds no PDC latency");
    auto held=send.sourceLifetime();a.reset();b.reset();source.keepAlive.reset();
    send.setSource({},&transport);send.setAutomation(nullptr,{});
    check(!wa.expired() && !wb.expired(),"retired graph retains independent source and send programs");
    held.reset();check(wa.expired() && wb.expired(),"both programs retire after last graph releases its holder");
}
}
int main() {
    try {playback(false);playback(true);writePolicy();validationAndSolo();lifetimeAndPdc();check(allocations==0,"send playback and Session callbacks allocate nothing");}
    catch(const std::exception& e){std::printf("EXCEPTION: %s\n",e.what());check(false,"fixture must complete");}
    std::printf("send_automation: %d checks, %d failures\n",checks,failures);return failures?1:0;
}
