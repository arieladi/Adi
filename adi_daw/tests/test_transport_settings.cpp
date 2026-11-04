// SPDX-License-Identifier: AGPL-3.0-or-later
#include "adi/transport_settings.hpp"
#include "adi/engine/session.hpp"
#include "adi/engine/snapshot.hpp"
#include "adi/history.hpp"
#include "adi/store.hpp"
#include "temp_directory.hpp"
#include <SQLiteCpp/SQLiteCpp.h>
#include <algorithm>
#include <array>
#include <cstdio>
namespace {
using namespace adi;using namespace adi::engine;
int checks=0,failures=0;
void check(bool good,const char* name){++checks;if(!good){++failures;std::printf("FAIL: %s\n",name);}}
struct Fixture {
    test::TempDirectory temp{"transport_settings","project"};std::unique_ptr<Store> store;
    Fixture(){StoreError error{};store=Store::create(temp.path()/"song.adi",error);if(!store)throw std::runtime_error("fixture");
        store->db().exec("INSERT INTO tracks(id,kind,name) VALUES(9,'master','Main');INSERT INTO mixer_strip(track_id) VALUES(9);INSERT INTO tempo_map(id,pos_ticks,bpm,curve) VALUES(1,0,120,0)");}
    CommitResult op(const char* name,Payload payload){OpRequest request;request.opType=name;request.payload=std::move(payload);return OpJournal(*store).commit(request);}
};
void storage() {
    Fixture f;const auto initial=SnapshotBuilder::fromStore(*f.store);
    check(!initial->transport.metronome && !initial->transport.loopEnabled,"new project has loop/click off");
    const auto changed=f.op("project.setName",{{"name","Undo this"}});
    const auto head=History(*f.store).head();
    check(changed.ok && f.op("transport.setMetronome",{{"on",true}}).ok,"metronome op applies saved performance state");
    check(f.op("transport.setLoop",{{"on",true},{"start",100},{"end",1000}}).ok,"loop op applies a tick window");
    const auto next=SnapshotBuilder::fromStore(*f.store,*initial);
    check(next->transport.metronome,"snapshot carries metronome state");
    check(next->transport.loopEnabled,"snapshot carries loop switch");
    check(next->transport.loopStartTicks==100,"snapshot carries loop start ticks");
    check(next->transport.loopEndTicks==1000,"snapshot carries loop end ticks");
    check(next->tracks.front()==initial->tracks.front(),"transport-only snapshot retains unchanged track nodes");
    check(History(*f.store).head()==head && History(*f.store).undo().ok,"performance ops leave undo at the musical edit");
    check(readTransportSettings(*f.store)==next->transport,"undo leaves performance settings intact");
    check(f.op("transport.setLoop",{{"on",false}}).ok,"loop off can retain its window");
    const auto retained=readTransportSettings(*f.store);
    check(!retained.loopEnabled && retained.loopStartTicks==100 && retained.loopEndTicks==1000,"loop bounds survive toggling off");
    const auto count=f.store->db().execAndGet("SELECT count(*) FROM ops").getInt();
    check(!f.op("transport.setLoop",{{"on",true},{"start",1000},{"end",100}}).ok && !f.op("transport.setLoop",{{"on",true},{"start",-1},{"end",100}}).ok,"reversed and negative windows are refused");
    check(f.store->db().execAndGet("SELECT count(*) FROM ops").getInt()==count && readTransportSettings(*f.store)==retained,"invalid loop changes neither rows nor journal");
    OpRequest on;on.opType="transport.setMetronome";on.payload={{"on",false}};
    OpRequest invalid;invalid.opType="transport.setLoop";invalid.payload={{"on",true},{"start",2},{"end",1}};
    check(!OpJournal(*f.store).commit(std::vector<OpRequest>{on,invalid}).ok && readTransportSettings(*f.store).metronome,"invalid batched loop rolls back metronome too");
    check(f.store->close(),"performance fixture closes");const auto path=f.store->path();f.store.reset();
    StoreError error{};f.store=Store::open(path,error,true);
    check(f.store && readTransportSettings(*f.store)==retained,"saved performance state survives reopen read-only");
    f.store->close();f.store.reset();f.store=Store::open(path,error);
    f.store->db().exec("UPDATE session_state SET value='broken' WHERE key='transport.loop'");
    check(!readTransportSettings(*f.store).hasLoop,"malformed session metadata falls back without trusting a bad loop");
}
std::vector<float> render(Session& session,int frames) {
    std::vector<float> result;result.reserve(static_cast<std::size_t>(frames));
    std::array<float,128> l{},r{};float* outputs[]{l.data(),r.data()};AudioIo io;io.out=outputs;io.numOut=2;
    while(frames>0){io.frames=std::min(frames,128);session.process(io);result.insert(result.end(),l.begin(),l.begin()+io.frames);frames-=io.frames;}
    return result;
}
bool audible(const std::vector<float>& audio,std::size_t at,std::size_t count) {
    return std::any_of(audio.begin()+static_cast<std::ptrdiff_t>(at),audio.begin()+static_cast<std::ptrdiff_t>(at+count),[](float v){return std::abs(v)>0.01f;});
}
void playback() {
    Fixture f;check(f.op("transport.setMetronome",{{"on",true}}).ok && f.op("transport.setLoop",{{"on",true},{"start",0},{"end",textproj::kPPQ/2}}).ok,"playback controls commit");
    SessionSpec spec;spec.sampleRate=48000;spec.maxFrames=128;Session session;
    check(session.load(*f.store,{},spec),"session loads saved performance controls");session.transport().play();
    const auto clicks=render(session,30000);
    check(session.transport().position()==6000,"saved quarter-second loop wraps within variable-sized callbacks");
    check(audible(clicks,0,80) && audible(clicks,12000,80) && audible(clicks,24000,80),"metronome clicks on a beat and on each loop's beat restart");
    check(!audible(clicks,3600,80) && !audible(clicks,15600,80),"click decays to silence between beats");
    const auto revision=session.transport().revision();
    check(session.refresh(*f.store),"ordinary rebuild while loop is playing");render(session,1);
    check(session.transport().revision()==revision && session.transport().position()==6001,"same settings do not reset or seek a running loop");
    check(f.op("transport.setLoop",{{"on",false}}).ok && session.refresh(*f.store),"live loop-off publishes");render(session,18000);
    check(session.transport().position()==24001,"loop off advances through the old end");
    check(f.op("transport.setMetronome",{{"on",false}}).ok && session.refresh(*f.store),"live metronome-off publishes");const auto quiet=render(session,1000);
    check(std::all_of(quiet.begin(),quiet.end(),[](float v){return v==0;}),"metronome off leaves silent output untouched");
    check(f.op("transport.setMetronome",{{"on",true}}).ok && session.refresh(*f.store),"turn click back on");session.transport().play(false);const auto stopped=render(session,5000);
    check(std::all_of(stopped.begin(),stopped.end(),[](float v){return v==0;}),"stopped transport does not click");
    check(f.op("transport.setLoop",{{"on",true},{"start",0},{"end",textproj::kPPQ}}).ok,"one beat loop window");
    f.store->db().exec("UPDATE tempo_map SET bpm=60");check(session.refresh(*f.store),"tempo edit rebuilds loop conversion");session.transport().locate(0);session.transport().play();render(session,60000);
    check(session.transport().position()==12000,"loop uses the tempo map instead of fixed 120 BPM");
    Session offline;spec.performanceState=false;check(offline.load(*f.store,{},spec),"offline session can opt out of saved performance controls");offline.transport().play();const auto exported=render(offline,60000);
    check(offline.transport().position()==60000 && std::all_of(exported.begin(),exported.end(),[](float v){return v==0;}),"offline output neither loops nor includes monitor clicks");
}
class LateSilence final:public Node {
public:
    void process(const NodeIo& io) noexcept override{for(int c=0;c<io.channels;++c)for(int i=0;i<io.frames;++i)io.out[c][io.blockOffset+i]=0;}
    std::int32_t latencySamples() const noexcept override{return 100;}
};
void compensation() {
    Fixture f;f.op("transport.setMetronome",{{"on",true}});
    f.op("transport.setLoop",{{"on",true},{"start",0},{"end",textproj::kPPQ/2}});
    LateSilence late;Session session;session.setSourcesFor([&](std::int64_t id){return id==9?std::vector<Node*>{&late}:std::vector<Node*>{};});
    SessionSpec spec;spec.maxFrames=128;
    check(session.load(*f.store,{},spec),"compensated click fixture loads");session.transport().play();
    const auto clicks=render(session,13000);
    check(session.graph().renderedLatency()==100,"rendered graph reports the latency actually in play");
    check(std::all_of(clicks.begin(),clicks.begin()+100,[](float v){return v==0;}) && audible(clicks,100,80),"click waits for compensated Main audio");
    check(!audible(clicks,12000,80) && audible(clicks,12100,80),"click compensation stays aligned across a loop wrap");
    Transport transport;transport.loop(0,2,true);transport.play();transport.advance(50);
    check(transport.delayedSample(0,100)==-1,"delay longer than a loop does not invent pre-start history");
    transport.advance(50);check(transport.delayedSample(0,100)==0,"delayed clock resolves after multiple wraps");
    f.op("transport.setLoop",{{"on",true},{"start",0},{"end",1}});
    check(session.refresh(*f.store),"sub-sample tick window publishes");session.transport().locate(0);render(session,129);
    check(session.transport().position()==0,"valid sub-sample loop rounds to one frame rather than switching off");
}

void signatureAndWidth() {
    Fixture f;f.op("transport.setMetronome",{{"on",true}});
    f.store->db().exec("INSERT INTO time_signature_map(id,pos_ticks,numerator,denominator) VALUES(1,0,3,8)");
    Session session;SessionSpec spec;spec.maxFrames=128;spec.precision=Precision::Float64;
    check(session.load(*f.store,{},spec),"3/8 double session loads");session.transport().play();
    std::array<double,128> l{},r{};std::array<float,128> lf{},rf{};
    double* wide[]{l.data(),r.data()};float* narrow[]{lf.data(),rf.data()};
    AudioIo io;io.out=narrow;io.out64=wide;io.numOut=2;io.frames=128;session.process(io);
    check(l==r && std::any_of(l.begin(),l.end(),[](double v){return std::abs(v)>0.01;}),"native double output receives the click on both channels");
    check(std::equal(l.begin(),l.end(),lf.begin(),[](double a,float b){return static_cast<float>(a)==b;}),"dual output pointers receive the click once at each width");
    session.transport().locate(0);const auto clicks=render(session,40000);
    check(audible(clicks,12000,80) && audible(clicks,24000,80) && audible(clicks,36000,80),"signature denominator sets the eighth-note click grid");
    check(std::abs(clicks[1])>1.8f*std::abs(clicks[12001]) && std::abs(clicks[1]-clicks[36001])<1e-6f,"signature numerator accents the first of three beats");
}

}
int main(){try{storage();playback();compensation();signatureAndWidth();}catch(const std::exception& e){check(false,e.what());}std::printf("%s -- %d checks, %d failure(s)\n",failures?"FAIL":"PASS",checks,failures);return failures?1:0;}
