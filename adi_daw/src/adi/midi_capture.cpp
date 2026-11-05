// SPDX-License-Identifier: AGPL-3.0-or-later
#include "adi/midi_capture.hpp"
#include "adi/engine/snapshot.hpp"
#include "adi/audio/io_audit.hpp"
#include "adi/store.hpp"
#include <SQLiteCpp/SQLiteCpp.h>
#include <sqlite3.h>
#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <set>
#include <stdexcept>
namespace adi::capture {
namespace {
constexpr std::int64_t limit=std::int64_t{1}<<50;
int velocity(double value,bool onset) {
    if(!std::isfinite(value) || value<0 || value>1 || std::signbit(value) || (onset && value==0))
        throw std::runtime_error("invalid captured note velocity");
    const auto word=std::lround(value*127);
    if(static_cast<double>(word)/127!=value)
        throw std::runtime_error("captured note velocity needs a higher precision native storage contract");
    return static_cast<int>(word);
}
engine::TempoMap tempoOf(Store& store) {
    engine::TempoMap tempo;SQLite::Statement q(store.db(),"SELECT pos_ticks,bpm,curve FROM tempo_map ORDER BY pos_ticks,id");
    while(q.executeStep()) {
        const auto tick=q.getColumn(0).getInt64();const auto bpm=q.getColumn(1).getDouble();
        if(tick<0 || !std::isfinite(bpm) || bpm<=0 || bpm>=1000 || q.getColumn(2).getInt()!=0 ||
           (!tempo.events.empty() && tick<=tempo.events.back().posTicks))throw std::runtime_error("MIDI Capture requires an unambiguous valid step-tempo map");
        tempo.events.push_back({tick,bpm,0});
    }
    if(tempo.events.empty() || tempo.events.front().posTicks!=0)tempo.events.insert(tempo.events.begin(),{0,120,0});
    return tempo;
}
struct Clock {
    engine::TempoMap tempo;
    std::int64_t originTick=0,originSample=0;
    double rate=0,originSeconds=0,maxSeconds=0;
    Clock(engine::TempoMap map,std::int64_t tick,std::uint32_t sampleRate):tempo(std::move(map)),originTick(tick),rate(sampleRate) {
        originSeconds=tempo.ticksToSeconds(tick);maxSeconds=tempo.ticksToSeconds(limit);
        const auto sample=originSeconds*rate;
        if(!std::isfinite(sample) || sample<0 || sample>static_cast<double>(limit) || !std::isfinite(maxSeconds))
            throw std::runtime_error("MIDI Capture placement exceeds supported sample precision");
        originSample=std::llround(sample);
        if(at(0)!=0)throw std::runtime_error("MIDI Capture origin cannot preserve its tick");
    }
    std::int64_t at(std::int64_t relative) const {
        if(relative<0 || relative>limit-originSample)throw std::runtime_error("MIDI Capture extent exceeds supported sample precision");
        const auto seconds=originSeconds+static_cast<double>(relative)/rate;
        if(!std::isfinite(seconds) || seconds>maxSeconds)throw std::runtime_error("MIDI Capture extent exceeds supported tick precision");
        const auto tick=tempo.secondsToTicks(seconds);const auto sample=tempo.ticksToSeconds(tick)*rate;
        if(tick<originTick || tick>limit || !std::isfinite(sample) || sample<0 || sample>static_cast<double>(limit) ||
           std::llround(sample)-originSample!=relative)throw std::runtime_error("MIDI Capture boundary cannot roundtrip its exact sample");
        return tick-originTick;
    }
};
struct Note {std::uint64_t id=0;std::int64_t start=0,end=0;int key=0,channel=0,on=0,off=0;};
int channel(const engine::Event& e,engine::NoteChannelBase base) {
    const int result=static_cast<int>(e.channel)-(base==engine::NoteChannelBase::One?1:0);
    if(result<0 || result>15)throw std::runtime_error("captured note channel disagrees with its declared convention");
    return result;
}
std::int64_t positive(const Payload& value) {
    if(!value.is_number_integer() || (value.is_number_unsigned() && value.get<std::uint64_t>()>static_cast<std::uint64_t>(INT64_MAX)))
        throw std::runtime_error("Capture identity must be a positive int64");
    const auto result=value.get<std::int64_t>();if(result<=0)throw std::runtime_error("Capture identity must be positive");return result;
}
std::span<const std::byte> binary(const Payload& packet) {
    const auto& data=packet.at("bytes");if(!data.is_binary())throw std::runtime_error("Capture stream must contain captured binary bytes");
    const auto& bytes=data.get_binary();return {reinterpret_cast<const std::byte*>(bytes.data()),bytes.size()};
}
Payload encoded(std::span<const std::byte> bytes) {
    std::vector<std::uint8_t> data;data.reserve(bytes.size());for(auto value:bytes)data.push_back(std::to_integer<std::uint8_t>(value));return Payload::binary(std::move(data));
}
int addressPart(const Payload& value,int maximum) {
    if(!value.is_number_integer() || (value.is_number_unsigned() && value.get<std::uint64_t>()>static_cast<std::uint64_t>(maximum)))
        throw std::runtime_error("invalid captured controller address");
    const auto number=value.get<std::int64_t>();
    if(number<0 || number>maximum)throw std::runtime_error("captured controller address is outside its domain");
    return static_cast<int>(number);
}
template<class Record> StreamReader<Record> stream(std::span<const std::byte> bytes,FourCC type) {
    StreamReader<Record> reader(bytes,type);
    if(!reader.ok() || reader.version()!=1 || reader.recSize()!=sizeof(Record) || reader.header().flags!=StreamFlags::SortedByTime ||
       reader.count()==0 || reader.count()>(1u<<20) || bytes.size()!=sizeof(StreamHeader)+static_cast<std::size_t>(reader.count())*sizeof(Record))
        throw std::runtime_error("Capture requires a complete known sorted stream layout");
    return reader;
}
void insertStream(OpContext& context,std::int64_t clip,std::int64_t id,std::span<const std::byte> bytes,
    std::optional<std::pair<int,int>> control) {
    SQLite::Statement q(context.db,"INSERT INTO event_streams(id,clip_id,stream_kind,channel,controller,data) VALUES(?,?,?,?,?,?)");
    q.bind(1,id);q.bind(2,clip);q.bind(3,control?"cc":"notes");
    if(control){q.bind(4,control->first);q.bind(5,control->second);}else{q.bind(4);q.bind(5);}
    q.bind(6,bytes.data(),static_cast<int>(bytes.size()));q.exec();
}
bool applyCapture(OpContext& context,const Payload& payload,std::string& error) {
    try {
        const auto& clip=payload.at("clip");const auto* create=OpRegistry::instance().find("clip.create");
        if(!create || !validateSubmission(*create,clip).empty() || clip.at("kind")!="midi")throw std::runtime_error("Capture needs a valid explicit MIDI clip row");
        const auto id=positive(clip.at("id")),trackId=positive(clip.at("track"));
        const auto position=clip.at("pos").get<std::int64_t>(),length=clip.at("length").get<std::int64_t>();
        if(position<0 || position>limit || length<=0 || length>limit-position)throw std::runtime_error("invalid Capture clip extent");
        SQLite::Statement track(context.db,"SELECT kind,locked,frozen FROM tracks WHERE id=?");track.bind(1,trackId);
        if(!track.executeStep() || (track.getColumn(0).getString()!="midi" && track.getColumn(0).getString()!="audio") ||
           track.getColumn(1).getInt()!=0 || track.getColumn(2).getInt()!=0)throw std::runtime_error("Capture target is gone, locked or frozen");
        const auto& controls=payload.at("controllers");
        if(!controls.is_array() || controls.size()>2048 || (controls.empty() && !payload.contains("notes")))throw std::runtime_error("invalid Capture stream collection");
        if(!create->apply(context,clip,error))return false;
        std::set<std::int64_t> ids;std::set<std::pair<int,int>> addresses;std::size_t count=0;
        for(const auto& lane:controls) {
            if(!lane.is_object() || lane.size()!=4)throw std::runtime_error("invalid captured controller packet");
            const auto streamId=positive(lane.at("id"));const auto chan=addressPart(lane.at("channel"),15),cc=addressPart(lane.at("controller"),127);
            if(chan<0 || chan>15 || cc<0 || cc>127 || !ids.insert(streamId).second || !addresses.insert({chan,cc}).second)
                throw std::runtime_error("duplicate or invalid Capture stream address");
            const auto bytes=binary(lane);const auto reader=stream<AutomationPoint>(bytes,FourCC::Controller);
            if(reader.count()>(1u<<20)-count)throw std::runtime_error("Capture point budget exceeded");
            count+=reader.count();
            std::set<std::uint64_t> points;std::int64_t previous=0;
            for(std::uint32_t index=0;index<reader.count();++index) {
                const auto point=*reader.at(index);
                if(!point.point_id || !points.insert(point.point_id).second || point.time<previous || point.time<0 || point.time>=length ||
                   !std::isfinite(point.value) || point.value<0 || point.value>1 || !std::isfinite(point.tension) || point.tension<-1 || point.tension>1 ||
                   point.curve>5 || point.flags || point.reserved)throw std::runtime_error("invalid captured controller point");
                previous=point.time;
            }
            insertStream(context,id,streamId,bytes,{{chan,cc}});
        }
        if(payload.contains("notes")) {
            const auto& packet=payload.at("notes");if(!packet.is_object() || packet.size()!=2)throw std::runtime_error("invalid captured notes packet");
            const auto streamId=positive(packet.at("id"));if(!ids.insert(streamId).second)throw std::runtime_error("duplicate Capture notes stream identity");
            const auto bytes=binary(packet);const auto reader=stream<NoteRecord>(bytes,FourCC::Notes);
            if(reader.count()>(1u<<20)-count)throw std::runtime_error("Capture note budget exceeded");
            std::set<std::uint64_t> notes;std::int64_t previous=0;
            for(std::uint32_t index=0;index<reader.count();++index) {
                const auto note=*reader.at(index);
                if(!note.note_id || !notes.insert(note.note_id).second || note.start_ticks<previous || note.start_ticks<0 || note.start_ticks>=length ||
                   note.dur_ticks<=0 || note.dur_ticks>length-note.start_ticks || note.key>127 || note.channel>15 || note.vel_on<1 || note.vel_on>127 || note.vel_off>127 ||
                   note.probability>10000 || !std::isfinite(note.tuning_cents) || note.tuning_cents<-1200 || note.tuning_cents>1200 || note.flags || note.reserved)
                    throw std::runtime_error("invalid captured note record");
                previous=note.start_ticks;
            }
            insertStream(context,id,streamId,bytes,{});
        }
        return true;
    }catch(const std::exception& exception){error=exception.what();return false;}
}
bool inverseCapture(OpContext&,const Payload& payload,Payload& inverse,std::string& error) {
    try{inverse={{"id",positive(payload.at("clip").at("id"))}};return true;}
    catch(const std::exception& exception){error=exception.what();return false;}
}
}
std::optional<PreparedMidi> prepareMidi(Store& store,const engine::MidiMemorySnapshot& source,const MidiSpec& spec,std::string& error) {
    if(audio::onAudioThread){error="callback";return std::nullopt;}
    try {
        if(store.readOnly())throw std::runtime_error("project is read-only");
        if(spec.clip<=0 || spec.positionTicks<0 || spec.positionTicks>limit || spec.firstSample<0 || spec.endSample<=spec.firstSample ||
           spec.endSample>source.endSample || source.inputId<=0 || source.trackId<=0 || source.epoch==0 || source.sampleRate<8000 ||
           source.sampleRate>768000 || source.events.empty() || source.events.size()>(1u<<20) || source.prefixDropped ||
           (source.noteChannels!=engine::NoteChannelBase::Zero && source.noteChannels!=engine::NoteChannelBase::One))
            throw std::runtime_error("MIDI Capture needs a complete input history, explicit range and new clip placement");
        std::optional<SQLite::Transaction> transaction;
        if(sqlite3_get_autocommit(store.db().getHandle())!=0)transaction.emplace(store.db(),SQLite::TransactionBehavior::DEFERRED);
        PreparedMidi result;result.owner_=&store;result.client_=store.clientId();result.head=OpJournal(store).headSeq();
        SQLite::Statement track(store.db(),"SELECT kind,locked,frozen FROM tracks WHERE id=?");track.bind(1,source.trackId);
        if(!track.executeStep() || (track.getColumn(0).getString()!="midi" && track.getColumn(0).getString()!="audio") || track.getColumn(1).getInt()!=0 || track.getColumn(2).getInt()!=0)
            throw std::runtime_error("MIDI Capture target is missing, locked, frozen or not a regular track");
        SQLite::Statement occupied(store.db(),"SELECT 1 FROM clips WHERE id=?");occupied.bind(1,spec.clip);
        if(occupied.executeStep())throw std::runtime_error("MIDI Capture clip ID is already occupied");
        const Clock clock(tempoOf(store),spec.positionTicks,source.sampleRate);
        const auto length=clock.at(spec.endSample-spec.firstSample);
        if(length<=0)throw std::runtime_error("MIDI Capture has no representable extent");
        std::vector<Note> notes;std::map<std::uint64_t,std::size_t> active;std::set<std::uint64_t> seen;
        std::map<std::pair<int,int>,std::vector<AutomationPoint>> controls;
        std::int64_t previous=spec.firstSample;std::optional<std::uint64_t> priorSerial;
        for(const auto& remembered:source.events) {
            const auto& e=remembered.event;
            if(remembered.sample<previous || remembered.sample<spec.firstSample || remembered.sample>=spec.endSample ||
               remembered.serial==UINT64_MAX || (priorSerial && (remembered.serial<=*priorSerial || remembered.serial-*priorSerial!=1)) ||
               e.frame<0 || !std::isfinite(e.value))throw std::runtime_error("MIDI Capture history has a gap, invalid event or mismatched source range");
            previous=remembered.sample;priorSerial=remembered.serial;
            const auto time=clock.at(remembered.sample-spec.firstSample);
            if(e.type==engine::EventType::Controller) {
                if(e.channel>15 || e.paramId>127 || e.value<0 || e.value>1)throw std::runtime_error("invalid captured controller");
                AutomationPoint point{};point.point_id=remembered.serial+1;point.time=time;point.value=e.value;
                controls[{e.channel,static_cast<int>(e.paramId)}].push_back(point);
                continue;
            }
            if(e.type!=engine::EventType::NoteOn && e.type!=engine::EventType::NoteOff)
                throw std::runtime_error("captured expression or addressed event has no exact native Capture path yet");
            if(!e.noteId || e.dim>127)throw std::runtime_error("MIDI Capture refuses unassigned note ownership or invalid keys");
            const auto chan=channel(e,source.noteChannels);const auto vel=velocity(e.value,e.type==engine::EventType::NoteOn);
            if(e.type==engine::EventType::NoteOn) {
                if(!seen.insert(e.noteId).second)throw std::runtime_error("MIDI Capture note identity was reused");
                active[e.noteId]=notes.size();notes.push_back({e.noteId,time,0,e.dim,chan,vel,0});
            } else {
                const auto found=active.find(e.noteId);
                if(found==active.end())throw std::runtime_error("MIDI Capture release has no retained note-on");
                auto& note=notes[found->second];
                if(note.key!=e.dim || note.channel!=chan || time<=note.start)throw std::runtime_error("MIDI Capture release mismatches its note or has zero duration");
                note.end=time;note.off=vel;active.erase(found);
            }
        }
        if(!active.empty() && !spec.closeHeldNotes)throw std::runtime_error("MIDI Capture has held notes without an allowed closing boundary");
        for(const auto& [id,index]:active){(void)id;notes[index].end=length;++result.closedHeldNotes;}
        OpRequest create;create.opType="capture.createMidi";create.payload={{"clip",{{"id",spec.clip},{"track",source.trackId},{"kind","midi"},{"name",spec.name},{"pos",spec.positionTicks},{"length",length}}},{"controllers",Payload::array()}};
        auto streamId=std::max<std::int64_t>(0,store.db().execAndGet("SELECT coalesce(max(id),0) FROM event_streams").getInt64());
        for(const auto& [address,points]:controls) {
            if(streamId==std::numeric_limits<std::int64_t>::max())throw std::runtime_error("no MIDI Capture stream identity available");
            const auto bytes=writeStream<AutomationPoint>(FourCC::Controller,points,1,StreamFlags::SortedByTime);
            create.payload["controllers"].push_back({{"id",++streamId},{"channel",address.first},{"controller",address.second},{"bytes",encoded(bytes)}});
        }
        if(!notes.empty() && streamId==std::numeric_limits<std::int64_t>::max())throw std::runtime_error("no MIDI Capture notes-stream identity available");
        std::vector<NoteRecord> records;records.reserve(notes.size());
        for(const auto& note:notes) {
            if(note.end<=note.start)throw std::runtime_error("captured note has no representable duration");
            NoteRecord record{};record.note_id=note.id;record.start_ticks=note.start;record.dur_ticks=note.end-note.start;
            record.key=static_cast<std::uint8_t>(note.key);record.channel=static_cast<std::uint8_t>(note.channel);record.vel_on=static_cast<std::uint8_t>(note.on);record.vel_off=static_cast<std::uint8_t>(note.off);record.probability=10000;
            records.push_back(record);
        }
        if(!records.empty()) {
            std::sort(records.begin(),records.end(),[](const auto& a,const auto& b){if(a.start_ticks!=b.start_ticks)return a.start_ticks<b.start_ticks;if(a.key!=b.key)return a.key<b.key;return a.note_id<b.note_id;});
            const auto bytes=writeStream<NoteRecord>(FourCC::Notes,records,1,StreamFlags::SortedByTime);
            create.payload["notes"]={{"id",++streamId},{"bytes",encoded(bytes)}};
        }
        result.requests.push_back(std::move(create));
        result.notes=notes.size();result.controllerLanes=controls.size();
        for(auto& request:result.requests){request.label="Capture MIDI";request.targetKind="clip";request.targetId=spec.clip;}
        if(transaction)transaction->commit();
        error.clear();return result;
    }catch(const std::exception& exception){error=exception.what();return std::nullopt;}
}
CommitResult commitMidi(Store& store,const PreparedMidi& prepared,const CommitOptions& supplied) {
    if(audio::onAudioThread){CommitResult result;result.error="callback";return result;}
    if(prepared.owner_!=&store || prepared.client_!=store.clientId() || prepared.requests.empty()) {
        CommitResult result;result.error="MIDI Capture belongs to another open document or callback";return result;
    }
    auto options=supplied;options.expectHead.emplace(prepared.head);return OpJournal(store).commit(prepared.requests,options);
}
std::span<const OpDescriptor> midiOps() {
    static constexpr Field fields[]={{"clip",FieldType::Object},{"controllers",FieldType::Array},{"notes",FieldType::Object,false}};
    static constexpr OpDescriptor ops[]={{"capture.createMidi","Create a MIDI Capture clip from captured streams",Scope::Edit,EngineImpact::GraphRebuild,
        fields,false,false,applyCapture,inverseCapture,"clip.delete"}};
    return ops;
}
} // namespace adi::capture
