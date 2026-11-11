// SPDX-License-Identifier: AGPL-3.0-or-later
#include "temp_directory.hpp"
#include "adi/blob.hpp"
#include "adi/digest.hpp"
#include "adi/edit_plan.hpp"
#include "adi/history.hpp"
#include "adi/settings/store.hpp"
#include "adi/store.hpp"
#include <SQLiteCpp/SQLiteCpp.h>
#include <sqlite3.h>
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <initializer_list>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {
using namespace adi;
using namespace adi::edit_plan;
constexpr std::int64_t q=5765760;
constexpr std::int64_t ns=500000000;
int checks=0,failures=0;
void check(bool ok,const char* what) {
    ++checks;
    if(!ok) {++failures;std::printf("FAIL %s\n",what);}
}
struct Project {
    test::TempDirectory temporary{"edit_plan","owner"};
    std::unique_ptr<Store> store;
    Project() {
        StoreError error{};store=Store::create(temporary.path()/"song.adi",error);
        if(!store)throw std::runtime_error(toString(error));
        store->db().exec("INSERT INTO tracks(id,kind,name) VALUES(21,'audio','One'),(22,'midi','Two'),(23,'audio','Three');"
                        "DELETE FROM tempo_map;INSERT INTO tempo_map(pos_ticks,bpm) VALUES(0,120)");
    }
    void reopen() {
        store.reset();StoreError error{};store=Store::open(temporary.path()/"song.adi",error);
        if(!store)throw std::runtime_error(toString(error));
    }
};
// Full typed snapshots distinguish absent/NULL/empty BLOB and retain future cells.
Payload tableRows(Store& store,const char* table,const char* where="") {
    Payload rows=Payload::array();
    SQLite::Statement query(store.db(),std::string("SELECT * FROM ")+table+" "+where+" ORDER BY 1");
    while(query.executeStep()) {
        Payload row=Payload::object();
        for(int column=0;column<query.getColumnCount();++column) {
            const auto cell=query.getColumn(column);Payload value;
            if(cell.isInteger())value=cell.getInt64();
            else if(cell.isFloat())value=cell.getDouble();
            else if(cell.isText())value=cell.getString();
            else if(cell.isBlob()) {
                std::vector<std::uint8_t> bytes;
                if(cell.getBytes()>0) {
                    const auto* begin=static_cast<const std::uint8_t*>(cell.getBlob());
                    bytes.assign(begin,begin+cell.getBytes());
                }
                value=Payload::binary(bytes);
            }
            row[query.getColumnName(column)]=Payload::array({cell.getType(),value});
        }
        rows.push_back(std::move(row));
    }
    return rows;
}
Payload typedRows(Store& store) {
    Payload result=Payload::object();
    for(const char* table:{"clips","event_streams","note_expression","audio_clips","media_files",
        "automation_lanes","automation_data","automation_segment_ranges","clip_properties",
        "clip_playback_details","clip_warp_fade_domains","tracks","lanes","project",
        "tempo_map","time_signature_map","key_map","markers","session_state","routing","device_chains","devices"})
        result[table]=tableRows(store,table);
    return result;
}
void addClip(Store& store,std::int64_t id,std::int64_t track,int base,std::int64_t begin,
             std::int64_t length,const char* kind="midi") {
    SQLite::Statement insert(store.db(),base==0?
        "INSERT INTO clips(id,track_id,kind,name,pos_ticks,length_ticks) VALUES(?,?,?, ?,?,?)":
        "INSERT INTO clips(id,track_id,kind,name,time_base,pos_ticks,length_ticks,pos_ns,length_ns) VALUES(?,?,?, ?,1,NULL,NULL,?,?)");
    insert.bind(1,id);insert.bind(2,track);insert.bind(3,kind);insert.bind(4,"Material "+std::to_string(id));
    insert.bind(5,begin);insert.bind(6,length);insert.exec();
}
bool spanIs(Store& store,std::int64_t id,std::int64_t track,int base,std::int64_t begin,
            std::int64_t length,std::int64_t offset=0) {
    SQLite::Statement query(store.db(),base==0?
        "SELECT count(*) FROM clips WHERE id=? AND track_id=? AND time_base=0 AND pos_ticks=? AND length_ticks=? AND content_offset_ticks=?":
        "SELECT count(*) FROM clips WHERE id=? AND track_id=? AND time_base=1 AND pos_ns=? AND length_ns=? AND content_offset_ticks=?");
    query.bind(1,id);query.bind(2,track);query.bind(3,begin);query.bind(4,length);query.bind(5,offset);
    return query.executeStep() && query.getColumn(0).getInt()==1;
}
EditScope scope(int base=0,std::int64_t end=32*q) {
    const std::array<std::int64_t,3> tracks{21,22,23};
    return materialScope(tracks,base==0?PlacementClock::Ticks:PlacementClock::Nanoseconds,{0,end});
}
clip_edit::PreparedEdit move(Store& store,const EditScope& where,std::initializer_list<Move> moves,
                            const Options& options={}) {
    return prepareMove(store,where,std::span<const Move>{moves.begin(),moves.size()},options);
}
Options locked() {Options result;result.policy.automationFollowsClips=false;return result;}
bool commit(Store& store,const clip_edit::PreparedEdit& edit) {
    if(!edit.ok) {std::printf("prepare: %s\n",edit.error.c_str());return false;}
    const auto result=clip_edit::commitPrepared(store,edit);
    if(!result.ok)std::printf("commit: %s\n",result.error.c_str());
    return result.ok;
}
struct ReadGuard {
    Payload rows;
    std::string digest;
    std::optional<std::int64_t> head;
    std::int64_t count=0;
    int writes=0;
    explicit ReadGuard(Store& store):rows(typedRows(store)),digest(digestProject(store).text),
        head(OpJournal(store).headSeq()),count(OpJournal(store).count()),
        writes(sqlite3_total_changes(store.db().getHandle())) {}
    bool unchanged(Store& store,bool checkWrites=true) const {
        return typedRows(store)==rows && digestProject(store).text==digest &&
            OpJournal(store).headSeq()==head && OpJournal(store).count()==count &&
            (!checkWrites || sqlite3_total_changes(store.db().getHandle())==writes);
    }
};
const Effect* effectFor(const Description& description,std::int64_t id) {
    const auto found=std::find_if(description.effects.begin(),description.effects.end(),
        [id](const Effect& effect){return effect.before && effect.before->id==id;});
    return found==description.effects.end()?nullptr:&*found;
}
void anchoredLeftTrim() {
    for(const int base:{0,1})for(const bool audio:{false,true}) {
        Project project;auto& store=*project.store;auto& db=store.db();const auto unit=base==0?q:ns;
        addClip(store,7,21,base,4*unit,4*unit,audio?"audio":"midi");
        db.exec("UPDATE clips SET content_offset_ticks=5765760,fade_in_ticks=11531520,fade_out_ticks=17297280,loop_enabled=1,loop_start_ticks=5765760,loop_len_ticks=5765760");
        if(audio)db.exec("INSERT INTO media_files(id,hash_blake3,format) VALUES(300,'trim-source','wav');"
                         "INSERT INTO audio_clips(clip_id,media_id,src_start_frames,src_len_frames) VALUES(7,300,0,4000000);"
                         "INSERT INTO clip_warp_fade_domains(clip_id,in_begin_ticks,in_end_ticks,out_begin_ticks,out_end_ticks) VALUES(7,5765760,17297280,11531520,28828800)");
        else db.exec("INSERT INTO event_streams(id,clip_id,stream_kind,data) VALUES(61,7,'notes',X'010203');"
                     "INSERT INTO note_expression(clip_id,note_id,dimension,data) VALUES(7,42,1,X'FF00')");
        const ReadGuard before(store);
        const auto prepared=prepareTrim(store,scope(base,32*unit),{7,{base,6*unit}},locked());
        check(prepared.ok && before.unchanged(store),"left trim preparation is a read-only captured edit in either native clock");
        if(!prepared.ok){std::printf("trim: %s\n",prepared.error.c_str());continue;}
        const auto description=describe(prepared);
        check(description.ok && description.effects.size()==1 && description.effects.front().kind==EffectKind::Trimmed &&
                  description.effects.front().after.size()==1 && description.effects.front().after.front().nativeSpan==PlacementSpan{6*unit,8*unit},
              "trim preview describes its actual anchored native extent");
        check(commit(store,prepared) && spanIs(store,7,21,base,6*unit,2*unit,3*q) &&
                  db.execAndGet("SELECT count(*) FROM clips WHERE fade_in_ticks=11531520 AND fade_out_ticks=11531520 AND loop_enabled=1 AND loop_start_ticks=5765760 AND loop_len_ticks=5765760").getInt()==1,
              "left trim advances source ticks, keeps the exact end/loop and clamps both fade handles");
        if(audio)check(db.execAndGet("SELECT content_offset_ns FROM clip_playback_details WHERE clip_id=7").getInt64()==1500000000 &&
                       db.execAndGet("SELECT in_begin_ns FROM clip_playback_details WHERE clip_id=7").getInt64()==1500000000 &&
                       db.execAndGet("SELECT out_end_ns FROM clip_playback_details WHERE clip_id=7").getInt64()==2500000000 &&
                       db.execAndGet("SELECT out_begin_ns FROM clip_playback_details WHERE clip_id=7").getInt64()==1500000000 &&
                       tableRows(store,"clip_warp_fade_domains")==before.rows.at("clip_warp_fade_domains"),
                       "unwarped trim advances pinned PCM and retargets/clamps physical fade domains");
        else check(tableRows(store,"event_streams")==before.rows.at("event_streams") &&
                   tableRows(store,"note_expression")==before.rows.at("note_expression"),
                   "trim retains opaque note and expression bytes; Crop is a separate content intent");
        const auto after=typedRows(store);const auto step=History(store).nextUndo();
        check(step && step->opCount==1 && History(store).undo().ok && typedRows(store)==before.rows &&
                  digestProject(store).text==before.digest && History(store).redo().ok && typedRows(store)==after &&
                  History(store).undo().ok && typedRows(store)==before.rows,
              "trim is one splice with exact Undo/Redo/Undo, including absence of physical clock rows");
        const auto expanded=prepareTrim(store,scope(base,32*unit),{7,{base,unit}},locked());
        check(commit(store,expanded) && spanIs(store,7,21,base,audio?3*unit:unit,audio?5*unit:7*unit,audio?0:-2*q),
              "audio extension clamps to source zero while MIDI can expose a signed source prefix");
        check(History(store).undo().ok && typedRows(store)==before.rows,"source-zero clamp and signed extension undo exactly");
    }
    Project zero;auto& store=*zero.store;
    addClip(store,7,21,0,4*q,4*q,"audio");
    store.db().exec("INSERT INTO media_files(id,hash_blake3,format) VALUES(300,'zero','wav');"
                    "INSERT INTO audio_clips(clip_id,media_id,src_start_frames,src_len_frames) VALUES(7,300,0,4000000)");
    const ReadGuard original(store);
    const auto clamped=prepareTrim(store,scope(),{7,{0,0}},locked());
    check(clamped.ok && clamped.requests.empty() && original.unchanged(store) && commit(store,clamped) && original.unchanged(store),
          "clamped source-zero no-op creates neither clock rows nor journal entries");
    check(!prepareTrim(store,scope(),{7,{0,9*q}},locked()).ok && original.unchanged(store),
          "trim beyond anchored end refuses without writes");
    check(!prepareTrim(store,scope(),{7,{1,0}},locked()).ok && original.unchanged(store),
          "trim clock must match explicit scope clock");
}
void trimPhysicalPinsAndCapture() {
    Project project;auto& store=*project.store;auto& db=store.db();
    addClip(store,7,21,0,4*q,4*q,"audio");
    db.exec("DELETE FROM tempo_map;INSERT INTO tempo_map(pos_ticks,bpm) VALUES(0,120),(11531520,240);"
            "UPDATE clips SET content_offset_ticks=5765760;"
            "INSERT INTO media_files(id,hash_blake3,format) VALUES(300,'pin','wav');"
            "INSERT INTO audio_clips(clip_id,media_id,src_start_frames,src_len_frames) VALUES(7,300,0,4000000);"
            "INSERT INTO clip_playback_details(clip_id,content_offset_ns,in_begin_ns,in_end_ns,out_begin_ns,out_end_ns) VALUES(7,500000000,100000013,200000019,100000013,200000019)");
    const ReadGuard before(store);const auto source=tableRows(store,"audio_clips");
    const auto trim=prepareTrim(store,scope(),{7,{0,0}},locked());
    check(trim.ok && before.unchanged(store) && commit(store,trim) && spanIs(store,7,21,0,2*q,6*q,-q),
          "physical source-zero clamp across tempo changes can retain a signed derived tick offset");
    check(db.execAndGet("SELECT content_offset_ns FROM clip_playback_details WHERE clip_id=7").getInt64()==0 &&
              db.execAndGet("SELECT count(*) FROM clip_playback_details WHERE in_begin_ns=100000013 AND in_end_ns=200000019 AND out_begin_ns=100000013 AND out_end_ns=200000019").getInt()==1 &&
              db.execAndGet("SELECT count(*) FROM clips WHERE fade_in_ticks=0 AND fade_out_ticks=0").getInt()==1 &&
              tableRows(store,"audio_clips")==source,
          "trim preserves exact dormant inherited domains and source metadata without synthesizing fades");
    check(History(store).undo().ok && typedRows(store)==before.rows,"tempo-crossing physical clamp undoes all source and fade rows exactly");
    auto stale=prepareTrim(store,scope(),{7,{0,5*q}},locked());
    OpRequest rename;rename.opType="track.rename";rename.payload={{"id",21},{"name","Intervening"}};
    check(OpJournal(store).commit(rename).ok,"intervening real op advances the trim snapshot head");
    const ReadGuard intervening(store);const auto refused=clip_edit::commitPrepared(store,stale);
    check(!refused.ok && refused.stale && intervening.unchanged(store),"stale trim refuses without writes or another history entry");
    auto fresh=prepareTrim(store,scope(),{7,{0,5*q}},locked());
    db.exec("CREATE TEMP TRIGGER trim_failure BEFORE UPDATE OF pos_ticks ON clips BEGIN SELECT RAISE(ABORT,'trim rollback');END");
    const auto count=OpJournal(store).count();const auto head=OpJournal(store).headSeq();
    check(!clip_edit::commitPrepared(store,fresh).ok && typedRows(store)==intervening.rows &&
              digestProject(store).text==intervening.digest && OpJournal(store).count()==count && OpJournal(store).headSeq()==head,
          "actual trim publication failure rolls back source-clock updates and audit rows atomically");
    db.exec("DROP TRIGGER trim_failure");
    check(commit(store,fresh),"the authentic captured trim can retry after a rolled-back publication");
    Project tied;auto& fixed=*tied.store;addClip(fixed,7,21,1,5,34999995,"audio");
    fixed.db().exec("UPDATE clips SET fade_out_ticks=24024;"
                    "INSERT INTO media_files(id,hash_blake3,format) VALUES(300,'tie','wav');"
                    "INSERT INTO audio_clips(clip_id,media_id,src_start_frames,src_len_frames) VALUES(7,300,0,4000000)");
    auto options=locked();options.referenceRate=44100;
    const auto physical=prepareTrim(fixed,scope(1,100000000),{7,{1,10000000}},options);
    check(commit(fixed,physical) && fixed.db().execAndGet("SELECT out_end_ns FROM clip_playback_details WHERE clip_id=7").getInt64()==35011338,
          "freezing and trimming a fractional native endpoint pins the correct final fade sample");
}
void trimWarpFadeDomains() {
    Project project;auto& store=*project.store;auto& db=store.db();
    addClip(store,7,21,0,4*q,4*q,"audio");
    db.exec("UPDATE clips SET content_offset_ticks=5765760,fade_in_ticks=11531520,fade_out_ticks=17297280;"
            "INSERT INTO media_files(id,hash_blake3,format) VALUES(300,'warp-domain','wav');"
            "INSERT INTO audio_clips(clip_id,media_id,src_start_frames,src_len_frames,warp_enabled) VALUES(7,300,0,4000000,1)");
    const ReadGuard before(store);const auto trim=prepareTrim(store,scope(),{7,{0,6*q}},locked());
    check(trim.ok && before.unchanged(store) && commit(store,trim) && spanIs(store,7,21,0,6*q,2*q,3*q),
          "warped trim uses the musical source clock without an unwarped physical pin");
    check(db.execAndGet("SELECT count(*) FROM clip_warp_fade_domains WHERE clip_id=7 AND in_begin_ticks=17297280 AND in_end_ticks=28828800 AND out_begin_ticks=17297280 AND out_end_ticks=28828800").getInt()==1 &&
              db.execAndGet("SELECT count(*) FROM clip_playback_details").getInt()==0,
          "warped trim retargets incoming and clamps outgoing tick fade domains to the retained span");
    check(History(store).undo().ok && typedRows(store)==before.rows,"warped fade trim undo restores absent domain rows");
    db.exec("INSERT INTO clip_playback_details(clip_id,content_offset_ns,source_end_ns) VALUES(7,0,250000000)");
    const auto oldPhysical=tableRows(store,"clip_playback_details");
    const auto extension=prepareTrim(store,scope(),{7,{0,0}},locked());
    check(commit(store,extension) && spanIs(store,7,21,0,3*q,5*q,0) &&
              tableRows(store,"clip_playback_details")==oldPhysical,
          "warped source-zero extension uses ticks and preserves its ignored obsolete physical pins");
}
void relationsAndSurvivors() {
    // Victim [2,6): disjoint, both touches, whole, prefix, suffix and middle.
    const std::array<std::pair<int,int>,8> masks={{{0,1},{0,2},{6,7},{2,6},{1,3},{5,7},{3,5},{2,3}}};
    for(const int base:{0,1})for(const auto& [begin,end]:masks) {
        Project p;auto& store=*p.store;auto& db=store.db();const auto unit=base==0?q:ns;
        addClip(store,7,21,base,2*unit,4*unit);
        addClip(store,8,22,base,10*unit,(end-begin)*unit);
        addClip(store,9,23,base,12*unit,unit);
        db.exec("UPDATE clips SET content_offset_ticks=2882880 WHERE id=7;UPDATE clips SET alias_of=7 WHERE id=9;"
                "ALTER TABLE clips ADD COLUMN future_root BLOB NOT NULL DEFAULT X'';"
                "ALTER TABLE event_streams ADD COLUMN future_blob BLOB;"
                "ALTER TABLE event_streams ADD COLUMN future_null TEXT;"
                "UPDATE clips SET future_root=X'0001FF' WHERE id=7;"
                "INSERT INTO event_streams(id,clip_id,stream_kind,data,future_blob) VALUES(61,7,'notes',X'00FF01',X'');"
                "INSERT INTO note_expression(id,clip_id,note_id,dimension,data) VALUES(62,7,-1,0,X'040506');"
                "INSERT INTO clip_properties(clip_id,end_marker_ticks,signature_numerator,signature_denominator) VALUES(7,57657600,3,4)");
        const ReadGuard before(store);
        const auto edit=move(store,scope(base,20*unit),{{8,{21,{}},{base,begin*unit}}});
        check(edit.ok && before.unchanged(store),"placement relation capture is read-only in both clocks");
        if(!edit.ok)continue;
        const auto description=describe(edit);
        const bool intersects=begin<6 && end>2,left=begin>2,right=end<6;
        const auto* victim=effectFor(description,7);
        const auto expected=left&&right?EffectKind::Split:(left||right?EffectKind::Trimmed:EffectKind::Removed);
        check(description.ok && (intersects?(victim && victim->kind==expected):victim==nullptr),
              "description names trim/split/removal lineage and omits touching victims");
        const bool applied=commit(store,edit);
        check(applied && edit.requests.size()==1 && edit.requests.front().opType=="arranger.splice",
              "placement clears and moves in one arrangement splice");
        if(!applied)continue;
        check(spanIs(store,8,21,base,begin*unit,(end-begin)*unit),"incoming identity is placed at its literal destination");
        if(!intersects) {
            check(spanIs(store,7,21,base,2*unit,4*unit,q/2) &&
                  db.execAndGet("SELECT count(*) FROM event_streams WHERE id=61 AND clip_id=7").getInt()==1,
                  "half-open touching and disjoint material retain root and child identities");
        } else if(left || right) {
            const int first=left?2:std::max(end,2),last=left?std::min(begin,6):6;
            check(spanIs(store,7,21,base,first*unit,(last-first)*unit,q/2+(first-2)*q) &&
                  db.execAndGet("SELECT count(*) FROM event_streams WHERE id=61 AND clip_id=7 AND hex(data)='00FF01' AND typeof(future_blob)='blob' AND length(future_blob)=0 AND future_null IS NULL").getInt()==1 &&
                  db.execAndGet("SELECT count(*) FROM note_expression WHERE id=62 AND clip_id=7 AND note_id=-1 AND hex(data)='040506'").getInt()==1 &&
                  db.execAndGet("SELECT count(*) FROM clips WHERE id=9 AND alias_of=7").getInt()==1,
                  "sole or left survivor keeps original IDs, opaque children, typed future cells and incoming alias");
            if(left && right) {
                SQLite::Statement tail(db,"SELECT id FROM clips WHERE id NOT IN(7,8,9)");const bool found=tail.executeStep();
                const auto id=found?tail.getColumn(0).getInt64():0;
                check(found && spanIs(store,id,21,base,end*unit,(6-end)*unit,q/2+(end-2)*q) &&
                      db.execAndGet("SELECT count(*) FROM clips WHERE hex(future_root)='0001FF'").getInt()==2 &&
                      db.execAndGet("SELECT count(*) FROM event_streams WHERE hex(data)='00FF01'").getInt()==2 &&
                      db.execAndGet("SELECT count(*) FROM clips").getInt()==4,
                      "interior replacement creates exactly one independently keyed offset-correct tail");
                check(victim && victim->after.size()==2 && victim->after.front().id==7 &&
                      victim->after.back().id==id && victim->before->name=="Material 7" &&
                      victim->after.back().span==PlacementSpan{end*unit,6*unit},
                      "split description owns exact original name, retained ID and newly allocated tail extent");
            }
        } else {
            check(db.execAndGet("SELECT count(*) FROM clips WHERE id=7").getInt()==0 &&
                  db.execAndGet("SELECT count(*) FROM event_streams WHERE clip_id=7").getInt()==0 &&
                  db.execAndGet("SELECT count(*) FROM clips WHERE id=9 AND alias_of IS NULL").getInt()==1,
                  "whole coverage removes native children and explicitly detaches the external alias");
        }
        const auto after=typedRows(store);const auto step=History(store).nextUndo();
        check(step && step->opCount==1 && History(store).undo().ok && typedRows(store)==before.rows &&
              History(store).redo().ok && typedRows(store)==after,
              "one placement undo/redo restores exact typed rows, identity and optional row absence");
    }
}
void swapsAndSparseGroups() {
    Project a,b;
    for(Store* store:{a.store.get(),b.store.get()}) {
        addClip(*store,7,21,0,0,q);addClip(*store,8,22,0,2*q,q);addClip(*store,9,23,0,4*q,q);
        store->db().exec("INSERT INTO event_streams(id,clip_id,stream_kind,data) VALUES(61,7,'notes',X'00FF'),(62,8,'notes',X'FF00');UPDATE clips SET alias_of=7 WHERE id=9");
    }
    const auto before=typedRows(*a.store);
    const auto first=move(*a.store,scope(),{{7,{22,{}},{0,2*q}},{8,{21,{}},{0,0}}});
    const auto reverse=move(*b.store,scope(),{{8,{21,{}},{0,0}},{7,{22,{}},{0,2*q}}});
    const bool applied=commit(*a.store,first) && commit(*b.store,reverse);
    check(applied && tableRows(*a.store,"clips")==tableRows(*b.store,"clips") &&
          tableRows(*a.store,"event_streams")==tableRows(*b.store,"event_streams"),
          "group swap is independent of input order and clears before placing all members");
    check(applied && spanIs(*a.store,7,22,0,2*q,q) && spanIs(*a.store,8,21,0,0,q) &&
          tableRows(*a.store,"event_streams")==before.at("event_streams") &&
          a.store->db().execAndGet("SELECT count(*) FROM clips WHERE id=9 AND alias_of=7").getInt()==1,
          "selected originals are never victims: group move retains child IDs and external aliases");
    const auto desc=describe(first);const auto* effect=effectFor(desc,7);
    check(desc.ok && desc.effects.size()==2 && effect && effect->kind==EffectKind::Moved &&
          effect->before->lane.track==21 && effect->after.size()==1 && effect->after.front().lane.track==22,
          "group description captures source and destination lanes for each moved identity");
    Project p;auto& store=*p.store;
    addClip(store,7,21,0,0,10*q);addClip(store,8,22,0,12*q,q);addClip(store,9,22,0,14*q,q);
    store.db().exec("INSERT INTO event_streams(id,clip_id,stream_kind,data) VALUES(61,7,'notes',X'010203')");
    const ReadGuard initial(store);
    const auto sparse=move(store,scope(),{{8,{21,{}},{0,2*q}},{9,{21,{}},{0,6*q}}});
    check(sparse.ok && initial.unchanged(store) && commit(store,sparse),"disjoint group destinations prepare together against one captured original");
    bool spans=spanIs(store,7,21,0,0,2*q);
    SQLite::Statement survivors(store.db(),"SELECT pos_ticks,length_ticks,content_offset_ticks FROM clips WHERE id NOT IN(8,9) ORDER BY pos_ticks");
    std::vector<std::array<std::int64_t,3>> actual;
    while(survivors.executeStep())actual.push_back({survivors.getColumn(0).getInt64(),survivors.getColumn(1).getInt64(),survivors.getColumn(2).getInt64()});
    const std::vector<std::array<std::int64_t,3>> expected{{0,2*q,0},{3*q,3*q,3*q},{7*q,3*q,7*q}};
    check(spans && actual==expected && store.db().execAndGet("SELECT count(*) FROM event_streams WHERE hex(data)='010203'").getInt()==3,
          "union of sparse masks retains the gap and cuts one staged original into three exact source pieces");
    check(History(store).undo().ok && typedRows(store)==initial.rows,"sparse-mask undo restores the one original and its child");
}
void completeChildrenAndResize() {
    for(const int base:{0,1}) {
        Project p;auto& store=*p.store;auto& db=store.db();const auto unit=base==0?q:ns;
        addClip(store,7,21,base,unit,4*unit,"audio");addClip(store,8,22,base,unit,4*unit);
        addClip(store,9,23,base,12*unit,unit);db.exec("UPDATE clips SET alias_of=7 WHERE id=9");
        db.exec("ALTER TABLE clips ADD COLUMN future_root BLOB;ALTER TABLE audio_clips ADD COLUMN future_blob BLOB;"
                "UPDATE clips SET content_offset_ticks=2882880,fade_in_ticks=11531520,fade_out_ticks=11531520,loop_enabled=1,loop_start_ticks=0,loop_len_ticks=23063040,future_root=X'00FF' WHERE id IN(7,8);"
                "INSERT INTO media_files(id,hash_blake3,rel_path,format,sample_rate,channels,frames) VALUES(300,'plan-audio','plan.wav','wav',48000,1,480000);"
                "INSERT INTO audio_clips(clip_id,media_id,src_start_frames,src_len_frames,future_blob) VALUES(7,300,17,400000,X'');"
                "INSERT INTO event_streams(id,clip_id,stream_kind,data) VALUES(61,8,'notes',X'010203');"
                "INSERT INTO note_expression(id,clip_id,note_id,dimension,data) VALUES(62,8,-1,0,X'040506');"
                "INSERT INTO clip_properties(clip_id,end_marker_ticks,signature_numerator,signature_denominator) VALUES(7,34594560,3,4),(8,34594560,3,4);"
                "INSERT INTO automation_lanes(id,owner_kind,owner_id,param_ref,value_domain) VALUES(91,'track',22,'pan','real');"
                "INSERT INTO automation_data(id,lane_id,clip_id,data) VALUES(71,91,8,X'070809');"
                "INSERT INTO automation_segment_ranges(id,data_id,clip_id,record_index,progress_begin,progress_end,source_first,source_last) VALUES(72,71,8,0,0.2,0.7,0,1);"
                "INSERT INTO clip_playback_details(clip_id,content_offset_ns,in_tension,out_tension) VALUES(7,0,0.4,-0.2)");
        const ReadGuard before(store);
        const auto moved=move(store,scope(base,20*unit),{{7,{22,{}},{base,7*unit}},{8,{21,{}},{base,7*unit}}},locked());
        check(moved.ok && before.unchanged(store) && commit(store,moved),"captured group move freezes source clocks without writing during preparation");
        const auto after=typedRows(store);bool children=true;
        for(const char* table:{"event_streams","note_expression","audio_clips","automation_data","automation_segment_ranges","clip_properties"})
            children=children && before.rows.at(table)==after.at(table);
        check(children && spanIs(store,7,22,base,7*unit,4*unit,q/2) && spanIs(store,8,21,base,7*unit,4*unit,q/2) &&
              db.execAndGet("SELECT count(*) FROM clip_playback_details WHERE clip_id=7 AND content_offset_ns=0 AND loop_begin_ns=0 AND loop_end_ns=2000000000 AND source_end_ns=3000000000").getInt()==1 &&
              db.execAndGet("SELECT count(*) FROM clips WHERE id=9 AND alias_of=7").getInt()==1,
              "cross-track move retains opaque rows, source phase including pinned zero, loops, end marker and alias identity");
        const auto resized=prepareResizeEnd(store,scope(base,20*unit),{7,{base,8*unit}},locked());
        const auto resizedDescription=describe(resized);const auto* resizeEffect=effectFor(resizedDescription,7);
        check(resizedDescription.ok && resizeEffect && resizeEffect->kind==EffectKind::Resized &&
              resizeEffect->after.size()==1 && resizeEffect->after.front().span==PlacementSpan{7*unit,8*unit} && commit(store,resized),
              "end resize captures its anchored-start effect and commits through the placement engine");
        check(spanIs(store,7,22,base,7*unit,unit,q/2) &&
              db.execAndGet("SELECT count(*) FROM clips WHERE id=7 AND fade_in_ticks=5765760 AND fade_out_ticks=5765760").getInt()==1 &&
              db.execAndGet("SELECT count(*) FROM clip_playback_details WHERE clip_id=7 AND content_offset_ns=0 AND in_begin_ns=0 AND in_end_ns=500000000 AND out_begin_ns=0 AND out_end_ns=500000000 AND loop_end_ns=2000000000 AND source_end_ns=3000000000").getInt()==1,
              "anchored resize clamps fade handles and source fade domains while keeping source phase, loop and source end");
        check(History(store).undo().ok && typedRows(store)==after && History(store).undo().ok && typedRows(store)==before.rows,
              "move and resize undo separately and restore exact whole typed children and original optional clocks");
    }
}
void explicitScopeAndConversions() {
    Project p;auto& store=*p.store;
    addClip(store,7,21,0,q,2*q);addClip(store,8,22,1,4*ns,ns);
    store.db().exec("INSERT INTO lanes(id,track_id,kind) VALUES(301,21,'take')");
    addClip(store,9,21,0,0,20*q);store.db().exec("UPDATE clips SET lane_id=301 WHERE id=9");
    const auto take=tableRows(store,"clips","WHERE lane_id IS NOT NULL");
    // A 120 -> 60 BPM boundary at 2q makes tick/ns conversion observable.
    store.db().exec("INSERT INTO tempo_map(pos_ticks,bpm) VALUES(11531520,60)");
    const ReadGuard before(store);
    auto nsScope=scope(1,12*ns);
    const auto converted=move(store,nsScope,{{7,{22,{}},{1,4*ns}}});
    check(converted.ok && before.unchanged(store) && commit(store,converted),"nanosecond scope converts a musical move through the captured tempo map");
    check(spanIs(store,7,22,0,3*q,2*q) && tableRows(store,"clips","WHERE lane_id IS NOT NULL")==take &&
          store.db().execAndGet("SELECT count(*) FROM clips WHERE id=8").getInt()==0,
          "native tick length is retained at the converted start, destination ns material is cleared and take lane stays untouched");
    check(History(store).undo().ok && typedRows(store)==before.rows,"mixed-clock replacement undo restores both native time representations");
    auto reject=[&](const EditScope& where,std::initializer_list<Move> moves,const Options& options,const char* reason) {
        const ReadGuard guard(store);const auto edit=move(store,where,moves,options);
        check(!edit.ok && !edit.error.empty() && guard.unchanged(store),reason);
    };
    auto limited=scope();limited.targets={{22,{}}};
    reject(limited,{{7,{22,{}},{0,4*q}}},{} ,"source lane outside literal scope refuses before writes");
    limited=scope();limited.targets={{21,{}}};
    reject(limited,{{7,{22,{}},{0,4*q}}},{},"destination lane outside literal scope refuses before writes");
    limited=scope();limited.bounds={2*q,10*q};
    reject(limited,{{7,{22,{}},{0,4*q}}},{},"source extent outside literal bounds refuses rather than silently expanding scope");
    limited=scope();limited.bounds={0,4*q};
    reject(limited,{{7,{22,{}},{0,3*q}}},{},"destination end outside literal bounds refuses");
    limited=scope();limited.targets.push_back(limited.targets.front());
    reject(limited,{{7,{22,{}},{0,4*q}}},{},"duplicate literal targets refuse");
    limited=scope();limited.targets.push_back({999,{}});
    reject(limited,{{7,{22,{}},{0,4*q}}},{},"nonexistent target track refuses");
    limited=scope();limited.targets.push_back({21,301});
    reject(limited,{{7,{22,{}},{0,4*q}}},{},"unsupported take-lane scope refuses instead of flattening lanes");
    limited=scope();limited.domains=limited.domains|Domains::Markers;
    reject(limited,{{7,{22,{}},{0,4*q}}},{},"unimplemented scope domains refuse explicitly");
    limited=scope();limited.clock=static_cast<PlacementClock>(19);
    reject(limited,{{7,{22,{}},{0,4*q}}},{},"undeclared operation clock refuses");
    reject(scope(),{{7,{22,{}},{3,4*q}}},{},"invalid destination clock refuses");
    reject(scope(),{{7,{22,{}},{0,-1}}},{},"negative destination coordinate refuses");
    reject(scope(),{{7,{22,{}},{0,4*q}},{7,{23,{}},{0,5*q}}},{},"duplicate relocated identity refuses");
    reject(scope(),{{7,{23,{}},{0,4*q}},{8,{23,{}},{0,4*q}}},{},"overlapping incoming members refuse without choosing an order-dependent winner");
    reject(scope(),{{9,{22,{}},{0,4*q}}},{},"main-lane intent does not silently move an original take clip");
    Options bad;bad.referenceRate=std::numeric_limits<double>::quiet_NaN();
    reject(scope(),{{7,{22,{}},{0,4*q}}},bad,"nonfinite reference rate refuses");
    bad={};bad.referenceRate=0;reject(scope(),{{7,{22,{}},{0,4*q}}},bad,"zero reference rate refuses");
    bad={};bad.policy.deleteOverlaps=false;
    reject(scope(),{{7,{22,{}},{0,4*q}}},bad,"unimplemented deleteOverlaps=false policy is not silently ignored");
    bad={};bad.policy.ripple=Ripple::AllTracks;
    reject(scope(),{{7,{22,{}},{0,4*q}}},bad,"unimplemented ripple policy refuses explicitly");
    auto enormous=scope(0,std::numeric_limits<std::int64_t>::max());
    reject(enormous,{{7,{22,{}},{0,std::numeric_limits<std::int64_t>::max()-q}}},{},"overflowing final extent refuses before mutation");
    const ReadGuard resizeGuard(store);
    const auto backwards=prepareResizeEnd(store,scope(),{7,{0,0}},{});
    check(!backwards.ok && resizeGuard.unchanged(store),"end resize cannot cross its anchored start");
    Project tiny;addClip(*tiny.store,7,21,1,0,1);
    const ReadGuard tinyBefore(*tiny.store);
    const auto collapsed=move(*tiny.store,scope(),{{7,{22,{}},{0,q}}});
    check(collapsed.ok && tinyBefore.unchanged(*tiny.store) && commit(*tiny.store,collapsed) &&
          spanIs(*tiny.store,7,22,1,ns,1),
          "physically positive one-nanosecond clip remains editable when its projected tick description collapses");
    const auto tinyDescription=describe(collapsed);const auto* tinyEffect=effectFor(tinyDescription,7);
    check(tinyDescription.ok && tinyEffect && tinyEffect->after.size()==1 &&
          tinyEffect->after.front().nativeClock==PlacementClock::Nanoseconds &&
          tinyEffect->after.front().nativeSpan==PlacementSpan{ns,ns+1} &&
          tinyEffect->after.front().physicalSpan==PlacementSpan{ns,ns+1} &&
          tinyEffect->after.front().span==PlacementSpan{q,q},
          "description retains exact native and physical endpoints when projected musical endpoints coincide");
    check(History(*tiny.store).undo().ok && typedRows(*tiny.store)==tinyBefore.rows,
          "one-nanosecond native extent survives exact undo under a musical operation scope");
    Project cut;addClip(*cut.store,7,21,0,0,1);addClip(*cut.store,8,22,1,5000,1);
    const ReadGuard cutBefore(*cut.store);
    const auto unrepresentable=move(*cut.store,scope(1,10000),{{8,{21,{}},{1,1}}});
    check(!unrepresentable.ok && cutBefore.unchanged(*cut.store),
          "a required positive survivor that collapses in its native clock refuses rather than dropping material");
}
void roundedPhysicalBoundaries() {
    Project p;auto& store=*p.store;addClip(store,7,21,1,0,900);addClip(store,8,22,0,100,10);
    const ReadGuard before(store);const auto edit=move(store,scope(0,1000),{{8,{21,{}},{0,10}}});
    constexpr std::int64_t roundedTen=(10*ns+q/2)/q;
    check(roundedTen==867 && edit.ok && before.unchanged(store) && commit(store,edit) &&
          spanIs(store,7,21,1,0,roundedTen) && spanIs(store,8,21,0,10,10),
          "rounded musical touching cannot hide physical overlap: ns victim ends at the incoming 867ns boundary");
    const auto description=describe(edit);const auto* effect=effectFor(description,8);
    check(description.ok && effect && effect->after.size()==1 &&
          effect->after.front().nativeClock==PlacementClock::Ticks && effect->after.front().nativeSpan==PlacementSpan{10,20} &&
          effect->after.front().physicalSpan==PlacementSpan{roundedTen,(20*ns+q/2)/q},
          "mixed-clock description owns exact tick geometry and independently calculated physical nanoseconds");
    check(History(store).undo().ok && typedRows(store)==before.rows,
          "mixed-clock rounded boundary clear restores the exact 900ns original on Undo");
    Project reverse;auto& other=*reverse.store;addClip(other,7,21,0,0,20);addClip(other,8,22,1,5000,100);
    const ReadGuard reverseBefore(other);const auto placed=move(other,scope(1,10000),{{8,{21,{}},{1,900}}});
    check(placed.ok && reverseBefore.unchanged(other) && commit(other,placed) &&
          spanIs(other,7,21,0,0,10) && spanIs(other,8,21,1,900,100) &&
          other.db().execAndGet("SELECT count(*) FROM clips WHERE id NOT IN(7,8) AND pos_ticks=12 AND length_ticks=8 AND content_offset_ticks=12").getInt()==1,
          "reverse ns clear rounds tick prefix down and tail start up, leaving a harmless gap instead of renewed overlap");
    constexpr std::int64_t floorEnd=(10*ns+q/2)/q,ceilBegin=(12*ns+q/2)/q;
    check(floorEnd<=900 && ceilBegin>=1000,"independent fixed-tempo physical arithmetic proves both directed tick survivors stay outside the clear span");
    check(History(other).undo().ok && typedRows(other)==reverseBefore.rows,
          "directed native-clock cuts preserve exact outer endpoints and typed Undo");
}
void signedIdentitiesAndKinds() {
    Project p;auto& store=*p.store;
    store.db().exec("INSERT INTO tracks(id,kind,name) VALUES(0,'audio','Zero'),(-21,'midi','Negative')");
    addClip(store,0,0,0,0,q,"midi");addClip(store,-7,-21,0,2*q,q,"audio");
    store.db().exec("INSERT INTO event_streams(id,clip_id,stream_kind,data) VALUES(-61,0,'notes',X'00FF')");
    const auto before=typedRows(store);const std::array<std::int64_t,2> tracks{0,-21};
    const auto where=materialScope(tracks,PlacementClock::Ticks,{0,8*q});
    const auto edit=move(store,where,{{0,{-21,{}},{0,4*q}},{-7,{0,{}},{0,4*q}}});
    check(edit.ok && commit(store,edit) && spanIs(store,0,-21,0,4*q,q) && spanIs(store,-7,0,0,4*q,q) &&
          store.db().execAndGet("SELECT count(*) FROM event_streams WHERE id=-61 AND clip_id=0 AND hex(data)='00FF'").getInt()==1,
          "schema-valid zero/negative clip, track and child identities and mixed track kinds remain editable");
    const auto after=typedRows(store);
    check(History(store).undo().ok && typedRows(store)==before && History(store).redo().ok && typedRows(store)==after,
          "signed identity placement has exact native undo and redo");
}
OpRequest rename(std::int64_t track,const char* name) {
    OpRequest result;result.opType="track.rename";result.label="Rename owner test track";
    result.payload={{"id",track},{"name",name}};return result;
}
void preparationAndTransactions() {
    Project p,other;auto& store=*p.store;addClip(store,7,21,0,0,q);addClip(*other.store,7,21,0,0,q);
    const ReadGuard emptyHead(store);
    const auto unchanged=move(store,scope(),{{7,{21,{}},{0,0}}});
    const auto noMembers=move(store,scope(),{});
    check(unchanged.ok && unchanged.requests.empty() && describe(unchanged).ok && describe(unchanged).effects.empty() &&
          noMembers.ok && noMembers.requests.empty() && emptyHead.unchanged(store),
          "unchanged and empty group intents produce a read-only described no-op instead of an empty row-delta failure");
    auto edit=move(store,scope(),{{7,{22,{}},{0,3*q}}});
    check(edit.ok && edit.options.expectHead && !*edit.options.expectHead && emptyHead.unchanged(store),
          "prepare captures an explicitly expected null head without writes");
    const auto foreignBefore=typedRows(*other.store);
    check(!clip_edit::commitPrepared(*other.store,edit).ok && typedRows(*other.store)==foreignBefore &&
          OpJournal(*other.store).count()==0,"prepared packet refuses a foreign project with matching row identities");
    check(OpJournal(store).commit(rename(21,"Head advanced")).ok,"unrelated native edit advances the null head");
    const ReadGuard staleNull(store);const auto refusedNull=clip_edit::commitPrepared(store,edit);
    check(!refusedNull.ok && refusedNull.stale && staleNull.unchanged(store),"expected null head refuses after any intervening transaction");
    edit=move(store,scope(),{{7,{22,{}},{0,3*q}}});
    check(edit.ok && edit.options.expectHead && edit.options.expectHead->has_value(),"non-null expected head is captured explicitly");
    check(OpJournal(store).commit(rename(21,"Advanced again")).ok,"second unrelated edit advances non-null head");
    const ReadGuard staleNonNull(store);const auto refused=clip_edit::commitPrepared(store,edit);
    check(!refused.ok && refused.stale && staleNonNull.unchanged(store),"non-null stale packet refuses without writes or extra history");
    edit=move(store,scope(),{{7,{22,{}},{0,3*q}}});
    auto bad=edit;OpRequest suffix;suffix.opType="clip.setMute";suffix.payload={{"id",999999},{"muted",true}};
    bad.requests.push_back(suffix);const ReadGuard rollback(store);
    check(!clip_edit::commitPrepared(store,bad).ok && rollback.unchanged(store,false),
          "late invalid suffix rolls back placement, journal, head and every typed row atomically");
    suffix.payload={{"id",7},{"muted",true}};suffix.label="Mute moved material";edit.requests.push_back(suffix);
    const auto description=describe(edit);
    check(description.ok && description.appended.size()==1 && description.appended.front().opType==suffix.opType &&
          description.appended.front().label==suffix.label && description.appended.front().payload==suffix.payload,
          "description exposes appended operations separately from captured placement lineage");
    const auto before=typedRows(store);const bool applied=commit(store,edit);const auto step=History(store).nextUndo();
    check(applied && step && step->opCount==2 && store.db().execAndGet("SELECT count(*) FROM clips WHERE id=7 AND muted=1 AND track_id=22").getInt()==1,
          "placement plus valid suffix is one journal transaction with both operations");
    const auto after=typedRows(store);const auto undone=History(store).undo();
    check(undone.ok && undone.opsApplied==2 && typedRows(store)==before,"one Undo reverses the complete compound placement");
    const auto redone=History(store).redo();
    check(redone.ok && redone.opsApplied==2 && typedRows(store)==after,"one Redo replays captured placement and suffix together");
    const auto unjournaled=move(store,scope(),{{7,{21,{}},{0,0}}});
    store.db().exec("UPDATE clips SET color=42 WHERE id=7");const ReadGuard changedRow(store);
    check(!clip_edit::commitPrepared(store,unjournaled).ok && changedRow.unchanged(store,false),
          "captured row conflict also refuses a direct state change with unchanged journal head");
}
void descriptionBinding() {
    Project p;addClip(*p.store,7,21,0,0,q);
    auto edit=move(*p.store,scope(),{{7,{22,{}},{0,2*q}}});
    check(edit.ok && describe(edit).ok,"prepared material exposes an owned description");
    if(!edit.ok || edit.requests.empty())return;
    auto changed=edit;changed.requests.front().payload["futureMutation"]=true;
    check(!describe(changed).ok,"description refuses a changed captured request payload");
    changed=edit;changed.requests.front().label="User-facing label";
    changed.requests.front().actor=Actor::Agent;changed.requests.front().actorDetail="Owner test";
    check(describe(changed).ok,"description tolerates caller actor stamps and presentation labels without changing semantic lineage");
    changed=edit;changed.requests.front().opType="clip.move";
    check(!describe(changed).ok,"description refuses a changed captured prefix op type");
    changed=edit;changed.requests.front().targetKind="clip";changed.requests.front().targetId=999;
    check(!describe(changed).ok,"description refuses a changed captured semantic target");
    changed=edit;changed.requests.clear();
    check(!describe(changed).ok,"description refuses removal of the captured prefix");
    changed=edit;changed.requests.insert(changed.requests.begin(),rename(21,"Inserted prefix"));
    check(!describe(changed).ok,"description refuses a prepended operation instead of misdescribing a shifted prefix");
    changed=edit;changed.sourceFile+=".other";
    check(!describe(changed).ok,"description binding includes source project identity");
    changed=edit;changed.options.expectHead.reset();
    check(!describe(changed).ok,"description binding distinguishes expected null head from absent guard");
    changed=edit;changed.options.expectHead=std::optional<std::int64_t>{42};
    check(!describe(changed).ok,"description binding includes exact expected history head");
    const auto original=describe(edit);p.store.reset();const auto detached=describe(edit);
    const auto* before=effectFor(original,7);const auto* after=effectFor(detached,7);
    check(detached.ok && before && after && before->before->name==after->before->name &&
          after->before->name=="Material 7" && after->after.size()==1 && after->after.front().span==PlacementSpan{2*q,3*q},
          "description owns names, scope and extents after the Store is closed");
}
void automationAdmissionAndPolicy() {
    Project settingsProject;
    settings::AppSettings app(appdata::App::Daw,settingsProject.temporary.path()/"settings.json");
    const auto defaults=capturePolicy(app);
    check(defaults.deleteOverlaps && defaults.automationFollowsClips && !defaults.mutedClipsAsDeleted &&
          defaults.preset==Preset::Live && defaults.ripple==Ripple::Off,"Live policy captures registered Replace and unlocked defaults");
    app.setLogUserChanges(false);std::string error;
    check(app.set("editing.automationFollowsClips",false,{},error) && !capturePolicy(app).automationFollowsClips,
          "effective Lock setting is read from the supplied application settings snapshot");
    for(const std::int64_t owner:{21,22,23})for(const bool crossing:{false,true}) {
        Project p;auto& store=*p.store;addClip(store,7,21,0,2*q,q);
        SQLite::Statement lane(store.db(),"INSERT INTO automation_lanes(id,owner_kind,owner_id,param_ref,value_domain,default_value,enabled,visible) VALUES(91,'track',?,'pan','real',0.25,0,0)");
        lane.bind(1,owner);lane.exec();
        if(crossing) {
            AutomationPoint a{},b{};a.time=0;a.value=-1;a.curve=3;a.tension=0.4f;a.point_id=17;
            b.time=20*q;b.value=1;b.point_id=18;
            const auto bytes=writeStream<AutomationPoint>(FourCC::Automation,std::vector<AutomationPoint>{a,b});
            if(!store.putAutomationData(91,{},bytes))throw std::runtime_error("write crossing automation fixture");
        }
        const ReadGuard before(store);const auto edit=move(store,scope(),{{7,{22,{}},{0,6*q}}});
        const bool relevant=owner!=23;
        check((relevant?!edit.ok:edit.ok) && before.unchanged(store),
              "unlocked admission checks source AND destination global lanes including crossing/default-only hidden lanes, not unrelated owners");
        if(relevant)check(!edit.error.empty(),"unsupported following automation returns a useful refusal");
        const Options capturedLock{48000,capturePolicy(app)};
        const auto lockedEdit=move(store,scope(),{{7,{22,{}},{0,6*q}}},capturedLock);
        check(lockedEdit.ok && before.unchanged(store) && commit(store,lockedEdit) &&
              !describe(lockedEdit).policy.automationFollowsClips &&
              tableRows(store,"automation_lanes")==before.rows.at("automation_lanes") &&
              tableRows(store,"automation_data")==before.rows.at("automation_data") &&
              tableRows(store,"automation_segment_ranges")==before.rows.at("automation_segment_ranges"),
              "explicit Lock permits material placement and keeps every global envelope row byte-identical");
        check(History(store).undo().ok && typedRows(store)==before.rows,"locked placement undo restores full state including unchanged envelopes");
    }
    for(const std::int64_t owner:{21,22})for(const bool send:{false,true}) {
        Project p;auto& store=*p.store;addClip(store,7,21,0,q,q);
        if(send) {
            SQLite::Statement route(store.db(),"INSERT INTO routing(id,src_kind,src_id,dst_kind,dst_id,kind) VALUES(401,'track',?,'track',23,'send')");
            route.bind(1,owner);route.exec();
            store.db().exec("INSERT INTO automation_lanes(id,owner_kind,owner_id,param_ref) VALUES(91,'routing',401,'gain_db')");
        } else {
            SQLite::Statement chain(store.db(),"INSERT INTO device_chains(id,track_id) VALUES(501,?)");chain.bind(1,owner);chain.exec();
            store.db().exec("INSERT INTO devices(id,chain_id,is_rack) VALUES(601,501,1);"
                            "INSERT INTO device_chains(id,parent_device_id) VALUES(502,601);"
                            "INSERT INTO devices(id,chain_id) VALUES(602,502);"
                            "INSERT INTO automation_lanes(id,owner_kind,owner_id,param_ref) VALUES(91,'device',602,'parameter.7')");
        }
        const ReadGuard before(store);const auto edit=move(store,scope(),{{7,{22,{}},{0,4*q}}});
        check(!edit.ok && before.unchanged(store),"unlocked automation admission follows source/destination ownership through nested devices and outgoing sends");
        const auto fixed=move(store,scope(),{{7,{22,{}},{0,4*q}}},locked());
        check(fixed.ok && commit(store,fixed) && tableRows(store,"automation_lanes")==before.rows.at("automation_lanes") &&
              tableRows(store,"devices")==before.rows.at("devices") && tableRows(store,"routing")==before.rows.at("routing") &&
              History(store).undo().ok && typedRows(store)==before.rows,
              "locked material placement retains nested device/send rows and restores exact typed state on Undo");
    }
}
void irrelevantRowsAndAliasDeletion() {
    for(const bool zeroLength:{false,true}) {
        Project p;auto& store=*p.store;
        addClip(store,7,22,0,0,zeroLength?0:q);
        addClip(store,9,21,0,0,2*q);
        addClip(store,99,23,0,-q,q/2);
        const ReadGuard before(store);const auto unrelated=tableRows(store,"clips","WHERE id IN(9,99)");
        const auto destination=zeroLength?q:4*q;
        const auto edit=move(store,scope(),{{7,{21,{}},{0,destination}}});
        check(edit.ok && before.unchanged(store),
              "a literal lane without positive clear masks does not validate unrelated negative-position material");
        check(commit(store,edit) && spanIs(store,7,21,0,destination,zeroLength?0:q) &&
              tableRows(store,"clips","WHERE id IN(9,99)")==unrelated,
              "zero-length incoming material clears nothing, and unrelated malformed lanes cannot veto other placement");
        const auto after=typedRows(store);
        check(History(store).undo().ok && typedRows(store)==before.rows &&
              History(store).redo().ok && typedRows(store)==after,
              "mask-free unrelated lanes and zero-length placement retain exact typed Undo and Redo");
    }
    for(int kind=0;kind<3;++kind) {
        Project p;auto& store=*p.store;addClip(store,7,21,0,0,q);
        if(kind==0)addClip(store,99,21,1,20*ns,1);
        else if(kind==1)addClip(store,99,21,0,std::numeric_limits<std::int64_t>::max()-10,100);
        else {
            addClip(store,99,21,0,20*q,q);
            store.db().exec("CREATE TABLE future_child(id INTEGER PRIMARY KEY,clip_id INTEGER REFERENCES clips(id) ON DELETE CASCADE,bytes BLOB) STRICT;"
                            "INSERT INTO future_child VALUES(501,99,X'00FF'),(502,7,X'FF00')");
        }
        const ReadGuard before(store);const auto far=tableRows(store,"clips","WHERE id=99");
        const auto future=kind==2?tableRows(store,"future_child"):Payload{};
        const auto edit=move(store,scope(),{{7,{21,{}},{0,4*q}}});
        check(edit.ok && before.unchanged(store) && commit(store,edit) && spanIs(store,7,21,0,4*q,q) &&
              tableRows(store,"clips","WHERE id=99")==far && (kind!=2 || tableRows(store,"future_child")==future),
              "unrelated tiny/overflowing extent and unknown extension rows do not block a retained-identity move outside their masks");
        check(History(store).undo().ok && typedRows(store)==before.rows &&
              (kind!=2 || tableRows(store,"future_child")==future),
              "unrelated or retained-identity future children survive exact placement undo");
    }
    for(const bool reverse:{false,true}) {
        Project p;auto& store=*p.store;const std::int64_t source=reverse?1:2,alias=reverse?2:1;
        addClip(store,source,21,0,3*q,q);addClip(store,alias,21,0,2*q,q);
        addClip(store,7,22,0,10*q,4*q);addClip(store,9,23,0,10*q,q);
        SQLite::Statement aliases(store.db(),"UPDATE clips SET alias_of=? WHERE id=? OR id=9");
        aliases.bind(1,source);aliases.bind(2,alias);aliases.exec();
        const auto before=typedRows(store);const auto edit=move(store,scope(),{{7,{21,{}},{0,q}}});
        check(edit.ok && commit(store,edit) && store.db().execAndGet("SELECT count(*) FROM clips WHERE id IN(1,2)").getInt()==0 &&
              store.db().execAndGet("SELECT count(*) FROM clips WHERE id=9 AND alias_of IS NULL").getInt()==1,
              "covering alias and source deletes both in either ID order and detaches the outside alias once");
        check(History(store).undo().ok && typedRows(store)==before,
              "alias/source joint deletion undo restores all alias links and original identities");
    }
    for(const bool split:{false,true}) {
        Project p;auto& store=*p.store;addClip(store,7,21,0,2*q,4*q);addClip(store,8,22,0,10*q,2*q);
        store.db().exec("CREATE TABLE future_child(id INTEGER PRIMARY KEY,clip_id INTEGER REFERENCES clips(id) ON DELETE CASCADE,bytes BLOB) STRICT;"
                        "INSERT INTO future_child VALUES(501,7,X'00FF')");
        const ReadGuard before(store);const auto future=tableRows(store,"future_child");
        const auto edit=move(store,scope(),{{8,{21,{}},{0,split?3*q:q}}});
        if(split)check(!edit.ok && before.unchanged(store) && tableRows(store,"future_child")==future,
                       "an unknown child refuses a required clone instead of silently omitting future material");
        else {
            check(edit.ok && before.unchanged(store) && commit(store,edit) && spanIs(store,7,21,0,3*q,3*q,q) &&
                  tableRows(store,"future_child")==future,
                  "sole-tail trim retains original unknown child references without a hypothetical deletion or clone");
            check(History(store).undo().ok && typedRows(store)==before.rows && tableRows(store,"future_child")==future,
                  "sole survivor with an unknown child undoes exactly");
        }
    }
    for(const bool interior:{false,true}) {
        Project p;auto& store=*p.store;addClip(store,7,21,0,2*q,4*q);addClip(store,8,22,0,10*q,2*q);
        addClip(store,std::numeric_limits<std::int64_t>::max(),23,0,0,q);
        const ReadGuard before(store);const auto edit=move(store,scope(),{{8,{21,{}},{0,interior?3*q:q}}});
        if(interior)check(!edit.ok && before.unchanged(store),"two-survivor placement refuses exhausted clip IDs without mutation");
        else {
            check(edit.ok && before.unchanged(store) && commit(store,edit) && spanIs(store,7,21,0,3*q,3*q,q),
                  "sole-tail replacement needs no allocation even when the clip ID space is exhausted");
            check(History(store).undo().ok && typedRows(store)==before.rows,"exhausted-ID sole survivor undoes exactly");
        }
    }
}
void placementAdapters() {
    {
        Project p;auto& store=*p.store;
        // At 120 BPM the source quarter contains 0.5 s. At 240 BPM the
        // destination quarter contains 0.25 s, while the linear image keeps
        // its 0.5 s duration and must replace material beyond the selection.
        store.db().exec("INSERT INTO tempo_map(pos_ticks,bpm) VALUES(23063040,240)");
        addClip(store,7,21,1,0,ns,"audio");
        addClip(store,8,22,1,2300000000,100000000);
        addClip(store,9,22,1,2500000000,100000000);
        store.db().exec("INSERT INTO media_files(id,hash_blake3,rel_path,format,sample_rate,channels,frames) VALUES(300,'range-tempo-audio','range-tempo.wav','wav',48000,1,48000);"
                        "INSERT INTO audio_clips(clip_id,media_id,src_start_frames,src_len_frames) VALUES(7,300,0,48000);"
                        "INSERT INTO event_streams(id,clip_id,stream_kind,data) VALUES(61,8,'notes',X'00FF01')");
        const ReadGuard before(store);
        const auto clipboard=clip_edit::copyRange(store,{{21},{0,0},{0,q}},48000);
        const auto pasted=clip_edit::preparePasteRange(store,clipboard,{22},{0,4*q},Options{});
        const auto description=describe(pasted);const auto* victim=effectFor(description,8);
        check(clipboard.ok && pasted.ok && before.unchanged(store) && description.ok &&
              description.scope.clock==PlacementClock::Nanoseconds && description.scope.bounds.end>=2500000000 &&
              victim && victim->kind==EffectKind::Removed,
              "range paste scopes the retained physical image beyond a faster-tempo destination selection and describes its extra victim");
        check(commit(store,pasted) && pasted.requests.size()==1 && pasted.requests.front().opType=="arranger.splice" &&
              store.db().execAndGet("SELECT count(*) FROM clips WHERE track_id=22 AND time_base=1 AND pos_ns=2000000000 AND length_ns=500000000").getInt()==1 &&
              store.db().execAndGet("SELECT count(*) FROM clips WHERE id=8").getInt()==0 &&
              store.db().execAndGet("SELECT count(*) FROM event_streams WHERE id=61").getInt()==0 &&
              spanIs(store,9,22,1,2500000000,100000000),
              "range paste retains a linear half-second, clears the victim after the requested quarter, and preserves the touching next clip");
        const auto after=typedRows(store);const auto step=History(store).nextUndo();
        check(step && step->opCount==1 && History(store).undo().ok && typedRows(store)==before.rows &&
              History(store).redo().ok && typedRows(store)==after,
              "tempo-changing range paste is one exact native Undo and Redo including removed opaque children");
    }
    {
        Project p;auto& store=*p.store;addClip(store,7,21,0,0,q);addClip(store,8,22,0,2*q,4*q);
        store.db().exec("INSERT INTO event_streams(id,clip_id,stream_kind,data) VALUES(61,7,'notes',X'010203'),(62,8,'notes',X'040506')");
        const ReadGuard before(store);const auto clipboard=clip_edit::copyClip(store,7,48000);
        const auto pasted=clip_edit::preparePaste(store,clipboard,70,22,{0,3*q},Options{});
        check(clipboard.ok && pasted.ok && before.unchanged(store) && describe(pasted).ok && commit(store,pasted),
              "ordinary paste adapter uses the captured shared planner and its description without early mutation");
        check(pasted.requests.size()==1 && pasted.requests.front().opType=="arranger.splice" &&
              spanIs(store,70,22,0,3*q,q) && spanIs(store,8,22,0,2*q,q) &&
              store.db().execAndGet("SELECT count(*) FROM clips WHERE track_id=22 AND pos_ticks=23063040 AND length_ticks=11531520 AND content_offset_ticks=11531520").getInt()==1 &&
              store.db().execAndGet("SELECT count(*) FROM event_streams WHERE clip_id=70 AND hex(data)='010203'").getInt()==1,
              "paste reserves the requested identity, clears covered material and clones only one independent tail");
        check(History(store).undo().ok && typedRows(store)==before.rows,"paste adapter has one exact native Undo");
        const auto duplicate=clip_edit::prepareDuplicate(store,7,71,21,{0,0},Options{});
        check(duplicate.ok && describe(duplicate).ok && commit(store,duplicate) &&
              store.db().execAndGet("SELECT count(*) FROM clips WHERE id=7").getInt()==0 && spanIs(store,71,21,0,0,q),
              "duplicate at its source clears the old identity rather than exempting copied material as relocated");
        check(History(store).undo().ok && typedRows(store)==before.rows,"duplicate-over-source undo returns the original identity and bytes");
    }
    {
        Project p;auto& store=*p.store;addClip(store,7,21,0,0,q);addClip(store,8,21,0,3*q,q);addClip(store,9,22,0,10*q,6*q);
        const clip_edit::RangeSelection selection{{21},{0,0},{0,4*q}};
        const ReadGuard before(store);const auto clipboard=clip_edit::copyRange(store,selection,48000);
        const auto pasted=clip_edit::preparePasteRange(store,clipboard,{22},{0,11*q},Options{});
        check(clipboard.ok && pasted.ok && before.unchanged(store) && describe(pasted).ok && commit(store,pasted),
              "range paste adapter plans captured clipboard members and one explicit clear span together");
        check(spanIs(store,9,22,0,10*q,q) &&
              store.db().execAndGet("SELECT count(*) FROM clips WHERE track_id=22 AND pos_ticks>=69261120 AND pos_ticks<80720640").getInt()==0 &&
              store.db().execAndGet("SELECT count(*) FROM clips WHERE track_id=22 AND pos_ticks=86486400 AND length_ticks=5765760 AND content_offset_ticks=28828800").getInt()==1,
              "range paste clears its declared silent gap while retaining both outside pieces and source offsets");
        check(History(store).undo().ok && typedRows(store)==before.rows,"range paste adapter undoes its complete interval replacement");
        const auto duplicate=clip_edit::prepareDuplicateRange(store,selection,Options{});
        check(duplicate.ok && describe(duplicate).ok && commit(store,duplicate) &&
              store.db().execAndGet("SELECT count(*) FROM clips WHERE track_id=21 AND pos_ticks IN(23063040,40360320)").getInt()==2,
              "ordinary range duplicate adapter preserves captured sparse member spacing after the source selection");
        check(History(store).undo().ok && typedRows(store)==before.rows,"range duplicate is one exact undo step");
    }
    {
        Project p;auto& store=*p.store;addClip(store,7,21,0,0,4*q);
        store.db().exec("INSERT INTO automation_lanes(id,owner_kind,owner_id,param_ref,default_value) VALUES(91,'track',21,'pan',0.25)");
        const auto clipboard=clip_edit::copyClip(store,7,48000);const clip_edit::RangeSelection selection{{21},{0,q},{0,3*q}};
        const auto range=clip_edit::copyRange(store,selection,48000);const ReadGuard before(store);
        check(!clip_edit::preparePaste(store,clipboard,70,22,{0,8*q},48000).ok &&
              !clip_edit::prepareDuplicate(store,7,70,22,{0,8*q},48000).ok &&
              !clip_edit::prepareDeleteRange(store,selection,48000).ok &&
              !clip_edit::preparePasteRange(store,range,{22},{0,8*q},48000).ok &&
              !clip_edit::prepareDuplicateRange(store,selection,48000).ok && before.unchanged(store),
              "legacy rate-only material adapters honor default unlocked automation refusal instead of silently locking");
        const auto cut=clip_edit::prepareCutRange(store,selection,locked());
        check(cut.copy.ok && cut.edit.ok && before.unchanged(store) && describe(cut.edit).ok && commit(store,cut.edit) &&
              tableRows(store,"automation_lanes")==before.rows.at("automation_lanes"),
              "explicit captured Lock policy enables the cut adapter with unchanged global lanes");
        check(History(store).undo().ok && typedRows(store)==before.rows,"locked adapter Undo restores the exact source and automation rows");
    }
    {
        Project p;auto& store=*p.store;addClip(store,7,21,0,0,q);
        const auto clipboard=clip_edit::copyClip(store,7,48000);
        store.db().exec("DELETE FROM clips WHERE id=7;DELETE FROM tracks WHERE id=21");
        const ReadGuard before(store);const auto defaultEdit=clip_edit::preparePaste(store,clipboard,70,22,{0,q},48000);
        const auto lockedEdit=clip_edit::preparePaste(store,clipboard,70,22,{0,q},locked());
        check(!defaultEdit.ok && lockedEdit.ok && before.unchanged(store) && commit(store,lockedEdit) && spanIs(store,70,22,0,q,q),
              "locked owned clipboard remains usable after source-track deletion while unlocked source-envelope uncertainty refuses");
        check(History(store).undo().ok && typedRows(store)==before.rows,"paste from a removed owner undoes without resurrecting the source track");
    }
}
// Independent physical-interval oracle on the fixed 120 BPM fixture. It neither
// calls TimeMap nor uses the planner's interval resolver. All generated musical
// endpoints are exact quarter-note fractions, representable in nanoseconds.
bool noOverlap(Store& store) {
    struct Interval {std::int64_t track=0,begin=0,end=0;};
    std::vector<Interval> intervals;
    SQLite::Statement query(store.db(),"SELECT track_id,time_base,pos_ticks,length_ticks,pos_ns,length_ns FROM clips WHERE lane_id IS NULL");
    while(query.executeStep()) {
        const int base=query.getColumn(1).getInt();
        const auto at=query.getColumn(base==0?2:4).getInt64(),length=query.getColumn(base==0?3:5).getInt64();
        if(at<0 || length<=0 || at>std::numeric_limits<std::int64_t>::max()-length)return false;
        const auto toNs=[](std::int64_t ticks){return (ticks/(q/2))*(ns/2);};
        if(base==0 && (at%(q/2)!=0 || length%(q/2)!=0))return false;
        intervals.push_back({query.getColumn(0).getInt64(),base==0?toNs(at):at,base==0?toNs(at+length):at+length});
    }
    for(std::size_t i=0;i<intervals.size();++i)for(std::size_t j=i+1;j<intervals.size();++j)
        if(intervals[i].track==intervals[j].track && intervals[i].begin<intervals[j].end && intervals[j].begin<intervals[i].end)return false;
    return true;
}
void seededTransactions() {
    // xorshift32, fixed unsigned arithmetic; no library distribution differences.
    constexpr std::uint32_t seed=0x217ed003U;std::uint32_t random=seed;
    auto next=[&]() {random^=random<<13;random^=random>>17;random^=random<<5;return random;};
    std::printf("edit_plan sequence seed=0x%08x, 40 native transactions\n",static_cast<unsigned>(seed));
    Project p;auto& initialStore=*p.store;
    initialStore.db().exec("ALTER TABLE clips ADD COLUMN future_blob BLOB NOT NULL DEFAULT X'';ALTER TABLE clips ADD COLUMN future_null TEXT;"
                           "INSERT INTO lanes(id,track_id,kind) VALUES(301,21,'take'),(302,22,'take'),(303,23,'take');"
                           "INSERT INTO media_files(id,hash_blake3,rel_path,format,sample_rate,channels,frames) VALUES(300,'sequence-audio','sequence.wav','wav',48000,1,4800000)");
    for(int track=0;track<3;++track)for(int item=0;item<4;++item) {
        const auto id=10+track*10+item;const int base=item%2;const auto unit=base==0?q:ns;
        addClip(initialStore,id,21+track,base,(2+item*5)*unit,3*unit,item%3==0?"audio":"midi");
        if(item%3==0) {
            SQLite::Statement source(initialStore.db(),"INSERT INTO audio_clips(clip_id,media_id,src_start_frames,src_len_frames,warp_enabled) VALUES(?,300,0,4000000,?)");
            source.bind(1,id);source.bind(2,track%2);source.exec();
        } else {
            SQLite::Statement stream(initialStore.db(),"INSERT INTO event_streams(clip_id,stream_kind,data) VALUES(?,'notes',X'00FF0102')");
            stream.bind(1,id);stream.exec();
        }
    }
    for(int track=0;track<3;++track) {
        addClip(initialStore,100+track,21+track,0,0,32*q);
        SQLite::Statement sentinel(initialStore.db(),"UPDATE clips SET lane_id=? WHERE id=?");
        sentinel.bind(1,301+track);sentinel.bind(2,100+track);sentinel.exec();
    }
    initialStore.db().exec("UPDATE clips SET future_blob=X'00FF8001',loop_enabled=1,loop_start_ticks=0,loop_len_ticks=17297280 WHERE lane_id IS NULL;"
                           "INSERT INTO markers(id,pos_ticks,kind) VALUES(71,11531520,'position');"
                           "INSERT INTO session_state(key,value) VALUES('transport.loop','{\"on\":true,\"start\":0,\"end\":57657600}')");
    const auto initialRows=typedRows(initialStore);const auto initialDigest=digestProject(initialStore).text;
    const auto take=tableRows(initialStore,"clips","WHERE lane_id IS NOT NULL");
    std::vector<std::string> states;std::vector<Payload> rows;
    int committed=0,groupCommits=0,resizeCommits=0;
    for(int index=0;index<40;++index) {
        auto& store=*p.store;
        struct Candidate {std::int64_t id=0,track=0,at=0,length=0;int base=0;};
        std::vector<Candidate> candidates;
        SQLite::Statement query(store.db(),"SELECT id,track_id,time_base,COALESCE(pos_ticks,pos_ns),COALESCE(length_ticks,length_ns) FROM clips WHERE lane_id IS NULL ORDER BY id");
        while(query.executeStep())candidates.push_back({query.getColumn(0).getInt64(),query.getColumn(1).getInt64(),query.getColumn(3).getInt64(),query.getColumn(4).getInt64(),query.getColumn(2).getInt()});
        if(candidates.empty()) {check(false,"seeded sequence retains material to edit");break;}
        const auto selected=candidates[next()%static_cast<std::uint32_t>(candidates.size())];
        const int operationBase=index%2;const auto unit=operationBase==0?q:ns;
        const auto where=scope(operationBase,40*unit);const ReadGuard before(store);
        clip_edit::PreparedEdit edit;
        bool grouped=false,resized=false;
        if(index==0) {
            edit=move(store,where,{{10,{22,{}},{0,2*q}},{20,{21,{}},{0,2*q}}});grouped=true;
        } else if(index%5==4) {
            const auto nativeUnit=selected.base==0?q:ns;
            auto length=(1+static_cast<std::int64_t>(next()%4U))*nativeUnit/2;
            if(length==selected.length)length+=nativeUnit/2;
            const auto end=(selected.at+length)/(nativeUnit/2)*(unit/2);
            edit=prepareResizeEnd(store,where,{selected.id,{operationBase,end}},{});resized=true;
        } else {
            const auto destination=21+static_cast<std::int64_t>(next()%3U);
            auto at=(1+static_cast<std::int64_t>(next()%48U))*unit/2;
            // Compare via exact half-quarter grid, avoiding a no-op transaction.
            const auto selectedHalf=selected.at/(selected.base==0?q/2:ns/2);
            if(destination==selected.track && at/(unit/2)==selectedHalf)at+=unit/2;
            if(index%7==0 && candidates.size()>1) {
                const auto other=candidates[(next()%static_cast<std::uint32_t>(candidates.size()-1)+1U)%candidates.size()];
                if(other.id!=selected.id && other.track!=selected.track) {
                    const auto otherAt=other.at/(other.base==0?q/2:ns/2)*(unit/2);
                    const auto selectedAt=selectedHalf*(unit/2);
                    edit=move(store,where,{{selected.id,{other.track,{}},{operationBase,otherAt}},
                                          {other.id,{selected.track,{}},{operationBase,selectedAt}}});
                    grouped=true;
                } else edit=move(store,where,{{selected.id,{destination,{}},{operationBase,at}}});
            } else edit=move(store,where,{{selected.id,{destination,{}},{operationBase,at}}});
        }
        const bool prepared=edit.ok && !edit.requests.empty();
        if(!prepared)std::printf("seed=0x%08x index=%d id=%lld error=%s\n",static_cast<unsigned>(seed),index,static_cast<long long>(selected.id),edit.error.c_str());
        check(prepared && before.unchanged(store),"seeded actual API preparation is nonempty and completely read-only");
        if(!prepared)break;
        const auto description=describe(edit);
        check(description.ok && !description.effects.empty() && description.scope.clock==where.clock,
              "seeded prepared effects describe the actual explicit operation clock");
        const bool applied=commit(store,edit);check(applied,"seeded actual placement commits successfully");
        if(!applied)break;
        ++committed;if(grouped)++groupCommits;if(resized)++resizeCommits;
        const auto step=History(store).nextUndo();
        check(edit.requests.size()==1 && edit.requests.front().opType=="arranger.splice" && step && step->opCount==1,
              "each seeded edit is exactly one native transaction and one arrangement splice");
        check(noOverlap(store),"independent SQL physical-interval oracle finds no overlap after each seeded commit");
        check(tableRows(store,"clips","WHERE lane_id IS NOT NULL")==take &&
              tableRows(store,"tempo_map")==initialRows.at("tempo_map") && tableRows(store,"markers")==initialRows.at("markers") &&
              tableRows(store,"session_state")==initialRows.at("session_state"),
              "seeded material placement leaves take sentinels, musical maps, markers and saved loop untouched");
        states.push_back(digestProject(store).text);rows.push_back(typedRows(store));
    }
    check(committed==40 && groupCommits>0 && resizeCommits==8,
          "seeded gate completes exactly 40 nonempty transactions including real groups and all eight end resizes");
    for(int i=committed;i>0;--i) {
        const bool undone=History(*p.store).undo().ok;
        check(undone && noOverlap(*p.store),"independent no-overlap oracle holds after every seeded Undo");
        if(!undone)break;
        const auto& expected=i==1?initialRows:rows[static_cast<std::size_t>(i-2)];
        check(typedRows(*p.store)==expected,"each seeded Undo restores its exact preceding typed state");
    }
    check(typedRows(*p.store)==initialRows && digestProject(*p.store).text==initialDigest && !History(*p.store).head(),
          "undo-all returns the initial project digest, whole typed rows and null history head exactly");
    p.reopen();
    p.store->db().exec("INSERT OR REPLACE INTO session_state(key,value) VALUES('unrelated.ui.selection','[999999]')");
    for(int i=0;i<committed;++i) {
        const auto redone=History(*p.store).redo();
        check(redone.ok && redone.opsApplied==1 && noOverlap(*p.store) && digestProject(*p.store).text==states[static_cast<std::size_t>(i)],
              "reopened journal replays each captured placement exactly under unrelated UI state");
        if(!redone.ok)break;
    }
}
} // namespace

int main() {
    try {
        anchoredLeftTrim();trimPhysicalPinsAndCapture();trimWarpFadeDomains();relationsAndSurvivors();swapsAndSparseGroups();completeChildrenAndResize();explicitScopeAndConversions();roundedPhysicalBoundaries();
        signedIdentitiesAndKinds();preparationAndTransactions();descriptionBinding();automationAdmissionAndPolicy();
        irrelevantRowsAndAliasDeletion();placementAdapters();seededTransactions();
    } catch(const std::exception& error) {check(false,error.what());}
    std::printf("%d checks, %d failures\n",checks,failures);return failures?1:0;
}
