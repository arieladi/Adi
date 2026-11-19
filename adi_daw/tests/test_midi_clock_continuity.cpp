// SPDX-License-Identifier: AGPL-3.0-or-later
#include "adi/audio/io_audit.hpp"
#include "adi/blob.hpp"
#include "adi/controller_lanes.hpp"
#include "adi/engine/session.hpp"
#include "adi/store.hpp"
#include "adi/textproj.hpp"
#include "temp_directory.hpp"
#include <SQLiteCpp/SQLiteCpp.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <new>

namespace {std::atomic<unsigned> allocations{0},frees{0};}
void* operator new(std::size_t n){if(adi::audio::onAudioThread)++allocations;if(auto* p=std::malloc(n?n:1))return p;throw std::bad_alloc();}
void* operator new[](std::size_t n){return operator new(n);}
void operator delete(void* p)noexcept{if(p && adi::audio::onAudioThread)++frees;std::free(p);}
void operator delete[](void* p)noexcept{operator delete(p);}
void operator delete(void* p,std::size_t)noexcept{operator delete(p);}
void operator delete[](void* p,std::size_t)noexcept{operator delete(p);}
// Nothrow allocations use the same counted malloc/free pair under sanitizers.
void* operator new(std::size_t n, const std::nothrow_t&) noexcept {
    try { return ::operator new(n); } catch (...) { return nullptr; }
}
void* operator new[](std::size_t n, const std::nothrow_t&) noexcept {
    try { return ::operator new[](n); } catch (...) { return nullptr; }
}
void operator delete(void* p, const std::nothrow_t&) noexcept { ::operator delete(p); }
void operator delete[](void* p, const std::nothrow_t&) noexcept { ::operator delete[](p); }

namespace {
using namespace adi;
using namespace adi::engine;
constexpr auto q=textproj::kPPQ;
int checks=0,failures=0;
void check(bool good,const char* label){++checks;if(!good){++failures;std::printf("FAIL: %s\n",label);}}
struct Heard {Event event;std::int64_t sample=0;};
struct Instrument final:device::DeviceInstance {
    device::DeviceIdentity identity_;
    std::array<Heard,512> heard{};
    std::array<std::uint64_t,128> voices{};
    std::size_t count=0;
    std::int64_t clock=0;
    const device::DeviceIdentity& identity()const noexcept override{return identity_;}
    bool loaded()const noexcept override{return true;}
    bool processesDouble()const noexcept override{return true;}
    EventFlow eventFlow()const noexcept override{return EventFlow::Consume;}
    std::int64_t tailSamples()const noexcept override{return kInfiniteTail;}
    void process(const NodeIo& io)noexcept override {
        for(const auto& event:io.events)if(count<heard.size())heard[count++]={event,clock+event.frame};
        for(std::int32_t i=0;i<io.frames;++i) {
            const auto at=io.blockOffset+i;
            for(const auto& event:io.events)if(event.frame==at) {
                if(event.type==EventType::NoteOn) {
                    for(auto& voice:voices)if(!voice){voice=event.noteId;break;}
                } else if(event.type==EventType::NoteOff) {
                    for(auto& voice:voices)if(voice==event.noteId)voice=0;
                }
            }
            const auto value=std::any_of(voices.begin(),voices.end(),[](auto voice){return voice!=0;})?0.25:0.;
            for(std::int32_t channel=0;channel<io.channels;++channel) {
                if(io.wide())io.out64[channel][at]=value;
                else io.out[channel][at]=static_cast<float>(value);
            }
        }
    }
    std::size_t events(EventType type)const {
        return static_cast<std::size_t>(std::count_if(heard.begin(),heard.begin()+static_cast<std::ptrdiff_t>(count),
            [=](const auto& value){return value.event.type==type;}));
    }
};
struct Delay final:device::DeviceInstance {
    device::DeviceIdentity identity_;
    EventFlow flow=EventFlow::Through;
    explicit Delay(EventFlow value=EventFlow::Through):flow(value){}
    const device::DeviceIdentity& identity()const noexcept override{return identity_;}
    bool loaded()const noexcept override{return true;}
    bool processesDouble()const noexcept override{return true;}
    EventFlow eventFlow()const noexcept override{return flow;}
    std::int32_t latencySamples()const noexcept override{return 512;}
    void process(const NodeIo& io)noexcept override {
        for(std::int32_t channel=0;channel<io.channels;++channel) {
            if(io.wide())std::fill_n(io.out64[channel]+io.blockOffset,io.frames,0.);
            else std::fill_n(io.out[channel]+io.blockOffset,io.frames,0.f);
        }
    }
};
struct Fixture {
    test::TempDirectory temp{"midi_clock_continuity","session"};
    std::unique_ptr<Store> store;
    Session session;
    Instrument* instrument=nullptr;
    std::array<Instrument*,4> instruments{};
    Precision width;
    std::int64_t clock=0;
    unsigned loads=0;
    std::array<float,200> narrow{};
    std::array<double,200> wide{};
    explicit Fixture(Precision precision):width(precision) {
        StoreError error{};store=Store::createEmpty(temp.path()/"session.adi",error);
        if(!store)throw std::runtime_error("create session");
        store->db().exec("INSERT INTO tracks(id,kind,name,index_in_parent) VALUES(1,'midi','Keys',0),(9,'master','Main',1); INSERT INTO plugin_refs(id,format,uid,name,subtype) VALUES(1,'vst3','continuity','Test','instrument'); INSERT INTO device_chains(id,track_id) VALUES(1,1); INSERT INTO devices(id,chain_id,ord,plugin_ref_id,name) VALUES(1,1,0,1,'Test'); INSERT INTO clips(id,track_id,kind,time_base,pos_ticks,length_ticks) VALUES(1,1,'midi',0,0,23063040); DELETE FROM tempo_map; INSERT INTO tempo_map(pos_ticks,bpm,curve) VALUES(0,120,0)");
        write({note(42)});
    }
    static NoteRecord note(std::uint64_t id,std::int64_t start=0,std::int64_t duration=q) {
        NoteRecord n{};n.note_id=id;n.start_ticks=start;n.dur_ticks=duration;n.key=60;n.channel=3;
        n.vel_on=100;n.vel_off=40;n.probability=10000;return n;
    }
    void write(std::initializer_list<NoteRecord> notes) {
        const std::vector<NoteRecord> data(notes);
        if(!store->putEventStream(1,"notes",writeStream<NoteRecord>(FourCC::Notes,data)))throw std::runtime_error("write notes");
    }
    void pedal(bool on) {
        const std::array<controllers::Point,1> points{{{0,0,on?1.:0.,0,0,0}}};
        const auto prepared=controllers::prepareWrite(*store,1,3,64,points);
        if(!prepared.ok || !controllers::commitPrepared(*store,prepared).ok)throw std::runtime_error("write sustain");
    }
    void ramp() {
        const std::array<controllers::Point,2> points{{{0,0,0,0,1,0},{0,q,1,0,1,0}}};
        const auto prepared=controllers::prepareWrite(*store,1,3,7,points);
        if(!prepared.ok || !controllers::commitPrepared(*store,prepared).ok)throw std::runtime_error("write ramp");
    }
    void delay(bool twice=false) {
        store->db().exec("UPDATE devices SET ord=1; INSERT INTO devices(id,chain_id,ord,plugin_ref_id,name) VALUES(2,1,0,1,'Delay')");
        if(twice)store->db().exec("UPDATE devices SET ord=2 WHERE id=1; INSERT INTO devices(id,chain_id,ord,plugin_ref_id,name) VALUES(3,1,1,1,'Delay 2')");
    }
    void fanout(bool stopped=false) {
        store->db().exec("INSERT INTO tracks(id,kind,name,index_in_parent) VALUES(2,'midi','Direct',2),(3,'midi','Delayed',3); UPDATE device_chains SET track_id=2 WHERE id=1; INSERT INTO device_chains(id,track_id) VALUES(2,3); INSERT INTO devices(id,chain_id,ord,plugin_ref_id,name) VALUES(2,2,0,1,'Delay'),(3,2,1,1,'Second instrument'); INSERT INTO routing(id,src_kind,src_id,dst_kind,dst_id,kind) VALUES(1,'track',1,'track',2,'main'),(2,'track',1,'track',3,'main')");
        if(stopped)store->db().exec("UPDATE devices SET name='Stop' WHERE id=3");
    }
    void load() {
        SessionSpec spec{2,48000,100,width};
        if(!session.load(*store,[this](const DeviceRequest& request,std::string&)->std::unique_ptr<device::DeviceInstance>{
            if(request.device->name.starts_with("Delay"))return std::make_unique<Delay>();
            if(request.device->name=="Stop")return std::make_unique<Delay>(EventFlow::Stop);
            ++loads;auto value=std::make_unique<Instrument>();instrument=value.get();
            if(loads<=instruments.size())instruments[loads-1]=instrument;
            return value;
        },spec))throw std::runtime_error(session.error());
        session.transport().play();
    }
    void refresh(){if(!session.refresh(*store))throw std::runtime_error(session.error());}
    void faster() {
        if(!session.setExternalTempoAt(*store,240.,240240))throw std::runtime_error(session.error());
    }
    bool render(Session::RenderBlock* held=nullptr) {
        float* out[]{narrow.data(),narrow.data()+100};double* out64[]{wide.data(),wide.data()+100};
        AudioIo io;io.frames=100;io.numOut=2;io.streamTimeSamples=clock;
        if(width==Precision::Float64)io.out64=out64;else io.out=out;
        for(auto* instance:instruments)if(instance)instance->clock=clock;
        if(held)(void)held->process(io);else session.process(io);
        clock+=100;
        return width==Precision::Float64?std::all_of(wide.begin(),wide.end(),[](double value){return value==0.25;}):
            std::all_of(narrow.begin(),narrow.end(),[](float value){return value==0.25f;});
    }
    void start(){load();for(int i=0;i<10;++i)render();}
};
void heldClock(Precision width) {
    Fixture f(width);f.pedal(true);f.start();
    check(f.session.model().notes.size()==1 && f.session.model().notes[0].noteId==42,"Store rows retain the persisted ANOT note identity");
    check(f.instrument->events(EventType::NoteOn)==1,"one native held voice before the clock change");
    const auto identity=f.instrument->heard[0].event.type==EventType::NoteOn?f.instrument->heard[0].event.noteId:f.instrument->heard[1].event.noteId;
    const auto before=f.instrument->count;
    f.faster();
    const auto a=allocations.load(),d=frees.load(),io=audio::callbackFileIo.load();
    const bool audible=f.render();
    check(a==allocations.load() && d==frees.load() && io==audio::callbackFileIo.load(),"native clock adoption allocates/frees/reads no files on the callback");
    check(audible && f.loads==1 && f.instrument->count==before,"clock-only rebuild preserves the actual instrument, held PCM and sustain without retrigger");
    while(f.clock<12600)f.render();
    bool exact=false;
    for(std::size_t i=0;i<f.instrument->count;++i)if(f.instrument->heard[i].event.type==EventType::NoteOff)
        exact=f.instrument->heard[i].sample==12500 && f.instrument->heard[i].event.noteId==identity;
    check(exact && f.instrument->events(EventType::NoteOn)==1 && f.instrument->events(EventType::NoteOff)==1,"the original voice releases once at the new performed-tempo endpoint");
    f.session.transport().play(false);f.render();
    bool zero=false;for(std::size_t i=before;i<f.instrument->count;++i)zero=zero || (f.instrument->heard[i].event.type==EventType::Controller && f.instrument->heard[i].event.paramId==64 && f.instrument->heard[i].event.value==0);
    check(zero,"stop after clock adoption releases the retained sustain owner");
}
void identities(Precision width) {
    Fixture f(width);f.start();
    const auto runtime=f.instrument->heard[0].event.noteId;
    f.write({Fixture::note(99,2*q),Fixture::note(42)});f.faster();f.render();
    check(f.instrument->events(EventType::NoteOn)==1 && f.instrument->events(EventType::NoteOff)==0,"inserting/reordering another stored note cannot steal the held identity");
    f.write({Fixture::note(43)});f.refresh();f.render();
    check(f.instrument->events(EventType::NoteOn)==2 && f.instrument->events(EventType::NoteOff)==1 &&
          f.instrument->heard[1].event.noteId==runtime && f.instrument->heard[2].event.noteId!=runtime,
          "replacing the stored identity releases the old voice and chases a distinct new one");
    auto edited=Fixture::note(43);edited.key=61;f.write({edited});f.refresh();f.render();
    check(f.instrument->events(EventType::NoteOn)==3 && f.instrument->events(EventType::NoteOff)==2 && f.instrument->heard[4].event.dim==61,"same identity with an authored pitch edit is not a clock-only continuation");
    f.store->db().exec("DELETE FROM clips");f.refresh();f.render();
    check(f.instrument->events(EventType::NoteOff)==3,"deleting the final clip releases its adopted voice");
}
void skippedControllers(Precision width) {
    for(const bool collectUnheard:{false,true}) {
    Fixture f(width);f.pedal(true);f.start();const auto before=f.instrument->count;
    // Prepare B and then A without ever rendering B. Version identity must be
    // compared with the source that actually rendered, not the owner's last.
    const auto original=f.session.model().controllers;
    f.store->db().exec("DELETE FROM event_streams WHERE stream_kind='cc'");f.refresh();
    if(collectUnheard) {
        {auto unused=f.session.beginRender();(void)unused;}
        check(f.session.graph().collect()>0,"the previously rendered controller source was actually collected after an unheard acquisition");
    }
    if(original.size()!=1)throw std::runtime_error("controller identity fixture");
    const auto& lane=original.front();
    SQLite::Statement restore(f.store->db(),"INSERT INTO event_streams(id,clip_id,stream_kind,channel,controller,data) VALUES(?,1,'cc',3,64,?)");
    restore.bind(1,lane.id);restore.bind(2,lane.data.data(),static_cast<int>(lane.data.size()));restore.exec();
    f.refresh();f.render();
    check(f.instrument->count==before,"unheard controller deletion/restoration does not release the actual held sustain");
    }
}
void exactBoundary(Precision width) {
    Fixture f(width);f.pedal(true);
    f.store->db().exec("UPDATE clips SET length_ticks=240240");f.start();const auto before=f.instrument->count;
    f.faster();f.render();
    bool zero=false;for(std::size_t i=before;i<f.instrument->count;++i)zero=zero || (f.instrument->heard[i].event.type==EventType::Controller && f.instrument->heard[i].event.paramId==64 && f.instrument->heard[i].event.value==0 && f.instrument->heard[i].sample==1000);
    check(zero,"a clock-only generation at the exact controller clip end still releases sustain");
}
void queuedEvents(Precision width) {
    for(const bool changeBeforeOn:{true,false}) {
        Fixture f(width);f.delay();f.pedal(true);f.write({Fixture::note(42,0,240240)});f.load();
        if(!changeBeforeOn)f.session.graph().setKeepHistory(false);
        Fixture reference(width);reference.delay();reference.pedal(true);reference.write({Fixture::note(42,0,240240)});reference.load();
        while(reference.clock<1700)reference.render();
        const auto boundary=changeBeforeOn?100:1100;
        while(f.clock<boundary)f.render();
        const auto observed=f.instrument->count;f.faster();
        while(f.clock<1700)f.render();
        std::int64_t on=-1,off=-1,pedal=-1;
        for(std::size_t i=0;i<f.instrument->count;++i) {
            const auto& e=f.instrument->heard[i];
            if(e.event.type==EventType::NoteOn)on=e.sample;
            if(e.event.type==EventType::NoteOff)off=e.sample;
            if(e.event.type==EventType::Controller && e.event.paramId==64 && e.event.value==1)pedal=e.sample;
        }
        check(on==512 && off==1512 && pedal==512 && f.instrument->events(EventType::NoteOn)==1 && f.instrument->events(EventType::NoteOff)==1,
              "retained route preserves exact delayed note-on/off and sustain timing across clock publication");
        bool equal=f.instrument->count==reference.instrument->count;
        for(std::size_t i=0;equal && i<f.instrument->count;++i) {
            const auto& a=f.instrument->heard[i];const auto& b=reference.instrument->heard[i];
            equal=a.sample==b.sample && a.event.type==b.event.type && a.event.value==b.event.value;
        }
        check(equal,"actual delayed event stream equals the no-rebuild native reference");
        check(changeBeforeOn?observed==0:observed==2,"queue regression actually swaps while the selected on/off is still deferred");
    }
}
void delayedCurve(Precision width) {
    Fixture f(width),reference(width);
    for(auto* fixture:{&f,&reference}){fixture->delay();fixture->ramp();fixture->load();}
    while(f.clock<300)f.render();
    f.faster();
    while(f.clock<900)f.render();
    while(reference.clock<900)reference.render();
    bool equal=f.instrument->count==reference.instrument->count;
    for(std::size_t i=0;equal && i<f.instrument->count;++i) {
        const auto& a=f.instrument->heard[i];const auto& b=reference.instrument->heard[i];
        equal=a.sample==b.sample && a.event.type==b.event.type && a.event.value==b.event.value;
    }
    check(equal && f.instrument->count>5,"all pending CC ramp samples survive publication, not only the newest serial");
}
void acquiredLocate(Precision width) {
    Fixture f(width);f.delay();f.load();f.render();f.faster();
    {auto held=f.session.beginRender();f.session.transport().locate(48000);f.render(&held);}
    while(f.clock<900)f.render();
    check(f.instrument->events(EventType::NoteOn)==0,"a locate after graph acquisition invalidates retained queued ons before they reach the instrument");
}
void multiHopLocate(Precision width) {
    Fixture f(width);f.delay(true);f.load();f.render();f.faster();
    while(f.clock<600)f.render();
    check(f.instrument->events(EventType::NoteOn)==0,"the transferred on has crossed the first delay but has not reached the second hop's instrument");
    f.session.transport().locate(48000);
    while(f.clock<1400)f.render();
    check(f.instrument->events(EventType::NoteOn)==0,"a later locate cancels a retained on after it was forwarded and deferred through another latency node");
}
void transportAndLegacy(Precision width) {
    for(int change=0;change<4;++change) {
        Fixture f(width);f.start();
        if(change==0)f.session.transport().locate(1200);
        if(change==1){f.session.transport().play(false);f.faster();}
        if(change==2){f.write({Fixture::note(0)});f.refresh();}
        if(change==3){f.write({Fixture::note(42),Fixture::note(42)});f.refresh();}
        f.render();
        check(f.instrument->events(EventType::NoteOff)>=1,"seek, stop, missing identity and duplicate identities never adopt stale ownership");
        f.session.transport().play(false);f.render();
        check(std::all_of(f.instrument->voices.begin(),f.instrument->voices.end(),[](auto voice){return voice==0;}),"transport and ambiguous identity transitions leave no held voices after stop");
    }
}
void pedalEdit(Precision width) {
    Fixture f(width);f.pedal(true);f.start();const auto before=f.instrument->count;
    f.store->db().exec("DELETE FROM event_streams WHERE stream_kind='cc'");f.refresh();f.render();
    bool zero=false;for(std::size_t i=before;i<f.instrument->count;++i)zero=zero || (f.instrument->heard[i].event.type==EventType::Controller && f.instrument->heard[i].event.paramId==64 && f.instrument->heard[i].event.value==0);
    check(zero,"real controller removal releases sustain even while unchanged notes remain held");
    check(f.instrument->events(EventType::NoteOn)==1 && f.instrument->events(EventType::NoteOff)==0,"a controller edit does not replace an unchanged stored note");
}
void destinationIdentity(Precision width) {
    Fixture replaced(width);replaced.pedal(true);replaced.start();
    const auto* previous=replaced.instrument;
    replaced.store->db().exec("DELETE FROM devices WHERE id=1; INSERT INTO devices(id,chain_id,ord,plugin_ref_id,name) VALUES(10,1,0,1,'Replacement')");
    replaced.refresh();
    check(replaced.instrument!=previous && replaced.loads==2 && replaced.render() &&
          replaced.instrument->events(EventType::NoteOn)==1,
          "a new actual instrument receives the held stored note instead of inheriting another instance's sounding state");
    bool pedal=false;
    for(std::size_t i=0;i<replaced.instrument->count;++i) {
        const auto& event=replaced.instrument->heard[i].event;
        pedal=pedal || (event.type==EventType::Controller && event.paramId==64 && event.value==1);
    }
    check(pedal,"a replacement MIDI consumer receives its own current controller state");
    const auto before=replaced.instrument->count;
    replaced.store->db().exec("INSERT INTO devices(id,chain_id,ord,plugin_ref_id,name) VALUES(11,1,1,1,'Delay after instrument')");
    replaced.refresh();replaced.render();
    check(replaced.instrument->count==before && replaced.loads==2,
          "editing an audio effect after the consuming instrument does not retrigger its held notes or sustain");

    Fixture restored(width);restored.pedal(true);restored.start();
    auto* original=restored.instrument;
    restored.session.transport().play(false);restored.render();
    restored.store->db().exec("INSERT INTO tracks(id,kind,name,index_in_parent) VALUES(2,'midi','Inactive destination',2); INSERT INTO device_chains(id,track_id) VALUES(2,2); UPDATE devices SET chain_id=2 WHERE id=1");
    restored.refresh();restored.session.transport().play();restored.render();
    const auto priorOn=original->events(EventType::NoteOn);
    restored.store->db().exec("UPDATE devices SET chain_id=1 WHERE id=1");restored.refresh();
    check(restored.instrument==original && restored.loads==1 && restored.render() &&
          original->events(EventType::NoteOn)==priorOn+1,
          "reactivating an existing destination chases the note which sounded only on the intervening event route");
}
void fillDestination(Fixture& f,std::int64_t device=1) {
    auto* graph=f.session.graph().currentGraph();graph->setEventCapacity(4);graph->prepare(48000,100);
    const auto target=graph->find(*f.session.nodeFor(device));
    Event filler;filler.type=EventType::ParamValue;
    for(int i=0;i<4;++i)graph->pushInputEvent(target,filler);
}
void rejection(Precision width) {
    Fixture f(width);f.write({Fixture::note(42,0,240240)});f.start();fillDestination(f);f.render();
    check(f.instrument->events(EventType::NoteOff)==0,"full instrument queue rejects the due note-off");
    f.faster();f.render();
    check(f.instrument->events(EventType::NoteOn)==1 && f.instrument->events(EventType::NoteOff)==1,"a rejected release remains owned across a clock generation and is retried without a new voice");
    Fixture pedal(width);pedal.pedal(true);pedal.start();
    pedal.store->db().exec("DELETE FROM event_streams WHERE stream_kind='cc'");pedal.refresh();fillDestination(pedal);pedal.render();
    const auto before=pedal.instrument->count;pedal.faster();pedal.render();
    bool zero=false;for(std::size_t i=before;i<pedal.instrument->count;++i)zero=zero || (pedal.instrument->heard[i].event.type==EventType::Controller && pedal.instrument->heard[i].event.paramId==64 && pedal.instrument->heard[i].event.value==0);
    check(zero && pedal.instrument->events(EventType::NoteOn)==1,"a rejected sustain release survives another clock generation after its lane disappears");
}
bool silent(const Instrument& instrument) {
    return std::all_of(instrument.voices.begin(),instrument.voices.end(),[](auto voice){return voice==0;});
}
void fanoutReleases(Precision width) {
    for(const std::int64_t duration:{100,1000}) {
        Fixture f(width),reference(width);
        for(auto* fixture:{&f,&reference}) {
            fixture->fanout();fixture->write({Fixture::note(42,0,duration*24024/100)});fixture->load();
        }
        const auto boundary=duration+100;
        while(f.clock<boundary)f.render();
        check(f.instruments[0]->events(EventType::NoteOff)==1 &&
              f.instruments[1]->events(EventType::NoteOff)==0 &&
              f.instruments[1]->events(EventType::NoteOn)==(duration<512?0u:1u),
              "fan-out swap occurs after direct release while the second off, and a short note's on, remain queued");
        if(!f.session.setExternalTempoAt(*f.store,240.,boundary*24024/100))throw std::runtime_error(f.session.error());
        while(f.clock<duration+700)f.render();
        while(reference.clock<duration+700)reference.render();
        bool exact=true,equal=true;
        for(std::size_t branch=0;branch<2;++branch) {
            const auto& actual=*f.instruments[branch];const auto& expected=*reference.instruments[branch];
            exact=exact && actual.events(EventType::NoteOn)==1 && actual.events(EventType::NoteOff)==1 && silent(actual);
            equal=equal && actual.count==expected.count;
            for(std::size_t i=0;i<actual.count;++i) {
                const auto& event=actual.heard[i];
                exact=exact && event.sample==static_cast<std::int64_t>(branch)*512+
                    (event.event.type==EventType::NoteOff?duration:0);
                if(i<expected.count)equal=equal && event.sample==expected.heard[i].sample &&
                    event.event.type==expected.heard[i].event.type && event.event.noteId==expected.heard[i].event.noteId;
            }
        }
        check(exact,"each actual fan-out consumer receives exactly one on and off at its own compensated arrival, including short notes");
        check(equal,"clock publication preserves both native fan-out event streams exactly against the no-rebuild reference");
    }
    Fixture seeking(width);seeking.fanout();seeking.write({Fixture::note(42,0,24024)});seeking.load();
    seeking.render();seeking.render();seeking.refresh();
    {auto held=seeking.session.beginRender();seeking.session.transport().locate(48000);seeking.render(&held);}
    while(seeking.clock<1000)seeking.render();
    check(seeking.instruments[1]->events(EventType::NoteOn)==0 && silent(*seeking.instruments[0]) && silent(*seeking.instruments[1]),
          "seek after adoption cancels a short note's future fan-out on while safely releasing every existing voice");

    Fixture rebased(width);rebased.fanout();rebased.write({Fixture::note(42,0,24024)});rebased.load();
    rebased.render();rebased.render();
    if(!rebased.session.setExternalTempo(*rebased.store,240.))throw std::runtime_error(rebased.session.error());
    while(rebased.clock<1000)rebased.render();
    check(rebased.instruments[1]->events(EventType::NoteOn)==0 && silent(*rebased.instruments[1]),
          "retiming an already released short note cannot retain a future on whose paired old release was discarded");

    Fixture limited(width);limited.fanout();limited.write({Fixture::note(42,0,24024)});limited.load();
    limited.render();limited.render();limited.refresh();
    auto* candidate=limited.session.graph().currentGraph();candidate->setEventCapacity(1);candidate->prepare(48000,100);
    while(limited.clock<1000)limited.render();
    check(limited.instruments[1]->events(EventType::NoteOn)==0 && silent(*limited.instruments[0]) && silent(*limited.instruments[1]),
          "a queue which retains the future on but cannot retain its paired off never resurrects the force-stopped voice");

    Fixture held(width);held.fanout();held.write({Fixture::note(42,0,240240)});held.load();held.render();
    if(!held.session.setExternalTempoAt(*held.store,240.,24024))throw std::runtime_error(held.session.error());
    while(held.clock<1200)held.render();
    bool moved=true;
    for(std::size_t branch=0;branch<2;++branch) {
        const auto& actual=*held.instruments[branch];
        moved=moved && actual.events(EventType::NoteOn)==1 && actual.events(EventType::NoteOff)==1 && silent(actual);
        for(std::size_t i=0;i<actual.count;++i)
            moved=moved && actual.heard[i].sample==static_cast<std::int64_t>(branch)*512+
                (actual.heard[i].event.type==EventType::NoteOff?550:0);
    }
    check(moved,"a still-held voice retains its queued fan-out onset when a live tempo update moves its not-yet-issued release");

    Fixture refused(width);refused.fanout();refused.store->db().exec("DELETE FROM devices WHERE id=2");
    refused.write({Fixture::note(42,0,240240)});refused.start();fillDestination(refused,3);refused.render();
    check(refused.instruments[0]->events(EventType::NoteOff)==1 && refused.instruments[1]->events(EventType::NoteOff)==0 &&
          !silent(*refused.instruments[1]),"one actual fan-out target can reject a release after the other consumes it");
    refused.refresh();refused.render();
    check(refused.instruments[1]->events(EventType::NoteOff)==1 && refused.instruments[1]->events(EventType::NoteOn)==1 &&
          silent(*refused.instruments[1]),"a partial fan-out rejection retains release ownership across publication until retry succeeds");

    Fixture stopped(width);stopped.fanout(true);
    std::vector<NoteRecord> notes;
    for(std::uint64_t id=1;id<=130;++id)notes.push_back(Fixture::note(id,static_cast<std::int64_t>(id-1)*24024,240));
    if(!stopped.store->putEventStream(1,"notes",writeStream<NoteRecord>(FourCC::Notes,notes)))throw std::runtime_error("write fan-out notes");
    stopped.load();while(stopped.clock<13100)stopped.render();
    check(stopped.instruments[0]->events(EventType::NoteOn)==130 && stopped.instruments[0]->events(EventType::NoteOff)==130 &&
          silent(*stopped.instruments[0]),"a delayed dead-end Stop branch cannot retain released ownership and exhaust the bounded voice table");
}
}
int main(){try{for(auto width:{Precision::Float32,Precision::Float64}){heldClock(width);identities(width);transportAndLegacy(width);pedalEdit(width);rejection(width);skippedControllers(width);exactBoundary(width);queuedEvents(width);delayedCurve(width);acquiredLocate(width);multiHopLocate(width);destinationIdentity(width);fanoutReleases(width);}check(allocations.load()==0 && frees.load()==0 && audio::callbackFileIo.load()==0,"every tested callback, queued handover, retry and acquired transport edit has zero allocation/free/file I/O");}catch(const std::exception& e){++failures;std::printf("EXCEPTION: %s\n",e.what());}std::printf("%d checks, %d failures\n",checks,failures);return failures?1:0;}
