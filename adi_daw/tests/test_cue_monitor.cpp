// SPDX-License-Identifier: AGPL-3.0-or-later
#include "adi/cue_settings.hpp"
#include "adi/engine/session.hpp"
#include "adi/history.hpp"
#include "adi/store.hpp"
#include "temp_directory.hpp"
#include <SQLiteCpp/SQLiteCpp.h>
#include <array>
#include <algorithm>
#include <cstdio>
#include <atomic>
#include <cstdlib>
#include <new>
#include <bit>
namespace {std::atomic<long> allocations{0};std::atomic<bool> countAllocations{false};}
void* operator new(std::size_t n){if(countAllocations.load(std::memory_order_relaxed))allocations.fetch_add(1,std::memory_order_relaxed);void* p=std::malloc(n==0?1:n);if(!p)throw std::bad_alloc();return p;}
void* operator new[](std::size_t n){return operator new(n);}
void operator delete(void* p) noexcept {std::free(p);}
void operator delete[](void* p) noexcept {std::free(p);}
void operator delete(void* p,std::size_t) noexcept {std::free(p);}
void operator delete[](void* p,std::size_t) noexcept {std::free(p);}
namespace {
using namespace adi;using namespace adi::engine;
int checks=0,failures=0;
void check(bool good,const char* name){++checks;if(!good){++failures;std::printf("FAIL: %s\n",name);}}
bool near(double a,double b,double tolerance=1e-6){return std::abs(a-b)<tolerance;}
struct Tone final:Node {
    double value;std::int64_t frames=0;
    explicit Tone(double v):value(v){}
    bool processesDouble() const noexcept override{return true;}
    void process(const NodeIo& io) noexcept override {
        for(int c=0;c<io.channels;++c)for(int i=0;i<io.frames;++i)if(io.wide())io.out64[c][io.blockOffset+i]=value;else io.out[c][io.blockOffset+i]=static_cast<float>(value);
        frames+=io.frames;
    }
};
struct Effect final:device::DeviceInstance {
    device::DeviceIdentity identity_;double gain;int delay;std::int64_t& frames;
    std::array<std::array<double,512>,2> history{};int head=0;
    Effect(double g,int d,std::int64_t& f):gain(g),delay(d),frames(f){}
    const device::DeviceIdentity& identity() const noexcept override{return identity_;}
    bool loaded() const noexcept override{return true;}
    bool processesDouble() const noexcept override{return true;}
    std::int32_t latencySamples() const noexcept override{return delay;}
    void process(const NodeIo& io) noexcept override {
        for(int i=0;i<io.frames;++i){for(int c=0;c<io.channels;++c){
            const int at=io.blockOffset+i;const double x=io.wide()?(io.in64?io.in64[c][at]:0):(io.in?static_cast<double>(io.in[c][at]):0);
            auto& ring=history[static_cast<std::size_t>(c)];
            const double y=delay?ring[static_cast<std::size_t>((head+512-delay)%512)]:x;
            ring[static_cast<std::size_t>(head)]=x;
            if(io.wide())io.out64[c][at]=y*gain;else io.out[c][at]=static_cast<float>(y*gain);
        }head=(head+1)%512;}frames+=io.frames;
    }
};
struct Block {
    std::array<std::array<float,512>,4> narrow{};std::array<std::array<double,512>,4> wide{};
    float* outputs[4];double* doubles[4];AudioIo io;
    explicit Block(int frames=512,int channels=4){for(int c=0;c<4;++c){outputs[c]=narrow[static_cast<std::size_t>(c)].data();doubles[c]=wide[static_cast<std::size_t>(c)].data();}io.out=outputs;io.out64=doubles;io.numOut=channels;io.frames=frames;}
};
struct Fixture {
    test::TempDirectory temp{"cue_monitor","project"};std::unique_ptr<Store> store;
    Tone main{0.25},reference{0.5};std::int64_t refFrames=0,masterFrames=0;
    Session session;
    Fixture(){StoreError error{};store=Store::create(temp.path()/"song.adi",error);if(!store)throw std::runtime_error("fixture store");
        store->db().exec("INSERT INTO tracks(id,kind,name) VALUES(1,'audio','Project'),(2,'audio','Reference'),(9,'master','Main');"
            "INSERT INTO mixer_strip(track_id,volume_db) VALUES(1,0),(2,0),(9,-20);"
            "INSERT INTO tempo_map(id,pos_ticks,bpm,curve) VALUES(1,0,120,0)");}
    bool op(const char* name,Payload payload,bool refresh=true){OpRequest r;r.opType=name;r.payload=std::move(payload);const auto result=OpJournal(*store).commit(r);if(!result.ok){std::printf("op diagnostic %s: %s\n",name,result.error.c_str());return false;}return !refresh || !session.loaded() || session.refresh(*store);}
    void effects(int delay=0){store->db().exec("INSERT INTO plugin_refs(id,format,uid,name) VALUES(1,'vst3','cue.test','Test');"
        "INSERT INTO device_chains(id,track_id) VALUES(2,2),(9,9);"
        "INSERT INTO devices(id,chain_id,ord,plugin_ref_id,name) VALUES(2,2,0,1,'Reference FX'),(9,9,0,1,'Main FX')");masterDelay=delay;}
    int masterDelay=0;
    bool load(Precision precision=Precision::Float32,bool performance=true){session.setSourcesFor([this](std::int64_t id){return id==1?std::vector<Node*>{&main}:id==2?std::vector<Node*>{&reference}:std::vector<Node*>{};});
        SessionSpec spec;spec.maxFrames=512;spec.precision=precision;spec.performanceState=performance;
        return session.load(*store,[this](const DeviceRequest& request,std::string&){return std::make_unique<Effect>(request.device->id==2?2.0:0.1,request.device->id==9?masterDelay:0,request.device->id==2?refFrames:masterFrames);},spec);}
    void process(Block& b){session.process(b.io);}
};
void settings() {
    Fixture f;const auto initial=SnapshotBuilder::fromStore(*f.store);
    check(!initial->cue.cueMode && initial->cue.soloInPlace && initial->cue.volumeDb==0,"Cue defaults match saved Solo/SIP and unity cue volume");
    check(f.op("cue.setMode",{{"on",true}}) && f.op("cue.setOutput",{{"first",2}}) && f.op("cue.setVolume",{{"db",-6.0}}),"Cue mode, output and volume commit");
    check(f.op("mixer.setSoloInPlace",{{"on",false}}) && f.op("cue.setReference",{{"on",false},{"id",2}}),"SIP and reference designation commit");
    const auto next=SnapshotBuilder::fromStore(*f.store,*initial);
    check(next->cue.cueMode && !next->cue.soloInPlace && next->cue.firstOutputChannel==2 && next->cue.volumeDb==-6 && next->cue.referenceTrack==2 && !next->cue.referenceOn,"snapshot exposes every cue field");
    check(next->tracks==initial->tracks,"cue settings preserve unchanged track sharing");
    check(!History(*f.store).canUndo(),"cue session commands never become undo steps");
    const auto saved=readCueSettings(*f.store);OpRequest bad;bad.opType="cue.setOutput";bad.payload={{"first",3}};
    check(!OpJournal(*f.store).commit(bad).ok && readCueSettings(*f.store)==saved,"unrepresentable stereo pair is refused atomically");
    bad.payload={{"first",std::int64_t{4294967296LL}}};check(!OpJournal(*f.store).commit(bad).ok,"large output index cannot truncate to a valid pair");
    bad.opType="cue.setVolume";bad.payload={{"db",99.0}};check(!OpJournal(*f.store).commit(bad).ok,"cue volume outside the fader range is refused");
    bad.opType="cue.setReference";bad.payload={{"on",true},{"id",9}};check(!OpJournal(*f.store).commit(bad).ok,"Main cannot be its own exclusive reference");
    bad.payload={{"on",true},{"id",123}};check(!OpJournal(*f.store).commit(bad).ok,"missing reference target is refused");
    check(f.store->close(),"Cue fixture closes");const auto path=f.store->path();f.store.reset();StoreError error{};f.store=Store::open(path,error,true);
    check(f.store && readCueSettings(*f.store)==saved,"Cue choices survive read-only reopen");
}
void referenceSwap(Precision precision) {
    Fixture f;f.effects();f.store->db().exec("UPDATE tracks SET muted=1 WHERE id=2");f.op("cue.setOutput",{{"first",2}});f.op("cue.setReference",{{"on",false},{"id",2}});
    check(f.load(precision),"reference fixture loads");Block b;f.process(b);
    check(near(b.wide[0][511],0.0025) && b.wide[2][511]==0,"designated reference stays outside Main while off");
    const auto before=f.refFrames;
    check(f.op("cue.setReference",{{"on",true}}),"reference on publishes");f.process(b);
    check(f.refFrames==before+512 && f.masterFrames==f.refFrames,"device DSP runs once per callback with monitoring enabled");
    const double project=0.0025,ref=1;
    check(near(b.wide[0][0],project*(239.0/240)+ref/240),"reference starts with the five-ms complementary swap");
    check(near(b.wide[0][239],ref) && near(b.wide[0][511],ref),"reference fully bypasses Main's FX and fader after five ms");
    check(near(b.wide[2][239],ref) && b.wide[2]==b.wide[3],"exclusive reference reaches the selected Cue Out pair too");
    check(std::equal(b.wide[0].begin(),b.wide[0].end(),b.narrow[0].begin(),[](double x,float y){return near(x,y,2e-7);}),"both output widths carry the same reference swap");
    check(f.op("cue.setVolume",{{"db",-6.020599913279624}}),"Cue Volume updates");f.process(b);
    check(near(b.wide[0][511],0.5) && near(b.wide[2][511],0.5),"Cue Volume scales reference on monitor and Cue Out");
    check(f.op("mixer.setVolume",{{"id",9},{"db",-60.0}}),"Main fader changes behind reference");f.process(b);
    check(near(b.wide[0][511],0.5),"Main fader never changes active reference level");
    check(f.op("cue.setReference",{{"on",false}}),"reference off publishes");f.process(b);
    check(b.wide[0][0]>0.49 && near(b.wide[0][239],0.000025),"five-ms release keeps reference audio until Main returns");
    check(b.wide[2][511]==0 && near(b.wide[0][511],0.000025),"reference off restores the project and clears headphones");
    check(f.session.refresh(*f.store),"unchanged cue rebuild succeeds");f.process(b);
    check(near(b.wide[0][511],0.000025),"unchanged rebuild does not retrigger a reference swap");
    check(f.op("cue.setOutput",{{"first",0}}) && f.op("cue.setReference",{{"on",true}}),"reference can share the monitor stereo pair");f.process(b);
    check(near(b.wide[0][511],0.5) && b.wide[2][511]==0,"shared monitor/Cue Out receives reference once at the right level");
}
void soloAndCue() {
    Fixture f;f.store->db().exec("INSERT INTO tracks(id,kind,name) VALUES(3,'return','Return');INSERT INTO mixer_strip(track_id) VALUES(3);"
        "INSERT INTO routing(id,src_kind,src_id,dst_kind,dst_id,kind,gain_db) VALUES(10,'track',1,'track',3,'send',0)");
    f.op("track.setSolo",{{"id",1},{"soloed",true}});check(f.load(),"solo/send fixture loads");Block b;f.process(b);
    check(near(b.wide[0][511],0.05),"Solo in Place keeps the soloed source's return audible");
    check(f.op("mixer.setSoloInPlace",{{"on",false}}),"Solo in Place off publishes");f.process(b);
    check(near(b.wide[0][511],0.025),"Solo in Place off closes that return");
    check(f.op("cue.setOutput",{{"first",2}}) && f.op("cue.setMode",{{"on",true}}) && f.op("cue.setTrack",{{"id",1},{"on",true}}),"switch from Solo to Cue publishes");f.process(b);
    check(near(b.wide[0][511],0.1) && near(b.wide[2][511],0.25),"Cue selection leaves Main's mix open and hears PFL on headphones");
    check(f.op("track.setMute",{{"id",1},{"muted",true}}) && f.op("mixer.setVolume",{{"id",1},{"db",-60.0}}),"mute/fader change on a cued track");f.process(b);
    check(near(b.wide[0][511],0.05) && near(b.wide[2][511],0.25),"PFL cue remains audible despite track mute and fader");
    check(f.op("cue.setTrack",{{"id",1},{"on",false}}),"Cue button releases");f.process(b);
    check(b.wide[2][0]>0.24 && b.wide[2][239]==0 && b.wide[2][511]==0,"released cue ramps over five ms and clears the destination");
}
void compensatedAndClick() {
    Fixture f;f.effects(100);f.op("cue.setOutput",{{"first",2}});f.op("cue.setMode",{{"on",true}});f.op("cue.setTrack",{{"id",1},{"on",true}});
    check(f.load(),"compensated cue fixture loads");Block b;f.process(b);
    check(std::all_of(b.wide[2].begin(),b.wide[2].begin()+100,[](double v){return v==0;}) && b.wide[2][100]>0 && near(b.wide[2][511],0.25),"Cue PFL is aligned to Main's compensated output clock");
    check(f.session.refresh(*f.store),"cue graph rebuild during playback");f.process(b);
    check(std::all_of(b.wide[2].begin(),b.wide[2].end(),[](double v){return near(v,0.25);}),"cue delay history survives the graph swap");
    Fixture click;click.main.value=click.reference.value=0;click.op("cue.setOutput",{{"first",2}});click.op("transport.setMetronome",{{"on",true}});
    check(click.load(),"Cue metronome fixture loads");click.session.transport().play();click.process(b);
    check(std::all_of(b.wide[0].begin(),b.wide[0].end(),[](double v){return v==0;}) && std::any_of(b.wide[2].begin(),b.wide[2].end(),[](double v){return std::abs(v)>0.01;}),"metronome follows Cue Out and leaves Main untouched");
}
void offline() {
    Fixture f;f.effects();f.store->db().exec("UPDATE tracks SET muted=1 WHERE id=2");f.op("cue.setOutput",{{"first",2}});f.op("cue.setMode",{{"on",true}});f.op("cue.setTrack",{{"id",1},{"on",true}});
    f.op("cue.setReference",{{"on",true},{"id",2}});
    check(f.load(Precision::Float64,false),"offline session loads saved Cue/reference choices");Block b;f.process(b);
    check(near(b.wide[0][511],0.0025) && b.wide[2][511]==0,"offline rendering excludes reference and ignores Cue audition/mode solo gates");
}
void selectionAndRole() {
    Fixture f;f.op("cue.setReference",{{"on",false},{"id",2}});check(f.load(),"generic audition fixture loads");Block b;f.process(b);
    check(near(b.wide[0][511],0.075),"performance audition designation alone does not alter the project mix");
    check(f.op("cue.setOutput",{{"first",2}}) && f.op("cue.setMode",{{"on",true}}) && f.op("cue.setTrack",{{"id",1},{"on",true}}),"independent Cue selection commits");
    check(!f.session.model().tracks.front().soloed && readCueSettings(*f.store).selectedTracks==std::vector<std::int64_t>{1},"Cue selection never overwrites a stored solo flag");
    const auto snapshot=SnapshotBuilder::fromStore(*f.store);check(snapshot->cue.selectedTracks==std::vector<std::int64_t>{1},"snapshot carries selected Cue track ids");
    // Read compatibility only: win's N38 owns the real DDL/version/migration.
    f.store->db().exec("ALTER TABLE tracks ADD COLUMN role TEXT;UPDATE tracks SET role='reference' WHERE id=2");
    check(f.session.refresh(*f.store),"optional native role can be read when the format supplies it");f.process(b);
    check(readCueSettings(*f.store).projectReferenceTrack==2 && near(b.wide[0][511],0.025),"native reference role excludes its track from Main");
    check(f.op("cue.setReference",{{"on",true}}),"native reference audition engages");f.process(b);
    check(near(b.wide[0][511],0.5),"native reference auditions past Main");
}
void defaultRole() {
    Fixture f;f.store->db().exec("ALTER TABLE tracks ADD COLUMN role TEXT;UPDATE tracks SET role='reference' WHERE id=2");
    check(readCueSettings(*f.store).referenceTrack==2 && readCueSettings(*f.store).referenceFromRole,"native role supplies the default audition target");
    check(f.op("cue.setVolume",{{"db",-3.0}}),"unrelated Cue control saves with a native role present");
    f.store->db().exec("UPDATE tracks SET role=NULL;UPDATE tracks SET role='reference' WHERE id=1");
    check(readCueSettings(*f.store).referenceTrack==1,"Cue preference save does not pin an old native reference role");
    check(f.op("cue.setReference",{{"on",true}}),"reference enable can use the new native role without an explicit id");
}
void automationLifetime() {
    rows::Model model;rows::Track ref;ref.id=2;ref.kind="audio";model.tracks.push_back(ref);
    rows::Track main;main.id=9;main.kind="master";model.tracks.push_back(main);
    CueSettings settings;settings.referenceTrack=2;Transport transport;CueMonitor monitor;
    auto owner=std::make_shared<StripAutomation>();owner->byTrack[2]={};
    std::weak_ptr<StripAutomation> weak=owner;
    monitor.configure(model,settings,&transport,owner,true);
    GraphHost graph;RealizeOptions options;options.monitors=monitor.points({});
    check(graph.rebuild(model,options,48000,64),"published reference automation fixture builds");
    Block block(64);graph.process(block.io);
    monitor.configure(model,settings,&transport,{},true);owner.reset();monitor.collect();
    check(!weak.expired(),"retired reference lanes stay alive until the audio reader moves");
    graph.process(block.io);monitor.collect();
    check(weak.expired(),"reference lane owners are reclaimed off audio after the new parameters run");
}
void automationPlays() {
    Fixture f;f.effects();f.store->db().exec("UPDATE tracks SET muted=1 WHERE id=2;"
        "INSERT INTO automation_lanes(id,owner_kind,owner_id,param_ref,time_base,value_domain) VALUES(100,'track',2,'volume',1,'real'),(101,'track',2,'mute',1,'real')");
    auto lane=[&](std::int64_t id,double value){AutomationPoint point{};point.point_id=1;point.value=value;
        const std::array<AutomationPoint,1> points{point};
        const auto bytes=writeStream<AutomationPoint>(FourCC::Automation,points,1,StreamFlags::SortedByTime|StreamFlags::TimeIsNanos);
        if(!f.store->putAutomationData(id,std::nullopt,bytes))throw std::runtime_error("cue automation fixture");};
    lane(100,-6.020599913279624);lane(101,1);
    f.op("cue.setReference",{{"on",true},{"id",2}});check(f.load(Precision::Float64),"reference volume/mute automation fixture loads");Block b;f.process(b);
    check(near(b.wide[0][511],0.5),"reference follows volume automation while PFL ignores Main mute automation");
    lane(100,-12.041199826559248);check(f.session.refresh(*f.store),"changed reference lane publishes");f.process(b);f.session.tick(1000);
    check(near(b.wide[0][511],0.25),"newly published reference automation plays after the graph swap");
}
void inactiveBits() {
    CueMonitor monitor;CueSettings settings;monitor.publish(settings,48000,64,2,true);GraphHost graph;Block b(64);
    for(auto& channel:b.narrow)for(auto& sample:channel)sample=std::bit_cast<float>(std::uint32_t{0x80000000u});
    for(auto& channel:b.wide)for(auto& sample:channel)sample=std::bit_cast<double>(std::uint64_t{0x8000000000000000ull});
    monitor.process(graph,b.io,{});
    check(std::bit_cast<std::uint32_t>(b.narrow[0][0])==0x80000000u && std::bit_cast<std::uint64_t>(b.wide[0][0])==0x8000000000000000ull,
          "inactive monitoring preserves negative-zero output bits");
}
void coldReference() {
    Fixture f;f.effects(100);check(f.load(),"cold reference with delayed Main loads");Block b;f.process(b);
    check(f.op("cue.setReference",{{"on",true},{"id",2}}),"select and engage a new reference while the project plays");f.process(b);
    check(std::all_of(b.wide[0].begin(),b.wide[0].begin()+100,[](double v){return near(v,0.0125);}),"cold cue delay fills before Main starts fading");
    check(near(b.wide[0][100],0.0125*(239.0/240)+1.0/240) && near(b.wide[0][339],1),"five-ms swap starts exactly when the new reference's compensated audio is ready");
}
void allocation() {
    Fixture f;f.effects(100);f.op("cue.setMode",{{"on",true}});f.op("cue.setTrack",{{"id",1},{"on",true}});
    f.op("cue.setOutput",{{"first",2}});f.op("cue.setReference",{{"on",true},{"id",2}});f.op("transport.setMetronome",{{"on",true}});
    check(f.load(Precision::Float64),"allocation fixture loads off-thread");f.session.transport().play();Block b(128);
    allocations.store(0);countAllocations.store(true);for(int i=0;i<50;++i)f.process(b);countAllocations.store(false);
    check(allocations.load()==0,"Cue graph copy, reference ramp, volume and metronome allocate nothing");
}

}
int main(){try{settings();referenceSwap(Precision::Float32);referenceSwap(Precision::Float64);soloAndCue();compensatedAndClick();offline();selectionAndRole();defaultRole();automationLifetime();automationPlays();inactiveBits();coldReference();allocation();}
    catch(const std::exception& e){check(false,e.what());}std::printf("%s -- %d checks, %d failure(s)\n",failures?"FAIL":"PASS",checks,failures);return failures?1:0;}
