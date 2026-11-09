// SPDX-License-Identifier: GPL-3.0-or-later
#include "adi/audio/io_audit.hpp"
#include "adi/controller_lanes.hpp"
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
#include <initializer_list>
#include <new>
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
    EventFlow eventFlow()const noexcept override{return EventFlow::Consume;}
    std::int64_t tailSamples()const noexcept override{return tail;}
    void process(const NodeIo& io)noexcept override{
        ++calls;
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
        StoreError e{};store=Store::createEmpty(dir.path()/"session.adi",e);if(!store)throw std::runtime_error("create");
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

// MI-01: signed ANOT origins must survive rows -> Session -> native events.
// Keep this recorder independent of the older float-only test instrument so
// both graph widths are observed at the actual consuming device boundary.
struct SignedOriginRecorder final : device::DeviceInstance {
    device::DeviceIdentity id;
    std::array<Heard, 64> heard{};
    std::size_t count = 0;
    std::int64_t clock = 0;
    Precision expected = Precision::Float32;
    bool overflow = false, wrongWidth = false, wrongFrame = false;
    unsigned calls = 0;
    const device::DeviceIdentity& identity() const noexcept override { return id; }
    bool loaded() const noexcept override { return true; }
    bool processesDouble() const noexcept override { return true; }
    EventFlow eventFlow() const noexcept override { return EventFlow::Consume; }
    std::int64_t tailSamples() const noexcept override { return kInfiniteTail; }
    void process(const NodeIo& io) noexcept override {
        ++calls;
        wrongWidth = wrongWidth || io.wide() != (expected == Precision::Float64);
        for (const auto& event : io.events) {
            wrongFrame = wrongFrame || event.frame < io.blockOffset ||
                         event.frame >= io.blockOffset + io.frames;
            if (count < heard.size()) heard[count++] = {event, clock + event.frame};
            else overflow = true;
        }
        for (int channel = 0; channel < io.channels; ++channel) {
            if (io.wide()) std::fill_n(io.out64[channel] + io.blockOffset, io.frames, 0.0);
            else std::fill_n(io.out[channel] + io.blockOffset, io.frames, 0.0f);
        }
    }
};

struct ExpectedSignedVoice {
    std::int64_t on, off;
    std::uint16_t key;
};

void signedOriginCase(Precision precision, bool irregular, const char* label,
                      const char* properties, std::initializer_list<ExpectedSignedVoice> expected,
                      int invalidBound = 0) {
    constexpr std::int64_t hundredFrames = 24024; // 120 BPM, PPQ 5765760, 48 kHz
    constexpr std::int64_t tickLimit = std::int64_t{1} << 50;
    Fixture f(48000, 256);
    f.clip(1, 0, hundredFrames, 10 * hundredFrames);
    f.store->db().exec("UPDATE clips SET pos_ticks=24024"); // clip starts at frame 100
    f.store->db().exec(properties);
    auto note = [](std::uint64_t id, std::int64_t start, std::int64_t duration,
                   std::uint8_t key) {
        NoteRecord n{};
        n.note_id = id; n.start_ticks = start; n.dur_ticks = duration;
        n.key = key; n.channel = 3; n.vel_on = 93; n.vel_off = 37;
        n.probability = 10000;
        return n;
    };
    // The two inclusive arithmetic-domain endpoints are valid but inaudible.
    // Finished negative notes and one ending exactly at zero must not cause
    // the crossing note or the later positive note to be discarded.
    const std::array<NoteRecord, 7> notes{{
        note(1, -tickLimit - (invalidBound < 0 ? 1 : 0), hundredFrames, 46),
        note(2, -3 * hundredFrames, hundredFrames, 48),
        note(3, -hundredFrames, hundredFrames, 49),
        note(4, -hundredFrames, 4 * hundredFrames, 60),
        note(5, 0, hundredFrames, 62),
        note(6, 3 * hundredFrames, hundredFrames, 64),
        note(7, tickLimit + (invalidBound > 0 ? 1 : 0), hundredFrames, 66)
    }};
    const auto bytes = writeStream<NoteRecord>(FourCC::Notes, notes);
    if (!f.store->putEventStream(1, "notes", bytes))
        throw std::runtime_error("signed-origin ANOT write");
    SignedOriginRecorder* recorder = nullptr;
    f.session.graph().setFadeFrames(0);
    SessionSpec spec;
    spec.channels = 2; spec.sampleRate = 48000; spec.maxFrames = 256;
    spec.precision = precision;
    if (!f.session.load(*f.store, [&](const DeviceRequest&, std::string&)
                        -> std::unique_ptr<device::DeviceInstance> {
            auto p = std::make_unique<SignedOriginRecorder>();
            p->expected = precision; recorder = p.get(); return p;
        }, spec)) throw std::runtime_error(f.session.error());
    if (!recorder || !f.session.midiClips())
        throw std::runtime_error("signed-origin native recorder/source missing");
    const auto beforeFailures = failures;
    check(invalidBound ? !f.session.midiClips()->problems().empty()
                       : f.session.midiClips()->problems().empty(),
          "signed note time bounds admit endpoints and refuse beyond the retained cap");
    f.session.transport().play();
    std::array<float, 512> floats{};
    std::array<double, 512> doubles{};
    float* out[] = {floats.data(), floats.data() + 256};
    double* out64[] = {doubles.data(), doubles.data() + 256};
    constexpr std::array<int, 7> partitions{{1, 73, 26, 211, 5, 128, 13}};
    std::size_t partition = 0;
    const auto beforeAllocations = allocations.load();
    const auto beforeIo = audio::callbackFileIo.load();
    while (f.clock < 1400) {
        const auto frames = irregular ? partitions[partition++ % partitions.size()] : 256;
        recorder->clock = f.clock;
        AudioIo io;
        io.out = out; io.numOut = 2; io.frames = frames; io.streamTimeSamples = f.clock;
        if (precision == Precision::Float64) io.out64 = out64;
        f.session.process(io);
        f.clock += frames;
    }
    check(allocations.load() == beforeAllocations && audio::callbackFileIo.load() == beforeIo,
          "signed-origin callbacks allocate nothing and perform no file I/O");
    check(recorder->calls > 0 && !recorder->wrongWidth && !recorder->wrongFrame && !recorder->overflow,
          "signed-origin events reach the requested native width inside their callback segment");
    check(recorder->count == 2 * expected.size(),
          "signed-origin playback emits exactly the expected on/off pairs and no earlier notes");
    std::array<bool, 64> matched{};
    bool voicesMatch = true;
    for (const auto& voice : expected) {
        const Heard* on = nullptr;
        const Heard* off = nullptr;
        for (std::size_t i = 0; i < recorder->count; ++i) {
            const auto& h = recorder->heard[i];
            if (matched[i] || h.e.dim != voice.key) continue;
            if (!on && h.e.type == EventType::NoteOn && h.clock == voice.on) {
                on = &h; matched[i] = true;
            } else if (!off && h.e.type == EventType::NoteOff && h.clock == voice.off) {
                off = &h; matched[i] = true;
            }
        }
        voicesMatch = voicesMatch && on && off;
        if (on && off) {
            voicesMatch = voicesMatch && on->e.noteId != 0 && on->e.noteId == off->e.noteId &&
                          on->e.channel == 3 && off->e.channel == 3 &&
                          on->e.value == 93.0 / 127 && off->e.value == 37.0 / 127;
        }
    }
    for (std::size_t i = 0; i < recorder->count; ++i) voicesMatch = voicesMatch && matched[i];
    check(voicesMatch, "signed-origin onsets, original ends, identity and velocities match the source oracle");
    check(f.session.midiClips()->droppedNotes() == 0,
          "signed-origin chase and loop repeats leave no refused notes");
    if (failures != beforeFailures) {
        std::printf("signed-origin case %s, %s, %s partition\n", label,
                    precision == Precision::Float64 ? "Float64" : "Float32",
                    irregular ? "irregular" : "regular");
        for (std::size_t i = 0; i < recorder->count; ++i) {
            const auto& h = recorder->heard[i];
            std::printf("  %s key %u at %lld\n", h.e.type == EventType::NoteOn ? "on" : "off",
                        static_cast<unsigned>(h.e.dim), static_cast<long long>(h.clock));
        }
    }
}

void signedOrigins() {
    for (auto precision : {Precision::Float32, Precision::Float64}) {
        for (bool irregular : {false, true}) {
            signedOriginCase(precision, irregular, "nonloop source zero",
                             "UPDATE clips SET content_offset_ticks=0",
                             {{100, 400, 60}, {100, 200, 62}, {400, 500, 64}});
            signedOriginCase(precision, irregular, "nonloop source offset equals early note end",
                             "UPDATE clips SET content_offset_ticks=24024",
                             {{100, 300, 60}, {300, 400, 64}});
            signedOriginCase(precision, irregular, "nonloop source offset equals crossing end and later onset",
                             "UPDATE clips SET content_offset_ticks=72072", {{100, 200, 64}});
            signedOriginCase(precision, irregular, "nonloop End clips negative-origin note",
                             "UPDATE clips SET content_offset_ticks=24024;"
                             "INSERT INTO clip_properties(clip_id,end_marker_ticks) VALUES(1,48048)",
                             {{100, 200, 60}});
            signedOriginCase(precision, irregular, "loop prefix once then repeats",
                             "UPDATE clips SET loop_enabled=1,loop_start_ticks=24024,loop_len_ticks=72072",
                             {{100, 400, 60}, {100, 200, 62}, {400, 500, 64},
                              {500, 700, 60}, {700, 800, 64}, {800, 1000, 60}, {1000, 1100, 64}});
            signedOriginCase(precision, irregular, "loop starts exactly at repeat origin",
                             "UPDATE clips SET loop_enabled=1,loop_start_ticks=24024,loop_len_ticks=72072,content_offset_ticks=24024",
                             {{100, 300, 60}, {300, 400, 64}, {400, 600, 60}, {600, 700, 64},
                              {700, 900, 60}, {900, 1000, 64}, {1000, 1100, 60}});
            signedOriginCase(precision, irregular, "loop starts inside repeat",
                             "UPDATE clips SET loop_enabled=1,loop_start_ticks=24024,loop_len_ticks=72072,content_offset_ticks=48048",
                             {{100, 200, 60}, {200, 300, 64}, {300, 500, 60}, {500, 600, 64},
                              {600, 800, 60}, {800, 900, 64}, {900, 1100, 60}});
            signedOriginCase(precision, irregular, "loop source End wraps exactly to origin",
                             "UPDATE clips SET loop_enabled=1,loop_start_ticks=24024,loop_len_ticks=72072,content_offset_ticks=96096",
                             {{100, 300, 60}, {300, 400, 64}, {400, 600, 60}, {600, 700, 64},
                              {700, 900, 60}, {900, 1000, 64}, {1000, 1100, 60}});
            signedOriginCase(precision, irregular, "below signed arithmetic cap",
                             "UPDATE clips SET content_offset_ticks=0", {}, -1);
            signedOriginCase(precision, irregular, "above original arithmetic cap",
                             "UPDATE clips SET content_offset_ticks=0", {}, 1);
        }
    }
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
    try { if (only == "all" || only == "signed") signedOrigins(); }
    catch (const std::exception& e) { check(false, e.what()); }
    try{if(only=="all"||only=="controllers")sessionControllers();if(only=="all"||only=="active")activeFlag();if(only=="all"||only=="markers")sourceMarkers();if(only=="all"||only=="placement")placement();if(only=="all"||only=="cleanup")cleanup();if(only=="all"||only=="musical")musical();if(only=="all"||only=="pending")pendingRelease();if(only=="all"||only=="concurrent")concurrentPublication();if(only=="all"||only=="capacity")capacity();if(only=="all"||only=="overflow")overflowRelease();if(only=="all"||only=="ns")nanosecondEnd();if(only=="all"||only=="maximum")maximumLocate();if(only=="all"||only=="entry")loopEntry();if(only=="all"||only=="held")heldNote();}catch(const std::exception& e){check(false,e.what());}
    check(allocations.load()==0,"MIDI callback allocates nothing");check(audio::callbackFileIo.load()==0,"MIDI callback performs no file I/O");
    std::printf("%s -- %d checks, %d failure(s)\n",failures?"FAIL":"PASS",checks,failures);return failures?1:0;
}
