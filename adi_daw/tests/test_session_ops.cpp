// SPDX-License-Identifier: AGPL-3.0-or-later
#include "temp_directory.hpp"
#include "session_schema_fixture.hpp"

#include "adi/history.hpp"
#include "adi/ops.hpp"
#include "adi/row_capture.hpp"
#include "adi/session_ops.hpp"
#include "adi/store.hpp"

#include <SQLiteCpp/SQLiteCpp.h>
#include <array>
#include <chrono>
#include <cstdio>
#include <stdexcept>
#include <utility>

namespace {
using namespace adi;

int checks = 0;
void require(bool passed, const char* message) {
    ++checks;
    if (!passed) throw std::runtime_error(message);
}

OpRequest request(const char* name, Payload payload) {
    OpRequest result;
    result.opType = name;
    result.payload = std::move(payload);
    return result;
}

struct Project {
    test::TempDirectory temp{"session_ops","journal"};
    std::unique_ptr<Store> store;

    Project() {
        StoreError error{};
        store = Store::create(temp.path()/"session.adi",error);
        require(store != nullptr,"create a Session project");
        if (scalar("SELECT count(*) FROM sqlite_master WHERE type='table' AND name='scenes'") == 0)
            test::applySessionSchema(db());
        db().exec("INSERT INTO tracks(id,kind,name) VALUES(21,'audio','A'),(22,'midi','M'),(23,'group','G');"
                  "INSERT INTO lanes(id,track_id,ord,name) VALUES(31,21,0,'Take');"
                  "INSERT INTO clips(id,track_id,lane_id,kind,name,pos_ticks,length_ticks) VALUES"
                  "(41,21,31,'audio','Loop A',123,456),(42,21,NULL,'audio','Loop B',321,456),"
                  "(43,21,NULL,'audio','Alias',654,456),(51,22,NULL,'midi','MIDI A',300,456),"
                  "(52,22,NULL,'midi','MIDI B',400,456);"
                  "UPDATE clips SET alias_of=41 WHERE id=43;"
                  "INSERT INTO clips(id,track_id,kind,name,time_base,pos_ns,length_ns) "
                  "VALUES(44,21,'audio','Linear',1,124000,902000);"
                  "INSERT INTO media_files(id,hash_blake3,rel_path,format) VALUES(91,'session-media','Media/A.wav','wav');"
                  "INSERT INTO audio_clips(clip_id,media_id,src_start_frames,src_len_frames,warp_markers)"
                  " VALUES(41,91,11,900,X'00FF00'),(42,91,21,700,NULL);"
                  "INSERT INTO event_streams(id,clip_id,stream_kind,data) VALUES(101,51,'notes',X'00FF'),(102,51,'unknown',X'');"
                  "INSERT INTO note_expression(id,clip_id,note_id,dimension,data) VALUES(201,51,77,1,X'AA00FF');"
                  "INSERT INTO clip_properties(clip_id,end_marker_ticks) VALUES(41,333);"
                  "INSERT INTO clip_playback_details(clip_id,source_end_ns) VALUES(41,123456);"
                  "INSERT INTO clip_warp_fade_domains(clip_id,in_begin_ticks,in_end_ticks) VALUES(41,100,120);"
                  "INSERT INTO extensions(id,ns,key,data) VALUES(1,'example.fixture','kept',X'FE00');");
    }
    SQLite::Database& db() { return store->db(); }
    void edit(const char* name, Payload payload) {
        const auto result = OpJournal(*store).commit(request(name,std::move(payload)));
        if (!result.ok) throw std::runtime_error(std::string(name) + ": " + result.error);
    }
    bool refuses(const char* name, Payload payload) {
        return !OpJournal(*store).commit(request(name,std::move(payload))).ok;
    }
    void undo() {
        const auto result = History(*store).undo();
        if (!result.ok) throw std::runtime_error("undo: " + result.error);
    }
    void redo() {
        const auto result = History(*store).redo();
        if (!result.ok) throw std::runtime_error("redo: " + result.error);
    }
    std::int64_t scalar(const std::string& sql) { return db().execAndGet(sql).getInt64(); }
    Payload rows() {
        auto result = Payload::object();
        for (const auto* table : {"scenes","clip_slots","clip_launch","tracks","lanes","clips","audio_clips","event_streams",
                                  "note_expression","clip_properties","clip_playback_details",
                                  "clip_warp_fade_domains","extensions","media_files"}) {
            auto& rows = result[table];
            rows = Payload::array();
            SQLite::Statement query(db(),std::string("SELECT * FROM ") + table + " ORDER BY 1,2");
            while (query.executeStep()) rows.push_back(detail::captureQueryRow(query));
        }
        return result;
    }
};

void scenesAndSettings() {
    Project p;
    p.edit("scene.create",{{"id",11},{"ord",0},{"name","First"},{"color",17}});
    p.edit("scene.create",{{"id",12},{"ord",1},{"name","Second"}});
    const auto original = p.rows();
    p.edit("scene.reorder",{{"scenes",Payload::array({12,11})}});
    require(p.scalar("SELECT id FROM scenes WHERE ord=0") == 12,"reorder swaps occupied unique ordinals");
    p.undo();
    require(p.rows() == original,"scene reorder undo is exact");
    p.redo();
    require(p.scalar("SELECT id FROM scenes WHERE ord=0") == 12,"scene order replays");
    p.edit("scene.rename",{{"id",11},{"name","A Ω"}});
    p.edit("scene.setTempo",{{"id",11},{"bpm",137.5}});
    p.edit("scene.setSignature",{{"id",11},{"numerator",7},{"denominator",8}});
    require(p.scalar("SELECT tempo_enabled+signature_enabled FROM scenes WHERE id=11") == 0,
        "stored tempo and signature remain dormant until enabled");
    p.edit("scene.setEnabled",{{"id",11},{"tempo",true}});
    p.edit("scene.setEnabled",{{"id",11},{"signature",true}});
    require(p.scalar("SELECT count(*) FROM scenes WHERE id=11 AND tempo_enabled=1 AND signature_enabled=1"
                     " AND tempo_bpm=137.5 AND signature_numerator=7 AND signature_denominator=8 AND color=17") == 1,
        "enable flags activate exact stored values independently");
    const auto before = p.rows();
    const auto count = p.scalar("SELECT count(*) FROM ops");
    require(p.refuses("scene.reorder",{{"scenes",Payload::array({11,11})}}),"duplicate scene reorder refused");
    require(p.refuses("scene.setSignature",{{"id",11},{"numerator",7},{"denominator",3}}),"non-power-of-two signature refused");
    require(p.refuses("scene.setEnabled",{{"id",11}}),"empty enable change refused");
    require(p.refuses("scene.rename",{{"id",11},{"name","forgery"},{"_session",Payload::object()}}),
        "submission rejects the private restoration image");
    require(p.rows() == before && p.scalar("SELECT count(*) FROM ops") == count,
        "failed scene edits preserve store and journal");
}

void slotsAndReplacement() {
    Project p;
    p.edit("scene.create",{{"id",11},{"ord",0}});
    p.edit("scene.create",{{"id",12},{"ord",1}});
    p.edit("session.setStopButton",{{"track",21},{"scene",11},{"on",false}});
    const auto beforeFirst = p.rows();
    p.edit("session.setSlotClip",{{"track",21},{"scene",11},{"clip",41}});
    require(p.scalar("SELECT count(*) FROM clips WHERE id=41 AND lane_id IS NULL AND pos_ticks=0") == 1,
        "slot owns the clip at zero origin with no take lane");
    require(p.scalar("SELECT count(*) FROM extensions WHERE ns='org.adidaw.session' AND key='clip_slots'"
                     " AND criticality='essential' AND min_reader=1026") == 1,
        "first occupied slot marks the native reader requirement");
    const auto first = p.rows();
    p.undo();
    require(p.rows() == beforeFirst,"first assignment undo restores take origin, Stop, clips and marker state");
    p.redo();
    require(p.rows() == first,"first assignment redo reuses the exact marker and clip IDs");
    p.edit("session.setLaunchMode",{{"clip",41},{"mode",3}});
    p.edit("session.setLaunchQuant",{{"clip",41},{"quant",14}});
    p.edit("session.setLaunchSettings",{{"clip",41},{"legato",true},{"velocity",0.375},{"fade",true},{"ram",true}});
    p.edit("session.setStopButton",{{"track",21},{"scene",11},{"on",true}});
    require(p.scalar("SELECT count(*) FROM clip_launch WHERE clip_id=41 AND launch_mode=3") == 1,
        "editing the occupied slot's Stop button preserves its FK-owned launch settings");
    p.undo();
    const auto configured = p.rows();
    p.edit("session.setSlotClip",{{"track",21},{"scene",12},{"clip",41}});
    require(p.scalar("SELECT count(*) FROM clip_slots WHERE track_id=21 AND scene_id=11 AND clip_id IS NULL AND has_stop=0") == 1,
        "moving a slot leaves the original Stop override in place");
    require(p.scalar("SELECT count(*) FROM clip_launch WHERE clip_id=41 AND launch_mode=3 AND launch_quant=14"
                     " AND legato=1 AND velocity_amount=0.375 AND clip_fade=1 AND ram_mode=1") == 1,
        "moving a clip retains all authored launch settings");
    p.undo();
    require(p.rows() == configured,"slot move Undo restores both slot rows and settings");
    p.redo();
    const auto moved = p.rows();
    p.edit("session.setSlotClip",{{"track",21},{"scene",12},{"clip",42}});
    require(p.scalar("SELECT count(*) FROM clips WHERE id=41") == 0 &&
        p.scalar("SELECT count(*) FROM clips WHERE id=43 AND alias_of IS NULL") == 1,
        "replacement deletes only the overwritten clip and unlinks an external alias");
    require(p.scalar("SELECT count(*) FROM media_files WHERE id=91") == 1,"replacement preserves pooled media");
    const auto replaced = p.rows();
    p.undo();
    require(p.rows() == moved,"replacement Undo restores complete audio clip, alias, launch and placement images");
    p.redo();
    require(p.rows() == replaced,"replacement replays exactly");
    p.edit("session.setStopButton",{{"track",21},{"scene",12},{"on",false}});
    const auto beforeClear = p.rows();
    p.edit("session.clearSlot",{{"track",21},{"scene",12}});
    require(p.scalar("SELECT count(*) FROM clip_slots WHERE scene_id=12 AND clip_id IS NULL AND has_stop=0") == 1,
        "clearing a clip preserves a disabled Stop button");
    p.undo();
    require(p.rows() == beforeClear,"clear Undo restores all overwritten slot and clip material");
    const auto journalCount = p.scalar("SELECT count(*) FROM ops");
    require(p.refuses("session.setSlotClip",{{"track",21},{"scene",11},{"clip",51}}),"cross-track assignment rejected");
    require(p.refuses("session.setStopButton",{{"track",23},{"scene",11},{"on",false}}),"group slots rejected");
    require(p.refuses("session.setLaunchMode",{{"clip",44},{"mode",2}}),"Arrangement clips cannot gain launch settings");
    require(p.rows() == beforeClear && p.scalar("SELECT count(*) FROM ops") == journalCount,
        "failed slot edits preserve store and journal");
    p.edit("session.setSlotClip",{{"track",21},{"scene",11},{"clip",44}});
    require(p.scalar("SELECT count(*) FROM clips WHERE id=44 AND time_base=1 AND pos_ns=0"
                     " AND pos_ticks IS NULL AND length_ns=902000") == 1,
        "linear slot clips preserve physical duration and zero the native origin");
}

void sceneDeletionAndAtomicFailure() {
    Project p;
    p.edit("scene.create",{{"id",11},{"ord",4}});
    p.edit("scene.create",{{"id",12},{"ord",5}});
    p.edit("session.setSlotClip",{{"track",21},{"scene",11},{"clip",41}});
    p.edit("session.setSlotClip",{{"track",22},{"scene",11},{"clip",51}});
    p.edit("session.setSlotClip",{{"track",22},{"scene",12},{"clip",52}});
    const auto follow = Payload{{"enabled",true},{"duration",5765760},{"actionA",9},{"jumpA",11},
                                {"actionB",0},{"chanceB",38.5}};
    auto sceneFollow = follow;
    sceneFollow["scene"] = 12;
    p.edit("session.setFollowAction",std::move(sceneFollow));
    auto clipFollow = follow;
    clipFollow["clip"] = 52;
    clipFollow["linked"] = true;
    clipFollow["loops"] = 3;
    p.edit("session.setFollowAction",std::move(clipFollow));
    const auto before = p.rows();
    const auto count = p.scalar("SELECT count(*) FROM ops");
    p.db().exec("CREATE TRIGGER fail_scene_delete BEFORE DELETE ON scenes WHEN OLD.id=11 "
                "BEGIN SELECT RAISE(ABORT,'fixture scene failure'); END;");
    require(p.refuses("scene.delete",{{"id",11}}) && p.rows() == before && p.scalar("SELECT count(*) FROM ops") == count,
        "failure at final scene deletion rolls back deleted clips, children, settings and journal");
    p.db().exec("DROP TRIGGER fail_scene_delete");
    p.edit("scene.delete",{{"id",11}});
    require(p.scalar("SELECT count(*) FROM clips WHERE id IN(41,51)") == 0,
        "deleting a scene removes its occupied audio and MIDI slots");
    require(p.scalar("SELECT count(*) FROM scenes WHERE id=12 AND follow_a=0 AND follow_a_scene_id IS NULL") == 1 &&
        p.scalar("SELECT count(*) FROM clip_launch WHERE clip_id=52 AND follow_a=0 AND follow_a_scene_id IS NULL") == 1,
        "deleting a Jump target clears incoming clip and scene references deterministically");
    const auto deleted = p.rows();
    p.db().exec("CREATE TRIGGER fail_slot_restore BEFORE INSERT ON clip_slots WHEN NEW.clip_id=51 "
                "BEGIN SELECT RAISE(ABORT,'fixture restore failure'); END;");
    require(!History(*p.store).undo().ok && p.rows() == deleted,"failure in late restore rolls back recreated clips and scenes");
    p.db().exec("DROP TRIGGER fail_slot_restore");
    p.undo();
    require(p.rows() == before,"scene Undo restores clips, exact binary streams, aliases and incoming Jump rows");
    p.redo();
    require(p.rows() == deleted,"scene deletion replays deterministically");
    p.undo();
    require(p.rows() == before && p.scalar("SELECT count(*) FROM pragma_foreign_key_check") == 0,
        "second Undo restores a foreign-key-complete image");
    const auto restore = std::chrono::steady_clock::now();
    p.store.reset();
    StoreError error{};
    p.store = Store::open(p.temp.path()/"session.adi",error);
    require(p.store && p.rows() == before,"reopen retains the restored Session state");
    p.redo();
    require(p.rows() == deleted,"reopened journal can replay scene deletion");
    const auto elapsed = std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-restore).count();
    std::printf("Session scene reopen and replay: %.3f ms\n",elapsed);
}

void genericDeletionUndo() {
    Project p;
    p.edit("scene.create",{{"id",11},{"ord",0}});
    p.edit("session.setStopButton",{{"track",21},{"scene",11},{"on",false}});
    p.edit("session.setSlotClip",{{"track",21},{"scene",11},{"clip",41}});
    p.edit("session.setSlotClip",{{"track",22},{"scene",11},{"clip",51}});
    p.edit("session.setLaunchMode",{{"clip",41},{"mode",2}});
    p.edit("session.setLaunchQuant",{{"clip",51},{"quant",7}});
    const auto before = p.rows();
    p.edit("clip.delete",{{"id",41}});
    require(p.scalar("SELECT count(*) FROM clip_slots WHERE track_id=21 AND scene_id=11 "
                     "AND clip_id IS NULL AND has_stop=0") == 1 &&
        p.scalar("SELECT count(*) FROM clip_launch WHERE clip_id=41") == 0,
        "generic clip deletion retains Stop override and removes owned launch settings");
    const auto deletedClip = p.rows();
    p.undo();
    require(p.rows() == before,"generic clip deletion Undo restores exact clip, slot and launch settings");
    p.redo();
    require(p.rows() == deletedClip,"generic clip deletion Redo is exact");
    p.undo();
    p.edit("track.delete",{{"id",21}});
    require(p.scalar("SELECT count(*) FROM clip_slots WHERE track_id=21") == 0 &&
        p.scalar("SELECT count(*) FROM clip_slots WHERE track_id=22 AND clip_id=51") == 1,
        "generic track deletion removes only the owned slots");
    const auto deletedTrack = p.rows();
    p.undo();
    require(p.rows() == before && p.scalar("SELECT count(*) FROM pragma_foreign_key_check") == 0,
        "generic track deletion Undo restores clip, slot, track, and launch settings exactly");
    p.redo();
    require(p.rows() == deletedTrack,"generic track deletion Redo is exact");
}
} // namespace

int main() {
    try {
        require(sessionEditOps().size() == 14,"fourteen persisted Session edit operations");
        scenesAndSettings();
        slotsAndReplacement();
        sceneDeletionAndAtomicFailure();
        genericDeletionUndo();
        std::printf("Session edit journal: %d checks passed\n",checks);
        return 0;
    } catch (const std::exception& error) {
        std::printf("FAIL after %d checks: %s\n",checks,error.what());
        return 1;
    }
}
