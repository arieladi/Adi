// SPDX-License-Identifier: GPL-3.0-or-later
#include "temp_directory.hpp"
#include "adi/digest.hpp"
#include "adi/history.hpp"
#include "adi/ops.hpp"
#include "adi/store.hpp"
#include "adi/track_image.hpp"
#include <SQLiteCpp/SQLiteCpp.h>
#include <array>
#include <cstdio>
#include <stdexcept>

namespace {
using namespace adi;
int checks=0,failures=0;
void check(bool ok,const std::string& message){++checks;if(!ok){++failures;std::printf("FAIL %s\n",message.c_str());}}
OpRequest op(const char* name,Payload payload){OpRequest result;result.opType=name;result.payload=std::move(payload);return result;}
struct Project {
    test::TempDirectory directory{"track_delete","project"};std::unique_ptr<Store> store;
    Project(){StoreError error{};store=Store::create(directory.path()/"song.adi",error);if(!store)throw std::runtime_error(toString(error));seed();}
    void seed() {
        store->db().exec(
            "INSERT INTO media_files(id,hash_blake3,orig_name,frames,channels,sample_rate,peaks) VALUES(1,'media','Keep.wav',96,2,44100,X'00ff');"
            "INSERT INTO state_blobs(hash_blake3,data,size_bytes) VALUES('state',X'001122ff',4);"
            "INSERT INTO plugin_refs(id,format,uid) VALUES(1,'internal','test');"
            "INSERT INTO tracks(id,kind,name,parent_id,color,input_ref,frozen,freeze_media_id) VALUES"
            "(100,'group','Group',NULL,5,'midi:all',1,1),(101,'audio','Audio',100,6,'hw:1',0,NULL),"
            "(103,'group','Nested',100,7,NULL,0,NULL),(104,'midi','MIDI',103,8,NULL,0,NULL),"
            "(200,'audio','Outside',NULL,9,NULL,0,NULL),(900,'master','Main',NULL,NULL,NULL,0,NULL);"
            "INSERT INTO mixer_strip(track_id,volume_db,delay_samples) SELECT id,-3.5,-23 FROM tracks;"
            "UPDATE mixer_strip SET vca_group_id=100 WHERE track_id=200;"
            "INSERT INTO group_summing(track_id,enabled,flavor,drive_db) VALUES(100,1,'console9',4);"
            "INSERT INTO lanes(id,track_id,name) VALUES(1,101,'Take');"
            "INSERT INTO clips(id,track_id,lane_id,kind,name,pos_ticks,length_ticks,alias_of) VALUES"
            "(10,101,1,'audio','Audio clip',4,960,NULL),(11,104,NULL,'midi','Notes',100,960,NULL),"
            "(12,200,1,'audio','External alias',200,960,10);"
            "INSERT INTO audio_clips(clip_id,media_id,src_start_frames,src_len_frames,warp_markers) VALUES(10,1,3,72,X'00ff'),(12,1,0,48,NULL);"
            "INSERT INTO event_streams(id,clip_id,stream_kind,data) VALUES(1,11,'notes',X'004200ff');"
            "INSERT INTO note_expression(id,clip_id,note_id,dimension,data) VALUES(1,11,3,0,X'ff00');"
            "INSERT INTO device_chains(id,track_id,name,key_zone) VALUES(1,101,'Root',X'00ff'),(3,200,'Other',NULL);"
            "INSERT INTO devices(id,chain_id,name,is_rack,plugin_ref_id) VALUES(100,1,'Rack',1,NULL),(200,3,'Outside effect',0,1);"
            "INSERT INTO device_chains(id,parent_device_id,name) VALUES(2,100,'Nested');"
            "INSERT INTO devices(id,chain_id,name,plugin_ref_id,latency_samples,missing) VALUES(101,2,'Inside effect',1,27,1);"
            "INSERT INTO plugin_state(device_id,stream_role,state_hash,format_hint) VALUES(101,'component','state','opaque');"
            "INSERT INTO plugin_params(device_id,param_id,name,normalized_value,display) VALUES(101,'gain','Gain',0.75,'3 dB');"
            "INSERT INTO device_expression_routes(device_id,route) VALUES(101,'mpe_midi');"
            "INSERT INTO device_panels(device_id) VALUES(101);"
            "INSERT INTO device_panel_params(device_id,ord,param_id) VALUES(101,4,'gain');"
            "INSERT INTO macros(id,device_id,ord,name) VALUES(55,100,0,'Macro'),(56,200,0,'Other');"
            "INSERT INTO macro_mappings(id,macro_id,target_device_id,target_param_ref) VALUES(66,55,200,'gain'),(67,56,101,'gain');"
            "INSERT INTO routing(id,src_kind,src_id,dst_kind,dst_id,kind,channel_map) VALUES"
            "(1,'track',101,'track',200,'main',X'ff00'),(2,'track',200,'track',100,'send',NULL),"
            "(3,'device',101,'device',200,'sidechain',NULL),(4,'track',100,'track',200,'vca',NULL),"
            "(5,'track',101,'hw_out',100,'cue',NULL),(6,'track',200,'hw_out',100,'main',X''),"
            "(7,'device',200,'hw_out',101,'main',NULL);"
            "INSERT INTO automation_lanes(id,owner_kind,owner_id,param_ref) VALUES(1,'track',101,'volume'),(2,'device',101,'gain'),(3,'clip',11,'gain'),(4,'routing',2,'gain'),(5,'track',200,'pan');"
            "INSERT INTO automation_data(id,lane_id,clip_id,data) VALUES(1,1,NULL,X'ff00'),(2,2,NULL,X'0011'),(3,3,11,X''),(4,4,NULL,X'55'),(5,5,NULL,X'77');"
            "INSERT INTO controller_maps(id,target_kind,target_id,target_param) VALUES(1,'track',101,'mute'),(2,'device',101,'gain'),(3,'routing',2,'gain'),(4,'track',200,'pan');"
            "INSERT INTO remarks(id,target_kind,target_id,author,text,created_utc) VALUES(1,'track',101,'user','Keep orphan',1),(2,'device',101,'agent','Keep provenance',2);"
            "INSERT INTO ui_view(id,scope_kind,scope_id,key,value) VALUES(1,'track',101,'height','80');"
            "INSERT INTO extensions(id,ns,key,scope_kind,scope_id,data) VALUES(1,'org.test','future','track',101,X'ff00');"
            "ALTER TABLE tracks ADD COLUMN future_track BLOB;UPDATE tracks SET future_track=X'00ff' WHERE id=100;"
            "ALTER TABLE devices ADD COLUMN future_device TEXT;UPDATE devices SET future_device='keep' WHERE id=101;"
            "CREATE TABLE future_child(track_id INTEGER NOT NULL REFERENCES tracks(id) ON DELETE CASCADE,tag TEXT NOT NULL,bytes BLOB NOT NULL,PRIMARY KEY(track_id,tag)) STRICT,WITHOUT ROWID;"
            "INSERT INTO future_child VALUES(101,'a',X''),(104,'b',X'00ff');"
            "CREATE TABLE future_grandchild(id INTEGER PRIMARY KEY,track INTEGER,tag TEXT,payload BLOB,FOREIGN KEY(track,tag) REFERENCES future_child(track_id,tag) ON DELETE CASCADE) STRICT;"
            "INSERT INTO future_grandchild VALUES(1,101,'a',X'1122');");
    }
    std::int64_t scalar(const char* sql){return store->db().execAndGet(sql).getInt64();}
};
void fullInverse() {
    Project project;auto& store=*project.store;OpJournal journal(store);History history(store);const auto before=digestProject(store).text;
    const auto removed=journal.commit(op("track.delete",{{"id",100}}));check(removed.ok,"delete populated group: "+removed.error);
    check(project.scalar("SELECT count(*) FROM tracks")==2 && project.scalar("SELECT count(*) FROM clips")==1 && project.scalar("SELECT count(*) FROM devices")==1,"delete removes the entire descendant hierarchy and nested devices");
    check(project.scalar("SELECT count(*) FROM routing")==2 && project.scalar("SELECT count(*) FROM routing WHERE id IN(6,7)")==2,"all internal routing kinds removed, equal hardware ids and surviving endpoints retained");
    check(project.scalar("SELECT count(*) FROM automation_lanes")==1 && project.scalar("SELECT count(*) FROM automation_data")==1 && project.scalar("SELECT count(*) FROM controller_maps")==1,"polymorphic automation and controller targets removed with their owner");
    check(project.scalar("SELECT alias_of IS NULL AND lane_id IS NULL FROM clips WHERE id=12")==1 && project.scalar("SELECT vca_group_id IS NULL FROM mixer_strip WHERE track_id=200")==1,"surviving alias, take-lane and VCA links follow SQLite SET NULL");
    check(project.scalar("SELECT count(*) FROM future_child")==0 && project.scalar("SELECT count(*) FROM future_grandchild")==0,"future composite foreign-key descendants are captured");
    check(project.scalar("SELECT count(*) FROM media_files")==1 && project.scalar("SELECT count(*) FROM state_blobs")==1 && project.scalar("SELECT count(*) FROM plugin_refs")==1,"shared media, state bytes and plug-in identity stay pooled");
    check(project.scalar("SELECT count(*) FROM remarks")==2 && project.scalar("SELECT count(*) FROM ui_view")==1 && project.scalar("SELECT count(*) FROM extensions")==1,"weak remarks, session views and unknown scoped extension data remain preserved");
    const auto after=digestProject(store).text;
    for(int cycle=0;cycle<2;++cycle) {
        const auto undone=history.undo();check(undone.ok,"undo full track image: "+undone.error);
        check(digestProject(store).text==before,"every project row and storage value returns exactly on undo");
        check(project.scalar("SELECT typeof(bytes)='blob' AND length(bytes)=0 FROM future_child WHERE tag='a'")==1 && project.scalar("SELECT future_track=X'00ff' FROM tracks WHERE id=100")==1,"future columns and empty BLOB remain distinct from NULL");
        const auto redone=history.redo();check(redone.ok && digestProject(store).text==after,"redo repeats exact deletion and cross-owner cleanup: "+redone.error);
    }
    const auto path=store.path();project.store.reset();StoreError error{};project.store=Store::open(path,error);
    check(project.store && History(*project.store).undo().ok && digestProject(*project.store).text==before,"CBOR inverse survives close/reopen with full typed metadata");
    check(project.scalar("PRAGMA defer_foreign_keys")==0 && project.scalar("PRAGMA foreign_keys")==1,"temporary FK deferral never leaks to the caller");
}
void batchAndConflicts() {
    Project project;auto& store=*project.store;const auto before=digestProject(store).text;
    std::array<OpRequest,2> batch={op("track.delete",{{"id",100}}),op("track.rename",{{"id",200},{"name","Kept"}})};
    check(OpJournal(store).commit(batch).ok,"delete and surviving track edit are one transaction");
    check(History(store).undo().ok && digestProject(store).text==before,"batch undo restores deletion after undoing later edit");
    std::array<OpRequest,2> bad={op("track.delete",{{"id",100}}),op("track.create",{{"id",200},{"kind","audio"}})};
    const auto count=project.scalar("SELECT count(*) FROM ops");check(!OpJournal(store).commit(bad).ok && digestProject(store).text==before && project.scalar("SELECT count(*) FROM ops")==count,"late batch failure rolls back deletion, captures and log together");
    check(OpJournal(store).commit(op("track.delete",{{"id",100}})).ok,"delete for collision guard");
    store.db().exec("INSERT INTO tracks(id,kind,name) VALUES(101,'audio','Collision')");const auto collision=digestProject(store).text;
    check(!History(store).undo().ok && digestProject(store).text==collision,"undo refuses occupied descendant ids without partial restoration");
    store.db().exec("DELETE FROM tracks WHERE id=101;UPDATE clips SET name='Unrelated edit' WHERE id=12");const auto changed=digestProject(store).text;
    check(!History(store).undo().ok && digestProject(store).text==changed,"changed SET NULL row refuses before overwriting unrelated work");
}
void ungroupAndGuards() {
    Project project;auto& store=*project.store;const auto before=digestProject(store).text;
    std::array<OpRequest,3> ungroup={op("track.setParent",{{"id",101}}),op("track.setParent",{{"id",103}}),op("track.delete",{{"id",100}})};
    const auto result=OpJournal(store).commit(ungroup);check(result.ok,"ungroup reparent + delete transaction: "+result.error);
    check(project.scalar("SELECT count(*) FROM tracks")==5 && project.scalar("SELECT count(*) FROM clips")==3,"ungroup keeps reparented children and their content");
    check(History(store).undo().ok && digestProject(store).text==before,"undo ungroup restores group metadata and exact routing");
    store.db().exec("CREATE TRIGGER unknown_delete AFTER DELETE ON tracks BEGIN UPDATE project SET name='Surprise';END;");
    const auto triggerState=digestProject(store).text;const auto rejected=OpJournal(store).commit(op("track.delete",{{"id",100}}));
    check(!rejected.ok && rejected.error.find("trigger")!=std::string::npos && digestProject(store).text==triggerState,"unknown affected triggers refuse atomically rather than losing side effects");
    store.db().exec("DROP TRIGGER unknown_delete");
    Payload packet=detail::captureTrack(store.db(),100);packet["rows"].push_back({{"table","project"},{"key",{{"id",1}}},{"before",{{"id",1}}},{"after",nullptr}});
    check(OpJournal(store).commit(op("track.delete",{{"id",100}})).ok,"delete before malformed restore test");
    const auto deleted=digestProject(store).text;const auto malformed=OpJournal(store).commit(op("track.create",{{"id",100},{"kind","group"},{"contents",packet}}));
    check(!malformed.ok && digestProject(store).text==deleted,"captured inverse validates row shapes before any write");
    packet["rows"].back()={{"table","arranger_sections"},{"key",{{"id",77}}},
        {"before",{{"id",77},{"name","Injected"},{"start_ticks",0},{"end_ticks",1},{"color",nullptr}}},{"after",nullptr}};
    const auto unrelated=OpJournal(store).commit(op("track.create",{{"id",100},{"kind","group"},{"contents",packet}}));
    check(!unrelated.ok && unrelated.error.find("unrelated")!=std::string::npos && digestProject(store).text==deleted && project.scalar("PRAGMA defer_foreign_keys")==0,
        "well-shaped unrelated captured rows refuse after causal verification and roll back all restoration");

}
void legacyAndReplay() {
    test::TempDirectory directory{"track_delete","legacy"};StoreError error{};auto first=Store::create(directory.path()/"one.adi",error);auto second=Store::create(directory.path()/"two.adi",error);
    const std::array<OpRequest,4> log={op("track.create",{{"id",1},{"kind","group"},{"name","G"}}),op("track.create",{{"id",2},{"kind","audio"},{"parent",1}}),op("track.setColor",{{"id",2},{"color",55}}),op("track.delete",{{"id",1}})};
    first->db().exec("INSERT INTO session_state VALUES('selection','1')");second->db().exec("INSERT INTO session_state VALUES('selection','other')");
    for(const auto& request:log)check(OpJournal(*first).commit(request).ok && OpJournal(*second).commit(request).ok,"same literal op replays under different ambient selection");
    check(digestProject(*first).text==digestProject(*second).text && History(*first).undo().ok && History(*second).undo().ok && digestProject(*first).text==digestProject(*second).text,"captured inverses replay the same complete subtree");
    check(first->db().execAndGet("SELECT color FROM tracks WHERE id=2").getInt()==55,"descendant colour survives replay undo");
    OpContext context{*first,first->db()};std::string why;
    // Old logs captured the four descriptive track fields, without contents.
    {SQLite::Transaction transaction(first->db());check(OpRegistry::instance().find("track.create")->apply(context,{{"id",3},{"kind","audio"},{"name","Legacy"},{"parent",nullptr},{"index",4}},why),"old four-field captured inverse remains playable");transaction.commit();}
}
}
int main(){try{fullInverse();batchAndConflicts();ungroupAndGuards();legacyAndReplay();}catch(const std::exception& error){check(false,error.what());}std::printf("%d checks, %d failures\n",checks,failures);return failures?1:0;}
