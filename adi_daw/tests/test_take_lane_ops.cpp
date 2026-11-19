// SPDX-License-Identifier: AGPL-3.0-or-later
#include "temp_directory.hpp"
#include "adi/clip_image.hpp"
#include "adi/engine/timeline_clips.hpp"
#include "adi/history.hpp"
#include "adi/ops.hpp"
#include "adi/row_capture.hpp"
#include "adi/store.hpp"
#include "adi/textproj_store.hpp"
#include "adi/take_lane_ops.hpp"
#include <SQLiteCpp/SQLiteCpp.h>
#include <array>
#include <cstdio>
#include <stdexcept>
#include <string>

namespace {
using namespace adi;
int checks=0,failures=0;
void check(bool ok,const char* message){++checks;if(!ok){++failures;std::printf("FAIL %s\n",message);}}
OpRequest request(const char* type,Payload value){OpRequest result;result.opType=type;result.payload=std::move(value);return result;}
struct Project {
    test::TempDirectory temp{"take_lane_ops","journal"};std::unique_ptr<Store> store;
    Project(){StoreError error{};store=Store::create(temp.path()/"test.adi",error);if(!store)throw std::runtime_error(toString(error));
        store->db().exec("INSERT INTO tracks(id,kind,name) VALUES(21,'audio','Audio'),(22,'midi','MIDI'),(23,'group','Group');"
            "INSERT INTO lanes(id,track_id,ord,name) VALUES(10,21,7,'First'),(11,21,42,'Last'),(20,22,3,'Other');"
            "INSERT INTO clips(id,track_id,lane_id,kind,pos_ticks,length_ticks) VALUES"
            "(30,21,10,'audio',0,960),(31,21,10,'audio',960,960),(32,21,NULL,'audio',1920,960),(40,22,20,'midi',0,960),(50,21,NULL,'audio',2880,960);");}
    CommitResult op(const char* type,Payload value){return OpJournal(*store).commit(request(type,std::move(value)));}
    std::int64_t count(){return store->db().execAndGet("SELECT count(*) FROM ops").getInt64();}
    Payload material(){Payload result=Payload::object();
        for(const auto* table:{"lanes","clips","event_streams","note_expression","audio_clips","automation_lanes","automation_data","automation_segment_ranges","clip_properties","clip_playback_details","clip_warp_fade_domains","media_files"}){
            SQLite::Statement rows(store->db(),std::string("SELECT * FROM ")+table+" ORDER BY 1");auto& out=result[table];out=Payload::array();while(rows.executeStep())out.push_back(detail::captureQueryRow(rows));}
        return result;
    }
    Payload laneRow(std::int64_t id){return *detail::captureRow(store->db(),"lanes","id",id);}
};
void structure() {
    Project p;auto& db=p.store->db();db.exec("ALTER TABLE lanes ADD COLUMN future_blob BLOB NOT NULL DEFAULT X'';UPDATE lanes SET future_blob=X'00FF' WHERE id=10;");
    const auto before=p.material();
    auto creation=createTakeLane(*p.store,12,21);
    check(creation.payload.at("row").at("future_blob").is_binary()&&creation.payload.at("row").at("future_blob").get_binary().empty(),"request captures native typed future default before journaling");
    creation.payload["row"]["future_blob"]=Payload::binary({0xAB,0x00,0xFF});
    check(OpJournal(*p.store).commit(creation).ok,"create addressed lane");
    check(p.laneRow(12).at("name")=="Lane"&&p.laneRow(12).at("ord")==0,"new observed name and top insertion");
    check(p.laneRow(10).at("ord")==1&&p.laneRow(11).at("ord")==2&&p.laneRow(20).at("ord")==3,"top insertion shifts this track's take lanes only");
    check(p.laneRow(12).at("future_blob")==creation.payload.at("row").at("future_blob"),"apply uses exact captured future bytes rather than reevaluating defaults");
    const auto created=p.material();
    check(History(*p.store).undo().ok&&p.material()==before,"creation undo restores exact sparse old ordering and future bytes");
    check(History(*p.store).redo().ok&&p.material()==created,"creation redo retains identity and captured ordering");
    check(p.op("lane.reorder",{{"track",21},{"lanes",Payload::array({11,12,10})}}).ok,"reorder all named take lanes");
    check(p.laneRow(11).at("ord")==0&&p.laneRow(12).at("ord")==1&&p.laneRow(10).at("ord")==2,"literal reorder applied");
    check(History(*p.store).undo().ok&&p.material()==created,"reorder undo exact");
    check(History(*p.store).redo().ok,"reorder redo");
    const auto reordered=p.material();
    check(p.op("lane.rename",{{"id",12},{"name","Vocal Ω"}}).ok,"rename take lane");
    check(p.op("lane.setMute",{{"id",12},{"on",true}}).ok,"store lane mute");
    check(p.laneRow(12).at("muted")==1&&p.laneRow(12).at("name")=="Vocal Ω","name and mute values retained");
    check(History(*p.store).undo().ok&&History(*p.store).undo().ok&&p.material()==reordered,"scalar undo preserves complete typed rows");
    check(History(*p.store).redo().ok&&History(*p.store).redo().ok,"scalar redo");
    const auto expected=p.material();const auto text=textproj::projectStore(*p.store).text;
    check(text.find("Vocal Ω")!=std::string::npos,"lane name reaches existing text projection");
    p.store.reset();StoreError error{};p.store=Store::open(p.temp.path()/"test.adi",error);
    check(p.store&&p.material()==expected,"reopen preserves lane state and extension storage classes");
    check(History(*p.store).undo().ok&&p.laneRow(12).at("muted")==0,"reopened journal undo works");
    const auto beforeEmptyDelete=p.material();
    check(p.op("lane.delete",{{"id",12}}).ok,"empty take lane can be deleted");
    check(History(*p.store).undo().ok&&p.material()==beforeEmptyDelete,"empty lane deletion undo restores exact ordering");
    check(!OpRegistry::instance().find("lane.setCompTarget"),"ambiguous legacy comp-target operation remains unregistered");
}
void attachment() {
    Project p;auto& db=p.store->db();const auto before=p.material();
    auto playing=[&]{const auto model=rows::readModel(*p.store);std::vector<std::int64_t> ids;for(const auto& clip:engine::timelineClips(model))if(clip.trackId==21)ids.push_back(clip.id);return ids;};
    check(p.op("clip.setLane",{{"id",50},{"lane",10}}).ok,"main clip attaches to its own take lane");
    check(playing()==std::vector<std::int64_t>{32},"attached clip no longer appears in main playback selection");
    check(db.execAndGet("SELECT id FROM clips WHERE id=50 AND lane_id=10").getInt64()==50,"attachment retains source identity");
    check(History(*p.store).undo().ok&&p.material()==before,"attachment undo exact");
    check(History(*p.store).redo().ok,"attachment redo");
    check(p.op("clip.setLane",{{"id",50},{"lane",nullptr}}).ok,"explicit null returns to main lane");
    check(playing()==std::vector<std::int64_t>({32,50}),"main playback selection includes detached clip");
    check(History(*p.store).undo().ok,"undo main detach");
    check(p.op("clip.setLane",{{"id",50}}).ok,"omitted lane also means main");
    const auto valid=p.material();const auto count=p.count();
    check(!p.op("clip.setLane",{{"id",50},{"lane",20}}).ok,"cross-track attachment refused");
    check(!p.op("clip.setLane",{{"id",50},{"lane",999}}).ok,"missing lane refused");
    check(!p.op("clip.setLane",{{"id",999},{"lane",10}}).ok,"missing clip refused");
    check(p.material()==valid&&p.count()==count,"attachment refusals preserve material and journal");
    const std::array badBatch{request("clip.setLane",{{"id",50},{"lane",10}}),request("clip.move",{{"id",50},{"track",22},{"to",0}})};
    check(!OpJournal(*p.store).commit(badBatch).ok&&p.material()==valid&&p.count()==count,"composed attach then cross-track move rolls back atomically");
    const std::array goodBatch{request("clip.setLane",{{"id",30},{"lane",nullptr}}),request("clip.move",{{"id",30},{"track",22},{"to",0}}),request("clip.setLane",{{"id",30},{"lane",20}})};
    check(OpJournal(*p.store).commit(goodBatch).ok,"explicit detach then move then attach is allowed as one transaction");
    check(History(*p.store).undo().ok&&p.material()==valid,"composed legal move undo restores original track and lane");
}
void capturedDeletion() {
    Project p;auto& db=p.store->db();
    db.exec("ALTER TABLE clips ADD COLUMN future_blob BLOB NOT NULL DEFAULT X'';"
        "ALTER TABLE lanes ADD COLUMN future_real REAL;UPDATE lanes SET future_real=0.125 WHERE id=10;"
        "UPDATE clips SET alias_of=31,future_blob=X'0100FF' WHERE id=30;UPDATE clips SET alias_of=30 WHERE id IN (31,32);"
        "INSERT INTO media_files(id,hash_blake3,rel_path,abs_path_hint,format) VALUES(91,'owned','Media/Take.wav','original/take.wav','wav');"
        "INSERT INTO audio_clips(clip_id,media_id,src_start_frames,src_len_frames,warp_markers) VALUES(30,91,17,987,X'0011FF');"
        "INSERT INTO event_streams(id,clip_id,stream_kind,data) VALUES(101,40,'notes',X'0080FF'),(102,40,'future-stream',X'');"
        "INSERT INTO note_expression(id,clip_id,note_id,dimension,data) VALUES(201,40,7,2,X'0100');"
        "INSERT INTO automation_lanes(id,owner_kind,owner_id,param_ref) VALUES(301,'track',21,'gain');"
        "INSERT INTO automation_data(id,lane_id,clip_id,data) VALUES(302,301,30,X'AA00');"
        "INSERT INTO automation_segment_ranges(id,data_id,clip_id,record_index,progress_begin,progress_end,source_first,source_last) VALUES(303,302,30,0,0.25,0.75,0.0,1.0);"
        "INSERT INTO clip_properties(clip_id,end_marker_ticks) VALUES(30,720);"
        "INSERT INTO clip_playback_details(clip_id,in_tension,source_end_ns) VALUES(30,0.25,123000);"
        "INSERT INTO clip_warp_fade_domains(clip_id,in_begin_ticks,in_end_ticks) VALUES(30,10,20);"
        "CREATE TRIGGER no_take_promotion BEFORE UPDATE OF lane_id ON clips WHEN OLD.lane_id IS NOT NULL AND NEW.lane_id IS NULL BEGIN SELECT RAISE(ABORT,'implicit promotion'); END;");
    const auto before=p.material();const auto count=p.count();
    db.exec("CREATE TRIGGER reject_lane_delete BEFORE DELETE ON lanes WHEN OLD.id=10 BEGIN SELECT RAISE(ABORT,'late lane failure'); END;");
    check(!p.op("lane.delete",{{"id",10}}).ok&&p.material()==before&&p.count()==count,"late lane deletion failure rolls back every removed child and alias update");
    db.exec("DROP TRIGGER reject_lane_delete");
    const std::array removes{request("lane.delete",{{"id",10}}),request("lane.delete",{{"id",20}})};
    const auto result=OpJournal(*p.store).commit(removes);check(result.ok,"nonempty audio and MIDI lanes deleted in one transaction");
    if(!result.ok)std::printf("delete detail: %s\n",result.error.c_str());
    check(db.execAndGet("SELECT count(*) FROM clips WHERE id IN(30,31,40)").getInt()==0,"lane contents removed, never silently promoted");
    check(db.execAndGet("SELECT count(*) FROM clips WHERE id=32 AND alias_of IS NULL").getInt()==1,"external alias unlink captured explicitly");
    check(p.laneRow(11).at("ord")==0&&db.execAndGet("SELECT count(*) FROM media_files WHERE id=91").getInt()==1,"remaining lane compacted, source media pool and paths retained");
    const auto deleted=p.material();
    db.exec("CREATE TRIGGER reject_restore BEFORE INSERT ON clips WHEN NEW.id=40 BEGIN SELECT RAISE(ABORT,'late restore failure'); END;");
    check(!History(*p.store).undo().ok&&p.material()==deleted,"late failed Undo rolls back recreated lanes, clips and earlier transaction members");
    db.exec("DROP TRIGGER reject_restore");
    check(History(*p.store).undo().ok&&p.material()==before,"one undo restores all clip identities, child streams, exact BLOBs, aliases and old ordering");
    check(History(*p.store).redo().ok&&p.material()==deleted,"redo repeats explicit lane/clip deletion");
    check(History(*p.store).undo().ok&&p.material()==before,"second undo remains exact");
    check(db.execAndGet("SELECT count(*) FROM pragma_foreign_key_check").getInt()==0,"restored lane and clip dependency graph has valid foreign keys");
}
void refusal() {
    Project p;auto& db=p.store->db();
    db.exec("INSERT INTO lanes(id,track_id,ord,kind,name) VALUES(90,21,1,'comp','Legacy');");
    const auto before=p.material();const auto count=p.count();
    for(const auto& op:std::array{
        createTakeLane(*p.store,10,21),createTakeLane(*p.store,12,999),
        createTakeLane(*p.store,12,23),createTakeLane(*p.store,0,21),
        request("lane.delete",{{"id",999}}),request("lane.rename",{{"id",999},{"name","X"}}),
        request("lane.reorder",{{"track",21},{"lanes",Payload::array({10})}}),
        request("lane.reorder",{{"track",21},{"lanes",Payload::array({10,10})}}),
        request("lane.reorder",{{"track",21},{"lanes",Payload::array({10,20})}}),
        request("lane.reorder",{{"track",21},{"lanes",Payload::array({10,11,90})}}),
        request("lane.rename",{{"id",90},{"name","Take"}}),request("lane.delete",{{"id",90}}),
        request("lane.setMute",{{"id",10},{"on",1}})})
        check(!OpJournal(*p.store).commit(op).ok,"invalid lane request refused");
    check(p.material()==before&&p.count()==count,"all malformed requests are atomic and unjournaled");
    auto forged=*detail::captureRow(db,"clips","id",50),attached=forged;attached["lane_id"]=20;
    check(!p.op("lane.restore",{{"changes",Payload::array({{{"table","clips"},{"id",50},{"before",forged},{"after",attached}}})}}).ok&&p.material()==before&&p.count()==count,"raw restoration also refuses cross-track attachment");
    attached["lane_id"]=90;
    check(!p.op("lane.restore",{{"changes",Payload::array({{{"table","clips"},{"id",50},{"before",forged},{"after",attached}}})}}).ok&&p.material()==before&&p.count()==count,"raw restoration refuses an existing legacy non-take lane");
    attached=forged;attached["track_id"]=23;
    check(!p.op("lane.restore",{{"changes",Payload::array({{{"table","clips"},{"id",50},{"before",forged},{"after",attached}}})}}).ok&&p.material()==before&&p.count()==count,"raw restoration refuses a non-media track even on main");
    auto creation=createTakeLane(*p.store,12,21);creation.payload["row"]["track_id"]=22;
    check(!OpJournal(*p.store).commit(creation).ok&&p.material()==before&&p.count()==count,"creation row must match its explicit owner identity");
    check(!p.op("lane.rename",{{"id",10},{"name","First"}}).ok&&p.count()==count,"unchanged values do not create a journal step");
    db.exec("CREATE TABLE future_lane_child(id INTEGER PRIMARY KEY,lane INTEGER REFERENCES lanes(id) ON DELETE CASCADE,payload BLOB);INSERT INTO future_lane_child VALUES(1,10,X'00FF');");
    check(!p.op("lane.delete",{{"id",10}}).ok&&p.material()==before&&p.count()==count,"uncaptured future lane cascade refuses before mutation");
    check(db.execAndGet("SELECT hex(payload) FROM future_lane_child").getString()=="00FF","future lane child preserved");
    db.exec("DROP TABLE future_lane_child;CREATE TABLE future_clip_child(id INTEGER PRIMARY KEY,clip INTEGER REFERENCES clips(id) ON DELETE SET NULL);INSERT INTO future_clip_child VALUES(1,30);");
    check(!p.op("lane.delete",{{"id",10}}).ok&&p.material()==before&&p.count()==count,"uncaptured future clip SET NULL refuses before mutation");
    check(db.execAndGet("SELECT clip FROM future_clip_child").getInt64()==30,"future clip reference preserved");
    db.exec("DROP TABLE future_clip_child;UPDATE clips SET track_id=22 WHERE id=30;");
    const auto malformed=p.material();
    check(!p.op("lane.delete",{{"id",10}}).ok&&p.material()==malformed&&p.count()==count,"pre-existing cross-track lane corruption refuses deletion");
    check(!p.op("clip.setLane",{{"id",30},{"lane",nullptr}}).ok&&p.material()==malformed,"detach refuses a corrupt before-image that could not be restored");
    check(!p.op("clip.move",{{"id",30},{"track",21},{"to",0}}).ok&&p.material()==malformed&&p.count()==count,"move refuses an implicit repair with an invalid inverse");
}
}
int main(){try{structure();attachment();capturedDeletion();refusal();}catch(const std::exception& error){++failures;std::printf("EXCEPTION %s\n",error.what());}
    std::printf("%d checks, %d failures\n",checks,failures);return failures?1:0;}
