// SPDX-License-Identifier: GPL-3.0-or-later
#include "temp_directory.hpp"
#include "adi/blob.hpp"
#include "adi/digest.hpp"
#include "adi/history.hpp"
#include "adi/ops.hpp"
#include "adi/store.hpp"

#include <SQLiteCpp/SQLiteCpp.h>
#include <array>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <limits>
#include <stdexcept>

namespace {
using namespace adi;
constexpr auto wanted=std::numeric_limits<std::uint64_t>::max();
int checks=0,failures=0;
void check(bool ok,const std::string& what) {
    ++checks;if(!ok){++failures;std::printf("FAIL %s\n",what.c_str());}
}
NoteRecord note(std::uint64_t id,std::int64_t start,std::uint16_t flags) {
    NoteRecord n{};n.note_id=id;n.start_ticks=start;n.dur_ticks=5765760;n.key=60;n.vel_on=87;n.vel_off=37;
    n.channel=2;n.flags=flags;n.probability=8100;n.tuning_cents=23.5f;return n;
}
std::vector<std::byte> stream(bool future=false) {
    const std::array notes={note(wanted,0,0x8007),note(17,5765760,NoteFlags::TiedToNext)};
    if(!future)return writeStream<NoteRecord>(FourCC::Notes,notes);
    StreamHeader header{static_cast<std::uint32_t>(FourCC::Notes),3,48,2,StreamFlags::SortedByTime};
    std::vector<std::byte> bytes(sizeof header+96+7,std::byte{0x53});
    std::memcpy(bytes.data(),&header,sizeof header);
    for(std::size_t i=0;i<notes.size();++i)std::memcpy(bytes.data()+sizeof header+i*48,&notes[i],sizeof(NoteRecord));
    return bytes;
}
struct Fixture {
    test::TempDirectory temp;
    std::unique_ptr<Store> store;
    explicit Fixture(const char* name,bool future=false):temp("note_flags",name) {
        StoreError error=StoreError::Ok;store=Store::create(temp.path()/"song.adi",error);
        if(!store)throw std::runtime_error(toString(error));
        store->db().exec("INSERT INTO tracks(id,kind,name) VALUES(1,'midi','Keys')");
        store->db().exec("INSERT INTO clips(id,track_id,kind,pos_ticks,length_ticks) VALUES(1,1,'midi',0,23063040)");
        if(!store->putEventStream(1,"notes",stream(future)))throw std::runtime_error(store->lastError());
    }
    std::vector<std::byte> bytes() {return *store->getEventStream(1,"notes");}
};
OpRequest request(Payload payload) {OpRequest op;op.opType="note.setMute";op.payload=std::move(payload);return op;}
CommitResult mute(Store& store,bool muted,std::uint64_t id=wanted) {
    return OpJournal(store).commit(request({{"clip",1},{"note",id},{"muted",muted}}));
}
void exactBit(bool future) {
    Fixture f(future?"future":"native",future);
    f.store->db().exec("ALTER TABLE event_streams ADD COLUMN extension TEXT");
    f.store->db().exec("UPDATE event_streams SET extension='future column'");
    f.store->db().exec("INSERT INTO event_streams(id,clip_id,stream_kind,channel,controller,data) VALUES(77,1,'cc',2,7,X'0011FF')");
    const auto before=digestProject(*f.store).text;const auto original=f.bytes();
    const auto id=f.store->db().execAndGet("SELECT id FROM event_streams WHERE stream_kind='notes'").getInt64();
    const auto* descriptor=OpRegistry::instance().find("note.setMute");
    check(descriptor && descriptor->scope==Scope::Edit && descriptor->engineImpact==EngineImpact::Snapshot &&
              !descriptor->ephemeral && descriptor->buildInverse,"mute is an undoable snapshot edit");
    check(mute(*f.store,false).ok,"activate an inactive note with a full uint64 id");
    auto expected=original;
    // ANOT flags low byte is offset28, after its 16-byte header. Only bit0.
    expected[44]^=std::byte{1};
    check(f.bytes()==expected,"only the target's muted bit changes; original extent/header/tails retained");
    // Keep the bytes alive while inspecting a reader (StreamReader is a view).
    const auto changed=f.bytes();const StreamReader<NoteRecord> stable(changed,FourCC::Notes);
    check(stable.ok() && stable.count()==2,"edited stream remains readable with its original note count");
    if(!stable.ok() || stable.count()!=2)return;
    check(stable.at(0)->flags==0x8006 && stable.at(1)->flags==NoteFlags::TiedToNext &&
              stable.at(0)->probability==8100 && stable.at(0)->tuning_cents==23.5f,
          "all other flags, other notes and expressive values survive");
    check(f.store->db().execAndGet("SELECT id FROM event_streams WHERE stream_kind='notes'").getInt64()==id &&
              f.store->db().execAndGet("SELECT extension FROM event_streams WHERE stream_kind='notes'").getString()=="future column" &&
              f.store->db().execAndGet("SELECT hex(data) FROM event_streams WHERE id=77").getString()=="0011FF",
          "update retains stream row identity, unknown columns and other streams");
    const auto after=digestProject(*f.store).text;
    check(History(*f.store).undo().ok && f.bytes()==original && digestProject(*f.store).text==before,"exact undo including future bytes");
    check(History(*f.store).redo().ok && f.bytes()==expected && digestProject(*f.store).text==after,"exact redo");
    check(mute(*f.store,true).ok && f.bytes()==original,"deactivate sets bit0 without disturbing selected/unknown flags");
    check(History(*f.store).undo().ok && f.bytes()==expected,"undo deactivation activates again");
    const auto path=f.store->path();f.store.reset();StoreError error=StoreError::Ok;f.store=Store::open(path,error);
    check(f.store && History(*f.store).redo().ok && f.bytes()==original,"flag history replays after reopen");
    check(mute(*f.store,true,17).ok,"mute the second record by its own identity");
    auto second=original;
    second[sizeof(StreamHeader)+(future?48u:40u)+28u]^=std::byte{1};
    check(f.bytes()==second && History(*f.store).undo().ok && f.bytes()==original,
          "second record uses its stored stride and undoes without changing its future tail");
    if(future)check(stable.header().version==3 && stable.header().rec_size==48,"future stream version/stride are retained");
}

void refusedEdits() {
    Fixture f("invalid");
    const auto before=digestProject(*f.store).text;const auto count=OpJournal(*f.store).count();
    const std::array bad={
        request({{"clip",1},{"note",999},{"muted",true}}),
        request({{"clip",1},{"note",0},{"muted",true}}),
        request({{"clip",1},{"note",-1},{"muted",true}}),
        request({{"clip",wanted},{"note",wanted},{"muted",true}}),
        request({{"clip",999},{"note",wanted},{"muted",true}}),
        request({{"clip",1},{"note",wanted},{"muted",1}}),
        request({{"clip",1},{"note",wanted},{"muted",true},{"flags",0}})
    };
    for(const auto& op:bad)check(!OpJournal(*f.store).commit(op).ok && digestProject(*f.store).text==before &&
                                OpJournal(*f.store).count()==count,"invalid target/shape leaves all state and log intact");
    const std::array batch={request({{"clip",1},{"note",wanted},{"muted",false}}),bad[0]};
    check(!OpJournal(*f.store).commit(batch).ok && digestProject(*f.store).text==before && OpJournal(*f.store).count()==count,
          "failed second op rolls back the first bit and audit");
    f.store->db().exec("INSERT INTO clips(id,track_id,kind,pos_ticks,length_ticks) VALUES(2,1,'midi',0,4)");
    const auto missing=OpJournal(*f.store).commit(request({{"clip",2},{"note",17},{"muted",true}}));
    check(!missing.ok && !f.store->getEventStream(2,"notes"),"missing stream is refused without creating an empty one");
    f.store->db().exec("UPDATE clips SET kind='audio' WHERE id=1");
    check(!mute(*f.store,true).ok && f.bytes()==stream(),"non-MIDI owner refused without changing bytes");
}

void malformedStreams() {
    Fixture f("streams");
    auto duplicate=stream();
    std::memcpy(duplicate.data()+sizeof(StreamHeader)+sizeof(NoteRecord)+offsetof(NoteRecord,note_id),&wanted,sizeof wanted);
    std::vector<std::vector<std::byte>> bad;
    bad.push_back(duplicate);bad.push_back({std::byte{1}});
    auto truncated=stream();truncated.pop_back();bad.push_back(truncated);
    auto wrongStride=stream();StreamHeader header{};std::memcpy(&header,wrongStride.data(),sizeof header);header.rec_size=39;
    std::memcpy(wrongStride.data(),&header,sizeof header);bad.push_back(wrongStride);
    auto wrongUnits=stream();std::memcpy(&header,wrongUnits.data(),sizeof header);header.flags|=StreamFlags::TimeIsNanos;
    std::memcpy(wrongUnits.data(),&header,sizeof header);bad.push_back(wrongUnits);
    for(const auto& bytes:bad) {
        check(f.store->putEventStream(1,"notes",bytes),"malformed fixture stored");
        const auto before=digestProject(*f.store).text;const auto count=OpJournal(*f.store).count();
        check(!mute(*f.store,false).ok && f.bytes()==bytes && digestProject(*f.store).text==before && OpJournal(*f.store).count()==count,
              "ambiguous/unreadable stream refused without lossy rewrite");
    }
    check(f.store->putEventStream(1,"notes",stream()),"restore valid stream");
    const auto path=f.store->path();f.store.reset();StoreError error=StoreError::Ok;f.store=Store::open(path,error,true);
    check(f.store && !mute(*f.store,false).ok && f.bytes()==stream(),"read-only project cannot mutate flag");
}
} // namespace
int main() {
    try {exactBit(false);exactBit(true);refusedEdits();malformedStreams();}
    catch(const std::exception& error){check(false,std::string("exception: ")+error.what());}
    std::printf("%d checks, %d failures\n",checks,failures);return failures?1:0;
}
