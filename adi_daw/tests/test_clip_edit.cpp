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

} // namespace
int main(){try{stateAndSharing();refusalAndCurves();clipboardAndSplits();silenceAndCurves();rangeCommands();clockSharing();pureLoopAndCut();}catch(const std::exception& error){check(false,error.what());}std::printf("%d checks, %d failures\n",checks,failures);return failures?1:0;}
