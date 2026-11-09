// SPDX-License-Identifier: GPL-3.0-or-later
#include "adi/audio/io_audit.hpp"
#include "adi/controller_lanes.hpp"
#include "adi/clip_commands.hpp"
#include "adi/engine/session.hpp"
#include "adi/store.hpp"
#include "adi/ops.hpp"
#include "adi/history.hpp"
#include "temp_directory.hpp"
#include <SQLiteCpp/SQLiteCpp.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <new>
#include <limits>
#include <span>
#include <thread>
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
    std::int64_t tail=kInfiniteTail;int calls=0; // ADR-0158: most plugins report no tail
    const device::DeviceIdentity& identity()const noexcept override{return id;}
    bool loaded()const noexcept override{return true;}
    bool processesDouble()const noexcept override{return true;}
    EventFlow eventFlow()const noexcept override{return EventFlow::Consume;}
    std::int64_t tailSamples()const noexcept override{return tail;}
    void process(const NodeIo& io)noexcept override{
        ++calls;
        for(const auto& e:io.events)if(count<heard.size())heard[count++]={e,clock+e.frame};
        for(int c=0;c<io.channels;++c) {
            if(io.wide())std::fill_n(io.out64[c]+io.blockOffset,io.frames,0.0);
            else std::fill_n(io.out[c]+io.blockOffset,io.frames,0.0f);
        }
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
    double rate;int block;Precision width;std::int64_t clock=0;
    explicit Fixture(double r=48000,int b=512,Precision w=Precision::Float32):rate(r),block(b),width(w){
        StoreError e{};store=Store::createEmpty(dir.path()/"session.adi",e);if(!store)throw std::runtime_error("create");
        store->db().exec("INSERT INTO tracks(id,kind,name,index_in_parent) VALUES(1,'midi','Keys',0),(9,'master','Master',1); INSERT INTO plugin_refs(id,format,uid,name,subtype) VALUES(1,'vst3','test.midi','Recorder','instrument'); INSERT INTO device_chains(id,track_id) VALUES(1,1); INSERT INTO devices(id,chain_id,ord,plugin_ref_id,name) VALUES(1,1,0,1,'Recorder')");
    }
    void clip(int id=1,std::int64_t on=24024,std::int64_t duration=480480,std::int64_t length=5765760){
        SQLite::Statement q(store->db(),"INSERT INTO clips(id,track_id,kind,time_base,pos_ticks,length_ticks) VALUES(?,1,'midi',0,0,?)");q.bind(1,id);q.bind(2,length);q.exec();
        NoteRecord note{};note.start_ticks=on;note.dur_ticks=duration;note.note_id=42;note.key=60;note.channel=3;note.vel_on=93;note.vel_off=37;note.probability=10000;note.tuning_cents=31.25f;
        auto bytes=writeStream<NoteRecord>(FourCC::Notes,std::vector<NoteRecord>{note});
        if(!store->putEventStream(id,"notes",bytes))throw std::runtime_error("ANOT write");
    }
    void load(){session.graph().setFadeFrames(0);if(!session.load(*store,[this](const DeviceRequest& request,std::string&) -> std::unique_ptr<device::DeviceInstance> {if(request.device->id==2)return std::make_unique<LatencyEffect>();auto p=std::make_unique<Instrument>();instrument=p.get();return p;},{2,rate,block,width}))throw std::runtime_error(session.error());session.transport().play();}
    void render(int n=0){
        if(!n)n=block;
        std::array<float,8192> out{};std::array<double,8192> out64{};
        float* channels[]={out.data(),out.data()+4096};double* wide[]={out64.data(),out64.data()+4096};
        AudioIo io;io.frames=n;io.numOut=2;
        if(width==Precision::Float64)io.out64=wide;else io.out=channels;
        instrument->clock=clock;session.process(io);clock+=n;
    }
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
    const auto target=graph->find(*f.session.nodeFor(1)); // the instrument itself: outputFor is the strip (ADR-0163)
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
void loopEntry(){
    Fixture f;f.clip(1,0,240240,240240);
    f.store->db().exec("UPDATE clips SET loop_enabled=1,loop_start_ticks=24024,loop_len_ticks=48048,content_offset_ticks=12012");
    f.load();while(f.clock<1100)f.render();
    int ons=0,offs=0;std::int64_t firstOff=-1;
    for(std::size_t i=0;i<f.instrument->count;++i){const auto& h=f.instrument->heard[i];
        if(h.e.type==EventType::NoteOn)++ons;
        if(h.e.type==EventType::NoteOff){++offs;if(firstOff<0)firstOff=h.clock;}}
    check(ons==5 && offs==5 && firstOff==250,"entering a clip loop does not retrigger before its first wrap");
}
void heldNote(){
    Fixture f;f.clip(1,0,5765760);f.load();f.instrument->tail=0;
    for(int i=0;i<20;++i)f.render();
    check(f.instrument->balance()==1 && f.instrument->calls>=20,"a zero-tail instrument keeps running a held clip note (ADR-0158)");
    while(f.clock<26000)f.render();
    const auto released=f.instrument->calls;f.render();f.render();
    check(f.instrument->balance()==0 && f.instrument->calls==released,"and sleeps once the clip releases it");
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
void activeFlag() {
    Fixture f(48000,128);f.clip(1,0,240240,5765760);
    auto mute = [&] {
        OpRequest op;op.opType="note.setMute";op.payload={{"clip",1},{"note",42},{"muted",true}};
        if(!OpJournal(*f.store).commit(op).ok)throw std::runtime_error("note mute failed");
    };
    mute();f.load();f.render();
    check(f.instrument->count==0,"a note deactivated through the op emits no playback event");
    if(!History(*f.store).undo().ok)throw std::runtime_error("note mute undo failed");
    f.refresh();f.session.transport().locate(0);f.clock=0;f.render();
    check(f.instrument->balance()==1,"undo and refresh reactivate the note with its original event");
    mute();f.refresh();f.render();
    check(f.instrument->balance()==0 && f.instrument->count>0 && f.instrument->heard[f.instrument->count-1].e.type==EventType::NoteOff,
          "deactivating a held note releases it on the next callback without a stuck voice");
}

// The source records stay whole. Each independent clip clips/chases them at
// its visible boundaries; stored IDs need not equal runtime voice IDs.
struct SplitVoice {
    std::int64_t on=0,off=0;
    std::uint16_t key=0;
    std::uint8_t velocity=0,release=0;
    bool tuned=false;
};
bool splitSchedule(const Instrument& instrument,std::span<const SplitVoice> expected) {
    std::array<std::uint64_t,10> ids{};
    if(expected.size()>ids.size() || instrument.count>=instrument.heard.size())return false;
    std::size_t expressions=0;
    for(std::size_t voice=0;voice<expected.size();++voice) {
        const auto& wanted=expected[voice];
        std::size_t ons=0,offs=0,tuning=0;
        for(std::size_t i=0;i<instrument.count;++i) {
            const auto& heard=instrument.heard[i];const auto& event=heard.e;
            if(event.type==EventType::NoteOn && heard.clock==wanted.on && event.channel==3 &&
               event.dim==wanted.key && event.value==static_cast<double>(wanted.velocity)/127) {
                ++ons;ids[voice]=event.noteId;
            }
        }
        if(ons!=1 || ids[voice]==0)return false;
        for(std::size_t earlier=0;earlier<voice;++earlier)
            if(ids[earlier]==ids[voice])return false;
        for(std::size_t i=0;i<instrument.count;++i) {
            const auto& heard=instrument.heard[i];const auto& event=heard.e;
            if(event.noteId!=ids[voice])continue;
            if(event.type==EventType::NoteOff && heard.clock==wanted.off && event.channel==3 &&
               event.dim==wanted.key && event.value==static_cast<double>(wanted.release)/127)++offs;
            if(event.type==EventType::NoteExpression && heard.clock==wanted.on &&
               event.dim==static_cast<std::uint16_t>(ExpressionDim::Pitch) && event.value==0.3125)++tuning;
        }
        if(offs!=1 || tuning!=(wanted.tuned?1u:0u))return false;
        expressions+=tuning;
    }
    if(instrument.count!=2*expected.size()+expressions || instrument.balance()!=0)return false;
    for(const std::int64_t boundary:{4000,6000}) {
        bool started=false;
        for(std::size_t i=0;i<instrument.count;++i) {
            const auto& heard=instrument.heard[i];
            if(heard.clock!=boundary)continue;
            if(heard.e.type==EventType::NoteOn)started=true;
            if(heard.e.type==EventType::NoteOff && started)return false;
        }
    }
    return true;
}
bool renderSplitSchedule(Fixture& fixture,bool irregular,std::span<const SplitVoice> expected) {
    fixture.session.transport().play(false);fixture.render(1);
    fixture.session.transport().locate(0);fixture.clock=0;fixture.instrument->count=0;
    fixture.session.transport().play();
    constexpr std::array<int,6> parts{17,61,5,101,73,29};
    std::size_t part=0;
    while(fixture.clock<9000) {
        const auto next=irregular?parts[part++%parts.size()]:128;
        const auto frames=static_cast<int>(std::min<std::int64_t>(next,9000-fixture.clock));
        fixture.render(frames);
    }
    const bool good=splitSchedule(*fixture.instrument,expected);
    fixture.session.transport().play(false);fixture.render(1);
    return good;
}
bool splitNoteBytes(Store& store,std::span<const std::int64_t> clips,const std::vector<std::byte>& original) {
    for(const auto clip:clips) {
        const auto copied=store.getEventStream(clip,"notes");
        if(!copied || *copied!=original)return false;
        StreamReader<NoteRecord> reader(*copied,FourCC::Notes);
        bool maximum=false,highBit=false;
        for(std::uint32_t i=0;i<reader.count();++i) {
            const auto note=reader.at(i);
            if(!note)return false;
            maximum=maximum || note->note_id==std::numeric_limits<std::uint64_t>::max();
            highBit=highBit || note->note_id==(std::uint64_t{1}<<63)+7;
        }
        if(!reader.ok() || !maximum || !highBit)return false;
    }
    return true;
}
void twoPointSplits() {
    constexpr std::int64_t k=240240; // 1000 samples at 120 BPM / 48 kHz
    constexpr std::array<SplitVoice,7> originalVoices{{
        {2000,4000,60,91,31,false},{4000,5000,61,92,32,false},
        {3000,7000,62,93,33,true}, {6000,8000,63,94,34,false},
        {5000,6000,64,95,35,false},{4000,7000,62,96,36,false},
        {2000,3000,66,98,38,false}}};
    constexpr std::array<SplitVoice,10> splitVoices{{
        {2000,4000,60,91,31,false},{4000,5000,61,92,32,false},
        {3000,4000,62,93,33,true}, {4000,6000,62,93,33,true},
        {6000,7000,62,93,33,true}, {6000,8000,63,94,34,false},
        {5000,6000,64,95,35,false},{4000,6000,62,96,36,false},
        {6000,7000,62,96,36,false},{2000,3000,66,98,38,false}}};
    constexpr std::array<std::int64_t,3> pieces{1,2,3};
    for(const auto width:{Precision::Float32,Precision::Float64})for(const bool irregular:{false,true}) {
        Fixture fixture(48000,128,width);
        fixture.store->db().exec("INSERT INTO clips(id,track_id,kind,time_base,pos_ticks,length_ticks,content_offset_ticks) VALUES(1,1,'midi',0,480480,1441440,240240); DELETE FROM tempo_map; INSERT INTO tempo_map(pos_ticks,bpm,curve) VALUES(0,120,0)");
        auto make=[](std::uint64_t id,std::int64_t start,std::int64_t end,std::uint8_t key,
                     std::uint8_t velocity,std::uint8_t release,float tuning=0) {
            NoteRecord note{};note.note_id=id;note.start_ticks=start*k;note.dur_ticks=(end-start)*k;
            note.key=key;note.channel=3;note.vel_on=velocity;note.vel_off=release;
            note.probability=10000;note.tuning_cents=tuning;return note;
        };
        std::array<NoteRecord,8> records{{
            make(std::numeric_limits<std::uint64_t>::max(),1,3,60,91,31),
            make((std::uint64_t{1}<<63)+7,3,4,61,92,32),make(3,2,6,62,93,33,31.25f),
            make(4,5,7,63,94,34),make(5,4,5,64,95,35),make(6,3,6,62,96,36),
            make(7,7,8,65,97,37),make(8,0,2,66,98,38)}};
        std::stable_sort(records.begin(),records.end(),[](const auto& a,const auto& b){return a.start_ticks<b.start_ticks;});
        const auto original=writeStream<NoteRecord>(FourCC::Notes,records);
        if(!fixture.store->putEventStream(1,"notes",original))throw std::runtime_error("split ANOT write");
        fixture.load();
        check(renderSplitSchedule(fixture,irregular,originalVoices),"native unsplit reference honors source offset, overlap and exclusive end in both widths and callback partitions");
        const auto head=OpJournal(*fixture.store).headSeq();
        const auto prepared=clip_edit::prepareSplit(*fixture.store,1,2,3,{0,4*k},{0,6*k},48000);
        check(prepared.ok && prepared.requests.size()==1 && prepared.requests[0].opType=="clip.split",
              "two-point MIDI split is prepared as one captured native operation");
        if(!prepared.ok)continue;
        const auto committed=clip_edit::commitPrepared(*fixture.store,prepared);
        check(committed.ok && committed.seqs.size()==1,"both MIDI cuts commit as one journal operation");
        if(!committed.ok || committed.seqs.size()!=1)continue;
        check(splitNoteBytes(*fixture.store,pieces,original),"both cuts retain complete ANOT bytes and full-width stored note identities on all pieces");
        fixture.refresh();
        check(renderSplitSchedule(fixture,irregular,splitVoices),"native two-cut boundaries release before chase, retain tuning and keep overlapping same-key voices independent");
        const auto undone=History(*fixture.store).undo();
        check(undone.ok && undone.opsApplied==1 && undone.txnId==committed.txnId &&
              OpJournal(*fixture.store).headSeq()==head,"one undo rejoins both cuts through the original single operation");
        if(!undone.ok)continue;
        fixture.refresh();
        check(renderSplitSchedule(fixture,irregular,originalVoices),"one undo restores the original native MIDI schedule");
        const auto redone=History(*fixture.store).redo();
        check(redone.ok && redone.opsApplied==1 && redone.txnId==committed.txnId &&
              OpJournal(*fixture.store).headSeq()==committed.seqs[0],"one redo replays both captured cuts in the same transaction");
        if(!redone.ok)continue;
        fixture.refresh();
        const bool restoredBytes=splitNoteBytes(*fixture.store,pieces,original);
        const bool restoredPlayback=renderSplitSchedule(fixture,irregular,splitVoices);
        check(restoredBytes && restoredPlayback,"redo restores full-width stored identities and the exact native two-cut schedule");
    }
}

void sourceMarkers() {
    constexpr std::int64_t quarter=5765760;
    auto set = [](Store& store,const char* name,Payload payload) {
        OpRequest op;op.opType=name;op.payload=std::move(payload);
        if(!OpJournal(store).commit(op).ok)throw std::runtime_error("clip property op failed");
    };
    Fixture plain;plain.clip(1,0,2*quarter,4*quarter);
    set(*plain.store,"clip.setStartMarker",{{"id",1},{"ticks",quarter/2}});
    set(*plain.store,"clip.setEndMarker",{{"id",1},{"ticks",quarter}});
    plain.load();while(plain.clock<13000)plain.render();
    check(plain.instrument->count==3 && plain.instrument->heard[0].clock==0 &&
              plain.instrument->heard[2].e.type==EventType::NoteOff && plain.instrument->heard[2].clock==12000,
          "Start chases a held note and End releases it at the exact source marker");
    Fixture loop;loop.clip(1,0,2*quarter,4*quarter);
    set(*loop.store,"clip.setStartMarker",{{"id",1},{"ticks",quarter/2}});
    set(*loop.store,"clip.setEndMarker",{{"id",1},{"ticks",quarter}});
    set(*loop.store,"clip.setLoop",{{"id",1},{"on",true},{"start",0},{"length",2*quarter}});
    loop.load();while(loop.clock<38000)loop.render();
    std::int64_t firstOff=-1;
    for(std::size_t i=0;i<loop.instrument->count;++i)
        if(loop.instrument->heard[i].e.type==EventType::NoteOff){firstOff=loop.instrument->heard[i].clock;break;}
    check(firstOff==36000,"MIDI Loop ignores End and releases at its own repeat boundary");
}

}
namespace {
void sessionControllers() {
    Fixture f;
    f.clip(1, 0, 5765760);
    const std::array<controllers::Point, 1> points{{{0, 0, 0.123456789012345, 0, 0, 0}}};
    auto prepared = controllers::prepareWrite(*f.store, 1, 3, 7, points);
    if (!prepared.ok || !controllers::commitPrepared(*f.store, prepared).ok)
        throw std::runtime_error("controller prepare/commit");
    f.load();
    f.render();
    check(f.instrument->count >= 2 && f.instrument->heard[0].e.type == EventType::Controller &&
              f.instrument->heard[0].e.value == points[0].value && f.instrument->heard[0].e.channel == 3 &&
              f.instrument->heard[1].e.type == EventType::NoteOn,
          "committed controller bytes pass through Session rows and graph before their note");
    const std::array<controllers::Point, 1> pedal{{{0, 0, 1, 0, 0, 0}}};
    auto sustain = controllers::prepareWrite(*f.store, 1, 3, 64, pedal);
    if (!sustain.ok || !controllers::commitPrepared(*f.store, sustain).ok)
        throw std::runtime_error("sustain prepare/commit");
    f.refresh();
    f.render();
    const auto before = f.instrument->count;
    f.store->db().exec("DELETE FROM clips");
    f.refresh();
    f.render();
    bool zero = false, off = false;
    for (std::size_t i = before; i < f.instrument->count; ++i) {
        const auto &e = f.instrument->heard[i].e;
        if (e.type == EventType::Controller && e.paramId == 64 && e.value == 0)
            zero = true;
        if (e.type == EventType::NoteOff && zero)
            off = true;
    }
    check(zero && off, "Session refresh retains pedal ownership after cascade-deleting the stream");
}
} // namespace

int main(int argc,char** argv){
    std::setvbuf(stdout,nullptr,_IONBF,0);const std::string only=argc>1?argv[1]:"all";
    try{if(only=="all"||only=="split")twoPointSplits();if(only=="all"||only=="controllers")sessionControllers();if(only=="all"||only=="active")activeFlag();if(only=="all"||only=="markers")sourceMarkers();if(only=="all"||only=="placement")placement();if(only=="all"||only=="cleanup")cleanup();if(only=="all"||only=="musical")musical();if(only=="all"||only=="pending")pendingRelease();if(only=="all"||only=="concurrent")concurrentPublication();if(only=="all"||only=="capacity")capacity();if(only=="all"||only=="overflow")overflowRelease();if(only=="all"||only=="ns")nanosecondEnd();if(only=="all"||only=="maximum")maximumLocate();if(only=="all"||only=="entry")loopEntry();if(only=="all"||only=="held")heldNote();}catch(const std::exception& e){check(false,e.what());}
    check(allocations.load()==0,"MIDI callback allocates nothing");check(audio::callbackFileIo.load()==0,"MIDI callback performs no file I/O");
    std::printf("%s -- %d checks, %d failure(s)\n",failures?"FAIL":"PASS",checks,failures);return failures?1:0;
}
