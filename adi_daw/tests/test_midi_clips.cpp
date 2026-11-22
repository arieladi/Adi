// SPDX-License-Identifier: GPL-3.0-or-later
#include "adi/audio/io_audit.hpp"
#include "adi/controller_lanes.hpp"
#include "adi/clip_commands.hpp"
#include "adi/engine/clip_details.hpp"
#include "adi/engine/session.hpp"
#include "adi/engine/midi_input_capture.hpp"
#include "adi/store.hpp"
#include "adi/ops.hpp"
#include "adi/history.hpp"
#include "adi/media/media_ops.hpp"
#include "adi/edit_plan.hpp"
#include "adi/engine/clip_details.hpp"
#include "temp_directory.hpp"
#include <SQLiteCpp/SQLiteCpp.h>
#include <sqlite3.h>
#include <string_view>
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <initializer_list>
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
    bool audible=false;int voices=0;
    const device::DeviceIdentity& identity()const noexcept override{return id;}
    bool loaded()const noexcept override{return true;}
    bool processesDouble()const noexcept override{return true;}
    EventFlow eventFlow()const noexcept override{return EventFlow::Consume;}
    std::int64_t tailSamples()const noexcept override{return tail;}
    void process(const NodeIo& io)noexcept override{
        ++calls;
        for(const auto& e:io.events)if(count<heard.size())heard[count++]={e,clock+e.frame};
        if(audible) {
            auto event=io.events.begin();
            for(int frame=io.blockOffset;frame<io.blockOffset+io.frames;++frame) {
                while(event!=io.events.end() && event->frame<=frame) {
                    if(event->type==EventType::NoteOn)++voices;
                    if(event->type==EventType::NoteOff)--voices;
                    ++event;
                }
                for(int c=0;c<io.channels;++c) {
                    if(io.wide())io.out64[c][frame]=voices>0?0.5:0.0;
                    else io.out[c][frame]=voices>0?0.5f:0.0f;
                }
            }
            return;
        }
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
    std::unique_ptr<Store> store;Session session;Instrument* instrument=nullptr;Instrument* otherInstrument=nullptr;
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
    void load(){session.graph().setFadeFrames(0);if(!session.load(*store,[this](const DeviceRequest& request,std::string&) -> std::unique_ptr<device::DeviceInstance> {if(request.device->id==2)return std::make_unique<LatencyEffect>();auto p=std::make_unique<Instrument>();(request.device->id==3?otherInstrument:instrument)=p.get();return p;},{2,rate,block,width}))throw std::runtime_error(session.error());session.transport().play();}
    void render(int n=0, std::span<double> capture={}){
        if(!n)n=block;
        std::array<float,8192> out{};std::array<double,8192> out64{};
        float* channels[]={out.data(),out.data()+4096};double* wide[]={out64.data(),out64.data()+4096};
        AudioIo io;io.frames=n;io.numOut=2;io.streamTimeSamples=clock;
        if(width==Precision::Float64)io.out64=wide;else io.out=channels;
        if(otherInstrument)otherInstrument->clock=clock;
        instrument->clock=clock;
        { const audio::CallbackScope callback; session.process(io); }
        if(width==Precision::Float64)std::copy_n(out64.data(),capture.size(),capture.data());
        else std::copy_n(out.data(),capture.size(),capture.data());
        clock+=n;
    }
    void refresh(){if(!session.refresh(*store))throw std::runtime_error(session.error());}
};
void liveInput() {
    for (const auto width : {Precision::Float32, Precision::Float64}) {
        Fixture f(48000, 64, width);
        f.store->db().exec("UPDATE tracks SET record_armed=1,monitor_mode=2 WHERE id=1;"
            "INSERT INTO tracks(id,kind,name,index_in_parent,monitor_mode) VALUES(3,'midi','Other',2,2);"
            "INSERT INTO device_chains(id,track_id) VALUES(3,3);"
            "INSERT INTO devices(id,chain_id,ord,plugin_ref_id,name) VALUES(3,3,0,1,'Other')");
        f.load();
        f.instrument->audible = true;
        f.session.transport().play(false);
        f.session.transport().locate(24000);
        auto send = [&](std::int64_t sample, EventType type, std::uint64_t id, std::uint16_t dim, double value) {
            return f.session.enqueueMidiInput(sample, Event{0,type,3,dim,id,0,value});
        };
        check(send(31, EventType::NoteOn, 42, 60, 0.72) &&
              send(63, EventType::NoteExpression, 42, static_cast<std::uint16_t>(ExpressionDim::Pitch), 0.314159) &&
              send(64, EventType::NoteOff, 42, 60, 0.43), "Track input accepts typed, timestamped notes");
        std::array<double,64> pcm{};
        f.render(64, pcm);
        auto& i = *f.instrument;
        check(i.count == 2 && i.heard[0].clock == 31 && i.heard[1].clock == 63 &&
              i.heard[1].e.value == 0.314159 && i.heard[0].e.noteId == i.heard[1].e.noteId &&
              i.heard[0].e.channel == 3, "live note/expression use physical frames and one voice identity while stopped");
        check(pcm[30] == 0 && pcm[31] > 0.2 && pcm[63] > 0.2,
              "a live instrument reaches the main output at its exact onset");
        f.render(17, std::span(pcm).first(17));
        check(i.count == 3 && i.heard[2].clock == 64 && i.heard[2].e.noteId == i.heard[0].e.noteId &&
              i.heard[2].e.value == 0.43 && i.balance() == 0 && f.otherInstrument->count == 0 && pcm[0] == 0,
              "boundary note-off lands in the next short block on the armed instrument only");
        check(send(80, EventType::NoteOn, 43, 62, 0.8), "late event enters the queue");
        f.render();
        check(i.count == 4 && i.heard[3].clock == 81 && i.balance() == 1,
              "late input is played once at the earliest available sample");
        OpRequest arm;
        arm.opType = "track.setArm";
        arm.payload = {{"id",1},{"armed",false}};
        if (!OpJournal(*f.store).commit(arm).ok) throw std::runtime_error("track disarm failed");
        arm.payload = {{"id",3},{"armed",true}};
        if (!OpJournal(*f.store).commit(arm).ok) throw std::runtime_error("track arm failed");
        f.refresh();
        check(send(150, EventType::NoteOff, 43, 62, 0.1) &&
              send(151, EventType::NoteOn, 44, 64, 0.82) &&
              send(152, EventType::NoteOff, 44, 64, 0), "old release and new key both reach the changed graph");
        f.render();
        check(i.count == 5 && i.heard[4].clock == 145 && i.heard[4].e.noteId == i.heard[3].e.noteId &&
              i.balance() == 0, "disarming releases a held voice even after a graph publication");
        check(f.otherInstrument->count == 2 && f.otherInstrument->heard[0].clock == 151 &&
              f.otherInstrument->heard[1].clock == 152 && f.otherInstrument->balance() == 0,
              "arming the other instrument routes new keys without old note-offs");
        const auto beforeReset = f.clock;
        check(send(beforeReset, EventType::NoteOn, 45, 65, 0.8) &&
              f.session.enqueueMidiInput(beforeReset + 1, {0,EventType::Controller,3,0,0,64,1}),
              "new target accepts a held note and sustain");
        f.render();
        const auto resetAt = f.clock;
        check(send(resetAt + 600, EventType::NoteOn, 46, 66, 0.8), "queue a future event from old input");
        f.session.resetMidiInput();
        check(send(resetAt + 4, EventType::NoteOn, 47, 67, 0.8) &&
              send(resetAt + 7, EventType::NoteOff, 47, 67, 0.1),
              "rebound producer can submit beneath the old timestamps");
        f.render();
        auto& other = *f.otherInstrument;
        check(other.count == 8 && other.heard[4].clock == resetAt &&
              other.heard[4].e.noteId == other.heard[2].e.noteId &&
              other.heard[5].clock == resetAt && other.heard[5].e.type == EventType::Controller &&
              other.heard[5].e.paramId == 64 && other.heard[5].e.value == 0 &&
              other.heard[6].clock == resetAt + 4 && other.heard[7].clock == resetAt + 7 && other.balance() == 0,
              "port reset releases notes and sustain and discards obsolete future input");
        check(send(f.clock, EventType::NoteOn, 48, 68, 0.8), "sound another held voice before overflow");
        f.render();
        bool overflow = false;
        for (int queued = 0; queued < 2048; ++queued) {
            if (f.session.enqueueMidiInput(f.clock + 900, {0,EventType::Controller,3,0,0,7,1})) continue;
            overflow = true;
            break;
        }
        f.render();
        check(overflow && other.count == 10 && other.balance() == 0 &&
              other.heard[8].e.noteId == other.heard[9].e.noteId,
              "a full queue refuses and releases held notes safely even if the lost packet was its release");
        check(!send(f.clock, EventType::NoteOn, 0, 68, 0.5) &&
              !send(f.clock, EventType::ParamValue, 20, 68, 0.5),
              "one MIDI input cannot forge addressed parameter events or anonymous notes");
        const auto topChannel = f.clock;
        check(f.session.enqueueMidiInput(topChannel, {0,EventType::NoteOn,15,70,49,0,0.75}) &&
              f.session.enqueueMidiInput(topChannel + 1, {0,EventType::NoteExpression,15,
                  static_cast<std::uint16_t>(ExpressionDim::Pitch),0,0,0.25}) &&
              f.session.enqueueMidiInput(topChannel + 2, {0,EventType::NoteOff,15,70,49,0,0}),
              "the sixteenth channel accepts notes and a channel-wide expression");
        f.render();
        check(other.count == 13 && other.heard[10].e.channel == 15 && other.heard[11].clock == topChannel + 1 &&
              other.heard[11].e.noteId == 0 && other.heard[11].e.value == 0.25 &&
              other.heard[12].e.noteId == other.heard[10].e.noteId && other.balance() == 0,
              "channel-wide expression reaches the instrument with its unaddressed identity");
    }
    Fixture overlap;
    overlap.store->db().exec("UPDATE tracks SET record_armed=1,monitor_mode=2 WHERE id=1");
    overlap.clip();
    overlap.load();
    check(overlap.session.enqueueMidiInput(100, {0,EventType::NoteOn,3,60,42,0,0.5}) &&
          overlap.session.enqueueMidiInput(110, {0,EventType::NoteOff,3,60,42,0,0}),
          "enqueue a live voice on the Arrangement note's channel and key");
    overlap.render();
    const auto& inst = *overlap.instrument;
    check(inst.count == 4 && inst.heard[0].clock == 100 && inst.heard[1].clock == 100 &&
          inst.heard[0].e.noteId != inst.heard[1].e.noteId && inst.heard[3].clock == 110 &&
          inst.heard[3].e.noteId == inst.heard[0].e.noteId && inst.balance() == 1,
          "releasing the live same-pitch note leaves the Arrangement voice active");
}
void driverMidiCapture() {
    Fixture f(48000, 64);
    f.store->db().exec("UPDATE tracks SET record_armed=1 WHERE id=1;"
        "INSERT INTO tracks(id,kind,name,index_in_parent,record_armed)"
        " VALUES(3,'midi','Armed without instrument',2,1),(4,'midi','Unarmed',3,0)");
    f.load();
    auto& capture = f.session.midiInputCapture();
    auto& transport = f.session.transport();
    using Kind = MidiCapturePacket::Kind;
    const std::array<std::int64_t,2> tracks{3,1};
    std::string error;
    const auto start = [&] { return f.session.startMidiInputCapture(tracks, error); };
    const auto send = [&](std::int64_t sample, EventType type, std::uint64_t id, std::uint16_t dim = 60) {
        return f.session.enqueueMidiInput(sample, {0,type,3,dim,id,0,0.5});
    };
    check(!f.session.startMidiInputCapture(std::array<std::int64_t,1>{4}, error) &&
          !f.session.startMidiInputCapture(std::array<std::int64_t,2>{3,3}, error),
          "capture rejects an unarmed or duplicate destination");
    transport.play(false);
    transport.loop(6,42,true);
    transport.locate(36);
    check(start() && capture.epoch() != 0 && capture.tracks().size() == 2 &&
          capture.tracks()[0] == 3 && capture.tracks()[1] == 1 && capture.sampleRate() == 48000,
          "capture freezes its epoch, rate and destination list including a track with no instrument");
    const auto epoch = capture.epoch();
    std::array<MidiCapturePacket,16> packets{};
    f.render(16);
    check(capture.state() == MidiCaptureState::Waiting && capture.drain(packets) == 0,
          "stopped rendering does not start a recording span");
    transport.play();
    check(send(15, EventType::NoteOn, 500) && send(18, EventType::NoteOn, 501) &&
          send(22, EventType::NoteExpression, 501, static_cast<std::uint16_t>(ExpressionDim::Pitch)) &&
          send(59, EventType::NoteOff, 501), "queued input crosses two loop boundaries and includes a pre-epoch key");
    f.render();
    check(capture.state() == MidiCaptureState::Recording, "the first playable slice starts the epoch");
    check(send(79, EventType::NoteOn, 502) && send(80, EventType::NoteOff, 502),
          "a late packet and an on-time packet share the next driver's first frame");
    f.render(16);
    capture.stop();
    f.render();
    check(capture.state() == MidiCaptureState::Complete && !start(),
          "stop leaves the completed epoch available until the consumer drains it");
    const auto count = capture.drain(packets);
    const auto span = [&](std::size_t i, std::uint64_t pass, std::int64_t stream,
                          std::int64_t timeline, std::int32_t frames) {
        const auto& p = packets[i];
        return p.kind == Kind::Span && p.epoch == epoch && p.pass == pass &&
               p.streamSample == stream && p.timelineSample == timeline && p.frames == frames;
    };
    const auto event = [&](std::size_t i, std::uint64_t pass, std::int64_t stream,
                           std::int64_t timeline, std::int64_t input, std::int32_t frame,
                           EventType type, std::uint64_t id) {
        const auto& p = packets[i];
        return p.kind == Kind::Event && p.epoch == epoch && p.pass == pass &&
               p.streamSample == stream && p.timelineSample == timeline && p.inputSample == input &&
               p.event.type == type && p.event.frame == frame && p.event.noteId == id && p.event.channel == 3;
    };
    check(count == 10 && span(0,0,16,36,6) && span(2,1,22,6,36) && span(4,2,58,6,22) &&
          span(6,2,80,28,14) && span(9,3,94,6,2),
          "capture emits exact loop spans including the silent final pass and a pass split across callbacks");
    check(event(1,0,18,38,18,2,EventType::NoteOn,501) &&
          event(3,1,22,6,22,0,EventType::NoteExpression,501) &&
          event(5,2,59,7,59,1,EventType::NoteOff,501),
          "input IDs carry held-note ownership across multiple wraps independently of the audible voice");
    check(event(7,2,80,28,79,0,EventType::NoteOn,502) &&
          event(8,2,80,28,80,0,EventType::NoteOff,502) &&
          f.instrument->heard[0].e.noteId != 500,
          "capture excludes the pre-epoch key and preserves the order and raw stamp of late input");
    transport.locate(12);
    check(start() && capture.epoch() == epoch + 1, "draining permits a new independent recording epoch");
    f.render(12);
    f.session.resetMidiInput();
    capture.stop();
    f.render(12);
    check(capture.state() == MidiCaptureState::InputReset && capture.drain(packets) == 1,
          "losing the source terminates the epoch without a partial next slice");
    check(start(), "start a capture for the seek test");
    f.render(8);
    transport.locate(17);
    f.render(8);
    check(capture.state() == MidiCaptureState::Discontinuity && capture.drain(packets) > 0,
          "a seek finishes the epoch with an explicit discontinuity");
    check(start(), "start a capture for the physical stream gap");
    f.render(8);
    ++f.clock;
    f.render(8);
    check(capture.state() == MidiCaptureState::Discontinuity && capture.drain(packets) > 0,
          "a missing driver frame cannot be joined onto the recording");
    check(start(), "start and wait through count-in");
    transport.locate(10);
    transport.countIn(5);
    const auto beforeLead = f.clock;
    check(send(beforeLead+4,EventType::NoteOn,503) && send(beforeLead+5,EventType::NoteOn,504),
          "count-in and first downbeat receive separate input stamps");
    f.render(8);
    check(capture.drain(packets) == 2 && packets[0].streamSample == beforeLead+5 &&
          packets[0].timelineSample == 10 && packets[0].frames == 3 &&
          packets[1].event.noteId == 504 && packets[1].event.frame == 0,
          "count-in emits no packets and the first downbeat records the event at its exact sample");
    capture.stop();
    f.render(2);
    check(capture.state() == MidiCaptureState::Complete, "stop the count-in epoch");
    transport.loop(0,1,true);
    transport.locate(0);
    check(start(), "start shortest-loop overflow test");
    for (int block = 0; block < 63; ++block) f.render();
    f.render(63);
    f.render(2);
    std::vector<MidiCapturePacket> ring(4096);
    check(capture.state() == MidiCaptureState::Overflow && capture.drain(ring) == 4095 &&
          ring[4094].pass == 4094, "overflow publishes no partial slice and preserves the complete prefix");
    check(start(), "restart after draining the failed capture");
    f.session.release();
    check(capture.state() == MidiCaptureState::DeviceStopped && capture.drain(packets) == 0,
          "closing the audio device terminates even a capture waiting for its first slice");
}
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
void takeLanePlayback() {
    Fixture f;
    f.clip(1, 24024);
    f.clip(2, 12012);
    f.store->db().exec("INSERT INTO lanes(id,track_id,name) VALUES(7,1,'Lane');"
                       "UPDATE clips SET lane_id=7 WHERE id=2");
    f.load();
    f.render();
    const auto& instrument = *f.instrument;
    check(f.session.model().clips.size() == 2 && instrument.balance() == 1 &&
          instrument.count == 2 && instrument.heard[0].clock == 100,
          "main lane sends its note and expression while an overlapping take stays silent");
    f.store->db().exec("DELETE FROM clips WHERE id=1");
    f.refresh();
    f.render();
    check(f.session.model().clips.size() == 1 && f.instrument->balance() == 0,
          "refresh releases the main note without activating a stored take");
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
void trimSourcePrefix(){
    Fixture f(48000,4096);f.clip(1,0,5765760,5765760);
    f.store->db().exec("UPDATE clips SET pos_ticks=11531520");
    const std::array<std::int64_t,1> tracks{1};
    edit_plan::Options options;options.policy.automationFollowsClips=false;
    const auto prepared=edit_plan::prepareTrim(*f.store,
        edit_plan::materialScope(tracks,edit_plan::PlacementClock::Ticks,{0,4*5765760}),
        {1,{0,5765760}},options);
    check(prepared.ok && clip_edit::commitPrepared(*f.store,prepared).ok,
          "MIDI left extension commits a signed source prefix through the material planner");
    const auto details=readClipDetails(*f.store,1);
    check(details.clip && details.clip->contentOffsetTicks==-5765760 &&
              details.clip->endMarkerTicks==5765760 && details.clip->propertiesProblem.empty(),
          "Clip View derives the original End from a signed Start and extended visible span");
    f.load();while(f.clock<76000)f.render();
    bool on=false,off=false;
    for(std::size_t i=0;i<f.instrument->count;++i) {
        const auto& heard=f.instrument->heard[i];
        if(heard.e.type==EventType::NoteOn)on=on || heard.clock==48000;
        if(heard.e.type==EventType::NoteOff)off=off || heard.clock==72000;
    }
    check(on && off && f.instrument->balance()==0,
          "MIDI signed source prefix preserves anchored note-on/off and releases at the original end");
    OpRequest marker;marker.opType="clip.setStartMarker";marker.payload={{"id",1},{"ticks",0}};
    check(OpJournal(*f.store).commit(marker).ok &&
              f.store->db().execAndGet("SELECT end_marker_ticks FROM clip_properties WHERE clip_id=1").getInt64()==5765760,
          "moving Start after a signed trim pins the original derived End");
    check(History(*f.store).undo().ok &&
              f.store->db().execAndGet("SELECT content_offset_ticks FROM clips WHERE id=1").getInt64()==-5765760,
          "marker undo restores the signed trimmed source prefix");
    for(const int begin:{0,5}) {
        Fixture tie(44100,2048);tie.clip(1,0,5765760,5765760);
        SQLite::Statement row(tie.store->db(),"UPDATE clips SET time_base=1,pos_ticks=NULL,length_ticks=NULL,pos_ns=?,length_ns=?");
        row.bind(1,begin);row.bind(2,35000000-begin);row.exec();tie.load();tie.render();
        bool end=false;
        for(std::size_t i=0;i<tie.instrument->count;++i)
            if(tie.instrument->heard[i].e.type==EventType::NoteOff)end=end || tie.instrument->heard[i].clock==1544;
        check(end && tie.instrument->balance()==0,"linear MIDI retains the native end sample when its left edge changes");
    }
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
bool sustainedExpressionBlock(Precision width, int capacity = 0) {
    constexpr int block = 4096;
    constexpr int end = 2 * block;
    constexpr int grid = 96; // 500 Hz at 48 kHz
    constexpr int begin = grid - 1; // chase, then a full set of updates in block one
    constexpr int firstGrid = 1;
    constexpr auto ticksPer100Samples = 24024;
    constexpr auto startTicks = static_cast<std::int64_t>(begin) * ticksPer100Samples / 100;
    constexpr auto duration = static_cast<std::int64_t>(end) * ticksPer100Samples / 100;
    constexpr std::size_t voices = 16;
    constexpr std::array<std::array<float, 2>, 5> values{{
        {0, 8}, {0, 1}, {0, 1}, {-12, 0}, {-.5f, .5f},
    }};
    constexpr std::size_t updates = 2 + (end - 1 - firstGrid) / grid;
    constexpr std::size_t expected = updates * voices * values.size();

    Fixture f(48000, block, width);
    f.clip();
    std::array<NoteRecord, voices> notes{};
    for (std::size_t voice = 0; voice < notes.size(); ++voice) {
        auto& note = notes[voice];
        note.note_id = voice + 1;
        note.start_ticks = startTicks;
        note.dur_ticks = duration;
        note.key = static_cast<std::uint8_t>(48 + voice);
        note.channel = 3;
        note.vel_on = note.vel_off = 64;
        note.probability = 10000;
        for (std::size_t dimension = 0; dimension < values.size(); ++dimension) {
            std::array<ExpressionPoint, 2> points{};
            points[0].value = values[dimension][0];
            points[0].curve = static_cast<std::uint8_t>(Curve::Linear);
            points[1].time_ticks = duration;
            points[1].value = values[dimension][1];
            const auto blob = writeStream<ExpressionPoint>(FourCC::Expression, points);
            SQLite::Statement row(f.store->db(),
                "INSERT INTO note_expression(clip_id,note_id,dimension,data) VALUES(1,?,?,?)");
            row.bind(1, static_cast<std::int64_t>(note.note_id));
            row.bind(2, static_cast<int>(dimension));
            row.bind(3, blob.data(), static_cast<int>(blob.size()));
            row.exec();
        }
    }
    if (!f.store->putEventStream(1, "notes", writeStream<NoteRecord>(FourCC::Notes, notes)))
        throw std::runtime_error("write sustained expression's notes");

    f.load();
    if (capacity > 0) {
        auto* graph = f.session.graph().currentGraph();
        graph->setEventCapacity(capacity);
        graph->prepare(48000, block);
    }
    f.session.transport().locate(begin);
    f.render();
    f.render();
    f.render(1);

    std::array<std::uint64_t, voices> liveIds{};
    std::array<bool, voices> released{};
    std::array<bool, expected> seen{};
    std::size_t received = 0;
    bool exact = true;
    const auto& instrument = *f.instrument;
    for (std::size_t i = 0; i < instrument.count; ++i) {
        const auto& [event, clock] = instrument.heard[i];
        if (event.type == EventType::NoteOn || event.type == EventType::NoteOff) {
            if (event.dim < 48 || event.dim >= 48 + voices) {
                exact = false;
                continue;
            }
            const auto voice = static_cast<std::size_t>(event.dim - 48);
            if (event.type == EventType::NoteOn) {
                exact = exact && clock == 0 && event.noteId != 0 && liveIds[voice] == 0;
                liveIds[voice] = event.noteId;
            } else {
                exact = exact && clock == end && event.noteId == liveIds[voice] && !released[voice];
                released[voice] = true;
            }
            continue;
        }
        if (event.type != EventType::NoteExpression) continue;
        const auto voice = std::find(liveIds.begin(), liveIds.end(), event.noteId);
        if (event.noteId == 0 || voice == liveIds.end() ||
            (event.dim >= values.size() || clock < 0 || clock >= end) ||
            (clock > 0 && (clock - firstGrid) % grid != 0)) {
            exact = false;
            continue;
        }
        const auto voiceIndex = static_cast<std::size_t>(voice - liveIds.begin());
        const auto update = clock == 0 ? 0 : 1 + (clock - firstGrid) / grid;
        const auto index = (static_cast<std::size_t>(update) * voices + voiceIndex) *
            values.size() + event.dim;
        const auto ticks = clock > 0 ? (static_cast<double>(clock + begin) * ticksPer100Samples / 100.0
                                       - startTicks) : 0.0;
        const auto fraction = ticks / duration;
        const auto& endpoints = values[event.dim];
        const auto value = endpoints[0] + (endpoints[1] - endpoints[0]) * fraction;
        exact = exact && !seen[index] && event.paramId == kNativeNoteExpressionUnits &&
            std::abs(event.value - value) < 1e-7;
        seen[index] = true;
        ++received;
    }
    if (capacity > 0 || received != expected)
        std::printf("stored expression grid %zu/%zu updates delivered\n", received, expected);
    check(std::all_of(released.begin(), released.end(), [](bool done) { return done; }) &&
              instrument.balance() == 0,
          "16 sustained voices receive all note-offs at the block boundary");
    return exact && received == expected;
}

void sessionExpressionCapacity() {
    for (auto width : {Precision::Float32, Precision::Float64})
        check(sustainedExpressionBlock(width),
              "all five saved curves reach all 16 voices on each 500 Hz update");
    check(!sustainedExpressionBlock(Precision::Float32, 2144),
          "the former three-dimension event capacity loses the stored expression grid");
    check(!sustainedExpressionBlock(Precision::Float32, 3536),
          "a five-dimension grid without room for chased values still loses expression");
}

void coherentSessionExpression() {
    for (const bool refreshing : {false, true}) for (const bool borrow : {false, true}) {
        Fixture f;
        f.clip();
        if (refreshing) f.load();
        ExpressionPoint initial{};
        initial.value = 0.25f;
        const auto initialCurve = writeStream<ExpressionPoint>(FourCC::Expression, std::vector{initial});
        {
            SQLite::Statement create(f.store->db(),
                "INSERT INTO note_expression(clip_id,note_id,dimension,data) VALUES(1,42,2,?)");
            create.bind(1, initialCurve.data(), static_cast<int>(initialCurve.size()));
            create.exec();
        }
        auto updated = initial;
        updated.value = 0.75f;
        const auto changedCurve = writeStream<ExpressionPoint>(FourCC::Expression, std::vector{updated});
        NoteRecord note{};
        note.start_ticks = 48048;
        note.dur_ticks = 480480;
        note.note_id = 42;
        note.key = 60;
        note.channel = 3;
        note.vel_on = 93;
        note.vel_off = 37;
        note.probability = 10000;
        note.tuning_cents = 31.25f;
        const auto changedNotes = writeStream<NoteRecord>(FourCC::Notes, std::vector{note});
        SQLite::Database writer(media::pathUtf8(f.store->path()), SQLite::OPEN_READWRITE);
        struct Race {
            SQLite::Database& writer;
            const std::vector<std::byte>& notes;
            const std::vector<std::byte>& curve;
            bool fired = false;
            bool committed = false;
        } race{writer, changedNotes, changedCurve};
        auto* reader = f.store->db().getHandle();
        std::optional<SQLite::Transaction> outer;
        if (borrow) outer.emplace(f.store->db(), SQLite::TransactionBehavior::DEFERRED);
        sqlite3_trace_v2(reader, SQLITE_TRACE_STMT, [](unsigned, void* context, void* statement, void*) {
            auto& state = *static_cast<Race*>(context);
            const auto* sql = sqlite3_sql(static_cast<sqlite3_stmt*>(statement));
            if (state.fired || !sql ||
                !std::string_view(sql).starts_with("SELECT id,clip_id,note_id,dimension,data FROM note_expression"))
                return 0;
            state.fired = true;
            try {
                SQLite::Transaction edit(state.writer);
                SQLite::Statement notes(state.writer,
                    "UPDATE event_streams SET data=? WHERE clip_id=1 AND stream_kind='notes'");
                notes.bind(1, state.notes.data(), static_cast<int>(state.notes.size()));
                SQLite::Statement expression(state.writer,
                    "UPDATE note_expression SET data=? WHERE clip_id=1 AND note_id=42 AND dimension=2");
                expression.bind(1, state.curve.data(), static_cast<int>(state.curve.size()));
                const auto noteCount = notes.exec();
                const auto expressionCount = expression.exec();
                edit.commit();
                state.committed = noteCount == 1 && expressionCount == 1;
            } catch (const std::exception& error) {
                std::printf("coherence writer: %s\n", error.what());
            }
            return 0;
        }, &race);
        if (refreshing) f.refresh();
        else f.load();
        sqlite3_trace_v2(reader, 0, nullptr, nullptr);
        f.render();
        const auto timbreAt = [&](std::int64_t sample) {
            for (std::size_t i = 0; i < f.instrument->count; ++i) {
                const auto& received = f.instrument->heard[i];
                if (received.clock == sample && received.e.type == EventType::NoteExpression &&
                    received.e.dim == static_cast<std::uint16_t>(ExpressionDim::Timbre))
                    return received.e.value;
            }
            return -1.0;
        };
        std::printf("coherence %s%s: writer committed=%d, timbre at sample 100 %.2f, at 200 %.2f\n",
                    refreshing ? "refresh" : "load", borrow ? " borrowed" : "",
                    race.committed ? 1 : 0, timbreAt(100), timbreAt(200));
        check(race.fired && race.committed, "a second connection committed both streams inside the read");
        check(timbreAt(100) == 0.25 && timbreAt(200) < 0,
              refreshing ? "refresh hears old ANOT and AEXP from their shared read"
                         : "load hears old ANOT and AEXP from their shared read");
        check((sqlite3_get_autocommit(reader) == 0) == borrow,
              "only the caller owns the surrounding read transaction");
        if (outer) outer->commit();
        f.refresh();
        f.session.transport().locate(0);
        f.clock = 0;
        f.instrument->count = 0;
        f.render();
        check(timbreAt(200) == 0.75 && timbreAt(100) < 0,
              "a subsequent refresh hears both streams from the new revision");
    }
}

void sessionExpression() {
    for(const auto width:{Precision::Float32,Precision::Float64})for(int block:{64,257}) {
        Fixture f(48000,block,width);
        f.clip();
        auto& db=f.store->db();
        const auto put=[&](int dimension,const std::vector<ExpressionPoint>& points) {
            const auto bytes=writeStream<ExpressionPoint>(FourCC::Expression,points);
            SQLite::Statement row(db,"INSERT INTO note_expression(clip_id,note_id,dimension,data) VALUES(1,42,?,?)");
            row.bind(1,dimension);row.bind(2,bytes.data(),static_cast<int>(bytes.size()));row.exec();
        };
        ExpressionPoint a{},b{};
        a.curve=1;b.time_ticks=24024;b.value=12.0f;
        put(0,{a,b});
        a.value=-12.0f;put(3,{a});
        a.value=-0.5f;put(4,{a});
        if(width==Precision::Float32 && block==64) {
            Transport transport;
            transport.play();
            MidiClips midi(rows::readModel(*f.store),transport,48000,makeMidiClipState(),{},readNoteExpressions(*f.store));
            const auto source=midi.sourcesFor(1).at(0);
            std::array<Event,3> shortBuffer{};
            EventList crowded(shortBuffer.data(),3);
            source->sourceEvents(crowded,256);
            check(crowded.size()==2 && shortBuffer[0].type==EventType::NoteOn,
                  "expression preserves space for the owned note release under pressure");
            transport.advance(256);
            std::array<Event,8> availableBuffer{};
            EventList available(availableBuffer.data(),8);
            source->sourceEvents(available,256);
            bool recoveredGain=false;
            for(std::int32_t i=0;i<available.size();++i) {
                const auto& event=availableBuffer[static_cast<std::size_t>(i)];
                if(event.type==EventType::NoteExpression && event.dim==3 && event.frame==0 &&
                   event.noteId==shortBuffer[0].noteId && event.value==-12)recoveredGain=true;
            }
            check(recoveredGain,"the next block delivers the latest expression after a full event buffer");
        }
        f.load();
        while(f.clock<400)f.render();
        const auto& heard=f.instrument->heard;
        const auto count=f.instrument->count;
        const auto pitchOnsets=std::count_if(heard.data(),heard.data()+count,[](const auto& event) {
            return event.e.type==EventType::NoteExpression && event.e.dim==0 && event.clock==100;
        });
        const auto find=[&](std::int64_t sample,ExpressionDim dimension,double value) {
            for(std::size_t i=0;i<count;++i) {
                const auto& event=heard[i];
                if(event.e.type==EventType::NoteExpression && event.e.dim==static_cast<std::uint16_t>(dimension) &&
                   event.clock==sample && std::abs(event.e.value-value)<1e-7 &&
                   event.e.noteId==heard[0].e.noteId && event.e.paramId==kNativeNoteExpressionUnits)return true;
            }
            return false;
        };
        check(count>=6 && heard[0].e.type==EventType::NoteOn && heard[0].clock==100 && pitchOnsets==1 &&
              find(100,ExpressionDim::Pitch,0.3125) && find(100,ExpressionDim::Gain,-12) && find(100,ExpressionDim::Pan,-.5),
              "saved AEXP reaches the instrument after Note On with native gain and pan and combined tuning");
        check(find(192,ExpressionDim::Pitch,11.3525) && find(200,ExpressionDim::Pitch,12.3125),
              "the 500 Hz grid and off-grid curve knot reach the same note at exact samples");
        f.session.transport().locate(250);
        const auto before=f.instrument->count;
        const auto at=f.clock;
        f.render();
        bool chased=false;
        std::uint64_t voice=0;
        for(std::size_t i=before;i<f.instrument->count;++i) {
            const auto& event=heard[i];
            if(event.e.type==EventType::NoteOn)voice=event.e.noteId;
            if(event.e.type==EventType::NoteExpression && event.e.dim==0 && event.e.noteId==voice &&
               event.e.noteId!=heard[0].e.noteId && event.clock==at && event.e.value==12.3125)chased=true;
        }
        check(chased,"Locate restores the held expression on the new chased note ID");
        f.refresh();
        const auto previousCount=f.instrument->count;
        f.render();
        const bool retained=std::none_of(heard.data()+previousCount,heard.data()+f.instrument->count,[](const auto& event) {
            return event.e.type==EventType::NoteOn || event.e.type==EventType::NoteOff;
        });
        check(retained && f.instrument->balance()==1,"unchanged expression keeps the sounding note across a graph refresh");

        db.exec("UPDATE clips SET loop_enabled=1,loop_start_ticks=36036,loop_len_ticks=48048 WHERE id=1");
        f.refresh();
        f.session.transport().locate(0);
        f.clock=0;
        f.instrument->count=0;
        while(f.clock<460)f.render();
        std::uint64_t repeatedVoice=0;
        bool repeated=false;
        for(std::size_t i=0;i<f.instrument->count;++i) {
            const auto& event=heard[i];
            if(event.clock!=350)continue;
            if(event.e.type==EventType::NoteOn) repeatedVoice=event.e.noteId;
            if(event.e.type==EventType::NoteExpression && event.e.dim==0 && repeatedVoice &&
               event.e.noteId==repeatedVoice && event.e.value==6.3125)repeated=true;
        }
        check(repeated,"a clip loop chases a spanning note's expression from the cycle's source offset");
    }
}
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

const ClipNode& projected(const Snapshot& frame, std::int64_t track, std::int64_t clip) {
    const auto* node = frame.findTrack(track);
    if (node) {
        for (const auto& candidate : node->clips) {
            if (candidate->id == clip) return *candidate;
        }
    }
    throw std::runtime_error("missing projected split clip");
}
void storedSplitClock() {
    {
        Fixture f(48000,64);
        f.clip(1,1622,1000);
        f.store->db().exec("DELETE FROM tempo_map;"
            "INSERT INTO tempo_map(pos_ticks,bpm) VALUES(0,120),(1201,60);"
            "INSERT INTO time_signature_map(pos_ticks,numerator,denominator) VALUES(0,4,4),(1201,7,8);"
            "UPDATE clips SET time_base=1,pos_ticks=NULL,length_ticks=NULL,pos_ns=0,length_ns=2000000;");
        const auto cut=clip_edit::prepareSplit(*f.store,1,2,{0,1201},48000);
        if (!cut.ok) throw std::runtime_error(cut.error);
        const auto committed=clip_edit::commitPrepared(*f.store,cut);
        if (!committed.ok) throw std::runtime_error(committed.error);
        const auto snapshot=SnapshotBuilder::fromStore(*f.store);
        const auto& right=projected(*snapshot,1,2);
        check(right.posNs==104149 && right.posTicks==1201, "native MIDI split rounds its nanosecond origin down");
        const auto details=readClipDetails(*f.store,2);
        check(details.clip && details.clip->startMarkerTicks==right.posTicks &&
              details.clip->signature.numerator==7 && details.clip->signature.denominator==8,
              "Clip Details uses the split origin for inherited meter");
        f.load();
        f.render();
        auto firstAttack=std::find_if(f.instrument->heard.begin(),f.instrument->heard.begin()+f.instrument->count,
                                     [](const auto& event){return event.e.type==EventType::NoteOn;});
        if (firstAttack==f.instrument->heard.begin()+f.instrument->count)
            throw std::runtime_error("missing origin fixture note");
        check(firstAttack->clock==9 && f.instrument->balance()==0,
              "audible split MIDI attack follows the recovered tick across the tempo step");
    }
    {
        Fixture f(48000,64);
        f.clip(1,1321,20);
        f.store->db().exec("DELETE FROM tempo_map;"
            "INSERT INTO tempo_map(pos_ticks,bpm) VALUES(0,120);"
            "UPDATE clips SET time_base=1,pos_ticks=NULL,length_ticks=NULL,pos_ns=0,length_ns=114642;");
        const auto cut=clip_edit::prepareSplit(*f.store,1,2,{0,1321},48000);
        if (!cut.ok) throw std::runtime_error(cut.error);
        const auto committed=clip_edit::commitPrepared(*f.store,cut);
        if (!committed.ok) throw std::runtime_error(committed.error);
        const auto snapshot=SnapshotBuilder::fromStore(*f.store);
        const auto& right=projected(*snapshot,1,2);
        check(right.posNs==114556 && right.lengthNs==86 && right.lengthTicks==1,
              "native MIDI split has exactly one projected tick left");
        f.load();
        f.render();
        bool attack=false,release=false;
        for(std::size_t i=0;i<f.instrument->count;++i) {
            const auto& event=f.instrument->heard[i];
            if(event.e.type==EventType::NoteOn)attack=event.clock==5;
            else if(event.e.type==EventType::NoteOff)release=event.clock==6;
        }
        check(attack && release && f.instrument->balance()==0,
              "one-tick linear MIDI tail is heard across its absolute sample boundary");
    }
    {
        Fixture f;
        f.store->db().exec("DELETE FROM tempo_map;"
            "INSERT INTO tempo_map(pos_ticks,bpm) VALUES(0,120),(1201,60);"
            "INSERT INTO tracks(id,kind,name) VALUES(2,'audio','Faded');"
            "INSERT INTO media_files(id,hash_blake3,rel_path,format) VALUES(1,'test','audio.wav','wav');"
            "INSERT INTO clips(id,track_id,kind,time_base,pos_ns,length_ns,fade_in_ticks) VALUES(10,2,'audio',1,0,2000000,500);"
            "INSERT INTO audio_clips(clip_id,media_id,src_start_frames,src_len_frames) VALUES(10,1,0,2000);");
        const auto cut=clip_edit::prepareSplit(*f.store,10,11,{0,1201},48000);
        if (!cut.ok) throw std::runtime_error(cut.error);
        const auto committed=clip_edit::commitPrepared(*f.store,cut);
        if (!committed.ok) throw std::runtime_error(committed.error);
        const auto snapshot=SnapshotBuilder::fromStore(*f.store);
        const auto& left=projected(*snapshot,2,10);
        check(left.lengthNs==104149 && left.lengthTicks==1201 && left.fadeInTicks==500,
              "native faded audio split keeps the original authored fade on a recovered span");
        const auto details=readClipDetails(*f.store,10);
        check(details.clip && details.clip->endMarkerTicks==left.lengthTicks,
              "Clip Details End matches the faded split audio span");
        OpRequest fade;
        fade.opType="clip.setFades";
        fade.payload={{"id",10},{"in",{{"ticks",left.lengthTicks}}}};
        check(OpJournal(*f.store).commit(fade).ok, "the full projected faded split span is accepted");
    }
}
void expressionKnotFollowsNoteClock() {
    Fixture f;
    f.clip(1, 0);
    auto& db = f.store->db();
    db.exec("UPDATE clips SET time_base=1,pos_ticks=NULL,length_ticks=NULL,"
            "pos_ns=500000000,length_ns=750000000 WHERE id=1");
    constexpr std::int64_t knotTicks = 69069;
    constexpr std::int64_t knotSample = 24287;
    NoteRecord sustained{};
    sustained.dur_ticks = 480480;
    sustained.note_id = 42;
    sustained.key = 60;
    sustained.vel_on = 100;
    sustained.vel_off = 64;
    sustained.probability = 10000;
    auto cue = sustained;
    cue.note_id = 43;
    cue.key = 65;
    cue.start_ticks = knotTicks;
    const auto notes = writeStream<NoteRecord>(FourCC::Notes, std::vector{sustained,cue});
    if (!f.store->putEventStream(1,"notes",notes))
        throw std::runtime_error("write native half-sample note stream");

    ExpressionPoint initial{};
    initial.value = 0.25f;
    auto knot = initial;
    knot.time_ticks = knotTicks;
    knot.value = 0.75f;
    const auto expression = writeStream<ExpressionPoint>(FourCC::Expression, std::vector{initial,knot});
    {
        SQLite::Statement row(db, "INSERT INTO note_expression(clip_id,note_id,dimension,data) VALUES(1,42,2,?)");
        row.bind(1,expression.data(),static_cast<int>(expression.size()));
        row.exec();
    }
    f.load();
    const auto when = [&](EventType type,std::uint16_t dim,std::optional<double> value = std::nullopt) {
        for (std::size_t i=0;i<f.instrument->count;++i) {
            const auto& heard=f.instrument->heard[i];
            if (heard.e.type==type && heard.e.dim==dim && (!value || heard.e.value==*value))
                return heard.clock;
        }
        return std::int64_t{-1};
    };
    constexpr auto timbre = static_cast<std::uint16_t>(ExpressionDim::Timbre);
    f.clock=23936;
    f.session.transport().locate(f.clock);
    f.render();
    const auto attack=when(EventType::NoteOn,65);
    const auto transition=when(EventType::NoteExpression,timbre,0.75);
    check(attack==knotSample,"native linear note still lands on its original half-sample boundary");
    check(transition==attack,
          "an off-grid AEXP knot shares the sample of the same-tick note attack");

    f.instrument->count=0;
    f.clock=knotSample;
    f.session.transport().locate(knotSample);
    f.render(64);
    const auto chased=when(EventType::NoteExpression,timbre);
    const auto chasedValue=when(EventType::NoteExpression,timbre,0.75);
    check(chased==knotSample && chasedValue==knotSample,
          "locating onto the shared boundary chases the new expression immediately");
    if (transition!=attack || chasedValue!=knotSample)
        std::printf("half-sample clock: note %lld, knot %lld, first chased %lld, new chased %lld\n",
                    static_cast<long long>(attack),static_cast<long long>(transition),
                    static_cast<long long>(chased),static_cast<long long>(chasedValue));
}

} // namespace

int main(int argc,char** argv){
    std::setvbuf(stdout,nullptr,_IONBF,0);const std::string only=argc>1?argv[1]:"all";
    try { if (only == "all" || only == "signed") signedOrigins(); }
    catch (const std::exception& e) { check(false, e.what()); }
    try { if (only == "all" || only == "take-lanes") takeLanePlayback(); }
    catch (const std::exception& e) { check(false, e.what()); }
    try { if (only == "all" || only == "expression-capacity") sessionExpressionCapacity(); }
    catch (const std::exception& e) { check(false, e.what()); }
    try { if (only == "all" || only == "coherence") coherentSessionExpression(); }
    catch (const std::exception& e) { check(false, e.what()); }
    try { if(only=="all" || only=="expression")sessionExpression(); }
    catch(const std::exception& e) {check(false,e.what());}
    try { if (only == "all" || only == "live-input") liveInput(); }
    catch (const std::exception& e) { check(false, e.what()); }
    try { if (only == "all" || only == "driver-capture") driverMidiCapture(); }
    catch (const std::exception& e) { check(false, e.what()); }
    try{if(only=="all"||only=="split")twoPointSplits();if(only=="all"||only=="controllers")sessionControllers();if(only=="all"||only=="active")activeFlag();if(only=="all"||only=="markers")sourceMarkers();if(only=="all"||only=="placement")placement();if(only=="all"||only=="cleanup")cleanup();if(only=="all"||only=="musical")musical();if(only=="all"||only=="pending")pendingRelease();if(only=="all"||only=="concurrent")concurrentPublication();if(only=="all"||only=="capacity")capacity();if(only=="all"||only=="overflow")overflowRelease();if(only=="all"||only=="ns"){nanosecondEnd();trimSourcePrefix();}if(only=="all"||only=="maximum")maximumLocate();if(only=="all"||only=="entry")loopEntry();if(only=="all"||only=="held")heldNote();}catch(const std::exception& e){check(false,e.what());}
    try { if (only == "all" || only == "knot-clock") expressionKnotFollowsNoteClock(); }
    catch (const std::exception& e) { check(false,e.what()); }
    try { if (only=="all" || only=="stored") storedSplitClock(); }
    catch(const std::exception& e) { check(false,e.what()); }
    check(allocations.load()==0,"MIDI callback allocates nothing");check(audio::callbackFileIo.load()==0,"MIDI callback performs no file I/O");
    std::printf("%s -- %d checks, %d failure(s)\n",failures?"FAIL":"PASS",checks,failures);return failures?1:0;
}
