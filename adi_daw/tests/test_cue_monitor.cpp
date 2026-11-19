// SPDX-License-Identifier: AGPL-3.0-or-later
#include "adi/cue_settings.hpp"
#include "adi/dsp/stereo_envelope_meter.hpp"
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
    double value; double rightFactor=1.; std::int64_t frames=0;
    explicit Tone(double v):value(v){}
    bool processesDouble() const noexcept override{return true;}
    void process(const NodeIo& io) noexcept override {
        for(int c=0;c<io.channels;++c)for(int i=0;i<io.frames;++i)if(io.wide())io.out64[c][io.blockOffset+i]=value*(c?rightFactor:1.);else io.out[c][io.blockOffset+i]=static_cast<float>(value*(c?rightFactor:1.));
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
    bool load(Precision precision=Precision::Float32,bool performance=true, bool exporting=false){session.setSourcesFor([this](std::int64_t id){return id==1?std::vector<Node*>{&main}:id==2?std::vector<Node*>{&reference}:std::vector<Node*>{};});
        SessionSpec spec;spec.maxFrames=512;spec.precision=precision;spec.performanceState=performance;spec.exporting=exporting;
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
    f.session.setMainAudition(MainAudition::Side);
    f.process(b);
    check(b.wide[0][511] == 0. && b.wide[1][511] == 0. &&
              near(b.wide[2][511], .25) && near(b.wide[3][511], .25),
          "Main Side affects its speaker pair while an audible separate stereo Cue pair is unchanged");
    f.session.setMainAudition(MainAudition::FilterFlat);
    f.process(b);
    check(near(b.wide[0][511], .045) && near(b.wide[1][511], .045) &&
              near(b.wide[2][511], .25) && near(b.wide[3][511], .25),
          "Main Filter Listen leaves the separately routed Cue pair unchanged");
    f.session.setMainAudition(MainAudition::Stereo);
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
    check(f.load(Precision::Float64,false),"offline session loads saved Cue/reference choices");
    f.session.setMainAudition(MainAudition::Side);
    Block b;f.process(b);
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
void mainListening(Precision precision, bool exporting) {
    Fixture f;
    f.store->db().exec("UPDATE mixer_strip SET volume_db=0 WHERE track_id=9");
    f.main.rightFactor = -0.5;
    f.reference.value = 0.;
    check(f.load(precision, true, exporting), "Main listening fixture loads");
    const auto tap = f.session.openScope(9, .25, ScopePoint::PostFader);
    check(tap != nullptr, "Main post-fader tap observes the stored mix before the monitor pair");
    f.session.transport().play();
    Block block(128);
    f.process(block);
    const auto sample = [&](int channel, int i) {
        const auto ch = static_cast<std::size_t>(channel), at = static_cast<std::size_t>(i);
        return precision == Precision::Float32 ? static_cast<double>(block.narrow[ch][at]) : block.wide[ch][at];
    };
    const double l = sample(0,127), r = sample(1,127);
    check(l > 0. && r < 0., "left and right contain different signed programme");
    for (const auto mode : {MainAudition::Left, MainAudition::Right, MainAudition::Mid, MainAudition::Side, MainAudition::Stereo}) {
        f.session.setMainAudition(mode);
        allocations.store(0); countAllocations.store(true);
        f.process(block);
        countAllocations.store(false);
        const double solo = mode == MainAudition::Left ? l : mode == MainAudition::Right ? r :
                            mode == MainAudition::Mid ? 0.5*(l+r) : 0.5*(l-r);
        const bool dry = exporting || mode == MainAudition::Stereo;
        bool match = true;
        for (int i=0;i<128;++i)
            match = match && sample(0,i) == (dry?l:solo) && sample(1,i) == (dry?r:solo)
                          && sample(2,i)==0. && sample(3,i)==0.;
        check(match && !allocations.load(), "Main audition matrix is exact, physical pair only, export independent and allocation free");
        std::array<float, 128> storedLeft{}, storedRight{};
        std::int64_t stamp = 0;
        check(tap && tap->read(storedLeft, storedRight, stamp) &&
                  std::all_of(storedLeft.begin(), storedLeft.end(), [&](float x) { return x == static_cast<float>(l); }) &&
                  std::all_of(storedRight.begin(), storedRight.end(), [&](float x) { return x == static_cast<float>(r); }),
              "Main post-fader capture is identical in every monitor mode");
    }
    using Filter = dsp::StereoMeterWeighting;
    using Weighting = Filter::Weighting;
    constexpr std::array modes{MainAudition::FilterFlat, MainAudition::FilterA,
                               MainAudition::FilterC, MainAudition::FilterK};
    constexpr std::array weights{Weighting::Standard, Weighting::A, Weighting::C, Weighting::K};
    for (std::size_t m = 0; m < modes.size(); ++m) {
        f.session.setMainAudition(modes[m]);
        allocations.store(0); countAllocations.store(true);
        f.process(block);
        countAllocations.store(false);
        Filter filter;
        std::array<Filter::History, 2> history{};
        check(filter.prepare(48000., weights[m]), "Main listen reference coefficients prepare at the session rate");
        const auto matches = [&](double actual, double original, Filter::History &state) {
            const auto expected = exporting ? original : .45 * filter.process(original, state);
            return exporting ? actual == original : near(actual, expected, 5e-8);
        };
        bool match = true;
        for (int i = 0; i < 128; ++i)
            match = matches(sample(0,i), l, history[0]) && matches(sample(1,i), r, history[1]) &&
                    sample(2,i) == 0. && sample(3,i) == 0. && match;
        check(match && !allocations.load(), "Main Filter Listen applies its selected profile allocation-free to physical stereo only");
        std::array<float, 128> storedLeft{}, storedRight{};
        std::int64_t stamp = 0;
        check(tap->read(storedLeft, storedRight, stamp) &&
                  std::all_of(storedLeft.begin(), storedLeft.end(), [&](float x) { return x == static_cast<float>(l); }) &&
                  std::all_of(storedRight.begin(), storedRight.end(), [&](float x) { return x == static_cast<float>(r); }),
              "Main Filter Listen has no effect on the post-fader tap");
    }
}
void filterListenLifecycle() {
    CueMonitor monitor;
    const CueSettings settings;
    GraphHost graph;
    Block block(64);
    using Filter = dsp::StereoMeterWeighting;
    Filter reference;
    std::array<Filter::History, 4> referenceHistory{};
    const auto prepare = [&](double rate) {
        check(monitor.publish(settings, rate, 64, 2, true) &&
                  reference.prepare(rate, Filter::Weighting::C),
              "monitor filters prepare before audio");
        referenceHistory = {};
    };
    const auto run = [&](bool enabled, bool exporting = false) {
        constexpr std::array narrow{.91f, -.328f, .125f, -.18f};
        constexpr std::array wide{.1023, .7512, -.001, .375};
        for (std::size_t channel = 0; channel < narrow.size(); ++channel) {
            block.narrow[channel].fill(narrow[channel]);
            block.wide[channel].fill(wide[channel]);
        }
        block.io.exporting = exporting;
        monitor.process(graph, block.io, {});
        bool match = true;
        for (std::size_t channel = 0; channel < narrow.size(); ++channel)
            for (std::size_t i = 0; i < 64; ++i) {
                if (enabled && channel < 2) {
                    match = near(block.narrow[channel][i], .45 * reference.process(narrow[channel], referenceHistory[channel]), 5e-8) &&
                            near(block.wide[channel][i], .45 * reference.process(wide[channel], referenceHistory[channel + 2]), 1e-10) && match;
                } else {
                    match = block.narrow[channel][i] == narrow[channel] && block.wide[channel][i] == wide[channel] && match;
                }
            }
        return match;
    };
    prepare(48000.);
    monitor.setMainAudition(MainAudition::FilterC);
    check(run(true) && run(true), "four independent histories continue across callbacks");
    check(run(false, true) && run(true), "export neither hears nor advances the active monitor filter");
    check(monitor.publish(settings, 48000., 64, 2, true) && run(true),
          "same-format publication preserves the active monitor stream");
    monitor.setMainAudition(MainAudition::Stereo);
    check(run(false), "leaving Filter Listen returns bit-exact direct audio");
    monitor.setMainAudition(MainAudition::FilterC);
    referenceHistory = {};
    check(run(true), "reentering Filter Listen starts with clean state");
    prepare(44100.);
    check(run(true), "rate-only change uses the new coefficients and clean histories");
    monitor.release();
    prepare(44100.);
    check(run(true), "same-format device restart resets monitor histories");
    prepare(96000.);
    check(run(true), "96 kHz monitor follows its prepared weighting");
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
int main(){try{settings();referenceSwap(Precision::Float32);referenceSwap(Precision::Float64);soloAndCue();compensatedAndClick();offline();selectionAndRole();defaultRole();for(auto precision:{Precision::Float32,Precision::Float64}) { mainListening(precision,false); mainListening(precision,true); }filterListenLifecycle();automationLifetime();automationPlays();inactiveBits();coldReference();allocation();}
    catch(const std::exception& e){check(false,e.what());}std::printf("%s -- %d checks, %d failure(s)\n",failures?"FAIL":"PASS",checks,failures);return failures?1:0;}
