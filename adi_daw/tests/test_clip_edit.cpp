// SPDX-License-Identifier: AGPL-3.0-or-later
#include "temp_directory.hpp"
#include "adi/engine/clip_fade.hpp"
#include "adi/clip_commands.hpp"
#include "adi/blob.hpp"
#include "adi/engine/automation.hpp"
#include "adi/store_rows.hpp"
#include "adi/engine/snapshot.hpp"
#include "adi/history.hpp"
#include "adi/store.hpp"
#include "adi/textproj_store.hpp"
#include <SQLiteCpp/SQLiteCpp.h>
#include <sqlite3.h>
#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>
#include <stdexcept>

namespace {
using namespace adi;using namespace adi::engine;
int checks=0,failures=0;
void check(bool ok,const char* what){++checks;if(!ok){++failures;std::printf("FAIL %s\n",what);}}
struct Project {
    test::TempDirectory temporary{"clip_edit","fades"};std::unique_ptr<Store> store;
    Project(){StoreError error{};store=Store::create(temporary.path()/"song.adi",error);if(!store)throw std::runtime_error(toString(error));
        store->db().exec("INSERT INTO tracks(id,kind,name) VALUES(21,'audio','Fades')");
        store->db().exec("INSERT INTO clips(id,track_id,kind,pos_ticks,length_ticks) VALUES(7,21,'audio',0,5765760),(8,21,'midi',0,5765760),(9,21,'audio',0,5765760)");}
    bool fade(Payload value){OpRequest request;request.opType="clip.setFades";request.payload=std::move(value);return OpJournal(*store).commit(request).ok;}
    const ClipNode& clip(const Snapshot& snapshot,std::size_t index=0){return *snapshot.tracks[0]->clips[index];}
};
void stateAndSharing() {
    Project p;auto& db=p.store->db();auto old=SnapshotBuilder::fromStore(*p.store);
    check(p.clip(*old).fadeInTicks==0 && p.clip(*old).fadeOutTicks==0 && !p.clip(*old).fadeInTension,"new audio clips have no automatic fades or shape row");
    const auto unchanged=old->tracks[0]->clips[2];
    check(p.fade({{"id",7},{"in",{{"ticks",1000}}}}),"set fade-in length");auto a=SnapshotBuilder::fromStore(*p.store,*old);
    check(a->tracks[0]->clips[0]!=old->tracks[0]->clips[0] && p.clip(*a).fadeInTicks==1000 && a->tracks[0]->clips[2]==unchanged,"fade-in length changes sharing only where needed");
    check(p.fade({{"id",7},{"out",{{"ticks",2000}}}}),"set fade-out length");auto b=SnapshotBuilder::fromStore(*p.store,*a);
    check(b->tracks[0]->clips[0]!=a->tracks[0]->clips[0] && p.clip(*b).fadeOutTicks==2000,"fade-out length participates in sharing");
    check(p.fade({{"id",7},{"in",{{"curve",2}}}}),"set legacy curve without adding a continuous-shape row");auto c=SnapshotBuilder::fromStore(*p.store,*b);
    check(c->tracks[0]->clips[0]!=b->tracks[0]->clips[0] && p.clip(*c).fadeInCurve==2 && !p.clip(*c).fadeInTension,"fade-in curve participates in sharing");
    check(p.fade({{"id",7},{"out",{{"curve",4}}}}),"set fade-out curve");auto d=SnapshotBuilder::fromStore(*p.store,*c);
    check(d->tracks[0]->clips[0]!=c->tracks[0]->clips[0] && p.clip(*d).fadeOutCurve==4,"fade-out curve participates in sharing");
    check(p.fade({{"id",7},{"in",{{"tension",0.0}}}}),"explicit zero enables shared continuous evaluator");auto e=SnapshotBuilder::fromStore(*p.store,*d);
    check(e->tracks[0]->clips[0]!=d->tracks[0]->clips[0] && p.clip(*e).fadeInTension==0,"optional zero is distinct from absent shape for sharing");
    check(p.fade({{"id",7},{"out",{{"tension",-0.4}}}}),"set independent fade-out tension");auto f=SnapshotBuilder::fromStore(*p.store,*e);
    check(f->tracks[0]->clips[0]!=e->tracks[0]->clips[0] && p.clip(*f).fadeOutTension==-0.4 && p.clip(*f).fadeInTension==0,"both side tensions participate in sharing independently");
    auto same=SnapshotBuilder::fromStore(*p.store,*f);check(same->tracks[0]==f->tracks[0],"unchanged fade state reuses entire track");
    check(History(*p.store).undo().ok,"undo only last side edit");auto restored=SnapshotBuilder::fromStore(*p.store,*same);
    check(!p.clip(*restored).fadeOutTension && p.clip(*restored).fadeInTension==0,"undo retains prior other-side shape");
    check(History(*p.store).undo().ok && db.execAndGet("SELECT count(*) FROM clip_playback_details").getInt()==0,"undo first shape restores absent row rather than writing default zero");
    check(History(*p.store).redo().ok && History(*p.store).redo().ok,"redo restores both continuous controls");
    const auto text=textproj::projectStore(*p.store).text;
    check(text.find("fadeInTension 0")!=std::string::npos && text.find("fadeOutTension -0.4")!=std::string::npos,"projection preserves explicit zero and negative shape values");
    check(p.fade({{"id",7},{"in",{{"tension",nullptr}}}}),"null switches just one side back to legacy calculation");auto legacy=SnapshotBuilder::fromStore(*p.store);
    check(!p.clip(*legacy).fadeInTension && p.clip(*legacy).fadeOutTension==-0.4,"legacy reset preserves other side's continuous shape");
    check(History(*p.store).undo().ok,"undo shape reset");
    db.exec("ALTER TABLE clip_playback_details ADD COLUMN future_blob BLOB NOT NULL DEFAULT X''");db.exec("UPDATE clip_playback_details SET future_blob=X'0001FF'");
    check(p.fade({{"id",7},{"in",{{"ticks",1234},{"tension",0.9}}},{"out",{{"curve",5},{"tension",0.3}}}}) && History(*p.store).undo().ok,"both sides are one captured undo step");
    check(db.execAndGet("SELECT hex(future_blob) FROM clip_playback_details").getString()=="0001FF" && db.execAndGet("SELECT in_tension FROM clip_playback_details").getDouble()==0 && db.execAndGet("SELECT out_tension FROM clip_playback_details").getDouble()==-0.4 && db.execAndGet("SELECT fade_in_ticks FROM clips WHERE id=7").getInt64()==1000,"inverse preserves exact full typed extension row and native fields");
    p.store.reset();StoreError error{};p.store=Store::open(p.temporary.path()/"song.adi",error);auto reopened=SnapshotBuilder::fromStore(*p.store);
    check(p.clip(*reopened).fadeInTension==0 && p.clip(*reopened).fadeOutTension==-0.4,"reopen retains shapes and zero/absence distinction");
}
void refusalAndCurves() {
    Project p;auto& db=p.store->db();const auto count=db.execAndGet("SELECT count(*) FROM ops").getInt64();
    check(!p.fade({{"id",8},{"in",{{"ticks",100}}}}) && !p.fade({{"id",999},{"in",{{"ticks",100}}}}),"MIDI and missing targets refused");
    check(!p.fade({{"id",7}}) && !p.fade({{"id",7},{"in",Payload::object()}}) && !p.fade({{"id",7},{"in",{{"typo",1}}}}),"empty and unknown fade controls refused");
    check(!p.fade({{"id",7},{"in",{{"ticks",-1}}}}) && !p.fade({{"id",7},{"in",{{"ticks",5765761}}}}) && !p.fade({{"id",7},{"in",{{"ticks",std::numeric_limits<std::uint64_t>::max()}}}}),"negative, beyond-clip and unsigned-overflow lengths refused");
    check(!p.fade({{"id",7},{"in",{{"curve",6}}}}) && !p.fade({{"id",7},{"in",{{"tension",1.01}}}}) && !p.fade({{"id",7},{"in",{{"tension",std::numeric_limits<double>::infinity()}}}}),"unsupported curves and invalid tensions refused");
    check(!p.fade({{"id",7},{"in",{{"ticks",1000},{"tension",0.2}}},{"out",{{"ticks",-2}}}}),"bad second side rolls back valid first-side write");
    check(db.execAndGet("SELECT count(*) FROM ops").getInt64()==count && db.execAndGet("SELECT fade_in_ticks FROM clips WHERE id=7").getInt64()==0 && db.execAndGet("SELECT count(*) FROM clip_playback_details").getInt()==0,"refusal leaves no partial native edit, shape row or journal row");
    check(clipFadeShape(0.5,2)==0.25 && clipFadeShape(0.5,2,0.0)==0.5,"legacy square and explicit zero's linear exponent remain distinct");
    check(clipFadeShape(-0.1,5,0.3)==0 && clipFadeShape(1.1,5,0.3)==1,"continuous endpoints clamp exactly");
    for(int curve=0;curve<=5;++curve) {
        bool same=true;for(int i=0;i<=100;++i){const double x=i/100.0;same=same && clipFadeShape(x,curve,0.4)==curveShape(static_cast<std::uint8_t>(curve),x,0.4);}
        check(same,"window and playback share every continuous curve shape");
    }
    db.exec("UPDATE clips SET time_base=1,pos_ticks=NULL,length_ticks=NULL,pos_ns=0,length_ns=1000000000 WHERE id=7");
    db.exec("DELETE FROM tempo_map;INSERT INTO tempo_map(pos_ticks,bpm,curve) VALUES(0,120,0),(5765760,60,0)");
    check(p.fade({{"id",7},{"in",{{"ticks",8000000},{"curve",5},{"tension",0.5}}}}),"linear clip fade bound integrates variable tempo");
    check(!p.fade({{"id",7},{"in",{{"ticks",9000000}}}}),"linear clip fade refuses length beyond integrated span");
}
void clipboardAndSplits() {
    Project p;auto& db=p.store->db();
    db.exec("ALTER TABLE clips ADD COLUMN future_root BLOB NOT NULL DEFAULT X''");
    db.exec("UPDATE clips SET future_root=X'0001FF' WHERE id=8");
    db.exec("INSERT INTO event_streams(id,clip_id,stream_kind,data) VALUES(51,8,'notes',X'01020304'),(100,9,'cc',X'05')");
    db.exec("INSERT INTO note_expression(id,clip_id,note_id,dimension,data) VALUES(61,8,-1,0,X'090807')");
    db.exec("INSERT INTO automation_lanes(id,owner_kind,owner_id,param_ref,value_domain) VALUES(91,'track',21,'pan','real'),(92,'track',21,'mute','normalized')");
    db.exec("INSERT INTO automation_data(id,lane_id,clip_id,data) VALUES(71,91,8,X'060504'),(201,92,NULL,X'03')");
    db.exec("INSERT INTO clip_playback_details(clip_id,in_tension) VALUES(8,0.4)");
    db.exec("INSERT INTO automation_segment_ranges(id,data_id,clip_id,record_index,progress_begin,progress_end,source_first,source_last) VALUES(72,71,8,0,0.2,0.7,0,1)");
    db.exec("UPDATE clips SET kind='midi',alias_of=8 WHERE id=9");
    const auto clipboard=clip_edit::copyClip(*p.store,8,48000);
    check(clipboard.ok,"copy captures complete opaque native clip data without decoding/re-encoding blobs");
    OpRequest remove;remove.opType="clip.delete";remove.payload={{"id",8}};
    check(OpJournal(*p.store).commit(remove).ok,"delete captures core, all native children and external alias links");
    check(db.execAndGet("SELECT count(*) FROM event_streams WHERE clip_id=8").getInt()==0 && db.execAndGet("SELECT alias_of IS NULL FROM clips WHERE id=9").getInt()!=0,"native delete cascades and detaches alias");
    check(History(*p.store).undo().ok,"undo delete restores native clip image");
    check(db.execAndGet("SELECT hex(future_root) FROM clips WHERE id=8").getString()=="0001FF" &&
          db.execAndGet("SELECT hex(data) FROM event_streams WHERE id=51").getString()=="01020304" &&
          db.execAndGet("SELECT hex(data) FROM note_expression WHERE id=61").getString()=="090807" &&
          db.execAndGet("SELECT hex(data) FROM automation_data WHERE id=71").getString()=="060504" &&
          db.execAndGet("SELECT in_tension FROM clip_playback_details WHERE clip_id=8").getDouble()==0.4 &&
          db.execAndGet("SELECT alias_of FROM clips WHERE id=9").getInt64()==8,"undo returns future bytes, every native child and aliases exactly");
    const auto pasted=clip_edit::preparePaste(*p.store,clipboard,18,21,{0,1000},48000);
    check(pasted.ok && clip_edit::commitPrepared(*p.store,pasted).ok,"paste prepared image as one independent clip");
    check(db.execAndGet("SELECT id FROM event_streams WHERE clip_id=18").getInt64()>100 &&
          db.execAndGet("SELECT id FROM automation_data WHERE clip_id=18").getInt64()>201,"child id allocation accounts for every existing row, including global automation");
    check(db.execAndGet("SELECT hex(data) FROM event_streams WHERE clip_id=18").getString()=="01020304" &&
          db.execAndGet("SELECT note_id FROM note_expression WHERE clip_id=18").getInt64()==-1 &&
          db.execAndGet("SELECT hex(future_root) FROM clips WHERE id=18").getString()=="0001FF","paste preserves opaque data, full-width note identity bit pattern and future columns");
    check(db.execAndGet("SELECT alias_of FROM clips WHERE id=9").getInt64()==8,"creating copy never rewires original's aliases");
    check(db.execAndGet("SELECT count(*) FROM automation_segment_ranges r JOIN automation_data d ON d.id=r.data_id WHERE r.clip_id=18 AND d.clip_id=18 AND r.progress_begin=0.2").getInt()==1,"clipboard remaps a curve range to the copied data parent");
    check(History(*p.store).undo().ok && db.execAndGet("SELECT count(*) FROM clips WHERE id=18").getInt()==0,"paste undo deletes just its created clip");
    check(History(*p.store).redo().ok && db.execAndGet("SELECT hex(data) FROM automation_data WHERE clip_id=18").getString()=="060504","paste redo restores captured clipboard bytes");
    const auto split=clip_edit::prepareSplit(*p.store,7,17,{0,2882880},48000);
    check(split.ok && split.options.expectHead.has_value(),"split captures expected undo head");
    check(clip_edit::commitPrepared(*p.store,split).ok && db.execAndGet("SELECT length_ticks FROM clips WHERE id=7").getInt64()==2882880 &&
          db.execAndGet("SELECT content_offset_ticks FROM clips WHERE id=17").getInt64()==2882880,"split changes placement and material offset without auto edge fades");
    check(History(*p.store).undo().ok && db.execAndGet("SELECT length_ticks FROM clips WHERE id=7").getInt64()==5765760,"split undo restores original length and identity");
    auto stale=clip_edit::prepareSplit(*p.store,7,19,{0,2882880},48000);
    OpRequest rename;rename.opType="track.rename";rename.payload={{"id",21},{"name","Other edit"}};
    check(OpJournal(*p.store).commit(rename).ok,"intervening native op changes the undo head");
    const auto refused=clip_edit::commitPrepared(*p.store,stale);
    check(!refused.ok && refused.stale && db.execAndGet("SELECT count(*) FROM clips WHERE id=19").getInt()==0,"expected-head check rejects stale split before any mutation");
    db.exec("INSERT INTO lanes(id,track_id,ord,name,kind) VALUES(301,21,0,'Comp','take')");
    db.exec("UPDATE clips SET lane_id=301 WHERE id=8");
    db.exec("INSERT INTO tracks(id,kind,name) VALUES(22,'audio','Destination')");
    const auto laneCopy=clip_edit::copyClip(*p.store,8,48000);
    const auto lanePaste=clip_edit::preparePaste(*p.store,laneCopy,70,22,{0,0},48000);
    check(lanePaste.ok && clip_edit::commitPrepared(*p.store,lanePaste).ok &&
          db.execAndGet("SELECT lane_id IS NULL FROM clips WHERE id=70").getInt()==1,"paste to another track clears the source track's comp lane");
    Project other;const auto foreign=clip_edit::preparePaste(*other.store,clipboard,70,21,{0,0},48000);
    check(!foreign.ok,"clipboard cannot confuse media ids from another project");
}

void silenceAndCurves() {
    constexpr std::int64_t q=5765760;
    for(int curve=0;curve<=5;++curve) {
        Project p;auto& db=p.store->db();
        db.exec("DELETE FROM clips WHERE id<>8;UPDATE clips SET length_ticks=23063040 WHERE id=8");
        db.exec("UPDATE project SET length_ticks=34594560");
        db.exec("DELETE FROM tempo_map;INSERT INTO tempo_map(id,pos_ticks,bpm) VALUES(5,0,120),(2,11531520,60),(9,23063040,90)");
        db.exec("INSERT INTO time_signature_map(id,pos_ticks,numerator,denominator) VALUES(11,11531520,3,4)");
        db.exec("INSERT INTO key_map(id,pos_ticks,root) VALUES(12,11531520,5)");
        db.exec("INSERT INTO markers(id,pos_ticks,length_ticks,kind) VALUES(13,0,23063040,'cycle'),(14,11531520,NULL,'position')");
        db.exec("INSERT INTO automation_lanes(id,owner_kind,owner_id,param_ref,value_domain) VALUES(91,'track',21,'pan','real')");
        const std::string loop=Payload({{"on",true},{"start",0},{"end",4*q},{"future","kept"}}).dump();
        SQLite::Statement loopRow(db,"INSERT INTO session_state(key,value) VALUES('transport.loop',?)");loopRow.bind(1,loop);loopRow.exec();
        AutomationPoint a{},b{};a.time=0;a.value=-1;a.curve=static_cast<std::uint8_t>(curve);a.tension=0.4f;a.point_id=std::numeric_limits<std::uint64_t>::max();
        b.time=4*q;b.value=1;b.point_id=17;
        const auto bytes=writeStream<AutomationPoint>(FourCC::Automation,std::vector<AutomationPoint>{a,b});
        check(p.store->putAutomationData(91,{},bytes),"write source automation");
        AutomationProgram old(rows::readModel(*p.store),48000);
        const auto prepared=clip_edit::prepareInsertSilence(*p.store,q,q,48000);
        if(!prepared.ok)std::printf("prepare silence: %s\n",prepared.error.c_str());
        const auto committed=clip_edit::commitPrepared(*p.store,prepared);
        if(!committed.ok)std::printf("commit silence: %s\n",committed.error.c_str());
        check(prepared.ok && committed.ok,"Insert Silence is one prepared native undo step");
        check(db.execAndGet("SELECT count(*) FROM clips").getInt()==2 &&
              db.execAndGet("SELECT length_ticks FROM clips WHERE id=8").getInt64()==q &&
              db.execAndGet("SELECT content_offset_ticks FROM clips WHERE id<>8").getInt64()==q &&
              db.execAndGet("SELECT pos_ticks FROM clips WHERE id<>8").getInt64()==2*q,"crossing MIDI clip splits and keeps material after a silent gap");
        check(db.execAndGet("SELECT pos_ticks FROM tempo_map WHERE id=2").getInt64()==3*q &&
              db.execAndGet("SELECT pos_ticks FROM time_signature_map WHERE id=11").getInt64()==3*q &&
              db.execAndGet("SELECT pos_ticks FROM key_map WHERE id=12").getInt64()==3*q &&
              db.execAndGet("SELECT length_ticks FROM markers WHERE id=13").getInt64()==5*q &&
              db.execAndGet("SELECT pos_ticks FROM markers WHERE id=14").getInt64()==3*q &&
              db.execAndGet("SELECT length_ticks FROM project").getInt64()==7*q,"maps, spanning cycles, locators and declared project end shift");
        AutomationProgram next(rows::readModel(*p.store),48000);
        bool same=old.problems().empty() && next.problems().empty();
        for(std::int64_t sample=0;sample<144000;sample+=13) {
            const auto shifted=sample<24000?sample:sample+24000;
            same=same && std::abs(valueAt(*old.lane(91),sample)-valueAt(*next.lane(91),shifted))<1e-12;
        }
        check(same,"every curve retains its original shape on both sides of the insertion");
        bool held=true;
        for(std::int64_t sample=24000;sample<48000;sample+=11)
            held=held && valueAt(*next.lane(91),sample)==valueAt(*old.lane(91),24000);
        check(held,"automation holds the exact cut value throughout the inserted time");
        const auto movedLoop=Payload::parse(db.execAndGet("SELECT value FROM session_state WHERE key='transport.loop'").getString());
        check(movedLoop.at("end")==5*q && movedLoop.at("future")=="kept","Insert Silence extends the saved transport loop and retains future JSON fields");
        const auto after=*p.store->getAutomationData(91,{});
        StreamReader<AutomationPoint> reader(after,FourCC::Automation);
        check(reader.count()==4 && reader.at(0)->point_id==a.point_id && reader.at(3)->point_id==b.point_id,"surgery preserves full-width point identities and allocates separate boundary points");
        check(History(*p.store).undo().ok && *p.store->getAutomationData(91,{})==bytes &&
              db.execAndGet("SELECT count(*) FROM automation_segment_ranges").getInt()==0 &&
              db.execAndGet("SELECT count(*) FROM clips").getInt()==1,"undo restores original automation bytes, curve-row absence and clips");
        check(db.execAndGet("SELECT value FROM session_state WHERE key='transport.loop'").getString()==loop,"undo restores exact saved loop JSON bytes");
        check(History(*p.store).redo().ok && *p.store->getAutomationData(91,{})==after,"redo applies captured bytes and curve ranges");
        const auto again=clip_edit::prepareInsertSilence(*p.store,3*q,q/2,48000);
        check(again.ok && clip_edit::commitPrepared(*p.store,again).ok,"a second insert trims an already sliced curve");
        AutomationProgram twice(rows::readModel(*p.store),48000);
        same=twice.problems().empty();
        for(std::int64_t sample=0;sample<144000;sample+=17) {
            auto shifted=sample<24000?sample:sample+24000;
            if(shifted>=72000)shifted+=12000; // hold 120 BPM before the boundary event
            same=same && std::abs(valueAt(*old.lane(91),sample)-valueAt(*twice.lane(91),shifted))<2e-12;
        }
        check(same,"repeated insert retains the original nonlinear source curve");
    }
}

void rangeCommands() {
    Project p;auto& db=p.store->db();
    db.exec("DELETE FROM clips WHERE id<>8;UPDATE clips SET length_ticks=1000 WHERE id=8");
    db.exec("INSERT INTO event_streams(id,clip_id,stream_kind,data) VALUES(61,8,'notes',X'010203')");
    clip_edit::RangeSelection selection{{21},{0,200},{0,800}};
    const auto copy=clip_edit::copyRange(*p.store,selection,48000);
    check(copy.ok && copy.images.size()==1 && copy.images[0].at("row").at("length_ticks")==600 &&
          copy.images[0].at("row").at("content_offset_ticks")==200,"range clipboard trims both crossing edges and captures the material offset");
    const auto cut=clip_edit::prepareCutRange(*p.store,selection,48000);
    check(cut.copy.ok && cut.edit.ok && cut.copy.images==copy.images,"Cut captures its clipboard and deletion in one read snapshot");
    check(clip_edit::commitPrepared(*p.store,cut.edit).ok,"Cut range commits one native packet");
    check(db.execAndGet("SELECT count(*) FROM clips").getInt()==2 &&
          db.execAndGet("SELECT length_ticks FROM clips WHERE id=8").getInt64()==200 &&
          db.execAndGet("SELECT pos_ticks FROM clips WHERE id<>8").getInt64()==800 &&
          db.execAndGet("SELECT content_offset_ticks FROM clips WHERE id<>8").getInt64()==800,"delete leaves both outside pieces on the original timeline");
    check(History(*p.store).undo().ok && db.execAndGet("SELECT count(*) FROM clips").getInt()==1 &&
          db.execAndGet("SELECT hex(data) FROM event_streams").getString()=="010203","range delete undo restores exact original content");
    const auto paste=clip_edit::preparePasteRange(*p.store,copy,{21},{0,100},48000);
    check(paste.ok && clip_edit::commitPrepared(*p.store,paste).ok,"range paste replaces intersecting destination material");
    check(db.execAndGet("SELECT count(*) FROM clips").getInt()==3 &&
          db.execAndGet("SELECT content_offset_ticks FROM clips WHERE pos_ticks=100").getInt64()==200 &&
          db.execAndGet("SELECT content_offset_ticks FROM clips WHERE pos_ticks=700").getInt64()==700,"paste replaces exactly its span and preserves source-relative material clocks");
    check(History(*p.store).undo().ok,"undo paste as one step");
    const auto duplicate=clip_edit::prepareDuplicateRange(*p.store,selection,48000,true);
    check(duplicate.ok && clip_edit::commitPrepared(*p.store,duplicate).ok,"Duplicate Time inserts a gap and the clipped material in one step");
    check(db.execAndGet("SELECT content_offset_ticks FROM clips WHERE pos_ticks=800").getInt64()==200 &&
          db.execAndGet("SELECT content_offset_ticks FROM clips WHERE pos_ticks=1400").getInt64()==800,"Duplicate Time moves the original tail and places an independent copy");
    check(History(*p.store).undo().ok && db.execAndGet("SELECT length_ticks FROM clips").getInt64()==1000 &&
          History(*p.store).redo().ok,"Duplicate Time undo/redo retains one-step semantics");
    const auto bad=clip_edit::preparePasteRange(*p.store,copy,{}, {0,0},48000);
    check(!bad.ok,"paste refuses a mismatched track mapping");
}

void clockSharing() {
    Project p;auto& db=p.store->db();auto previous=SnapshotBuilder::fromStore(*p.store);
    db.exec("INSERT INTO clip_playback_details(clip_id) VALUES(7)");
    const std::array<const char*,10> edits={
        "content_offset_ns=0","source_end_ns=0","loop_begin_ns=0,loop_end_ns=3","loop_begin_ns=1","loop_end_ns=4",
        "in_begin_ns=0,in_end_ns=3","in_begin_ns=1","in_end_ns=4",
        "out_begin_ns=0,out_end_ns=3","out_begin_ns=1,out_end_ns=4"};
    for(const auto* fields:edits) {
        db.exec(std::string("UPDATE clip_playback_details SET ")+fields+" WHERE clip_id=7");
        auto next=SnapshotBuilder::fromStore(*p.store,*previous);
        check(next->tracks[0]->clips[0]!=previous->tracks[0]->clips[0] &&
              next->tracks[0]->clips[1]==previous->tracks[0]->clips[1],"each source/fade clock changes only its clip's structural sharing");
        previous=std::move(next);
    }
    const auto text=textproj::projectStore(*p.store).text;
    check(text.find("contentOffsetNs 0")!=std::string::npos && text.find("fadeInBeginNs 1")!=std::string::npos,"projection preserves source clocks and retained fade domains");
    p.store.reset();StoreError error{};p.store=Store::open(p.temporary.path()/"song.adi",error);
    const auto reopened=SnapshotBuilder::fromStore(*p.store);
    check(reopened->tracks[0]->clips[0]->contentOffsetNs==0 &&
          reopened->tracks[0]->clips[0]->fadeClockNs[3]==4,"reopen preserves exact optional clocks");
}

void pureLoopAndCut() {
    Project p;auto& db=p.store->db();
    const auto cut=clip_edit::prepareCutClip(*p.store,8,48000);
    check(cut.copy.ok && cut.edit.ok && clip_edit::commitPrepared(*p.store,cut.edit).ok &&
          History(*p.store).undo().ok,"Cut Clip captures a coherent clipboard and one undoable deletion");
    db.exec("DELETE FROM clips");
    const auto before=Payload({{"on",false},{"start",0},{"end",5765760}}).dump();
    SQLite::Statement insert(db,"INSERT INTO session_state(key,value) VALUES('transport.loop',?)");insert.bind(1,before);insert.exec();
    const auto silence=clip_edit::prepareInsertSilence(*p.store,0,5765760,48000);
    check(silence.ok && silence.requests.size()==1 && silence.requests[0].payload.at("changes").empty() &&
          clip_edit::commitPrepared(*p.store,silence).ok,"Insert Silence can shift a saved loop with no other arrangement rows");
    check(History(*p.store).undo().ok && db.execAndGet("SELECT value FROM session_state").getString()==before &&
          History(*p.store).redo().ok,"a metadata-only time insertion has exact undo and redo");
    Project other;
    for(const char* side:{"in","out"}) {
        bool rejected=false;
        try {other.store->db().exec(std::string("INSERT INTO clip_playback_details(clip_id,")+side+"_begin_ns) VALUES(7,0)");}
        catch(const std::exception&){rejected=true;}
        check(rejected,"schema refuses a retained fade begin without its end");
    }
}

void reusedMediaIds() {
    Project p;auto& db=p.store->db();const std::string original(64,'a'),replacement(64,'b');
    SQLite::Statement media(db,"INSERT INTO media_files(id,hash_blake3,rel_path,format) VALUES(300,?,'original.wav','wav')");
    media.bind(1,original);media.exec();
    db.exec("INSERT INTO audio_clips(clip_id,media_id,src_start_frames,src_len_frames) VALUES(7,300,0,1024)");
    const auto copy=clip_edit::copyClip(*p.store,7,48000);
    check(copy.ok && copy.image.at("mediaKeys")[0].at("hash")==original,"audio clipboard captures pooled content identity without reading media bytes");
    SQLite::Statement changed(db,"UPDATE media_files SET hash_blake3=? WHERE id=300");changed.bind(1,replacement);changed.exec();
    const auto wrong=clip_edit::preparePaste(*p.store,copy,17,21,{0,5765760},48000);
    check(!wrong.ok && wrong.error.find("media identity")!=std::string::npos,"reused media ID cannot make an old clipboard play another file");
    changed.reset();changed.bind(1,original);changed.exec();
    db.exec("UPDATE media_files SET rel_path='relocated.wav',missing=1 WHERE id=300");
    check(clip_edit::preparePaste(*p.store,copy,17,21,{0,5765760},48000).ok,"same content remains compatible with relocation and a missing-file status change");
    const auto range=clip_edit::copyRange(*p.store,{{21},{0,0},{0,5765760}},48000);
    changed.reset();changed.bind(1,replacement);changed.exec();
    const auto wrongRange=clip_edit::preparePasteRange(*p.store,range,{21},{0,5765760},48000);
    check(range.ok && !wrongRange.ok && wrongRange.error.find("media identity")!=std::string::npos,"range clipboard checks every referenced media identity too");
    db.exec("UPDATE media_files SET hash_blake3='' WHERE id=300");
    const auto legacy=clip_edit::copyClip(*p.store,7,48000);
    db.exec("UPDATE media_files SET rel_path='different-legacy.wav' WHERE id=300");
    check(legacy.ok && !clip_edit::preparePaste(*p.store,legacy,17,21,{0,5765760},48000).ok,"legacy unhashed media retains locator/dimension identity");
}

// Compare every typed cell, including extension columns and absent optional rows.
// Journal metadata is checked separately because successful undo retains its log.
Payload splitRows(Store& store) {
    constexpr std::array<std::pair<const char*,const char*>,9> tables={{
        {"clips","id"},{"event_streams","id"},{"note_expression","id"},
        {"audio_clips","clip_id"},{"automation_data","id"},{"automation_segment_ranges","id"},
        {"clip_properties","clip_id"},{"clip_playback_details","clip_id"},{"clip_warp_fade_domains","clip_id"}
    }};
    Payload result=Payload::object();
    for(const auto& [table,key]:tables) {
        auto& rows=result[table];rows=Payload::array();
        SQLite::Statement query(store.db(),std::string("SELECT * FROM ")+table+" ORDER BY "+key);
        while(query.executeStep()) {
            Payload row=Payload::object();
            for(int column=0;column<query.getColumnCount();++column) {
                const auto cell=query.getColumn(column);Payload value;
                if(cell.isInteger())value=cell.getInt64();
                else if(cell.isFloat())value=cell.getDouble();
                else if(cell.isText())value=cell.getString();
                else if(cell.isBlob()) {
                    std::vector<std::uint8_t> bytes;
                    const auto size=static_cast<std::size_t>(cell.getBytes());
                    if(size!=0) {
                        const auto* data=static_cast<const std::uint8_t*>(cell.getBlob());
                        bytes.assign(data,data+size);
                    }
                    value=Payload::binary(bytes);
                }
                row[query.getColumnName(column)]=Payload::array({cell.getType(),value});
            }
            rows.push_back(std::move(row));
        }
    }
    return result;
}
struct SplitState {
    Payload rows;
    std::int64_t ops=0;
    std::optional<std::int64_t> head;
    int changes=0;
};
SplitState splitState(Store& store) {
    return {splitRows(store),OpJournal(store).count(),OpJournal(store).headSeq(),sqlite3_total_changes(store.db().getHandle())};
}
bool sameSplitState(Store& store,const SplitState& before,bool noWrites) {
    return splitRows(store)==before.rows && OpJournal(store).count()==before.ops &&
        OpJournal(store).headSeq()==before.head &&
        (!noWrites || sqlite3_total_changes(store.db().getHandle())==before.changes);
}
bool splitSpan(SQLite::Database& db,std::int64_t id,int base,std::int64_t position,
               std::int64_t length,std::int64_t offset,bool muted) {
    SQLite::Statement row(db,"SELECT time_base,pos_ticks,length_ticks,pos_ns,length_ns,content_offset_ticks,muted FROM clips WHERE id=?");
    row.bind(1,id);
    if(!row.executeStep() || row.getColumn(0).getInt()!=base || row.getColumn(5).getInt64()!=offset ||
       row.getColumn(6).getInt()!=(muted?1:0))return false;
    const int at=base==0?1:3,absent=base==0?3:1;
    return row.getColumn(at).getInt64()==position && row.getColumn(at+1).getInt64()==length &&
        row.getColumn(absent).isNull() && row.getColumn(absent+1).isNull();
}

void twoPointGeometryAndMute() {
    constexpr std::int64_t q=5765760;
    for(const int storedBase:{0,1})for(const int inputBase:{0,1}) {
        Project p;auto& db=p.store->db();
        db.exec("DELETE FROM tempo_map;INSERT INTO tempo_map(pos_ticks,bpm,curve) VALUES(0,120,0)");
        db.exec("INSERT INTO media_files(id,hash_blake3,rel_path,format,sample_rate,channels,frames) VALUES(300,'split-source','source.wav','wav',48000,2,240000)");
        db.exec("INSERT INTO audio_clips(clip_id,media_id,src_len_frames) VALUES(7,300,240000)");
        if(storedBase==0)
            db.exec("UPDATE clips SET pos_ticks=5765760,length_ticks=23063040,content_offset_ticks=2882880 WHERE id=7");
        else
            db.exec("UPDATE clips SET time_base=1,pos_ticks=NULL,length_ticks=NULL,pos_ns=500000000,length_ns=2000000000,content_offset_ticks=2882880 WHERE id=7");
        const auto before=splitState(*p.store);
        const clip_edit::TimePoint begin{inputBase,inputBase==0?2*q:1000000000};
        const clip_edit::TimePoint end{inputBase,inputBase==0?4*q:2000000000};
        auto edit=clip_edit::prepareSplit(*p.store,7,17,18,begin,end,48000);
        check(edit.ok && edit.requests.size()==1 && edit.options.expectHead.has_value() &&
              edit.options.expectHead.value()==before.head && sameSplitState(*p.store,before,true),
              "two cuts prepare one operation in either input/stored clock without writing rows, history or optional details");
        if(!edit.ok || edit.requests.size()!=1)continue;
        OpRequest mute;mute.opType="clip.setMute";mute.payload={{"id",17},{"muted",true}};mute.label="Mute selected span";
        edit.requests.push_back(std::move(mute));
        const auto committed=clip_edit::commitPrepared(*p.store,edit);
        check(committed.ok,"caller appends mute for the known middle piece and commits both cuts atomically");
        if(!committed.ok)continue;
        const auto unit=storedBase==0?q:500000000;
        check(splitSpan(db,7,storedBase,unit,unit,q/2,false) &&
              splitSpan(db,17,storedBase,2*unit,2*unit,3*q/2,true) &&
              splitSpan(db,18,storedBase,4*unit,unit,7*q/2,false) &&
              db.execAndGet("SELECT count(*) FROM clip_playback_details WHERE clip_id=7 AND content_offset_ns=250000000").getInt()==1 &&
              db.execAndGet("SELECT count(*) FROM clip_playback_details WHERE clip_id=17 AND content_offset_ns=750000000").getInt()==1 &&
              db.execAndGet("SELECT count(*) FROM clip_playback_details WHERE clip_id=18 AND content_offset_ns=1750000000").getInt()==1,
              "three contiguous spans keep the source id, nonzero musical offset and pinned audio clock; only the selected middle is muted");
        const auto step=History(*p.store).nextUndo();
        check(step && step->opCount==2 && step->label=="Split Clip" &&
              db.execAndGet("SELECT count(DISTINCT txn_id) FROM ops WHERE tags<>'ephemeral'").getInt()==1,
              "two-cut split and appended mute have one transaction and the split label");
        const auto after=splitRows(*p.store);
        check(History(*p.store).undo().ok && splitRows(*p.store)==before.rows &&
              db.execAndGet("SELECT count(*) FROM clip_playback_details WHERE clip_id IN (7,17,18)").getInt()==0,
              "one undo restores exact typed source rows and the original absence of optional playback details");
        check(History(*p.store).redo().ok && splitRows(*p.store)==after,
              "one redo restores every captured row and the muted middle exactly");
    }
}

void twoPointOpaqueChildren() {
    Project p;auto& db=p.store->db();
    db.exec("UPDATE clips SET pos_ticks=5765760,length_ticks=23063040,content_offset_ticks=2882880 WHERE id=8");
    db.exec("ALTER TABLE clips ADD COLUMN future_root BLOB NOT NULL DEFAULT X''");
    db.exec("UPDATE clips SET future_root=X'0001FF' WHERE id=8");
    db.exec("ALTER TABLE event_streams ADD COLUMN future_blob BLOB NOT NULL DEFAULT X''");
    db.exec("ALTER TABLE event_streams ADD COLUMN future_null TEXT");
    db.exec("INSERT INTO event_streams(id,clip_id,stream_kind,data,future_blob) VALUES(51,8,'notes',X'01020304',X'00FF'),(1000,9,'cc',X'05',X'')");
    db.exec("INSERT INTO note_expression(id,clip_id,note_id,dimension,data) VALUES(61,8,-1,0,X'090807')");
    db.exec("INSERT INTO automation_lanes(id,owner_kind,owner_id,param_ref,value_domain) VALUES(91,'track',21,'pan','real'),(92,'track',21,'mute','normalized')");
    db.exec("INSERT INTO automation_data(id,lane_id,clip_id,data) VALUES(71,91,8,X'060504'),(901,92,NULL,X'03')");
    db.exec("INSERT INTO automation_segment_ranges(id,data_id,clip_id,record_index,progress_begin,progress_end,source_first,source_last) VALUES(72,71,8,0,0.2,0.7,0,1)");
    db.exec("INSERT INTO clip_properties(clip_id,end_marker_ticks,signature_numerator,signature_denominator) VALUES(8,28828800,3,4)");
    db.exec("INSERT INTO clip_playback_details(clip_id,in_tension) VALUES(8,0.4)");
    db.exec("INSERT INTO clip_warp_fade_domains(clip_id,in_begin_ticks,in_end_ticks) VALUES(8,0,5765760)");
    db.exec("UPDATE clips SET kind='midi',alias_of=8 WHERE id=9");
    const auto before=splitState(*p.store);
    const auto edit=clip_edit::prepareSplit(*p.store,8,17,18,{0,11531520},{0,23063040},48000);
    check(edit.ok && sameSplitState(*p.store,before,true),
          "two-cut preparation captures opaque native children and future columns without decoding or changing the source");
    if(!edit.ok)return;
    const auto committed=clip_edit::commitPrepared(*p.store,edit);
    check(committed.ok,"one shared child allocator makes both independently owned split pieces committable");
    if(!committed.ok)return;
    check(db.execAndGet("SELECT count(*) FROM clips WHERE id IN (8,17,18) AND hex(future_root)='0001FF'").getInt()==3 &&
          db.execAndGet("SELECT count(*) FROM event_streams WHERE clip_id IN (8,17,18) AND hex(data)='01020304' AND hex(future_blob)='00FF' AND future_null IS NULL").getInt()==3 &&
          db.execAndGet("SELECT count(*) FROM note_expression WHERE clip_id IN (8,17,18) AND note_id=-1 AND hex(data)='090807'").getInt()==3 &&
          db.execAndGet("SELECT count(*) FROM clip_properties WHERE clip_id IN (8,17,18) AND end_marker_ticks=28828800 AND signature_numerator=3 AND scale_id IS NULL").getInt()==3 &&
          db.execAndGet("SELECT count(*) FROM clip_playback_details WHERE clip_id IN (8,17,18) AND in_tension=0.4 AND out_tension IS NULL AND content_offset_ns IS NULL").getInt()==3 &&
          db.execAndGet("SELECT count(*) FROM clip_warp_fade_domains WHERE clip_id IN (8,17,18) AND in_begin_ticks=0 AND in_end_ticks=5765760 AND out_begin_ticks IS NULL").getInt()==3,
          "all three pieces retain complete opaque stream/expression bytes, future BLOB/NULL cells and optional native properties");
    check(db.execAndGet("SELECT count(DISTINCT id) FROM event_streams WHERE clip_id IN (8,17,18)").getInt()==3 &&
          db.execAndGet("SELECT count(DISTINCT id) FROM note_expression WHERE clip_id IN (8,17,18)").getInt()==3 &&
          db.execAndGet("SELECT count(DISTINCT d.id) FROM automation_segment_ranges r JOIN automation_data d ON d.id=r.data_id WHERE r.clip_id IN (8,17,18) AND d.clip_id=r.clip_id AND hex(d.data)='060504' AND r.progress_begin=0.2 AND r.progress_end=0.7").getInt()==3 &&
          db.execAndGet("SELECT alias_of FROM clips WHERE id=9").getInt64()==8 &&
          db.execAndGet("SELECT count(*) FROM clips WHERE alias_of IN (17,18)").getInt()==0,
          "both new child identity sets are distinct, curve ranges point to their own copied automation, and external aliases still name the original");
    const auto after=splitRows(*p.store);
    check(History(*p.store).undo().ok && splitRows(*p.store)==before.rows,
          "joining both pieces restores every original typed child row, absent field and alias exactly");
    check(History(*p.store).redo().ok && splitRows(*p.store)==after,
          "two-piece redo reuses the captured child identities and bytes");
}

void twoPointAtomicRefusals() {
    constexpr std::int64_t q=5765760;
    for(const bool withHead:{false,true}) {
        Project p;
        OpRequest rename;rename.opType="track.rename";rename.payload={{"id",21},{"name","First"}};
        if(withHead && !OpJournal(*p.store).commit(rename).ok)throw std::runtime_error("prepare non-null split head");
        const auto head=OpJournal(*p.store).headSeq();
        const auto edit=clip_edit::prepareSplit(*p.store,8,17,18,{0,q/4},{0,3*q/4},48000);
        check(edit.ok && edit.options.expectHead.has_value() && edit.options.expectHead.value()==head,
              "two-cut preparation captures both null and non-null expected heads");
        if(!edit.ok)continue;
        rename.payload["name"]="Intervening";
        if(!OpJournal(*p.store).commit(rename).ok)throw std::runtime_error("advance split fixture head");
        const auto before=splitState(*p.store);
        const auto refused=clip_edit::commitPrepared(*p.store,edit);
        check(!refused.ok && refused.stale && sameSplitState(*p.store,before,true),
              "stale null or non-null head refuses both cuts before any write");
    }
    Project p,other;
    auto edit=clip_edit::prepareSplit(*p.store,8,17,18,{0,q/4},{0,3*q/4},48000);
    check(edit.ok,"atomic refusal fixture prepares a valid two-cut packet");
    if(!edit.ok)return;
    const auto foreignBefore=splitState(*other.store);
    check(!clip_edit::commitPrepared(*other.store,edit).ok && sameSplitState(*other.store,foreignBefore,true),
          "a prepared two-cut packet cannot mutate a different project with matching row identities");
    const auto before=splitState(*p.store);
    OpRequest bad;bad.opType="clip.setMute";bad.payload={{"id",999999},{"muted",true}};
    edit.requests.push_back(std::move(bad));
    check(!clip_edit::commitPrepared(*p.store,edit).ok && sameSplitState(*p.store,before,false),
          "a bad appended operation rolls back both cuts, all new children, the journal and its head");

    const auto refusalBefore=splitState(*p.store);
    const std::array<std::pair<clip_edit::TimePoint,clip_edit::TimePoint>,9> cuts={{
        {{0,q/4},{1,375000000}},{{2,q/4},{2,3*q/4}},{{0,-1},{0,q/2}},
        {{0,3*q/4},{0,q/4}},{{0,q/2},{0,q/2}},{{0,0},{0,q/2}},
        {{0,q/2},{0,q}},{{0,q/2},{0,2*q}},{{1,50000001},{1,50000002}}
    }};
    bool refused=true;
    for(const auto& [begin,end]:cuts)
        refused=refused && !clip_edit::prepareSplit(*p.store,8,17,18,begin,end,48000).ok &&
            sameSplitState(*p.store,refusalBefore,true);
    check(refused,"two cuts refuse mixed/unknown clocks, reversed or equal points, edges, outside spans and conversion-collapsed interior points");
    const std::array<std::pair<std::int64_t,std::int64_t>,5> identities={{{8,17},{17,8},{17,17},{7,17},{17,9}}};
    bool distinct=true;
    for(const auto& [middle,right]:identities)
        distinct=distinct && !clip_edit::prepareSplit(*p.store,8,middle,right,{0,q/4},{0,3*q/4},48000).ok &&
            sameSplitState(*p.store,refusalBefore,true);
    check(distinct,"two-cut preparation refuses repeated source/piece identities and occupied middle or right IDs without partial edits");
}


// Typed snapshots include future columns and distinguish NULL, BLOB and REAL.
Payload rangeTable(SQLite::Database& db,const char* table,const char* suffix="") {
    Payload rows=Payload::array();
    SQLite::Statement query(db,std::string("SELECT * FROM ")+table+" "+suffix+" ORDER BY 1");
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
                    const auto* data=static_cast<const std::uint8_t*>(cell.getBlob());
                    bytes.assign(data,data+cell.getBytes());
                }
                value=Payload::binary(bytes);
            }
            row[query.getColumnName(column)]=Payload::array({cell.getType(),value});
        }
        rows.push_back(std::move(row));
    }
    return rows;
}
Payload rangeRows(Store& store) {
    Payload rows=Payload::object();
    for(const char* table:{"clips","event_streams","note_expression","audio_clips","automation_data",
        "automation_segment_ranges","clip_properties","clip_playback_details","clip_warp_fade_domains",
        "tracks","lanes","project","tempo_map","time_signature_map","key_map","markers"})
        rows[table]=rangeTable(store.db(),table);
    return rows;
}
bool rangeSpan(SQLite::Database& db,std::int64_t id,int base,std::int64_t at,std::int64_t length,std::int64_t offset) {
    SQLite::Statement row(db,base==0?
        "SELECT count(*) FROM clips WHERE id=? AND time_base=0 AND pos_ticks=? AND length_ticks=? AND content_offset_ticks=?":
        "SELECT count(*) FROM clips WHERE id=? AND time_base=1 AND pos_ns=? AND length_ns=? AND content_offset_ticks=?");
    row.bind(1,id);row.bind(2,at);row.bind(3,length);row.bind(4,offset);
    return row.executeStep() && row.getColumn(0).getInt()==1;
}
void rangeSurvivorRelations() {
    constexpr std::int64_t q=5765760;
    // Source [2,6): disjoint, touching, whole, prefix, suffix and interior erase.
    const std::array<std::pair<int,int>,9> intervals={{{0,1},{7,8},{0,2},{6,7},{2,6},{1,3},{5,7},{3,5},{2,3}}};
    for(const int base:{0,1})for(const auto& [begin,end]:intervals) {
        Project p;auto& db=p.store->db();const std::int64_t unit=base==0?q:500000000;
        db.exec("DELETE FROM clips WHERE id<>8;DELETE FROM tempo_map;INSERT INTO tempo_map(pos_ticks,bpm) VALUES(0,120)");
        if(base==0)db.exec("UPDATE clips SET pos_ticks=11531520,length_ticks=23063040,content_offset_ticks=2882880 WHERE id=8");
        else db.exec("UPDATE clips SET time_base=1,pos_ticks=NULL,length_ticks=NULL,pos_ns=1000000000,length_ns=2000000000,content_offset_ticks=2882880 WHERE id=8");
        db.exec("INSERT INTO event_streams(id,clip_id,stream_kind,data) VALUES(61,8,'notes',X'00FF01')");
        const auto before=rangeRows(*p.store);const int writes=sqlite3_total_changes(db.getHandle());
        const auto edit=clip_edit::prepareDeleteRange(*p.store,{{21},{base,begin*unit},{base,end*unit}},48000);
        check(edit.ok && rangeRows(*p.store)==before && sqlite3_total_changes(db.getHandle())==writes,
              "range relation preparation is read-only for tick and ns clips");
        if(!edit.ok)continue;
        const bool intersects=begin<6 && end>2;
        if(!intersects) {
            check(edit.requests.empty() && rangeRows(*p.store)==before,"disjoint and touching half-open ranges make no operation or zero-length survivor");
            continue;
        }
        check(edit.requests.size()==1 && clip_edit::commitPrepared(*p.store,edit).ok,"intersecting range erase is one captured operation");
        const int leftEnd=std::min(begin,6),rightBegin=std::max(end,2);
        const bool left=begin>2,right=end<6;const int count=(left?1:0)+(right?1:0);
        bool geometry=db.execAndGet("SELECT count(*) FROM clips").getInt()==count &&
            db.execAndGet("SELECT count(*) FROM event_streams").getInt()==count;
        if(left || right) {
            const int first=left?2:rightBegin,last=left?leftEnd:6;
            geometry=geometry && rangeSpan(db,8,base,first*unit,(last-first)*unit,q/2+(first-2)*q) &&
                db.execAndGet("SELECT count(*) FROM event_streams WHERE id=61 AND clip_id=8 AND hex(data)='00FF01'").getInt()==1;
        }
        if(left && right) {
            SQLite::Statement tail(db,"SELECT id FROM clips WHERE id<>8");
            const bool found=tail.executeStep();
            geometry=geometry && found;
            if(found)geometry=geometry && rangeSpan(db,tail.getColumn(0).getInt64(),base,rightBegin*unit,(6-rightBegin)*unit,q/2+(rightBegin-2)*q);
        }
        check(geometry,"sole survivors keep original clip and child IDs; two survivors keep the left ID and one offset-correct new tail");
        const auto after=rangeRows(*p.store);
        check(History(*p.store).undo().ok && rangeRows(*p.store)==before &&
              History(*p.store).redo().ok && rangeRows(*p.store)==after,
              "every intersecting range relation restores exact typed rows and captured identities through undo/redo");
    }
}
void rangeTailChildrenAndClocks() {
    constexpr std::int64_t q=5765760;
    for(const int base:{0,1})for(const bool pinned:{false,true}) {
        Project p;auto& db=p.store->db();
        db.exec("INSERT INTO tracks(id,kind,name) VALUES(22,'audio','External alias');UPDATE clips SET track_id=22,alias_of=7 WHERE id=9");
        db.exec("DELETE FROM tempo_map;INSERT INTO tempo_map(pos_ticks,bpm) VALUES(0,120),(11531520,60)");
        db.exec("UPDATE clips SET pos_ticks=5765760,length_ticks=23063040,content_offset_ticks=2882880,fade_in_ticks=11531520,fade_out_ticks=11531520,loop_enabled=1,loop_start_ticks=0,loop_len_ticks=23063040 WHERE id IN (7,8)");
        if(base==1)db.exec("UPDATE clips SET time_base=1,pos_ticks=NULL,length_ticks=NULL,pos_ns=500000000,length_ns=3500000000 WHERE id IN (7,8)");
        db.exec("ALTER TABLE clips ADD COLUMN future_root BLOB NOT NULL DEFAULT X'';UPDATE clips SET future_root=X'0001FF' WHERE id IN (7,8)");
        db.exec("ALTER TABLE event_streams ADD COLUMN future_blob BLOB;INSERT INTO event_streams(id,clip_id,stream_kind,data,future_blob) VALUES(61,8,'notes',X'010203',X'00FF')");
        db.exec("INSERT INTO note_expression(id,clip_id,note_id,dimension,data) VALUES(62,8,-1,0,X'040506')");
        db.exec("INSERT INTO media_files(id,hash_blake3,rel_path,format,sample_rate,channels,frames) VALUES(300,'range-audio','range.wav','wav',44100,1,441000)");
        db.exec("INSERT INTO audio_clips(clip_id,media_id,src_start_frames,src_len_frames) VALUES(7,300,17,400000)");
        db.exec("INSERT INTO clip_properties(clip_id,end_marker_ticks,signature_numerator,signature_denominator) VALUES(7,34594560,3,4),(8,34594560,3,4)");
        db.exec("INSERT INTO automation_lanes(id,owner_kind,owner_id,param_ref,value_domain) VALUES(91,'track',21,'pan','real')");
        db.exec("INSERT INTO automation_data(id,lane_id,clip_id,data) VALUES(71,91,8,X'070809')");
        db.exec("INSERT INTO automation_segment_ranges(id,data_id,clip_id,record_index,progress_begin,progress_end,source_first,source_last) VALUES(72,71,8,0,0.2,0.7,0,1)");
        db.exec("INSERT INTO clip_warp_fade_domains(clip_id,in_begin_ticks,in_end_ticks,out_begin_ticks,out_end_ticks) VALUES(7,2882880,14414400,14414400,25945920)");
        if(pinned)db.exec("INSERT INTO clip_playback_details(clip_id,in_tension,out_tension,content_offset_ns,in_begin_ns,in_end_ns,out_begin_ns,out_end_ns) VALUES(7,0.4,-0.2,0,0,1500000000,1500000000,3500000000)");
        const auto before=rangeRows(*p.store);const int writes=sqlite3_total_changes(db.getHandle());
        const auto edit=clip_edit::prepareDeleteRange(*p.store,{{21},{0,0},{0,3*q}},48000);
        check(edit.ok && rangeRows(*p.store)==before && sqlite3_total_changes(db.getHandle())==writes,
              "sole-tail preparation captures audio/MIDI children and source clocks without writes");
        if(!edit.ok)continue;
        const bool committed=clip_edit::commitPrepared(*p.store,edit).ok;
        check(committed,"sole-tail audio/MIDI erase commits with external aliases and opaque children");
        if(!committed)continue;
        const auto after=rangeRows(*p.store);
        bool children=true;
        for(const char* table:{"event_streams","note_expression","audio_clips","automation_data","automation_segment_ranges","clip_properties","clip_warp_fade_domains"})
            children=children && after.at(table)==before.at(table);
        check(children && db.execAndGet("SELECT count(*) FROM clips WHERE id=9 AND alias_of=7").getInt()==1 &&
              db.execAndGet("SELECT count(*) FROM clips WHERE id IN (7,8) AND hex(future_root)='0001FF'").getInt()==2,
              "sole-tail trimming retains every child ID/byte/parent, future root BLOB, fade domain and external alias");
        const std::int64_t at=base==0?3*q:2000000000,length=base==0?2*q:2000000000;
        SQLite::Statement clock(db,"SELECT count(*) FROM clip_playback_details WHERE clip_id=7 AND content_offset_ns=? AND loop_begin_ns=0 AND loop_end_ns=3500000000 AND source_end_ns=5500000000");
        clock.bind(1,static_cast<std::int64_t>(pinned?1500000000:1750000000));
        bool clocks=clock.executeStep() && clock.getColumn(0).getInt()==1;
        if(pinned)clocks=clocks && db.execAndGet("SELECT count(*) FROM clip_playback_details WHERE clip_id=7 AND in_tension=0.4 AND out_tension=-0.2 AND in_begin_ns=0 AND in_end_ns=1500000000 AND out_begin_ns=1500000000 AND out_end_ns=3500000000").getInt()==1;
        check(rangeSpan(db,7,base,at,length,5*q/2) && rangeSpan(db,8,base,at,length,5*q/2) && clocks,
              "retained tail advances source phase across tempo change, treats pinned zero as present and retains loop/end/fade clocks");
        check(History(*p.store).undo().ok && rangeRows(*p.store)==before &&
              History(*p.store).redo().ok && rangeRows(*p.store)==after,
              "sole-tail undo/redo restores exact typed rows, aliases and optional clock-row absence");
    }
}
void ordinaryRangeLaneScope() {
    constexpr std::int64_t q=5765760;
    for(const int action:{0,1,2,3}) {
        Project p;auto& db=p.store->db();
        db.exec("DELETE FROM clips;DELETE FROM tempo_map;INSERT INTO tempo_map(pos_ticks,bpm) VALUES(0,120)");
        db.exec("INSERT INTO tracks(id,kind,name) VALUES(22,'midi','Unselected');INSERT INTO lanes(id,track_id,kind) VALUES(301,21,'take')");
        db.exec("INSERT INTO clips(id,track_id,kind,pos_ticks,length_ticks,lane_id) VALUES(8,21,'midi',0,5765760,NULL),(9,21,'midi',5765760,5765760,NULL),(80,21,'midi',0,17297280,301),(81,21,'video',0,NULL,301),(99,22,'midi',0,17297280,NULL)");
        db.exec("INSERT INTO event_streams(id,clip_id,stream_kind,data) VALUES(61,8,'notes',X'010203'),(62,80,'notes',X'040506'),(63,99,'notes',X'070809')");
        const auto before=rangeRows(*p.store);const auto protectedRows=rangeTable(db,"clips","WHERE lane_id IS NOT NULL OR track_id<>21");
        const auto protectedChildren=rangeTable(db,"event_streams","WHERE clip_id IN (80,81,99)");
        const int writes=sqlite3_total_changes(db.getHandle());
        const clip_edit::RangeSelection selection{{21},{0,0},{0,q}};
        const auto copy=clip_edit::copyRange(*p.store,selection,48000);
        check(copy.ok && copy.images.size()==1 && copy.images[0].at("row").at("id")==8 &&
              rangeRows(*p.store)==before && sqlite3_total_changes(db.getHandle())==writes,
              "ordinary range reader captures only selected main material and ignores unsupported nonfinite take metadata");
        if(!copy.ok)continue;
        clip_edit::PreparedEdit edit;
        if(action==0) {
            const auto cut=clip_edit::prepareCutRange(*p.store,selection,48000);
            check(cut.copy.ok && cut.copy.images==copy.images,"ordinary Cut uses the same main-lane clipboard as Copy");
            edit=cut.edit;
        } else if(action==1)edit=clip_edit::prepareDeleteRange(*p.store,selection,48000);
        else if(action==2)edit=clip_edit::preparePasteRange(*p.store,copy,{21},{0,q},48000);
        else edit=clip_edit::prepareDuplicateRange(*p.store,selection,48000);
        check(edit.ok && rangeRows(*p.store)==before && sqlite3_total_changes(db.getHandle())==writes,
              "ordinary Cut/Delete/Paste/Duplicate preparation leaves every lane and global row untouched");
        if(!edit.ok)continue;
        const bool committed=clip_edit::commitPrepared(*p.store,edit).ok;
        check(committed,"ordinary Cut/Delete/Paste/Duplicate succeeds despite an unsupported take on the selected track");
        if(!committed)continue;
        const auto after=rangeRows(*p.store);bool globals=true;
        for(const char* table:{"tracks","lanes","project","tempo_map","time_signature_map","key_map","markers"})
            globals=globals && after.at(table)==before.at(table);
        const int expectedMain=action<2?1:2;
        const bool material=action<2?
            db.execAndGet("SELECT count(*) FROM clips WHERE id=8").getInt()==0 && rangeSpan(db,9,0,q,q,0):
            rangeSpan(db,8,0,0,q,0) && db.execAndGet("SELECT count(*) FROM clips WHERE id=9").getInt()==0 &&
            db.execAndGet("SELECT count(*) FROM clips c JOIN event_streams e ON e.clip_id=c.id WHERE c.track_id=21 AND c.lane_id IS NULL AND c.pos_ticks=5765760 AND c.length_ticks=5765760 AND hex(e.data)='010203'").getInt()==1;
        check(material && db.execAndGet("SELECT count(*) FROM clips WHERE track_id=21 AND lane_id IS NULL").getInt()==expectedMain &&
              rangeTable(db,"clips","WHERE lane_id IS NOT NULL OR track_id<>21")==protectedRows &&
              rangeTable(db,"event_streams","WHERE clip_id IN (80,81,99)")==protectedChildren && globals,
              "ordinary range commands affect only main-lane material, preserving independent takes, unselected tracks and global maps");
        check(History(*p.store).undo().ok && rangeRows(*p.store)==before &&
              History(*p.store).redo().ok && rangeRows(*p.store)==after,
              "ordinary main-lane range commands retain exact one-step captured undo/redo");
void rangeSurvivorExhaustedIds() {
    constexpr std::int64_t q=5765760,maximum=std::numeric_limits<std::int64_t>::max();
    for(const int mode:{0,1,2,3}) {
        Project p;auto& db=p.store->db();
        db.exec("DELETE FROM clips");
        if(mode==3) {
            db.exec("INSERT INTO clips(id,track_id,kind,pos_ticks,length_ticks) VALUES(9223372036854775806,21,'midi',0,23063040)");
            db.exec("INSERT INTO event_streams(id,clip_id,stream_kind,data) VALUES(9223372036854775806,9223372036854775806,'notes',X'0001FF')");
        } else {
            db.exec("INSERT INTO clips(id,track_id,kind,pos_ticks,length_ticks) VALUES(9223372036854775807,21,'midi',0,23063040)");
            db.exec("INSERT INTO event_streams(id,clip_id,stream_kind,data) VALUES(9223372036854775807,9223372036854775807,'notes',X'0001FF')");
        }
        const auto before=rangeRows(*p.store);const int writes=sqlite3_total_changes(db.getHandle());
        const auto head=OpJournal(*p.store).headSeq();const auto ops=OpJournal(*p.store).count();
        const clip_edit::RangeSelection selection{{21},{0,mode==0?0:mode==1?3*q:q},{0,mode==0?q:mode==1?4*q:3*q}};
        const auto edit=clip_edit::prepareDeleteRange(*p.store,selection,48000);
        const bool unchanged=rangeRows(*p.store)==before && sqlite3_total_changes(db.getHandle())==writes &&
            OpJournal(*p.store).headSeq()==head && OpJournal(*p.store).count()==ops;
        if(mode==2) {
            check(!edit.ok && unchanged,"two surviving pieces refuse exhausted new IDs atomically");
            continue;
        }
        if(mode==3) {
            check(edit.ok && unchanged,"two survivors prepare with exactly one clip and material-child identity remaining");
            if(!edit.ok)continue;
            const bool committed=clip_edit::commitPrepared(*p.store,edit).ok;
            check(committed && rangeSpan(db,maximum-1,0,0,q,0) && rangeSpan(db,maximum,0,3*q,q,3*q) &&
                  db.execAndGet("SELECT count(*) FROM clips").getInt()==2 &&
                  db.execAndGet("SELECT count(*) FROM event_streams WHERE id=clip_id AND id IN(9223372036854775806,9223372036854775807) AND hex(data)='0001FF'").getInt()==2,
                  "interior erase creates exactly one independent right piece using the sole remaining clip and child IDs");
            if(!committed)continue;
            const auto after=rangeRows(*p.store);
            check(History(*p.store).undo().ok && rangeRows(*p.store)==before &&
                  History(*p.store).redo().ok && rangeRows(*p.store)==after,
                  "one-slot interior erase has exact captured undo/redo without discarded-middle identities");
            continue;
        }
        check(edit.ok && unchanged,"a sole left or right survivor prepares with exhausted clip and material-child IDs");
        if(!edit.ok)continue;
        const bool committed=clip_edit::commitPrepared(*p.store,edit).ok;
        check(committed && rangeSpan(db,maximum,0,mode==0?q:0,3*q,mode==0?q:0) &&
              rangeRows(*p.store).at("event_streams")==before.at("event_streams"),
              "sole-side trimming needs no fresh identity and retains the maximum existing root and child IDs");
        if(!committed)continue;
        const auto after=rangeRows(*p.store);
        check(History(*p.store).undo().ok && rangeRows(*p.store)==before &&
              History(*p.store).redo().ok && rangeRows(*p.store)==after,
              "exhausted-ID sole survivors retain exact captured undo/redo");
void rangeTimeLaneScope() {
    constexpr std::int64_t q=5765760;
    for(int mode=0;mode<4;++mode) {
        Project p;auto& db=p.store->db();
        db.exec("DELETE FROM clips;DELETE FROM tempo_map;INSERT INTO tempo_map(pos_ticks,bpm) VALUES(0,120)");
        db.exec("INSERT INTO tracks(id,kind,name) VALUES(23,'group','Group')");
        db.exec("INSERT INTO tracks(id,parent_id,kind,name) VALUES(22,23,'midi','Grouped')");
        db.exec("INSERT INTO lanes(id,track_id,kind) VALUES(301,21,'take'),(302,22,'take')");
        db.exec("INSERT INTO clips(id,track_id,lane_id,kind,pos_ticks,length_ticks) VALUES"
                "(8,21,NULL,'midi',8648640,8648640),(9,21,301,'midi',8648640,8648640)");
        db.exec("INSERT INTO clips(id,track_id,lane_id,kind,time_base,pos_ns,length_ns) VALUES(10,22,302,'midi',1,750000000,750000000)");
        db.exec("INSERT INTO markers(id,pos_ticks,kind) VALUES(91,17297280,'position');UPDATE project SET length_ticks=23063040");
        const auto before=rangeRows(*p.store);const int writes=sqlite3_total_changes(db.getHandle());
        const auto head=OpJournal(*p.store).headSeq();const auto ops=OpJournal(*p.store).count();
        if(mode==0) {
            const auto copy=clip_edit::copyTime(*p.store,q,2*q,48000);
            check(copy.ok && copy.material.images.size()==3 && copy.ticks==q && copy.nanos==500000000 &&
                  rangeRows(*p.store)==before && sqlite3_total_changes(db.getHandle())==writes && OpJournal(*p.store).headSeq()==head && OpJournal(*p.store).count()==ops,
                  "Copy Time reads main and take material on every track in both clocks without writing");
        }
        clip_edit::PreparedEdit edit;bool captured=true;
        if(mode==0)edit=clip_edit::prepareDeleteTime(*p.store,q,2*q,48000);
        else if(mode==1) {
            const auto cut=clip_edit::prepareCutTime(*p.store,q,2*q,48000);
            captured=cut.copy.ok && cut.copy.material.images.size()==3;edit=cut.edit;
        } else if(mode==2)edit=clip_edit::prepareInsertSilence(*p.store,q,q,48000);
        else {
            const auto copy=clip_edit::copyRange(*p.store,{{21},{0,q},{0,2*q}},48000);
            captured=copy.ok && copy.images.size()==1;
            edit=clip_edit::preparePasteRange(*p.store,copy,{21},{0,0},48000,true);
        }
        check(captured && edit.ok && edit.requests.size()==1 && rangeRows(*p.store)==before && sqlite3_total_changes(db.getHandle())==writes &&
              OpJournal(*p.store).headSeq()==head && OpJournal(*p.store).count()==ops,
              "Time clearing and insertion prepare one read-only packet; Range insertion captures selected main-lane material only");
        if(!captured || !edit.ok || edit.requests.size()!=1)continue;
        const auto committed=clip_edit::commitPrepared(*p.store,edit);
        check(committed.ok,"all-lane Time operation commits its captured packet");
        if(!committed.ok)continue;
        const bool lanes=db.execAndGet("SELECT count(*) FROM clips WHERE (id=9 AND lane_id=301) OR (id=10 AND lane_id=302)").getInt()==2;
        bool geometry=false;
        if(mode<=1) {
            geometry=db.execAndGet("SELECT count(*) FROM clips WHERE id IN(8,9) AND pos_ticks=5765760 AND length_ticks=5765760 AND content_offset_ticks=2882880").getInt()==2 &&
                db.execAndGet("SELECT count(*) FROM clips WHERE id=10 AND pos_ns=500000000 AND length_ns=500000000 AND content_offset_ticks=2882880").getInt()==1 &&
                db.execAndGet("SELECT count(*) FROM clips").getInt()==3 &&
                db.execAndGet("SELECT count(*) FROM markers WHERE id=91 AND pos_ticks=11531520").getInt()==1;
        } else {
            geometry=db.execAndGet("SELECT count(*) FROM clips WHERE id IN(8,9) AND pos_ticks=14414400 AND length_ticks=8648640 AND content_offset_ticks=0").getInt()==2 &&
                db.execAndGet("SELECT count(*) FROM clips WHERE id=10 AND pos_ns=1250000000 AND length_ns=750000000 AND content_offset_ticks=0").getInt()==1 &&
                db.execAndGet("SELECT count(*) FROM clips").getInt()==(mode==2?3:4) &&
                db.execAndGet("SELECT count(*) FROM markers WHERE id=91 AND pos_ticks=23063040").getInt()==1;
            if(mode==3)geometry=geometry && db.execAndGet("SELECT count(*) FROM clips WHERE id NOT IN(8,9,10) AND track_id=21 AND lane_id IS NULL AND pos_ticks=2882880 AND length_ticks=2882880").getInt()==1;
        }
        check(lanes && geometry,mode<=1?
              "Delete and Cut Time trim every lane, keep each sole-tail identity, relocate both clocks and retain original take lanes":
              "Time insertion shifts main and independent take originals on every track; Range insertion pastes only its main-lane clipboard");
        const auto after=rangeRows(*p.store);
        check(History(*p.store).undo().ok && rangeRows(*p.store)==before &&
              History(*p.store).redo().ok && rangeRows(*p.store)==after,
              "Time lane edits undo and redo exact captured rows, original identities, lane links and global placement");
} // namespace
namespace {
void twoPointSplitPayloadBounds() {
    struct Forgery {int kind;std::int64_t piece;const char* label;};
    constexpr std::array<Forgery,10> forgeries{{
        {0,0,"captured split refuses a third created root with its bounded-piece diagnostic"},
        {1,0,"captured split refuses a repeated new identity with its distinct-piece diagnostic"},
        {2,17,"captured split refuses a middle piece on a different existing track"},
        {2,18,"captured split refuses a right piece on a different existing track"},
        {3,17,"captured split refuses a middle piece of a different valid clip kind"},
        {3,18,"captured split refuses a right piece of a different valid clip kind"},
        {4,17,"captured split refuses a SQL-valid linear-clock middle piece"},
        {4,18,"captured split refuses a SQL-valid linear-clock right piece"},
        {5,0,"captured split refuses a copied child owned by an unrelated existing clip"},
        {6,0,"captured split refuses a rewrite of the original MIDI material"}}};
    for(const auto& forgery:forgeries) {
        Project project;auto& db=project.store->db();
        db.exec("INSERT INTO tracks(id,kind,name) VALUES(22,'audio','Other track'); INSERT INTO event_streams(id,clip_id,stream_kind,data) VALUES(51,8,'notes',X'01020304')");
        const auto original=clip_edit::copyClip(*project.store,8,48000);
        check(original.ok,"payload-bound fixture captures its populated original MIDI image");
        if(!original.ok)continue;
        const auto head=OpJournal(*project.store).headSeq();
        const auto opCount=db.execAndGet("SELECT count(*) FROM ops").getInt64();
        auto prepared=clip_edit::prepareSplit(*project.store,8,17,18,{0,1921920},{0,3843840},48000);
        const bool packet=prepared.ok && prepared.requests.size()==1 &&
            prepared.requests[0].opType=="clip.split" && prepared.requests[0].payload.is_object() &&
            prepared.requests[0].payload.contains("changes") && prepared.requests[0].payload.at("changes").is_array();
        check(packet,"payload-bound fixture has exactly one captured two-point split request");
        if(!packet)continue;
        auto& changes=prepared.requests[0].payload.at("changes");
        auto root=[&](std::int64_t id)->Payload* {
            for(auto& change:changes) {
                if(change.is_object() && change.contains("table") && change.at("table")=="clips" &&
                   change.contains("id") && change.at("id")==id && change.contains("before") &&
                   change.contains("after") && change.at("after").is_object())return &change;
            }
            return nullptr;
        };
        auto* source=root(8);auto* middle=root(17);auto* right=root(18);
        Payload* child=nullptr;
        for(auto& change:changes) {
            if(change.is_object() && change.contains("table") && change.at("table")=="event_streams" &&
               change.contains("before") && change.at("before").is_null() && change.contains("after") &&
               change.at("after").is_object() && change.at("after").contains("clip_id") &&
               change.at("after").at("clip_id")==17) {
                child=&change;break;
            }
        }
        const bool shape=source && middle && right && child && source->at("before").is_object() &&
            middle->at("before").is_null() && right->at("before").is_null();
        check(shape,"payload-bound fixture contains its source, both new roots and copied MIDI child");
        if(!shape)continue;
        const char* reason="split must keep the track, kind and time base";
        if(forgery.kind==0) {
            auto extra=*middle;extra["id"]=20;extra["after"]["id"]=20;
            changes.push_back(extra);
            reason="split needs one or two distinct new clip identities";
        } else if(forgery.kind==1) {
            (*right)["id"]=17;(*right)["after"]["id"]=17;
            reason="split needs one or two distinct new clip identities";
        } else if(forgery.kind>=2 && forgery.kind<=4) {
            auto& row=(forgery.piece==17?*middle:*right).at("after");
            if(forgery.kind==2)row["track_id"]=22;
            if(forgery.kind==3)row["kind"]="audio";
            if(forgery.kind==4) {
                // Coherent alternate clock: deleting the validator's time_base
                // comparison must not be hidden by a SQLite clock CHECK failure.
                row["time_base"]=1;row["pos_ticks"]=nullptr;row["length_ticks"]=nullptr;
                row["pos_ns"]=1000000000;row["length_ns"]=200000000;
            }
        } else if(forgery.kind==5) {
            (*child)["after"]["clip_id"]=9;
            reason="split child belongs to another clip";
        } else {
            // The copied row contains all original typed columns. Only its
            // generated key/owner differ; restore those to forge a real update.
            auto before=child->at("after");before["id"]=51;before["clip_id"]=8;
            auto after=before;
            after["data"]=Payload::binary(std::vector<std::uint8_t>{0x04,0x03,0x02,0x01});
            changes.push_back(Payload{{"table","event_streams"},{"id",51},{"before",before},{"after",after}});
            reason="split cannot rewrite the original material";
        }
        const auto committed=clip_edit::commitPrepared(*project.store,prepared);
        check(!committed.ok && committed.error.find(reason)!=std::string::npos,forgery.label);
        const auto remaining=clip_edit::copyClip(*project.store,8,48000);
        check(OpJournal(*project.store).headSeq()==head &&
              db.execAndGet("SELECT count(*) FROM ops").getInt64()==opCount &&
              db.execAndGet("SELECT count(*) FROM clips").getInt64()==3 &&
              db.execAndGet("SELECT count(*) FROM clips WHERE id IN (17,18,20)").getInt64()==0 &&
              db.execAndGet("SELECT count(*) FROM event_streams").getInt64()==1 &&
              db.execAndGet("SELECT count(*) FROM event_streams WHERE clip_id=9").getInt64()==0 &&
              remaining.ok && remaining.image==original.image,
              "captured-payload refusal leaves every original row, child, undo head and journal unchanged");
    }
    // The schema permits these caller-provided identities. Extending the
    // two-point validator must retain the existing one-cut ID domain.
    for(const std::int64_t newId:{0,-17}) {
        Project project;auto& db=project.store->db();
        db.exec("INSERT INTO event_streams(id,clip_id,stream_kind,data) VALUES(51,8,'notes',X'01020304')");
        const auto head=OpJournal(*project.store).headSeq();
        const auto prepared=clip_edit::prepareSplit(*project.store,8,newId,{0,2882880},48000);
        check(prepared.ok && prepared.requests.size()==1,"one-cut compatibility accepts zero and negative new clip IDs");
        if(!prepared.ok || prepared.requests.size()!=1)continue;
        const auto committed=clip_edit::commitPrepared(*project.store,prepared);
        check(committed.ok && committed.seqs.size()==1,"one-cut zero/negative identity commits as one operation");
        if(!committed.ok || committed.seqs.size()!=1)continue;
        auto retained=[&]() {
            SQLite::Statement row(db,"SELECT c.pos_ticks,c.length_ticks,c.content_offset_ticks,hex(e.data) FROM clips c JOIN event_streams e ON e.clip_id=c.id WHERE c.id=?");
            row.bind(1,newId);
            return row.executeStep() && row.getColumn(0).getInt64()==2882880 &&
                row.getColumn(1).getInt64()==2882880 && row.getColumn(2).getInt64()==2882880 &&
                row.getColumn(3).getString()=="01020304";
        };
        check(retained(),"one-cut zero/negative identity keeps its source material and placement");
        const auto undone=History(*project.store).undo();
        SQLite::Statement absent(db,"SELECT count(*) FROM clips WHERE id=?");absent.bind(1,newId);
        const bool removed=absent.executeStep() && absent.getColumn(0).getInt64()==0;
        check(undone.ok && undone.opsApplied==1 && undone.txnId==committed.txnId && removed &&
              OpJournal(*project.store).headSeq()==head &&
              db.execAndGet("SELECT length_ticks FROM clips WHERE id=8").getInt64()==5765760,
              "one undo restores the source and removes the zero/negative split identity");
        if(!undone.ok)continue;
        const auto redone=History(*project.store).redo();
        check(redone.ok && redone.opsApplied==1 && redone.txnId==committed.txnId && retained() &&
              OpJournal(*project.store).headSeq()==committed.seqs[0],
              "one redo restores the same zero/negative identity and copied material");
    }
}
} // namespace

int main(){try{stateAndSharing();refusalAndCurves();clipboardAndSplits();silenceAndCurves();rangeCommands();clockSharing();pureLoopAndCut();reusedMediaIds();twoPointGeometryAndMute();twoPointOpaqueChildren();twoPointAtomicRefusals();twoPointSplitPayloadBounds();rangeSurvivorRelations();rangeTailChildrenAndClocks();ordinaryRangeLaneScope();rangeSurvivorExhaustedIds();rangeTimeLaneScope();}catch(const std::exception& error){check(false,error.what());}std::printf("%d checks, %d failures\n",checks,failures);return failures?1:0;}
