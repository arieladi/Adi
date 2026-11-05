// SPDX-License-Identifier: AGPL-3.0-or-later
#include "adi/engine/midi_memory.hpp"
#include "adi/engine/mpe_input.hpp"
#include "adi/audio/io_audit.hpp"
#include <atomic>
#include <bit>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <new>
#include <stdexcept>
#include <thread>
namespace {std::atomic<unsigned> allocations{0};}
void* operator new(std::size_t n){if(adi::audio::onAudioThread)++allocations;if(void* p=std::malloc(n?n:1))return p;throw std::bad_alloc();}
void* operator new[](std::size_t n){return operator new(n);}
void operator delete(void* p) noexcept{std::free(p);}
void operator delete[](void* p) noexcept{std::free(p);}
void operator delete(void* p,std::size_t) noexcept{std::free(p);}
void operator delete[](void* p,std::size_t) noexcept{std::free(p);}
namespace {
using namespace adi;using namespace adi::engine;
int checks=0,failures=0;
void check(bool ok,const char* label){++checks;if(!ok){++failures;std::printf("FAIL: %s\n",label);}}
Event note(std::uint64_t id=1){Event e;e.type=EventType::NoteOn;e.noteId=id;e.channel=3;e.dim=60;e.value=0.5+0x1p-40;e.frame=7;return e;}
bool push(MidiMemory& memory,const Event& event,std::int64_t sample){audio::CallbackScope callback;return memory.push(event,sample);}
void gatesClocksAndValues() {
    MidiMemory memory(11,22,48000,NoteChannelBase::Zero,8);MidiMemorySnapshot out;std::string error;
    check(!memory.listening() && !push(memory,note(),0) && memory.stats().ignored==1,"input is gated until armed or monitored");
    check(!memory.snapshot(out,error) && out.events.empty() && !error.empty(),"no input is an explicit empty capture refusal");
    {audio::CallbackScope callback;memory.setListening(true,false);}
    constexpr std::int64_t huge=(std::int64_t{1}<<53)+19;auto event=note(UINT64_MAX);
    check(push(memory,event,huge) && memory.snapshot(out,error) && out.events.size()==1 && out.events[0].sample==huge &&
        out.events[0].event.noteId==UINT64_MAX && out.events[0].event.value==event.value && out.events[0].event.frame==7,"full double value, uint64 identity, original frame and >2^53 driver timestamp survive");
    check(out.inputId==11 && out.trackId==22 && out.sampleRate==48000 && out.noteChannels==NoteChannelBase::Zero &&
        out.endSample==huge+1 && !out.prefixDropped,"snapshot names input, target, format, channel convention and exclusive end");
    const auto epoch=out.epoch;
    event.type=EventType::NoteExpression;event.value=-48.0-0x1p-38;event.dim=65535;
    check(push(memory,event,huge) && memory.snapshot(out,error) && out.events[0].serial<out.events[1].serial &&
        out.events[1].sample==huge && out.events[1].event.value==event.value && out.events[1].event.dim==65535,"coincident events retain order and full-width signed/plugin expression");
    event.type=EventType::ParamValue;
    check(!push(memory,event,huge) && memory.snapshot(out,error) && out.epoch==epoch && out.events.size()==2,"addressed parameter traffic is ignored without erasing MIDI history");
    {audio::CallbackScope callback;memory.setListening(false,true);memory.advance(huge+100000);}
    check(memory.snapshot(out,error) && out.epoch==epoch && out.endSample==huge+100000 && out.events.size()==2,"monitor gate and forward silent blocks preserve events while transport can be stopped");
    check(push(memory,note(9),5) && memory.snapshot(out,error) && out.events.size()==1 && out.events[0].sample==5 && out.epoch!=epoch,"backward driver clock starts a new epoch without splicing history");
    check(memory.discontinuity(100) && !memory.snapshot(out,error) && push(memory,note(),100) && memory.snapshot(out,error) && out.events.size()==1,"explicit device/input discontinuity clears history");
    check(memory.advance(10) && !memory.snapshot(out,error),"backward clock advance clears retained events");
    {audio::CallbackScope callback;memory.setListening(false,false);}
    check(!memory.advance(20) && !push(memory,note(),20) && !memory.snapshot(out,error),"neither gate retains or advances input");
}
void wrapAndIsolation() {
    MidiMemory memory(1,2,44100,NoteChannelBase::Zero,3);memory.setListening(true,false);
    for(std::uint64_t i=0;i<11;++i)push(memory,note(i+1),static_cast<std::int64_t>(i));
    MidiMemorySnapshot out;std::string error;
    check(memory.snapshot(out,error) && out.events.size()==3 && out.prefixDropped && out.events.front().serial==8 &&
        out.events.front().event.noteId==9 && out.events.back().event.noteId==11,"event budget retains latest ordered suffix and explicitly marks lost prefix");
    const auto held=out;
    for(std::uint64_t i=11;i<30;++i)push(memory,note(i+1),static_cast<std::int64_t>(i));
    check(held.events.front().event.noteId==9 && held.events.back().event.noteId==11,"later producer wrap cannot mutate returned Capture history");
    MidiMemory other(3,4,768000,NoteChannelBase::One,2);other.setListening(false,true);auto e=note();e.channel=16;
    check(push(other,e,0) && other.snapshot(out,error) && out.inputId==3 && out.trackId==4 && out.noteChannels==NoteChannelBase::One &&
        out.events[0].event.channel==16,"independent inputs explicitly preserve one-based parser note channels");
    e.type=EventType::Controller;e.channel=0;e.paramId=127;e.value=0.25+0x1p-43;e.noteId=0;
    check(push(other,e,0) && other.snapshot(out,error) && out.events[1].event.channel==0 && out.events[1].event.paramId==127 &&
        out.events[1].event.value==e.value,"controller addresses stay zero-based even with one-based note sources");
    e.channel=16;check(!push(other,e,1) && !other.snapshot(out,error),"invalid controller channel is not silently converted from note convention");
    e=note();e.channel=0;check(!push(other,e,2),"one-based note source refuses channel zero");
    e.channel=16;check(!push(memory,e,30),"zero-based note source refuses channel sixteen");
    check(memory.stats().copied==30 && memory.stats().invalid==1 && memory.stats().dropped==0,"sequential producer statistics distinguish rolling retention from malformed/drop loss");
}
void parser() {
    MidiMemory memory(1,2,48000,NoteChannelBase::One,32);memory.setListening(true,false);
    struct Context{MidiMemory* memory;std::int64_t block;};Context context{&memory,1000};
    const auto sink=[](void* pointer,const Event& e)->bool{auto& c=*static_cast<Context*>(pointer);return c.memory->push(e,c.block+e.frame);};
    MpeParser input;input.setZone(MpeZone::lower());const std::uint8_t on[]={0x91,60,100},bend[]={0xe1,1,65},off[]={0x81,60,7};
    {audio::CallbackScope callback;input.setFrame(7);input.feed(on,3,sink,&context);input.setFrame(9);input.feed(bend,3,sink,&context);input.setFrame(20);input.feed(off,3,sink,&context);memory.advance(1064);}
    MidiMemorySnapshot out;std::string error;
    check(memory.snapshot(out,error) && out.events.size()==3,"actual MPE input parser can feed retrospective typed memory without MIDI re-encoding");
    if(out.events.size()!=3)return;
    check(out.events[0].sample==1007 && out.events[1].sample==1009 && out.events[2].sample==1020 && out.endSample==1064,"parser packet frame plus explicit block clock sets exact input times");
    check(out.events[0].event.type==EventType::NoteOn && out.events[1].event.type==EventType::NoteExpression && out.events[2].event.type==EventType::NoteOff &&
        out.events[0].event.noteId!=0 && out.events[0].event.noteId==out.events[1].event.noteId && out.events[1].event.noteId==out.events[2].event.noteId,"MPE expression/release keeps parser-minted note identity");
    check(out.events[1].event.value!=static_cast<double>(static_cast<float>(out.events[1].event.value)),"captured actual 14-bit pitch retains its double precision");
}
void invalidInput() {
    MidiMemory memory(1,2,48000,NoteChannelBase::Zero,8);memory.setListening(true,false);MidiMemorySnapshot out;std::string error;
    int refused=0;
    for(int which=0;which<10;++which) {
        auto e=note();std::int64_t at=2;
        if(which==0)at=-1;
        if(which==1)at=std::numeric_limits<std::int64_t>::max();
        if(which==2)e.frame=-1;
        if(which==3)e.value=std::numeric_limits<double>::infinity();
        if(which==4)e.value=std::numeric_limits<double>::quiet_NaN();
        if(which==5)e.value=0;
        if(which==6)e.dim=128;
        if(which==7)e.channel=16;
        if(which==8)e.type=static_cast<EventType>(255);
        if(which==9){e.type=EventType::Controller;e.paramId=128;}
        if(!push(memory,e,at) && !memory.snapshot(out,error) && out.events.empty())++refused;
    }
    check(refused==10 && memory.stats().invalid==10,"malformed times, values, channels, note keys, types and CC addresses are refused whole");
    check(!memory.advance(-1) && !memory.discontinuity(-1) && memory.stats().invalid==12,"invalid clock controls are counted and safe");
    auto e=note();e.type=EventType::NoteOff;e.noteId=0;e.value=0;
    check(push(memory,e,0) && memory.snapshot(out,error) && out.events[0].event.noteId==0 && out.events[0].event.value==0,"unassigned release identity and zero release velocity are retained for owner resolution");
    int constructors=0;
    for(int which=0;which<7;++which)try {
        MidiMemory invalid(which==0?0:1,which==1?0:2,which==2?7999:which==3?768001:48000,
            which==4?static_cast<NoteChannelBase>(8):NoteChannelBase::Zero,which==5?1:which==6?(1u<<20)+1:16);
    }catch(const std::invalid_argument&){++constructors;}
    check(constructors==7,"invalid memory identity, rate, convention and capacity fail construction");
}
Event patterned(std::uint64_t serial) {
    Event e;e.type=EventType::Controller;e.channel=static_cast<std::uint8_t>(serial%16);e.paramId=static_cast<std::uint32_t>(serial%128);
    e.frame=static_cast<std::int32_t>(serial%1024);e.noteId=serial+(std::uint64_t{1}<<60);e.value=static_cast<double>(serial%8192)/8192+0x1p-45;return e;
}
bool exact(const MidiMemorySnapshot& out) {
    for(std::size_t i=0;i<out.events.size();++i){const auto& entry=out.events[i];const auto e=patterned(entry.serial);
        if(entry.sample!=(std::int64_t{1}<<53)+17+static_cast<std::int64_t>(entry.serial*3) || entry.event.type!=e.type ||
           entry.event.channel!=e.channel || entry.event.paramId!=e.paramId || entry.event.frame!=e.frame || entry.event.noteId!=e.noteId ||
           entry.event.value!=e.value || (i && entry.serial!=out.events[i-1].serial+1))return false;}
    return true;
}
void concurrent() {
    MidiMemory memory(1,2,48000,NoteChannelBase::Zero,97);memory.setListening(true,false);
    std::atomic<bool> start{false},done{false};std::thread producer([&]{while(!start.load(std::memory_order_acquire))std::this_thread::yield();
        audio::CallbackScope callback;for(std::uint64_t i=0;i<40000;++i)memory.push(patterned(i),(std::int64_t{1}<<53)+17+static_cast<std::int64_t>(i*3));done.store(true,std::memory_order_release);});
    bool healthy=true;unsigned successes=0,attempts=0;start.store(true,std::memory_order_release);
    do{MidiMemorySnapshot out;std::string error;const bool ok=memory.snapshot(out,error);++attempts;if(ok){++successes;healthy=healthy && out.events.size()<=97 && exact(out);}else healthy=healthy && out.events.empty() && !error.empty();}while(!done.load(std::memory_order_acquire));
    producer.join();check(healthy && attempts>0,"concurrent snapshots return exact ordered typed events or explicit empty refusal");
    check(memory.stats().copied+memory.stats().dropped==40000 && memory.stats().invalid==0,"concurrent owner cannot block or hide producer event accounting");
    check(push(memory,patterned(40000),(std::int64_t{1}<<53)+17+120000),"producer recovers after snapshot contention");
    MidiMemorySnapshot last;std::string error;check(memory.snapshot(last,error) && exact(last),"final snapshot remains coherent after repeated wraps and pinned drops");
    std::printf("concurrent MIDI snapshots: %u accepted / %u attempts, %llu dropped events\n",successes,attempts,static_cast<unsigned long long>(memory.stats().dropped));
}
}
int main(){try{gatesClocksAndValues();wrapAndIsolation();parser();invalidInput();concurrent();check(allocations==0 && audio::callbackFileIo==0,"input controls/parser/copies allocate nothing and perform no callback file I/O");}
catch(const std::exception& e){std::printf("EXCEPTION: %s\n",e.what());check(false,"fixture must complete");}
std::printf("midi_memory: %d checks, %d failures\n",checks,failures);return failures?1:0;}
