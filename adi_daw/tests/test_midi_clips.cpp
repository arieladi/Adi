// SPDX-License-Identifier: GPL-3.0-or-later
#include "temp_directory.hpp"
#include "adi/engine/session.hpp"
#include "adi/audio/io_audit.hpp"
#include "adi/store.hpp"
#include <SQLiteCpp/SQLiteCpp.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <new>
#include <thread>
#include <chrono>
using namespace adi;
using namespace adi::engine;
namespace { std::atomic<unsigned> allocations{0}; }
void* operator new(std::size_t n){if(audio::onAudioThread)++allocations;if(auto* p=std::malloc(n?n:1))return p;throw std::bad_alloc();}
void* operator new[](std::size_t n){return operator new(n);}
void* operator new(std::size_t n,const std::nothrow_t&) noexcept{try{return operator new(n);}catch(...){return nullptr;}}
void* operator new[](std::size_t n,const std::nothrow_t&) noexcept{try{return operator new[](n);}catch(...){return nullptr;}}
void operator delete(void* p,const std::nothrow_t&) noexcept{std::free(p);}
void operator delete[](void* p,const std::nothrow_t&) noexcept{std::free(p);}
void operator delete(void* p) noexcept{std::free(p);}
void operator delete[](void* p) noexcept{std::free(p);}
void operator delete(void* p,std::size_t) noexcept{std::free(p);}
void operator delete[](void* p,std::size_t) noexcept{std::free(p);}
namespace {
int checks=0,failures=0;
void check(bool ok,const char* label){++checks;if(!ok){++failures;std::printf("FAIL %s\n",label);}}
struct Heard { Event e;std::int64_t clock; };
struct Instrument final:device::DeviceInstance {
    device::DeviceIdentity id;
    std::array<Heard,8192> heard{};std::size_t count=0;std::int64_t clock=0;
    const device::DeviceIdentity& identity()const noexcept override{return id;}
    bool loaded()const noexcept override{return true;}
    EventFlow eventFlow()const noexcept override{return EventFlow::Consume;}
    void process(const NodeIo& io)noexcept override{
        for(const auto& e:io.events)if(count<heard.size())heard[count++]={e,clock+e.frame};
        for(int c=0;c<io.channels;++c)std::fill_n(io.out[c]+io.blockOffset,io.frames,0.0f);
    }
    int balance()const{int n=0;for(std::size_t i=0;i<count;++i){if(heard[i].e.type==EventType::NoteOn)++n;if(heard[i].e.type==EventType::NoteOff)--n;}return n;}
};
struct LatencyEffect final:device::DeviceInstance {
    device::DeviceIdentity id;
    const device::DeviceIdentity& identity()const noexcept override{return id;}
    bool loaded()const noexcept override{return true;}
    std::int32_t latencySamples()const noexcept override{return 512;}
    void process(const NodeIo& io)noexcept override{for(int c=0;c<io.channels;++c)std::fill_n(io.out[c]+io.blockOffset,io.frames,0.0f);}
};
struct Fixture {
    test::TempDirectory dir{"midi_clips","session"};
    std::unique_ptr<Store> store;Session session;Instrument* instrument=nullptr;
    double rate;int block;std::int64_t clock=0;
    explicit Fixture(double r=48000,int b=512):rate(r),block(b){
        StoreError e{};store=Store::create(dir.path()/"session.adi",e);if(!store)throw std::runtime_error("create");
        store->db().exec("INSERT INTO tracks(id,kind,name,index_in_parent) VALUES(1,'midi','Keys',0),(9,'master','Master',1); INSERT INTO plugin_refs(id,format,uid,name,subtype) VALUES(1,'vst3','test.midi','Recorder','instrument'); INSERT INTO device_chains(id,track_id) VALUES(1,1); INSERT INTO devices(id,chain_id,ord,plugin_ref_id,name) VALUES(1,1,0,1,'Recorder')");
    }
    void clip(int id=1,std::int64_t on=24024,std::int64_t duration=480480,std::int64_t length=5765760){
        SQLite::Statement q(store->db(),"INSERT INTO clips(id,track_id,kind,time_base,pos_ticks,length_ticks) VALUES(?,1,'midi',0,0,?)");q.bind(1,id);q.bind(2,length);q.exec();
        NoteRecord note{};note.start_ticks=on;note.dur_ticks=duration;note.note_id=42;note.key=60;note.channel=3;note.vel_on=93;note.vel_off=37;note.probability=10000;note.tuning_cents=31.25f;
        auto bytes=writeStream<NoteRecord>(FourCC::Notes,std::vector<NoteRecord>{note});
        if(!store->putEventStream(id,"notes",bytes))throw std::runtime_error("ANOT write");
    }
    void load(){session.graph().setFadeFrames(0);if(!session.load(*store,[this](const DeviceRequest& request,std::string&) -> std::unique_ptr<device::DeviceInstance> {if(request.device->id==2)return std::make_unique<LatencyEffect>();auto p=std::make_unique<Instrument>();instrument=p.get();return p;},{2,rate,block}))throw std::runtime_error(session.error());session.transport().play();}
    void render(int n=0){if(!n)n=block;std::array<float,8192> out{};float* channels[]={out.data(),out.data()+4096};instrument->clock=clock;session.process({channels,nullptr,2,0,n,0});clock+=n;}
    void refresh(){if(!session.refresh(*store))throw std::runtime_error(session.error());}
};
void placement(){
    for(double rate:{44100.,48000.,88200.,96000.,176400.,192000.})for(int block:{64,128,256,512,1024,2048,4096}){
        Fixture f(rate,block);f.clip();f.load();
        const auto on=std::llround(100*rate/48000),off=std::llround(2100*rate/48000);
        while(f.clock<off+block)f.render();
        auto& i=*f.instrument;
        if(i.count!=3 || i.heard[0].clock!=on || i.heard[2].clock!=off)std::printf("rate %.0f block %d got %lld/%lld want %lld/%lld count %zu\n",rate,block,static_cast<long long>(i.heard[0].clock),static_cast<long long>(i.heard[2].clock),on,off,i.count);
        check(i.count==3 && i.heard[0].clock==on && i.heard[2].clock==off,"exact note frames across rates and block sizes");
        check(i.count>=3 && i.heard[0].e.channel==3 && i.heard[0].e.dim==60 && i.heard[0].e.value==93.0/127 && i.heard[2].e.value==37.0/127,"key channel velocity and release velocity survive ANOT");
        check(i.count>=2 && i.heard[1].e.type==EventType::NoteExpression && i.heard[1].e.value==0.3125 && i.heard[1].e.noteId==i.heard[0].e.noteId,"tuning cents reaches instrument as per-note semitones");
    }
}
void cleanup(){
    for(int transition=0;transition<6;++transition){
        Fixture f;f.clip(1,0,5765760);f.load();f.render();check(f.instrument->balance()==1,"note sounding before transition");
        const auto before=f.instrument->count;
        if(transition==0)f.session.transport().locate(1000);
        if(transition==1)f.session.transport().play(false);
        if(transition==2)f.session.transport().loop(0,700);
        if(transition==3){f.store->db().exec("UPDATE clips SET muted=1");f.refresh();}
        if(transition==4)f.session.transport().locate(23999);
        if(transition==5){f.store->db().exec("DELETE FROM clips");f.refresh();}
        f.render();
        bool off=false;for(std::size_t n=before;n<f.instrument->count;++n)off=off||f.instrument->heard[n].e.type==EventType::NoteOff;
        if(transition==2) {
            bool atWrap=false;for(std::size_t n=before;n<f.instrument->count;++n)
                atWrap=atWrap || (f.instrument->heard[n].e.type==EventType::NoteOff && f.instrument->heard[n].clock==700);
            check(atWrap,"loop wrap releases note at exact interior frame");
        }
        check(off,transition==0?"locate releases owned note":transition==2?"loop wrap releases owned note":"stop mute end or deletion releases owned note");
        if(transition==0||transition==2){f.session.transport().play(false);f.render();}
        check(f.instrument->balance()==0,"transition leaves no hanging notes");
    }
    Fixture f;f.clip(1,0,5765760);f.load();f.render();f.session.transport().loop(0,1024);f.render();
    const auto before=f.instrument->count;f.render();bool off=false;
    for(std::size_t n=before;n<f.instrument->count;++n)off=off||f.instrument->heard[n].e.type==EventType::NoteOff;
    check(off,"loop wrap exactly at callback boundary releases note");
}
void pendingRelease(){
    Fixture f(48000,1024);f.clip(1,0,240240);
    f.store->db().exec("UPDATE devices SET ord=1;INSERT INTO devices(id,chain_id,ord,plugin_ref_id,name) VALUES(2,1,0,1,'Delay')");
    f.load();f.render();check(f.instrument->balance()==1,"compensated off is pending before refresh");
    f.store->db().exec("DELETE FROM clips");f.refresh();f.render();f.render();
    check(f.instrument->balance()==0,"refresh cannot discard ownership of a deferred note-off");
    Fixture stopped(48000,1024);stopped.clip(1,0,240240);
    stopped.store->db().exec("UPDATE devices SET ord=1;INSERT INTO devices(id,chain_id,ord,plugin_ref_id,name) VALUES(2,1,0,1,'Delay')");
    stopped.load();stopped.render();stopped.session.transport().play(false);
    stopped.store->db().exec("DELETE FROM clips");stopped.refresh();stopped.render();stopped.render();
    check(stopped.instrument->balance()==0,"stopped refresh retains deferred off ownership");
}
void concurrentPublication(){
    Fixture f(48000,64);f.clip(1,0,57657600,57657600);f.load();
    std::jthread driver([&](std::stop_token stop){while(!stop.stop_requested()){f.render();std::this_thread::sleep_for(std::chrono::microseconds(100));}});
    for(int i=0;i<8;++i){f.store->db().exec(i%2?"UPDATE clips SET muted=0":"UPDATE clips SET muted=1");f.refresh();}
    driver.request_stop();driver.join();
    f.session.transport().play(false);f.render();
    check(f.instrument->balance()==0,"concurrent publication and stop leave no owned notes");
    check(f.instrument->count<f.instrument->heard.size(),"concurrent recorder capacity covers all events");
}
void capacity(){
    Fixture f;f.clip(1,0,5765760);
    std::vector<NoteRecord> notes(200);
    for(std::size_t i=0;i<notes.size();++i){auto& n=notes[i];n.dur_ticks=5765760;n.note_id=i+1;n.key=60;n.vel_on=100;n.probability=10000;}
    f.store->putEventStream(1,"notes",writeStream<NoteRecord>(FourCC::Notes,notes));f.load();f.render();
    check(f.instrument->balance()==128 && f.session.midiClips()->droppedNotes()==72,"polyphony refuses and counts excess ons");
    f.session.transport().play(false);f.render();
    check(f.instrument->balance()==0,"capacity pressure still releases every admitted note");
}
void overflowRelease(){
    Fixture f;f.clip(1,0,240240);f.load();
    auto* graph=f.session.graph().currentGraph();graph->setEventCapacity(4);graph->prepare(48000,512);
    f.render();
    const auto target=f.session.graph().current()->outputFor(1);
    Event filler;filler.type=EventType::ParamValue;
    for(int i=0;i<4;++i)graph->pushInputEvent(target,filler);
    f.render();check(f.instrument->balance()==1,"full destination refused the scheduled off");
    f.render();check(f.instrument->balance()==0,"a dropped forwarded off is retried without hanging");
}
void nanosecondEnd(){
    Fixture f;f.clip(1,0,5765760);
    f.store->db().exec("UPDATE clips SET time_base=1,pos_ticks=NULL,length_ticks=NULL,pos_ns=0,length_ns=2093751");
    f.load();f.render();
    check(f.instrument->count==3 && f.instrument->heard[2].clock==101,"nanosecond clip end rounds the absolute endpoint once");
}
void maximumLocate(){
    Fixture f;f.clip(1,0,5765760);f.load();f.render();
    f.session.transport().locate(std::numeric_limits<std::int64_t>::max());f.render();
    check(f.instrument->balance()==0,"maximum sample locate releases notes without overflow");
}
void musical(){
    Fixture f;f.clip(1,0,240240,720720);f.clip(2,0,240240,720720);
    f.store->db().exec("UPDATE clips SET loop_enabled=1,loop_start_ticks=0,loop_len_ticks=240240,content_offset_ticks=120120");f.load();
    while(f.clock<3200)f.render();
    check(f.instrument->balance()==0,"overlapping looping clips both finish");
    int ons=0,offs=0;bool origin=false;
    for(std::size_t n=0;n<f.instrument->count;++n){const auto& e=f.instrument->heard[n];if(e.e.type==EventType::NoteOn)++ons;if(e.e.type==EventType::NoteOff)++offs;if(e.e.type==EventType::NoteOff && e.clock==500)origin=true;}
    check(ons==8 && offs==8 && origin,"clip loop and content offset repeat independent overlapping notes");
    Fixture tempo;tempo.clip(1,2882880,5765760,11531520);
    tempo.store->db().exec("DELETE FROM tempo_map;INSERT INTO tempo_map(pos_ticks,bpm,curve) VALUES(0,120,0),(5765760,60,0)");tempo.load();while(tempo.clock<49000)tempo.render();
    check(tempo.instrument->count==3 && tempo.instrument->heard[0].clock==12000 && tempo.instrument->heard[2].clock==48000,"note duration integrates a tempo change");
}
}
int main(int argc,char** argv){
    std::setvbuf(stdout,nullptr,_IONBF,0);const std::string only=argc>1?argv[1]:"all";
    try{if(only=="all"||only=="placement")placement();if(only=="all"||only=="cleanup")cleanup();if(only=="all"||only=="musical")musical();if(only=="all"||only=="pending")pendingRelease();if(only=="all"||only=="concurrent")concurrentPublication();if(only=="all"||only=="capacity")capacity();if(only=="all"||only=="overflow")overflowRelease();if(only=="all"||only=="ns")nanosecondEnd();if(only=="all"||only=="maximum")maximumLocate();}catch(const std::exception& e){check(false,e.what());}
    check(allocations.load()==0,"MIDI callback allocates nothing");check(audio::callbackFileIo.load()==0,"MIDI callback performs no file I/O");
    std::printf("%s -- %d checks, %d failure(s)\n",failures?"FAIL":"PASS",checks,failures);return failures?1:0;
}
