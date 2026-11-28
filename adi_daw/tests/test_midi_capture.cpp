// SPDX-License-Identifier: AGPL-3.0-or-later
#include "adi/midi_capture.hpp"
#include "adi/controller_lanes.hpp"
#include "adi/engine/mpe_input.hpp"
#include "adi/engine/session.hpp"
#include "adi/edit_time.hpp"
#include "adi/audio/io_audit.hpp"
#include "adi/digest.hpp"
#include "adi/history.hpp"
#include "adi/store.hpp"
#include "replace_operator_new.hpp"
#include "temp_directory.hpp"
#include <SQLiteCpp/SQLiteCpp.h>
#include <sqlite3.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <limits>
#include <new>
#include <stdexcept>
#include <tuple>
#include <utility>
namespace {std::atomic<unsigned> allocations{0};}
void adi::test::countReplacedNew() noexcept{if(adi::audio::onAudioThread)++allocations;}
namespace {
using namespace adi;using namespace adi::engine;
constexpr std::int64_t inputStart=(std::int64_t{1}<<53)+19;
constexpr double controlValue=0.25+0x1p-43;
int checks=0,failures=0;
void check(bool ok,const char* label){++checks;if(!ok){++failures;std::printf("FAIL: %s\n",label);}}
// MSVC's Debug CRT (_ITERATOR_DEBUG_LEVEL 2) allocates an iterator proxy whenever a
// std::string or std::vector is constructed, so the CommitResult that commitMidi's
// callback refusal returns costs one allocation per container there for each of its
// two constructions (built, then moved out: its returns name different objects) and
// none on the other CRTs. The refusal check allows exactly that, measured once on the
// callback before its baseline is read; the measurement itself is not counted.
template<class Result> unsigned returnedResultCost(){const auto before=allocations.load();
    {audio::CallbackScope scope;Result built;Result returned(std::move(built));(void)returned;}return allocations.exchange(before)-before;}
MidiMemorySnapshot transcript(std::uint32_t rate=48000) {
    MidiMemory memory(8,1,rate,NoteChannelBase::One,64);memory.setListening(true,false);MpeParser parser;
    const auto sink=[](void* context,const Event& e)->bool{return static_cast<MidiMemory*>(context)->push(e,inputStart+e.frame);};
    const auto feed=[&](std::uint8_t status,std::uint8_t a,std::uint8_t b,std::int32_t frame){const std::uint8_t bytes[]={status,a,b};
        audio::CallbackScope callback;parser.setFrame(frame);parser.feed(bytes,3,sink,&memory);};
    feed(0xb0,7,20,0);feed(0xb0,11,30,0);feed(0x90,60,100,0);feed(0x90,64,64,8);
    Event cc;cc.type=EventType::Controller;cc.channel=0;cc.paramId=7;cc.value=controlValue;cc.frame=16;
    {audio::CallbackScope callback;memory.push(cc,inputStart+16);}
    feed(0x80,60,7,24);feed(0x90,67,127,32);feed(0x80,64,8,48);
    memory.advance(inputStart+64);MidiMemorySnapshot result;std::string error;
    if(!memory.snapshot(result,error))throw std::runtime_error(error);
    return result;
}
capture::MidiSpec spec(std::int64_t clip=7,std::int64_t position=0){return {clip,position,inputStart,inputStart+64,"Captured MIDI",true};}
struct Probe final:device::DeviceInstance {
    device::DeviceIdentity id;int active=0,ons=0,offs=0;double cc=0,pressure=0;
    const device::DeviceIdentity& identity()const noexcept override{return id;}
    bool loaded()const noexcept override{return true;}
    bool processesDouble()const noexcept override{return true;}
    EventFlow eventFlow()const noexcept override{return EventFlow::Consume;}
    std::int64_t tailSamples()const noexcept override{return 0;}
    void process(const NodeIo& io)noexcept override {
        std::int32_t next=0;
        for(std::int32_t frame=0;frame<io.frames;++frame) {
            const auto at=io.blockOffset+frame;
            while(next<io.events.count && io.events.first[next].frame<=at) {
                const auto& e=io.events.first[next++];
                if(e.type==EventType::NoteOn){++active;++ons;}
                if(e.type==EventType::NoteOff){--active;++offs;}
                if(e.type==EventType::Controller && e.paramId==7)cc=e.value;
                if(e.type==EventType::NoteExpression && e.dim==static_cast<std::uint16_t>(ExpressionDim::Pressure))pressure=e.value;
            }
            for(std::int32_t channel=0;channel<io.channels;++channel) {
                const double value=static_cast<double>(active)*0.125;
                if(io.wide())io.out64[channel][at]=value;else io.out[channel][at]=static_cast<float>(value);
            }
        }
    }
};
struct Played {std::vector<double> audio;int ons=0,offs=0,active=0;double cc=0,pressure=0;};
struct Fixture {
    test::TempDirectory temp{"capture","midi_native"};std::unique_ptr<Store> store;
    Fixture(){StoreError error{};store=Store::createEmpty(temp.path()/"a.adi",error);if(!store)throw std::runtime_error("store");
        store->db().exec("INSERT INTO tracks(id,kind,name) VALUES(1,'midi','Capture'),(9,'master','Main');"
            "INSERT INTO plugin_refs(id,format,uid,name,subtype) VALUES(1,'vst3','capture.probe','Probe','instrument');"
            "INSERT INTO device_chains(id,track_id) VALUES(1,1);INSERT INTO devices(id,chain_id,ord,plugin_ref_id,name) VALUES(1,1,0,1,'Probe')");}
    Played play(Precision precision,double rate=48000,std::size_t frames=128,std::int64_t start=0) {
        Session session;Probe* probe=nullptr;session.graph().setFadeFrames(0);SessionSpec format{2,rate,64};format.precision=precision;
        if(!session.load(*store,[&](const DeviceRequest&,std::string&)->std::unique_ptr<device::DeviceInstance>{auto device=std::make_unique<Probe>();probe=device.get();return device;},format))throw std::runtime_error(session.error());
        if(!session.problems().empty())throw std::runtime_error(session.problems().front());
        session.transport().locate(start);session.transport().play();Played result;result.audio.resize(frames);
        std::array<float,128> narrow{};std::array<double,128> wide{};float* out[]={narrow.data(),narrow.data()+64};double* out64[]={wide.data(),wide.data()+64};
        for(std::size_t first=0;first<frames;first+=64) {
            AudioIo io;io.out=out;if(precision==Precision::Float64)io.out64=out64;io.numOut=2;io.frames=static_cast<std::int32_t>(std::min<std::size_t>(64,frames-first));
            {audio::CallbackScope callback;session.process(io);}
            for(std::int32_t frame=0;frame<io.frames;++frame)result.audio[first+static_cast<std::size_t>(frame)]=precision==Precision::Float64?wide[static_cast<std::size_t>(frame)]:narrow[static_cast<std::size_t>(frame)];
        }
        result.ons=probe->ons;result.offs=probe->offs;result.active=probe->active;result.cc=probe->cc;
        result.pressure=probe->pressure;return result;
    }
};
double expected(std::int64_t frame) {
    if(frame<0 || frame>=64)return 0;
    const int active=(frame<24?1:0)+(frame>=8 && frame<48?1:0)+(frame>=32?1:0);return static_cast<double>(active)*0.125;
}
void sampleOriginRounding() {
    constexpr auto quarter=static_cast<std::int64_t>(textproj::kPPQ);
    for(const auto& [bpm,position,rate]:std::array{
        std::tuple{110,quarter,48000u},std::tuple{128,4*quarter,44100u}}) {
        Fixture fixture;
        fixture.store->db().exec("INSERT INTO tempo_map(pos_ticks,bpm) VALUES(0,"+std::to_string(bpm)+")");
        const edit::TimeMap time(fixture.store->db());
        const auto start=std::llround(time.seconds(position)*static_cast<double>(rate));
        std::string error;
        const auto take=capture::prepareMidi(*fixture.store,transcript(rate),spec(7,position),error);
        check(take && take->notes==3 && capture::commitMidi(*fixture.store,*take).ok,
              "ordinary tempo and half-sample start both prepare and commit at the given tick");
        if(!take)continue;
        const auto bytes=fixture.store->getEventStream(7,"notes");
        const StreamReader<NoteRecord> notes(bytes?std::span<const std::byte>(*bytes):std::span<const std::byte>{},FourCC::Notes);
        check(notes.count()==3 && notes.at(0)->start_ticks==0 &&
              fixture.store->db().execAndGet("SELECT pos_ticks FROM clips WHERE id=7").getInt64()==position,
              "capture offset zero preserves the given musical origin rather than a nearby tick");
        const auto heard=fixture.play(Precision::Float64,rate,128,start);
        bool exact=heard.ons==3 && heard.offs==3 && heard.active==0;
        for(std::size_t frame=0;frame<heard.audio.size();++frame)
            exact=exact && heard.audio[frame]==expected(static_cast<std::int64_t>(frame));
        check(exact,"capture offsets preserve every rounded driver sample on the actual Session path");
    }
    for(const auto bpm:{90,110,140}) {
        Fixture fixture;
        fixture.store->db().exec("INSERT INTO tempo_map(pos_ticks,bpm) VALUES(0,"+std::to_string(bpm)+")");
        const edit::TimeMap time(fixture.store->db());
        bool exact=true,floor=true;
        for(std::int64_t tick=quarter;tick<quarter+512;++tick) {
            const auto nanos=time.nanos(tick);
            exact=exact && time.ticks(nanos)==tick;
            floor=floor && time.ticks(nanos-8)==tick-1;
        }
        check(exact && floor,"nanosecond round-trip recovers integer ticks and preserves the true floor at 90, 110 and 140 BPM");
    }
    TempoMap tempo;tempo.events.push_back({0,120,0});
    check(tempo.secondsToTicks(std::numeric_limits<double>::quiet_NaN())==0 &&
          tempo.secondsToTicks(std::numeric_limits<double>::infinity())==0 &&
          tempo.secondsToTicks(std::numeric_limits<double>::max())==std::numeric_limits<std::int64_t>::max(),
          "nonfinite and huge inverse tempo inputs avoid an out-of-range conversion");
}
void nativeHistory() {
    for(const auto precision:{Precision::Float32,Precision::Float64}) {
        Fixture f;auto source=transcript();
        for(auto& event:source.events)if(event.event.noteId)event.event.noteId=UINT64_MAX-event.event.noteId;
        // DEV-17: the parser's identities carry its own source prefix, so the
        // expected stored ID is the first captured note-on's full identity.
        std::uint64_t firstNoteId=0;
        for(const auto& event:source.events)if(event.event.type==EventType::NoteOn){firstNoteId=event.event.noteId;break;}
        const auto before=digestProject(*f.store).text;std::string error;const auto prepared=capture::prepareMidi(*f.store,source,spec(),error);
        check(prepared && prepared->notes==3 && prepared->controllerLanes==2 && prepared->closedHeldNotes==1 && prepared->requests.size()==1 &&
            digestProject(*f.store).text==before && !History(*f.store).canUndo(),"real parser transcript prepares one reviewable note/CC Capture without writes or reserved identities");
        if(!prepared){std::printf("prepare: %s\n",error.c_str());return;}
        const auto committed=capture::commitMidi(*f.store,*prepared);
        check(committed.ok && committed.seqs.size()==1,"explicit notes/CC images commit as one native Capture operation");if(!committed.ok)return;
        const auto bytes=f.store->getEventStream(7,"notes");bool notes=false;
        if(bytes){StreamReader<NoteRecord> reader(*bytes,FourCC::Notes);notes=reader.ok() && reader.count()==3 && firstNoteId!=0 && reader.at(0)->note_id==firstNoteId && reader.at(0)->vel_on==100 && reader.at(0)->vel_off==7 && reader.at(2)->vel_off==0;}
        check(notes,"stored ANOT retains original full uint64 identities and exact source/release velocity words");
        const auto controls=controllers::readLanes(*f.store,7);
        check(controls.ok && controls.lanes.size()==2 && controls.lanes[0].points.size()==2 && controls.lanes[0].points[1].value==controlValue &&
            f.store->db().execAndGet("SELECT count(DISTINCT id) FROM event_streams WHERE clip_id=7").getInt()==3,"full-double CC points and three independent stream identities are captured");
        const auto played=f.play(precision);bool exact=true;
        for(std::size_t frame=0;frame<played.audio.size();++frame)exact=exact && played.audio[frame]==expected(static_cast<std::int64_t>(frame));
        check(exact && played.ons==3 && played.offs==3 && played.active==0 && played.cc==controlValue,"actual Session plays all Capture notes/held-end closure and full-double CC at both graph widths");
        const auto after=digestProject(*f.store).text;
        Fixture replay;replay.store->db().exec("INSERT INTO session_state(key,value) VALUES('selection','track:9'),('playhead_ticks','999999'),('grid_ticks','17')");
        const auto logged=OpJournal(*f.store).recent(1).front();const auto encoded=encodePayload(logged.payload);std::string why;const auto decoded=decodeFromLog(encoded,why);
        OpRequest replayRequest;replayRequest.opType=logged.opType;if(decoded)replayRequest.payload=*decoded;
        check(decoded && OpJournal(*replay.store).commit(replayRequest).ok && digestProject(*replay.store).text==after && replay.play(precision).audio==played.audio,
            "captured binary op replays to identical rows and heard music under different selection/playhead/grid state");
        check(History(*f.store).nextUndo()->label=="Capture MIDI" && History(*f.store).undo().ok && digestProject(*f.store).text==before,"one Capture undo restores exact pre-capture rows and stream absence");
        check(History(*f.store).redo().ok && digestProject(*f.store).text==after && f.play(precision).audio==played.audio,"redo restores literal playing Capture without reading live input");
        f.store.reset();StoreError opened{};f.store=Store::open(f.temp.path()/"a.adi",opened);
        check(f.store && digestProject(*f.store).text==after && f.play(precision).audio==played.audio,"reopened project retains Capture history and actual heard notes");
    }
}
void expressionHistory() {
    Fixture fixture;
    const auto before = digestProject(*fixture.store).text;
    MidiMemory memory(8, 1, 48000, NoteChannelBase::One, 64);
    memory.setListening(true, false);
    MpeParser parser;
    parser.setZone(MpeZone::lower());
    const auto sink = [](void* context, const Event& event) {
        return static_cast<MidiMemory*>(context)->push(event, inputStart + event.frame);
    };
    const auto feed = [&](std::int32_t frame, std::initializer_list<std::uint8_t> packet) {
        audio::CallbackScope scope;
        parser.setFrame(frame);
        parser.feed(packet.begin(), static_cast<int>(packet.size()), sink, &memory);
    };
    feed(0, {0x91, 60, 100});
    feed(8, {0xd1, 64});
    feed(16, {0xd1, 96});
    feed(32, {0x81, 60, 0});
    memory.advance(inputStart + 64);
    MidiMemorySnapshot source;
    std::string error;
    check(memory.snapshot(source, error), "MPE parser produces a retained pressure transcript");
    if (source.events.empty()) return;
    const auto prepared = capture::prepareMidi(*fixture.store, source, spec(), error);
    check(prepared && capture::commitMidi(*fixture.store, *prepared).ok,
          "native Capture commits a real MPE transcript and its pressure lane");
    if (!prepared || !History(*fixture.store).canUndo()) return;
    const auto played = fixture.play(Precision::Float64);
    check(played.ons == 1 && played.offs == 1 && played.pressure == static_cast<float>(96. / 127),
          "the captured pressure reaches its instrument during Arrangement playback");
    const auto after = digestProject(*fixture.store).text;
    Fixture replay;
    const auto logged = OpJournal(*fixture.store).recent(1).front();
    auto payload = decodeFromLog(encodePayload(logged.payload), error);
    OpRequest request;
    request.opType = logged.opType;
    if (payload) request.payload = std::move(*payload);
    check(payload && OpJournal(*replay.store).commit(request).ok && digestProject(*replay.store).text == after,
          "the serialized Capture operation replays the same expression rows");
    check(History(*fixture.store).undo().ok && digestProject(*fixture.store).text == before &&
          History(*fixture.store).redo().ok && digestProject(*fixture.store).text == after,
          "Capture undo and redo restore both notes and expression rows");
}
void channelPitchHistory() {
    Fixture fixture;
    MidiMemory memory(8, 1, 48000, NoteChannelBase::One, 64);
    memory.setListening(true, false);
    MpeParser parser;
    parser.setZone(MpeZone::lower());
    const auto sink = [](void* context, const Event& event) {
        return static_cast<MidiMemory*>(context)->push(event, inputStart + event.frame);
    };
    const auto feed = [&](std::int32_t frame, std::array<std::uint8_t, 3> bytes) {
        audio::CallbackScope scope;
        parser.setFrame(frame);
        parser.feed(bytes.data(), 3, sink, &memory);
    };
    feed(0, {0xe0, 37, 73});
    feed(8, {0xb0, 7, 88});
    feed(16, {0x91, 60, 90});
    feed(24, {0xe1, 0, 76});
    feed(32, {0xe0, 0, 64});
    feed(40, {0x81, 60, 0});
    const Event typed{48, EventType::Controller, 15, 0, 0, controllers::kPitchBend, 1.};
    check(memory.push(typed, inputStart + 48), "MIDI memory also retains a typed normalized channel pitch wheel");
    memory.advance(inputStart + 64);
    MidiMemorySnapshot source;
    std::string error;
    check(memory.snapshot(source, error) && source.events.size() == 7,
          "MIDI memory preserves the plain channel wheel and the MPE member pitch");
    if (source.events.size() != 7)
        return;
    const auto prepared = capture::prepareMidi(*fixture.store, source, spec(), error);
    check(prepared && prepared->controllerLanes == 3 && capture::commitMidi(*fixture.store, *prepared).ok,
          "retrospective Capture commits both channel wheels beside the original CC");
    if (!prepared)
        return;
    const auto controllerRows = controllers::readLanes(*fixture.store, 7);
    const auto& lanes = controllerRows.lanes;
    check(controllerRows.ok && lanes.size() == 3 && lanes[0].controller == 7 && lanes[0].points.size() == 1 &&
              lanes[0].points[0].value == 88. / 127. && lanes[1].channel == 0 &&
              lanes[1].controller == controllers::kPitchBend && lanes[1].points.size() == 2 &&
              std::lround(controllers::pitchBendWordExact(lanes[1].points[0].value)) == (73 * 128 + 37) &&
              lanes[1].points[1].value == .5 && lanes[2].channel == 15 &&
              lanes[2].controller == controllers::kPitchBend && lanes[2].points.size() == 1 &&
              lanes[2].points[0].value == 1.,
          "Capture preserves the wire pitch bits, explicit center and channel conventions");
    const auto& packet = prepared->requests.front().payload;
    check(packet.contains("expressions") && packet.at("expressions").size() == 1,
          "MPE member pitch stays in its owning note's expression stream");
    History history(*fixture.store);
    check(history.undo().ok && controllers::readLanes(*fixture.store, 7).lanes.empty() &&
              history.redo().ok && controllers::readLanes(*fixture.store, 7).lanes == lanes,
          "Capture pitch lanes revert and replay atomically with the clip");
}

void liveHistoryConsumption() {
    Fixture fixture;
    fixture.store->db().exec("UPDATE tracks SET record_armed=1 WHERE id=1");
    Session session;
    check(session.load(*fixture.store, [](const DeviceRequest&, std::string&) {
        return std::make_unique<Probe>();
    }, SessionSpec{2, 48000, 64}), "create a live MIDI history on an armed track");
    std::array<float, 64> left{}, right{};
    float* output[]{left.data(), right.data()};
    const auto playNote = [&](std::int64_t stream, std::uint64_t id) {
        const Event on{0, EventType::NoteOn, 0, 60, id, 0, 1};
        const Event off{0, EventType::NoteOff, 0, 60, id, 0, 0};
        check(session.enqueueMidiInput(stream + 1, on) && session.enqueueMidiInput(stream + 16, off),
              "a completed note reaches its published MIDI history");
        audio::CallbackScope callback;
        session.process({output, nullptr, 2, 0, 64, stream});
    };
    std::string error;
    std::vector<MidiMemorySnapshot> original, replacement, more;
    playNote(0, 1);
    check(session.readMidiHistories(original, error) && original.size() == 1, "read the initial history");
    if (original.size() != 1) return;
    session.prepare(44100, 64);
    playNote(0, 2);
    check(session.readMidiHistories(replacement, error) && replacement.size() == 1 &&
              replacement.front().sampleRate == 44100 && replacement.front().inputId != original.front().inputId,
          "a new driver format replaces the history identity but retains the track");
    if (replacement.size() != 1) return;
    session.consumeMidiHistories(original);
    check(session.midiHistoryAvailable(), "consuming a stale snapshot leaves replacement notes available");
    playNote(64, 3);
    check(session.readMidiHistories(more, error) && more.size() == 1 && more.front().events.size() == 4,
          "both replacement notes remain available");
    session.consumeMidiHistories(more);
    session.consumeMidiHistories(replacement);
    check(!session.midiHistoryAvailable(), "consuming an earlier snapshot cannot move its history watermark back");

    const Event normalized{0, EventType::Controller, 15, 0, 0, controllers::kPitchBend, .75};
    const Event legacy{0, EventType::NoteExpression, 2, static_cast<std::uint16_t>(ExpressionDim::Pitch), 0, 0, 0};
    check(session.enqueueMidiInput(129, normalized) && session.enqueueMidiInput(140, legacy),
          "the live input dispatches normalized and parser-style channel pitch wheels");
    {
        audio::CallbackScope callback;
        session.process({output, nullptr, 2, 0, 64, 128});
    }
    check(session.readMidiHistories(more, error) && more.size() == 1 && more.front().events.size() == 2,
          "a pitch-only gesture remains available to Capture MIDI");
    if (more.size() != 1 || more.front().events.size() != 2)
        return;
    capture::MidiSpec choice{7, 0, 129, 192, "Captured Pitch Wheels", true};
    const auto prepared = capture::prepareMidi(*fixture.store, more.front(), choice, error);
    check(prepared && capture::commitMidi(*fixture.store, *prepared).ok,
          "Capture can create a clip from channel pitch movements alone");
    const auto pitch = controllers::readLanes(*fixture.store, 7);
    check(pitch.ok && pitch.lanes.size() == 2 && pitch.lanes[0].channel == 2 &&
              pitch.lanes[0].points.size() == 1 && pitch.lanes[0].points.front().value == .5 &&
              pitch.lanes[1].channel == 15 && pitch.lanes[1].points.size() == 1 &&
              pitch.lanes[1].points.front().value == .75,
          "session Capture retains zero-based physical channels, neutral and the normalized bend");
}
void clocksAndSourceConventions() {
    bool all=true;
    for(std::uint32_t rate:{44100u,48000u,96000u,192000u,384000u,768000u}) {
        Fixture f;const auto source=transcript(rate);std::string error;const auto prepared=capture::prepareMidi(*f.store,source,spec(),error);
        if(!prepared || !capture::commitMidi(*f.store,*prepared).ok){all=false;continue;}
        const auto played=f.play(Precision::Float64,rate);
        for(std::size_t frame=0;frame<played.audio.size();++frame)all=all && played.audio[frame]==expected(static_cast<std::int64_t>(frame));
    }
    check(all,"native captured sample positions roundtrip across every 44.1–768 kHz session rate");
    Fixture f;f.store->db().exec("INSERT INTO tempo_map(id,pos_ticks,bpm,curve) VALUES(1,0,120,0),(2,10000,60,0)");
    auto source=transcript();source.noteChannels=NoteChannelBase::Zero;for(auto& entry:source.events)if(entry.event.type!=EventType::Controller)--entry.event.channel;
    std::string error;const auto prepared=capture::prepareMidi(*f.store,source,spec(7,1000),error);
    check(prepared && capture::commitMidi(*f.store,*prepared).ok,"explicit zero-based note sources prepare through an actual step-tempo crossing");
    if(!prepared)return;
    const auto played=f.play(Precision::Float64);bool exact=true;
    for(std::size_t frame=0;frame<played.audio.size();++frame)exact=exact && played.audio[frame]==expected(static_cast<std::int64_t>(frame)-4);
    check(exact,"project placement and changing tempo preserve exact relative captured sample timing");
}
void guardsAndTransactions() {
    Fixture f;const auto source=transcript();std::string error;auto prepared=capture::prepareMidi(*f.store,source,spec(),error);check(prepared.has_value(),"guard fixture");if(!prepared)return;
    const auto before=digestProject(*f.store).text;
    f.store.reset();StoreError opened{};f.store=Store::open(f.temp.path()/"a.adi",opened);
    const auto reopenedResult=capture::commitMidi(*f.store,*prepared);
    check(!reopenedResult.ok && digestProject(*f.store).text==before,"Capture cannot cross a reopened Store even with the same filename and NULL head");
    if(reopenedResult.ok)return;
    prepared=capture::prepareMidi(*f.store,source,spec(),error);OpRequest rename;rename.opType="track.rename";rename.payload={{"id",1},{"name","Changed"}};
    const bool renamed=OpJournal(*f.store).commit(rename).ok;const auto stale=capture::commitMidi(*f.store,*prepared);
    check(renamed && stale.stale,"captured NULL head rejects a newer edit");if(stale.ok)return;
    prepared=capture::prepareMidi(*f.store,source,spec(),error);const auto current=digestProject(*f.store).text;
    CommitOptions veto;veto.beforeCommit=[](SQLite::Database&,std::int64_t,std::string& why){why="test veto";return false;};
    const auto rejected=capture::commitMidi(*f.store,*prepared,veto);
    check(!rejected.ok && digestProject(*f.store).text==current,"late Capture rejection rolls back notes, controllers, clip and history together");if(rejected.ok)return;
    Fixture other;check(!capture::commitMidi(*other.store,*prepared).ok,"another project cannot commit a prepared Capture");
    unsigned queries=0;sqlite3_trace_v2(f.store->db().getHandle(),SQLITE_TRACE_STMT,[](unsigned,void* context,void*,void*)->int{++*static_cast<unsigned*>(context);return 0;},&queries);
    // The request is built before the callback, as its owner would build it; the refusal's own allocations
    // are checked here and taken back out, so the final total still holds every other callback to zero.
    const auto request=spec();const auto cost=returnedResultCost<CommitResult>();const auto allocated=allocations.load();
    bool refused=false;{audio::CallbackScope callback;refused=!capture::prepareMidi(*f.store,source,request,error) && !capture::commitMidi(*f.store,*prepared).ok;}
    sqlite3_trace_v2(f.store->db().getHandle(),0,nullptr,nullptr);
    const auto spent=allocations.exchange(allocated)-allocated;
    check(refused && queries==0 && error=="callback" && spent==cost,"owner preparation/commit refuse callback before SQL or allocation");
    {SQLite::Transaction outer(f.store->db(),SQLite::TransactionBehavior::DEFERRED);prepared=capture::prepareMidi(*f.store,source,spec(),error);
        check(prepared && sqlite3_get_autocommit(f.store->db().getHandle())==0,"preparation respects a caller-owned read transaction");}
    check(prepared && capture::commitMidi(*f.store,*prepared).ok,"fresh Capture preparation can commit after rollback or stale work");
    check(!capture::prepareMidi(*f.store,source,spec(),error),"occupied clip identity refuses preparation instead of overwrite");
}
void refusalCases() {
    Fixture f;const auto good=transcript();const auto before=digestProject(*f.store).text;std::string error;int refused=0;
    for(int which=0;which<17;++which) {
        auto source=good;auto request=spec();
        if(which==0)source.prefixDropped=true;
        if(which==1)source.events.clear();
        if(which==2)source.events[3].event.value=0.5;
        if(which==3)source.events[3].event.noteId=0;
        if(which==4)source.events[3].event.noteId=source.events[2].event.noteId;
        if(which==5)source.events[5].event.noteId=999;
        if(which==6)source.events[5].event.dim=72;
        if(which==7)source.events[5].event.channel=2;
        if(which==8)source.events[4].event.type=EventType::NoteExpression;
        if(which==9)source.events[4].serial+=1;
        if(which==10)source.events[3].sample=inputStart-1;
        if(which==11)request.endSample=inputStart+32;
        if(which==12)request.closeHeldNotes=false;
        if(which==13)source.events[4].event.value=std::numeric_limits<double>::quiet_NaN();
        if(which==14)source.events[0].event.paramId=128;
        if(which==15)source.noteChannels=static_cast<NoteChannelBase>(5);
        if(which==16)source.trackId=999;
        if(!capture::prepareMidi(*f.store,source,request,error) && !error.empty())++refused;
    }
    check(refused==17 && digestProject(*f.store).text==before && !History(*f.store).canUndo(),"truncation, nonrepresentable values, ambiguous ownership, bad clocks/ranges and invalid targets refuse without writes");
    f.store->db().exec("UPDATE tracks SET locked=1 WHERE id=1");check(!capture::prepareMidi(*f.store,good,spec(),error),"locked Capture target is refused");
    f.store->db().exec("UPDATE tracks SET locked=0 WHERE id=1;INSERT INTO tempo_map(id,pos_ticks,bpm,curve) VALUES(1,0,120,1)");
    check(!capture::prepareMidi(*f.store,good,spec(),error),"tempo ramps are named rather than silently remapping Capture time");
    f.store->db().exec("DELETE FROM tempo_map;INSERT INTO tempo_map(id,pos_ticks,bpm,curve) VALUES(1,0,0.1,0)");
    check(!capture::prepareMidi(*f.store,good,spec(),error),"a musical grid too coarse to preserve captured samples is refused");
}
template<class Mutate> void noteImage(Payload& packet,std::size_t index,Mutate mutate) {
    auto& bytes=packet["notes"]["bytes"].get_binary();NoteRecord record{};
    const auto offset=sizeof(StreamHeader)+index*sizeof(NoteRecord);std::memcpy(&record,bytes.data()+offset,sizeof record);
    mutate(record);std::memcpy(bytes.data()+offset,&record,sizeof record);
}
void nativePackets() {
    Fixture f;std::string error;const auto prepared=capture::prepareMidi(*f.store,transcript(),spec(),error);check(prepared.has_value(),"native packet fixture");if(!prepared)return;
    const auto before=digestProject(*f.store).text;int refused=0;
    for(int which=0;which<18;++which) {
        auto request=prepared->requests.front();auto& packet=request.payload;
        if(which==0)packet["clip"]["kind"]="audio";
        if(which==1)packet["clip"]["track"]=9;
        if(which==2)packet["clip"]["length"]=0;
        if(which==3)packet["controllers"][1]["id"]=packet["controllers"][0]["id"];
        if(which==4)packet["controllers"][1]["controller"]=7;
        if(which==5)packet["controllers"][0]["channel"]=UINT64_MAX;
        if(which==6)packet["notes"]["id"]=packet["controllers"][0]["id"];
        // A duplicate ID: the second record takes the first's (DEV-17's prefixed identities are not 1, 2, 3).
        if(which==7){std::uint64_t first=0;noteImage(packet,0,[&first](NoteRecord& n){first=n.note_id;});
            noteImage(packet,1,[first](NoteRecord& n){n.note_id=first;});}
        if(which==8){auto& bytes=packet["notes"]["bytes"].get_binary();StreamHeader header{};std::memcpy(&header,bytes.data(),sizeof header);header.version=2;std::memcpy(bytes.data(),&header,sizeof header);}
        if(which==9)packet["notes"]["bytes"].get_binary().pop_back();
        if(which==10){auto& bytes=packet["controllers"][0]["bytes"].get_binary();AutomationPoint point{};std::memcpy(&point,bytes.data()+sizeof(StreamHeader),sizeof point);point.value=std::numeric_limits<double>::quiet_NaN();std::memcpy(bytes.data()+sizeof(StreamHeader),&point,sizeof point);}
        if(which==11)noteImage(packet,0,[](NoteRecord& n){n.dur_ticks=0;});
        if(which==12)packet["controllers"][0]["future"]=1;
        if(which==13){packet.erase("notes");packet["controllers"]=Payload::array();}
        if(which==14)noteImage(packet,0,[](NoteRecord& n){n.flags=NoteFlags::Muted;});
        if(which==15)noteImage(packet,0,[](NoteRecord& n){n.probability=10001;});
        if(which==16)noteImage(packet,0,[](NoteRecord& n){n.key=255;});
        if(which==17)packet["unexpected"]=1;
        const auto committed=OpJournal(*f.store).commit(request);
        if(!committed.ok && digestProject(*f.store).text==before && OpJournal(*f.store).count()==0)++refused;
    }
    check(refused==18,"native Capture validates binary records, addresses, IDs and closed packets and rolls back every late refusal");
    f.store->db().exec("UPDATE tracks SET frozen=1 WHERE id=1");
    check(!capture::prepareMidi(*f.store,transcript(),spec(),error) && !OpJournal(*f.store).commit(prepared->requests.front()).ok,"both owner and native operation refuse a frozen target");
}
void largeCapture() {
    Fixture f;MidiMemorySnapshot source;source.inputId=8;source.trackId=1;source.sampleRate=48000;source.epoch=1;source.noteChannels=NoteChannelBase::Zero;
    constexpr std::size_t notes=10000;source.events.reserve(notes*2);source.endSample=inputStart+static_cast<std::int64_t>(notes*2);
    for(std::size_t index=0;index<notes;++index) {
        Event on;on.type=EventType::NoteOn;on.noteId=index+1;on.dim=static_cast<std::uint16_t>(index%128);on.value=100.0/127;
        source.events.push_back({inputStart+static_cast<std::int64_t>(index*2),index*2,on});
        auto off=on;off.type=EventType::NoteOff;off.value=0;source.events.push_back({inputStart+static_cast<std::int64_t>(index*2+1),index*2+1,off});
    }
    auto request=spec();request.endSample=source.endSample;std::string error;const auto prepared=capture::prepareMidi(*f.store,source,request,error);
    check(prepared && prepared->notes==notes && prepared->requests.size()==1,"ten thousand captured notes prepare one bounded binary stream operation");if(!prepared)return;
    unsigned writes=0;
    sqlite3_trace_v2(f.store->db().getHandle(),SQLITE_TRACE_STMT,[](unsigned,void* context,void*,void* sql)->int {
        const auto* text=static_cast<const char*>(sql);if(text && std::string_view(text).starts_with("INSERT INTO event_streams("))++*static_cast<unsigned*>(context);return 0;
    },&writes);
    const auto committed=capture::commitMidi(*f.store,*prepared);sqlite3_trace_v2(f.store->db().getHandle(),0,nullptr,nullptr);
    check(committed.ok && committed.seqs.size()==1 && writes==1,"large Capture writes its notes stream once rather than rewriting it per note");
    const auto bytes=f.store->getEventStream(7,"notes");bool exact=false;
    if(bytes){StreamReader<NoteRecord> reader(*bytes,FourCC::Notes);exact=reader.ok() && reader.count()==notes && reader.at(9999)->note_id==10000;}
    check(exact && History(*f.store).undo().ok && f.store->db().execAndGet("SELECT count(*) FROM event_streams").getInt()==0 && History(*f.store).redo().ok,
        "large Capture retains every identity and one undo/redo restores the complete stream");
}
}
int main(){try{nativeHistory();expressionHistory();channelPitchHistory();liveHistoryConsumption();clocksAndSourceConventions();sampleOriginRounding();guardsAndTransactions();refusalCases();nativePackets();largeCapture();check(allocations==0 && audio::callbackFileIo==0,"actual input/Capture playback and callback refusal allocate nothing and perform no callback file I/O");}
catch(const std::exception& e){std::printf("EXCEPTION: %s\n",e.what());check(false,"fixture must complete");}
std::printf("midi_capture: %d checks, %d failures\n",checks,failures);return failures?1:0;}
