// SPDX-License-Identifier: AGPL-3.0-or-later
#include "adi/engine/mpe_input.hpp"
#include "adi/engine/mpe_output.hpp"
#include "adi/engine/midi_memory.hpp"
#include "adi/audio/io_audit.hpp"
#include <array>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <new>
#include <stdexcept>
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
struct Sink {
    std::array<Event,1024> events{};std::int32_t count=0;MidiMemory* memory=nullptr;bool refuse=false;
    static bool take(void* context,const Event& e){auto& self=*static_cast<Sink*>(context);if(self.refuse)return false;
        if(self.count>=static_cast<std::int32_t>(self.events.size()))return false;
        self.events[static_cast<std::size_t>(self.count++)]=e;
        return !self.memory || self.memory->push(e,1000+e.frame);}
};
int feed(MpeParser& parser,Sink& sink,std::uint8_t status,std::uint8_t first,std::uint8_t second,std::int32_t frame=0) {
    const std::uint8_t bytes[]={status,first,second};audio::CallbackScope callback;parser.setFrame(frame);return parser.feed(bytes,3,Sink::take,&sink);
}
void chordsAndRouting() {
    MpeParser parser;Sink sink;MidiMemory memory(1,2,48000,NoteChannelBase::One,64);memory.setListening(true,false);sink.memory=&memory;
    feed(parser,sink,0x92,60,100,1);feed(parser,sink,0x92,64,101,2);feed(parser,sink,0x92,67,102,3);
    feed(parser,sink,0x82,64,7,4);feed(parser,sink,0x82,60,8,5);feed(parser,sink,0x82,67,9,6);
    check(sink.count==6 && sink.events[0].noteId!=0 && sink.events[0].noteId!=sink.events[1].noteId && sink.events[1].noteId!=sink.events[2].noteId,"plain same-channel chord attacks have distinct source identities");
    check(sink.events[3].noteId==sink.events[1].noteId && sink.events[4].noteId==sink.events[0].noteId && sink.events[5].noteId==sink.events[2].noteId,"out-of-order chord releases keep each key's original identity");
    std::array<MpeOut,64> storage{};MpeOutList output(storage.data(),static_cast<std::int32_t>(storage.size()));MpeRouter router;router.configure(ExpressionRoute::Plain,{});
    {audio::CallbackScope callback;router.route(sink.events.data(),sink.count,0,output);memory.advance(1064);}
    check(router.liveNotes()==0 && router.unknownNotes()==0 && output.size()==6,"actual native router ends every chord voice without unknown or stuck notes");
    MidiMemorySnapshot captured;std::string error;
    check(memory.snapshot(captured,error) && captured.events.size()==6 && captured.events[3].event.noteId==captured.events[1].event.noteId &&
        captured.events[4].sample==1005 && captured.events[4].event.channel==3,"actual parser chord identities and timestamps reach retrospective Capture unchanged");
}
void allKeysAndReset() {
    MpeParser parser;Sink sink;
    for(std::uint8_t key=0;key<128;++key)feed(parser,sink,0x90,key,100);
    for(int key=127;key>=0;--key)feed(parser,sink,0x80,static_cast<std::uint8_t>(key),5);
    bool exact=sink.count==256;
    for(std::size_t i=0;i<128;++i)exact=exact && sink.events[i].noteId==sink.events[255-i].noteId && sink.events[i].dim==sink.events[255-i].dim;
    check(exact,"all 128 plain keys release correctly in reverse order on one channel");
    Sink zero;feed(parser,zero,0x90,60,100);feed(parser,zero,0x90,64,100);feed(parser,zero,0x90,60,0);feed(parser,zero,0x80,64,7);
    check(zero.events[2].type==EventType::NoteOff && zero.events[2].noteId==zero.events[0].noteId && zero.events[3].noteId==zero.events[1].noteId,"velocity-zero release ends an earlier chord key");
    Sink channels;feed(parser,channels,0x90,60,100);feed(parser,channels,0x91,60,100);feed(parser,channels,0x80,60,7);feed(parser,channels,0x81,60,7);
    check(channels.events[2].noteId==channels.events[0].noteId && channels.events[3].noteId==channels.events[1].noteId &&
        channels.events[0].noteId!=channels.events[1].noteId,"same key on independent plain channels never aliases identity");
    Sink reset;feed(parser,reset,0x90,60,100);const auto prior=reset.events[0].noteId;parser.reset();feed(parser,reset,0x80,60,0);feed(parser,reset,0x90,60,100);
    check(reset.events[1].noteId==0 && reset.events[2].noteId>prior,"reset clears per-key ownership without recycling source identities");
    Sink repeated;feed(parser,repeated,0x90,72,100);feed(parser,repeated,0x90,72,101);feed(parser,repeated,0x80,72,0);feed(parser,repeated,0x80,72,0);
    check(repeated.events[2].noteId==repeated.events[1].noteId && repeated.events[3].noteId==0,"same-key retrigger retains the existing newest-identity release policy");
}
void controllers() {
    MpeParser parser;Sink sink;
    for(std::uint16_t controller=0;controller<128;++controller)feed(parser,sink,0xb2,static_cast<std::uint8_t>(controller),static_cast<std::uint8_t>((controller*3)%128),controller);
    bool exact=sink.count==128;
    for(std::size_t i=0;i<128;++i){const auto& e=sink.events[i];exact=exact && e.type==EventType::Controller && e.channel==2 && e.paramId==i && e.noteId==0 && e.value==static_cast<double>((i*3)%128)/127;}
    check(exact,"every ordinary CC address survives as normalized typed input on its zero-based channel");
    std::array<MpeOut,256> storage{};MpeOutList output(storage.data(),static_cast<std::int32_t>(storage.size()));MpeRouter router;router.configure(ExpressionRoute::Plain,{});
    {audio::CallbackScope callback;router.route(sink.events.data(),sink.count,0,output);}
    bool routed=output.size()==128;
    for(std::int32_t i=0;i<output.size();++i){const auto e=output.at(i);routed=routed && e.kind==MpeOut::Kind::Control && e.ctrl==i && e.channel==2 && e.word==(i*3)%128;}
    check(routed,"actual controller router recovers every original MIDI-1 wire value/address without another narrowing stage");
    Sink paired;feed(parser,paired,0xb0,1,85);feed(parser,paired,0xb0,33,5);
    check(paired.count==2 && paired.events[0].paramId==1 && paired.events[1].paramId==33 &&
        std::lround(paired.events[0].value*127)*128+std::lround(paired.events[1].value*127)==10885,"ordinary high-resolution CC pairs retain both original addressed components");
    MpeParser mpe;mpe.setZone(MpeZone::lower());Sink expression;feed(mpe,expression,0x91,60,100);feed(mpe,expression,0xb1,74,64);feed(mpe,expression,0xb1,106,1);feed(mpe,expression,0xb1,64,127);
    check(expression.count==4 && expression.events[1].type==EventType::NoteExpression && expression.events[2].type==EventType::NoteExpression &&
        expression.events[1].noteId==expression.events[0].noteId && expression.events[2].noteId==expression.events[0].noteId &&
        expression.events[2].value==8193.0/16383.0,"MPE member CC74/106 still combines exact per-note 14-bit timbre");
    check(expression.events[3].type==EventType::Controller && expression.events[3].channel==1 && expression.events[3].paramId==64 && expression.events[3].value==1,"ordinary member-channel pedals stay controllers rather than expression");
    Sink master;feed(mpe,master,0xb0,74,64);feed(mpe,master,0xb0,106,1);
    check(master.count==2 && master.events[0].type==EventType::Controller && master.events[1].type==EventType::Controller && master.events[0].channel==0,"MPE master timbre addresses remain ordinary CC, not a guessed per-note binding");
    MidiMemory memory(1,2,48000,NoteChannelBase::One,256);memory.setListening(false,true);Sink capture;capture.memory=&memory;
    feed(parser,capture,0xb0,7,100,1);feed(parser,capture,0xbf,64,127,2);MidiMemorySnapshot snapshot;std::string error;
    check(memory.snapshot(snapshot,error) && snapshot.events.size()==2 && snapshot.events[0].event.paramId==7 &&
        snapshot.events[1].event.channel==15 && snapshot.events[1].event.value==1,"ordinary actual input CC reaches retrospective memory without being dropped");
}
void malformedAndRejection() {
    MpeParser parser;Sink sink;
    const auto prior=parser.notesStarted();
    for(const std::uint8_t status:{std::uint8_t{0x80},std::uint8_t{0x90},std::uint8_t{0xb0},std::uint8_t{0xe0}}) {
        feed(parser,sink,status,128,1);feed(parser,sink,status,1,128);
    }
    feed(parser,sink,0xd0,128,0);
    check(sink.count==0 && parser.notesStarted()==prior,"consumed data bytes with status bits are refused before state changes or indexing");
    MpeParser mpe;mpe.setZone(MpeZone::lower());Sink good;feed(mpe,good,0x91,60,100);feed(mpe,good,0xb1,74,128);feed(mpe,good,0xb1,106,1);
    check(good.count==1,"malformed MPE MSB cannot seed a later invented 14-bit expression");
    Sink refused;refused.refuse=true;const auto rejected=parser.sinkRejections();
    check(feed(parser,refused,0xb0,7,100)==0 && parser.sinkRejections()==rejected+1,"ordinary controller sink pressure is counted and nonblocking");
    refused.refuse=false;check(feed(parser,refused,0xb0,7,101)==1 && refused.count==1 && refused.events[0].value==101.0/127,"parser remains usable after controller delivery rejection");
}
}
int main(){try{chordsAndRouting();allKeysAndReset();controllers();malformedAndRejection();check(allocations==0 && audio::callbackFileIo==0,"polyphony/controller parsing and router/Capture delivery allocate nothing and perform no callback I/O");}
catch(const std::exception& e){std::printf("EXCEPTION: %s\n",e.what());check(false,"fixture must complete");}
std::printf("midi_capture_input: %d checks, %d failures\n",checks,failures);return failures?1:0;}
