// SPDX-License-Identifier: GPL-3.0-or-later
#include "temp_directory.hpp"
#include "adi/digest.hpp"
#include "adi/history.hpp"
#include "adi/ops.hpp"
#include "adi/store.hpp"
#include "adi/store_rows.hpp"
#include "adi/textproj_store.hpp"

#include <SQLiteCpp/SQLiteCpp.h>
#include <array>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <limits>
#include <stdexcept>

namespace {
using namespace adi;
int checks = 0, failures = 0;
void check(bool ok, const std::string& what) {
    ++checks;
    if (!ok) { ++failures; std::printf("FAIL %s\n",what.c_str()); }
}
struct Fixture {
    test::TempDirectory temp;
    std::unique_ptr<Store> store;
    explicit Fixture(const char* name) : temp("clip_properties",name) {
        StoreError error = StoreError::Ok;
        store = Store::create(temp.path()/"song.adi",error);
        if (!store) throw std::runtime_error(toString(error));
        store->db().exec("INSERT INTO tracks(id,kind,name) VALUES(1,'midi','Keys')");
        store->db().exec("INSERT INTO clips(id,track_id,kind,name,color,pos_ticks,length_ticks,content_offset_ticks) VALUES(1,1,'midi','Phrase',0,13,200,5)");
    }
    SQLite::Database& db() { return store->db(); }
};
OpRequest request(const char* type,Payload payload) {
    OpRequest op; op.opType=type;op.payload=std::move(payload);op.label=type;return op;
}
bool edit(Store& store,const char* type,Payload payload) {
    const auto result=OpJournal(store).commit(request(type,std::move(payload)));
    if(!result.ok)std::printf("  %s: %s\n",type,result.error.c_str());
    return result.ok;
}
std::string state(Store& store) { return digestProject(store).text; }
void roundTrip(Store& store,const char* type,Payload payload) {
    const auto before=state(store);
    check(edit(store,type,std::move(payload)),std::string(type)+" commits");
    const auto after=state(store);
    check(History(store).undo().ok && state(store)==before,std::string(type)+" exact undo");
    check(History(store).redo().ok && state(store)==after,std::string(type)+" exact redo");
}
std::string readFile(const std::filesystem::path& path) {
    std::ifstream input(path,std::ios::binary);
    if(!input)throw std::runtime_error("cannot read fixture");
    return {std::istreambuf_iterator<char>(input),std::istreambuf_iterator<char>()};
}

void valuesAndUndo() {
    Fixture f("values");
    for(const char* name:{"clip.setStartMarker","clip.setEndMarker","clip.setLoop","clip.setTimeSignature","clip.setScale"}) {
        const auto* op=OpRegistry::instance().find(name);
        check(op && op->scope==Scope::Edit && op->engineImpact==EngineImpact::Snapshot && !op->ephemeral && op->buildInverse,
              std::string(name)+" snapshot edit contract");
    }
    roundTrip(*f.store,"clip.setStartMarker",{{"id",1},{"ticks",17}});
    auto model=rows::readModel(*f.store);
    check(model.problems.empty() && model.clips[0].contentOffsetTicks==17 && model.clips[0].endMarkerTicks==205 &&
              model.clips[0].posTicks==13 && model.clips[0].lengthTicks==200,"Start pins previous End, preserving placement and visible length");
    roundTrip(*f.store,"clip.setEndMarker",{{"id",1},{"ticks",100}});
    roundTrip(*f.store,"clip.setLoop",{{"id",1},{"on",true},{"start",20},{"length",50}});
    model=rows::readModel(*f.store);
    check(model.clips[0].loopEnabled && model.clips[0].loopStartTicks==20 && model.clips[0].loopLenTicks==50,"loop switch and window stored atomically");
    roundTrip(*f.store,"clip.setLoop",{{"id",1},{"on",false}});
    roundTrip(*f.store,"clip.setTimeSignature",{{"id",1},{"num",7},{"den",8}});
    roundTrip(*f.store,"clip.setScale",{{"id",1},{"root",2},{"scale","minor"},{"mask",1453}});
    model=rows::readModel(*f.store);
    check(model.clips[0].signatureNumerator==7 && model.clips[0].signatureDenominator==8 && model.clips[0].scaleRoot==2 &&
              model.clips[0].scaleId=="minor" && model.clips[0].scaleMask==1453,"local signature and scale readable from model");
    const auto projection=textproj::projectStore(*f.store).text;
    check(projection.find("signature 7/8")!=std::string::npos && projection.find("scaleRoot 2")!=std::string::npos &&
              projection.find("scale minor")!=std::string::npos && projection.find("endMarker ")!=std::string::npos,"projection includes local clip properties");
    roundTrip(*f.store,"clip.setTimeSignature",{{"id",1}});
    roundTrip(*f.store,"clip.setScale",{{"id",1}});
    model=rows::readModel(*f.store);
    check(!model.clips[0].signatureNumerator && !model.clips[0].signatureDenominator && !model.clips[0].scaleRoot &&
              !model.clips[0].scaleId && !model.clips[0].scaleMask,"clear restores NULL inheritance");
    f.db().exec("UPDATE clips SET loop_start_ticks=-5,loop_len_ticks=-3");
    roundTrip(*f.store,"clip.setLoop",{{"id",1},{"on",false}});
    check(f.db().execAndGet("SELECT loop_enabled=0 AND loop_start_ticks=-5 AND loop_len_ticks=-3 FROM clips WHERE id=1").getInt()==1,
          "a malformed imported loop can still be switched off");
}

void validation() {
    Fixture f("bad");
    const auto before=state(*f.store);const auto count=OpJournal(*f.store).count();
    const std::array bad={
        request("clip.setStartMarker",{{"id",1},{"ticks",205}}),
        request("clip.setStartMarker",{{"id",1},{"ticks",-1}}),
        request("clip.setEndMarker",{{"id",1},{"ticks",5}}),
        request("clip.setEndMarker",{{"id",1},{"ticks",std::numeric_limits<std::uint64_t>::max()}}),
        request("clip.setLoop",{{"id",1},{"on",true}}),
        request("clip.setLoop",{{"id",1},{"on",true},{"length",0}}),
        request("clip.setLoop",{{"id",1},{"on",true},{"start",-1},{"length",50}}),
        request("clip.setLoop",{{"id",1},{"on",true},{"start",std::int64_t{1}<<50},{"length",50}}),
        request("clip.setTimeSignature",{{"id",1},{"num",3}}),
        request("clip.setTimeSignature",{{"id",1},{"num",0},{"den",4}}),
        request("clip.setTimeSignature",{{"id",1},{"num",3},{"den",3}}),
        request("clip.setScale",{{"id",1},{"root",2}}),
        request("clip.setScale",{{"id",1},{"root",12},{"scale","major"},{"mask",2741}}),
        request("clip.setScale",{{"id",1},{"root",2},{"scale",""},{"mask",2741}}),
        request("clip.setScale",{{"id",1},{"root",2},{"scale","major"},{"mask",2}}),
        request("clip.setEndMarker",{{"id",999},{"ticks",50}}),
        request("clip.setLoop",{{"id",1},{"on",true},{"row",Payload::object()}})
    };
    for(const auto& op:bad)check(!OpJournal(*f.store).commit(op).ok && state(*f.store)==before && OpJournal(*f.store).count()==count,
                                op.opType+" invalid edit rolls back state and log");
    const std::array batch={request("clip.setTimeSignature",{{"id",1},{"num",3},{"den",8}}),bad[11]};
    check(!OpJournal(*f.store).commit(batch).ok && state(*f.store)==before && OpJournal(*f.store).count()==count,
          "failed second op rolls back optional row creation and audit");
}

void extensionsAndDelete() {
    Fixture f("capture");
    check(edit(*f.store,"clip.setTimeSignature",{{"id",1},{"num",3},{"den",4}}),"capture seed");
    f.db().exec("ALTER TABLE clips ADD COLUMN extension BLOB");
    f.db().exec("UPDATE clips SET extension=X'0001FF'");
    f.db().exec("ALTER TABLE clip_properties ADD COLUMN extra TEXT");
    f.db().exec("UPDATE clip_properties SET extra='future property'");
    f.db().exec("CREATE TABLE property_child(id INTEGER PRIMARY KEY,clip_id INTEGER REFERENCES clip_properties(clip_id) ON DELETE CASCADE)");
    f.db().exec("INSERT INTO property_child VALUES(1,1)");
    roundTrip(*f.store,"clip.setTimeSignature",{{"id",1}});
    roundTrip(*f.store,"clip.setScale",{{"id",1},{"root",0},{"scale","major"},{"mask",2741}});
    roundTrip(*f.store,"clip.setStartMarker",{{"id",1},{"ticks",11}});
    check(f.db().execAndGet("SELECT count(*) FROM property_child").getInt()==1 &&
              f.db().execAndGet("SELECT extra FROM clip_properties").getString()=="future property", "scalar inverse/clear preserves extension fields and child rows");
    // The native delete contract covers native child tables; remove this
    // synthetic extension child before exercising that separate contract.
    f.db().exec("DROP TABLE property_child");
    f.db().exec("INSERT INTO clips(id,track_id,kind,name,pos_ticks,length_ticks,alias_of) VALUES(2,1,'midi','Alias',0,4,1)");
    f.db().exec("INSERT INTO media_files(id,hash_blake3,rel_path) VALUES(1,'test','sample.wav')");
    f.db().exec("INSERT INTO audio_clips(clip_id,media_id,src_start_frames,src_len_frames,warp_markers) VALUES(1,1,3,7,X'')");
    f.db().exec("INSERT INTO event_streams(id,clip_id,stream_kind,data) VALUES(11,1,'notes',X'0001FF'),(12,1,'cc',X'0102'),(13,2,'notes',X'0203')");
    f.db().exec("INSERT INTO note_expression(id,clip_id,note_id,dimension,data) VALUES(21,1,5,1,X'0001')");
    f.db().exec("INSERT INTO automation_lanes(id,owner_kind,owner_id,param_ref) VALUES(31,'track',1,'volume')");
    f.db().exec("INSERT INTO automation_data(id,lane_id,clip_id,data) VALUES(41,31,1,X'0001')");
    f.db().exec("ALTER TABLE event_streams ADD COLUMN extension TEXT");
    f.db().exec("UPDATE event_streams SET extension='future stream'");
    const auto before=state(*f.store);
    roundTrip(*f.store,"clip.delete",{{"id",1}});
    check(f.db().execAndGet("SELECT alias_of IS NULL FROM clips WHERE id=2").getInt()==1 &&
              f.db().execAndGet("SELECT count(*) FROM event_streams").getInt()==1,"delete removes owned streams and clears alias without touching alias's own stream");
    check(History(*f.store).undo().ok && state(*f.store)==before,"delete undo restores full rows, all native children, alias links and binary types");
    const auto path=f.store->path();f.store.reset();StoreError error=StoreError::Ok;f.store=Store::open(path,error);
    check(f.store && state(*f.store)==before && History(*f.store).redo().ok,"captured deletion survives reopen");
    f.db().exec("INSERT INTO clips(id,track_id,kind,pos_ticks,length_ticks) VALUES(1,1,'midi',0,1)");
    const auto collision=state(*f.store);
    check(!History(*f.store).undo().ok && state(*f.store)==collision,"restore never overwrites an existing clip or partially restores children");
}

void migrationAndLinear() {
    test::TempDirectory temp("clip_properties","old");const auto path=temp.path()/"old.adi";
    {
        SQLite::Database db(path.string(),SQLite::OPEN_READWRITE|SQLite::OPEN_CREATE);db.exec(readFile(ADI_CLIP_PROPERTIES_HISTORY));
        db.exec("INSERT INTO project(id,name) VALUES(1,'Old')");db.exec("INSERT INTO tracks(id,kind,name) VALUES(1,'midi','Keys')");
        db.exec("INSERT INTO clips(id,track_id,kind,time_base,pos_ns,length_ns) VALUES(1,1,'midi',1,750000000,1000000000)");
        db.exec("INSERT INTO tempo_map(pos_ticks,bpm) VALUES(0,120),(5765760,60)");
    }
    const auto bytes=readFile(path);StoreError error=StoreError::Ok;
    {
        auto store=Store::open(path,error,true);
        check(store && store->schemaMinor()==10 && rows::readModel(*store).problems.empty() &&
                  !rows::readModel(*store).clips[0].endMarkerTicks,"1.10 read-only sees no local properties via empty TEMP table");
    }
    check(readFile(path)==bytes,"read-only open leaves 1.10 bytes unchanged");
    auto store=Store::open(path,error);
    check(store && store->schemaMinor()==11 && store->upgradedFromMinor()==10,"1.10 upgrades additively to 1.11");
    if(!store)return;
    roundTrip(*store,"clip.setStartMarker",{{"id",1},{"ticks",17}});
    auto model=rows::readModel(*store);
    check(model.clips[0].endMarkerTicks==5765760 && model.clips[0].contentOffsetTicks==17 &&
              model.clips[0].timeBase==1 && model.clips[0].lengthNs==1000000000,"linear default End derives duration through tempo change while ns placement stays intact");
    roundTrip(*store,"clip.setTimeSignature",{{"id",1},{"num",5},{"den",4}});
    store.reset();store=Store::open(path,error);
    check(store && rows::readModel(*store).clips[0].signatureNumerator==5,"upgraded clip properties persist");
}
void retainedClocks() {
    Fixture f("retained");auto& db=f.db();
    // N17 is an independent schema branch. Exercise the SQL seam without
    // stacking or merging it, including future typed columns.
    db.exec("CREATE TABLE clip_playback_details(clip_id INTEGER PRIMARY KEY REFERENCES clips(id) ON DELETE CASCADE,"
        "content_offset_ns INTEGER,source_end_ns INTEGER,loop_begin_ns INTEGER,loop_end_ns INTEGER,"
        "in_begin_ns INTEGER,in_end_ns INTEGER,out_begin_ns INTEGER,out_end_ns INTEGER,future BLOB NOT NULL DEFAULT X'00FF') STRICT");
    db.exec("INSERT INTO clip_playback_details VALUES(1,123,999,111,555,0,22,33,44,X'01FF00')");
    const auto exact=db.execAndGet("SELECT hex(future) FROM clip_playback_details").getString();
    check(edit(*f.store,"clip.setStartMarker",{{"id",1},{"ticks",17}}),"Start edits through optional source clock seam");
    check(db.execAndGet("SELECT content_offset_ns IS NULL AND in_begin_ns IS NULL AND in_end_ns IS NULL AND out_begin_ns IS NULL AND out_end_ns IS NULL AND source_end_ns=999 AND loop_begin_ns=111 FROM clip_playback_details").getInt()==1,"Start clears its source offset and local fade domains, retaining End/loop");
    check(History(*f.store).undo().ok && db.execAndGet("SELECT content_offset_ns=123 AND in_end_ns=22 FROM clip_playback_details").getInt()==1 &&
          db.execAndGet("SELECT hex(future) FROM clip_playback_details").getString()==exact,"Start undo restores full typed clock row including future BLOB");
    check(edit(*f.store,"clip.setEndMarker",{{"id",1},{"ticks",100}}) &&
          db.execAndGet("SELECT source_end_ns IS NULL AND content_offset_ns=123 FROM clip_playback_details").getInt()==1,"End clears only its retained source limit");
    check(History(*f.store).undo().ok && db.execAndGet("SELECT source_end_ns FROM clip_playback_details").getInt64()==999,"End undo restores retained source limit");
    check(edit(*f.store,"clip.setLoop",{{"id",1},{"on",true},{"start",20},{"length",50}}) &&
          db.execAndGet("SELECT loop_begin_ns IS NULL AND loop_end_ns IS NULL AND content_offset_ns=123 FROM clip_playback_details").getInt()==1,"loop window edits clear both pinned endpoints");
    check(History(*f.store).undo().ok && db.execAndGet("SELECT loop_begin_ns=111 AND loop_end_ns=555 FROM clip_playback_details").getInt()==1,"loop undo restores both physical endpoints");
    check(edit(*f.store,"clip.setLoop",{{"id",1},{"on",false}}) &&
          db.execAndGet("SELECT loop_begin_ns=111 AND loop_end_ns=555 FROM clip_playback_details").getInt()==1,"a loop switch alone retains its dormant pinned window");
    check(History(*f.store).undo().ok,"undo loop switch");
    check(edit(*f.store,"clip.delete",{{"id",1}}) && History(*f.store).undo().ok &&
          db.execAndGet("SELECT content_offset_ns FROM clip_playback_details").getInt64()==123 &&
          db.execAndGet("SELECT hex(future) FROM clip_playback_details").getString()==exact,"clip deletion inverse carries optional playback details exactly");
}

} // namespace
int main() {
    try { valuesAndUndo();validation();extensionsAndDelete();migrationAndLinear();retainedClocks(); }
    catch(const std::exception& error){check(false,std::string("exception: ")+error.what());}
    std::printf("%d checks, %d failures\n",checks,failures);return failures?1:0;
}
