// SPDX-License-Identifier: AGPL-3.0-or-later
#include "temp_directory.hpp"
#include "adi/blob.hpp"
#include "adi/clip_commands.hpp"
#include "adi/digest.hpp"
#include "adi/engine/automation.hpp"
#include "adi/history.hpp"
#include "adi/store.hpp"
#include "adi/store_rows.hpp"
#include <SQLiteCpp/SQLiteCpp.h>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <stdexcept>

namespace {
using namespace adi;
constexpr std::int64_t q=5765760;
int checks=0,failures=0;
void check(bool ok,const char* text){++checks;if(!ok){++failures;std::printf("FAIL %s\n",text);}}
struct Project {
    test::TempDirectory temporary{"time_edit","project"};std::unique_ptr<Store> store;
    Project() {
        StoreError error{};store=Store::create(temporary.path()/"song.adi",error);
        if(!store)throw std::runtime_error(toString(error));
        store->db().exec("INSERT INTO tracks(id,kind,name) VALUES(21,'audio','One'),(22,'midi','Two')");
    }
    std::int64_t integer(const char* sql){return store->db().execAndGet(sql).getInt64();}
    bool commit(const clip_edit::PreparedEdit& edit) {
        if(!edit.ok){std::printf("prepare: %s\n",edit.error.c_str());return false;}
        const auto result=clip_edit::commitPrepared(*store,edit);
        if(!result.ok)std::printf("commit: %s\n",result.error.c_str());
        return result.ok;
    }
};
void compoundDeletion() {
    Project p;auto& db=p.store->db();
    db.exec("INSERT INTO clips(id,track_id,kind,pos_ticks,length_ticks) VALUES"
        "(7,21,'midi',0,34594560),(8,22,'midi',5765760,2882880),(9,22,'midi',23063040,5765760),(11,22,'midi',7207200,0);"
        "INSERT INTO clips(id,track_id,kind,time_base,pos_ns,length_ns) VALUES"
        "(10,21,'midi',1,2000000000,1000000000);"
        "UPDATE project SET length_ticks=34594560;"
        "DELETE FROM tempo_map;INSERT INTO tempo_map(id,pos_ticks,bpm) VALUES(10,0,120),(11,5765760,150),(12,8648640,60),(13,23063040,90);"
        "INSERT INTO time_signature_map(id,pos_ticks,numerator,denominator) VALUES(31,5765760,3,4),(32,8648640,7,8),(33,23063040,5,4);"
        "INSERT INTO key_map(id,pos_ticks,root) VALUES(41,5765760,1),(42,8648640,2),(43,23063040,3);"
        "INSERT INTO tuning_systems(id,name,source) VALUES(1,'Three','edo');"
        "INSERT INTO tuning_degrees(tuning_id,degree,cents) VALUES(1,0,0),(1,1,400);"
        "INSERT INTO key_map_tunings VALUES(41,1),(42,1);INSERT INTO key_map_degrees VALUES(41,0),(41,1),(42,1);"
        "ALTER TABLE key_map_degrees ADD COLUMN future BLOB NOT NULL DEFAULT X'';"
        "UPDATE key_map_degrees SET future=X'00FF' WHERE key_map_id=42;"
        "INSERT INTO markers(id,pos_ticks,length_ticks,kind) VALUES(51,0,34594560,'cycle'),(52,5765760,NULL,'position'),(53,11531520,NULL,'position'),(54,5765760,2882880,'cycle');"
        "INSERT INTO markers(id,time_base,pos_ns,length_ns,kind) VALUES(55,1,0,3000000000,'cycle');"
        "INSERT INTO arranger_sections(id,start_ticks,end_ticks) VALUES(61,5765760,8648640),(62,0,34594560);"
        "INSERT INTO arranger_chain(id,ord,section_id,repeats) VALUES(71,0,61,2),(72,1,62,3);"
        "ALTER TABLE arranger_chain ADD COLUMN future BLOB NOT NULL DEFAULT X'';"
        "UPDATE arranger_chain SET future=X'0100FF' WHERE id=71");
    const std::string loop="{\"end\":34594560,\"future\":\"kept\",\"on\":true,\"start\":0}";
    {SQLite::Statement saved(db,"INSERT INTO session_state(key,value) VALUES('transport.loop',?)");saved.bind(1,loop);saved.exec();}
    const auto before=digestProject(*p.store).text;
    const auto edit=clip_edit::prepareDeleteTime(*p.store,q,2*q,48000);
    check(edit.ok && edit.requests.size()==1 && edit.options.expectHead.has_value() && digestProject(*p.store).text==before,"Delete Time prepares one guarded packet without writing");
    check(p.commit(edit),"compound deletion commits");
    check(p.integer("SELECT length_ticks FROM clips WHERE id=7")==q &&
          p.integer("SELECT count(*) FROM clips WHERE id IN(8,11)")==0 &&
          p.integer("SELECT pos_ticks FROM clips WHERE id=9")==3*q &&
          p.integer("SELECT pos_ticks FROM clips WHERE id NOT IN(7,8,9,10,11)")==q &&
          p.integer("SELECT content_offset_ticks FROM clips WHERE id NOT IN(7,8,9,10,11)")==2*q,"clips on every track trim, disappear or shift with material offsets");
    // Old q..2q is 0.2 s at 150 BPM plus 0.5 s at 60 BPM.
    check(p.integer("SELECT pos_ns FROM clips WHERE id=10")==1300000000,"linear clips move by the original integrated tempo duration");
    check(p.integer("SELECT count(*) FROM tempo_map WHERE id=11")==0 &&
          p.integer("SELECT pos_ticks FROM tempo_map WHERE id=12")==q &&
          p.integer("SELECT pos_ticks FROM tempo_map WHERE id=13")==3*q &&
          p.integer("SELECT pos_ticks FROM time_signature_map WHERE id=32")==q &&
          p.integer("SELECT pos_ticks FROM key_map WHERE id=42")==q,"join retains the original end tempo, meter and key context");
    check(p.integer("SELECT count(*) FROM key_map_degrees WHERE key_map_id=41")==0 &&
          db.execAndGet("SELECT hex(future) FROM key_map_degrees WHERE key_map_id=42").getString()=="00FF","removed tuning closure is captured and retained key degrees preserve future bytes");
    check(p.integer("SELECT length_ticks FROM markers WHERE id=51")==5*q &&
          p.integer("SELECT count(*) FROM markers WHERE id IN(52,54)")==0 &&
          p.integer("SELECT pos_ticks FROM markers WHERE id=53")==q &&
          p.integer("SELECT length_ns FROM markers WHERE id=55")==2300000000,"point and cycle markers in either clock collapse correctly");
    check(p.integer("SELECT count(*) FROM arranger_chain WHERE id=71")==0 &&
          p.integer("SELECT end_ticks FROM arranger_sections WHERE id=62")==5*q &&
          p.integer("SELECT length_ticks FROM project")==5*q,"arranger references and declared end follow deletion");
    const auto nextLoop=Payload::parse(db.execAndGet("SELECT value FROM session_state WHERE key='transport.loop'").getString());
    check(nextLoop.at("end")==5*q && nextLoop.at("future")=="kept","saved loop collapses while preserving unknown JSON members");
    const auto after=digestProject(*p.store).text;
    check(History(*p.store).undo().ok && digestProject(*p.store).text==before &&
          db.execAndGet("SELECT value FROM session_state WHERE key='transport.loop'").getString()==loop,"undo restores every typed row, composite key, reference and exact loop bytes");
    check(History(*p.store).redo().ok && digestProject(*p.store).text==after,"redo reproduces the captured whole Set");
    p.store.reset();StoreError error{};p.store=Store::open(p.temporary.path()/"song.adi",error);
    check(p.store && digestProject(*p.store).text==after && History(*p.store).undo().ok && digestProject(*p.store).text==before,"reopened history restores composite tuning and arranger children");
}
void automationCurves() {
    for(int base=0;base<=1;++base)for(int curve=0;curve<=5;++curve)for(const float tension:{-0.6f,0.4f}) {
        Project p;auto& db=p.store->db();
        SQLite::Statement lane(db,"INSERT INTO automation_lanes(id,owner_kind,owner_id,param_ref,value_domain,time_base) VALUES(91,'track',21,'pan','real',?)");lane.bind(1,base);lane.exec();
        AutomationPoint a{},b{};a.time=0;a.value=-1;a.curve=static_cast<std::uint8_t>(curve);a.tension=tension;a.point_id=std::numeric_limits<std::uint64_t>::max();a.flags=5;
        b.time=base==0?6*q:3000000000;b.value=1;b.point_id=17;b.flags=6;
        auto bytes=writeStream<AutomationPoint>(FourCC::Automation,std::vector<AutomationPoint>{a,b},1,StreamFlags::SortedByTime|(base==1?StreamFlags::TimeIsNanos:0));
        bytes.push_back(std::byte{0x00});bytes.push_back(std::byte{0xff});
        check(p.store->putAutomationData(91,{},bytes),"write global automation with full-width identity and future trailer");
        engine::AutomationProgram old(rows::readModel(*p.store),48000);
        check(p.commit(clip_edit::prepareDeleteTime(*p.store,q,3*q,48000)),"Delete Time slices a global curve in either clock");
        engine::AutomationProgram next(rows::readModel(*p.store),48000);bool same=old.problems().empty() && next.problems().empty();
        for(std::int64_t position=0;position<96000 && same;position+=13) {
            const auto original=position<24000?position:position+48000;
            same=std::abs(engine::valueAt(*old.lane(91),original)-engine::valueAt(*next.lane(91),position))<2e-12;
        }
        check(same,"all six nonlinear kinds preserve the played shape on both sides of the join");
        const auto encoded=*p.store->getAutomationData(91,{});StreamReader<AutomationPoint> reader(encoded,FourCC::Automation);
        check(reader.count()==4 && reader.at(0)->point_id==a.point_id && reader.at(0)->flags==5 &&
              reader.at(3)->point_id==b.point_id && reader.at(3)->flags==6 &&
              encoded[encoded.size()-1]==std::byte{0xff},"unremoved point identities, flags and opaque trailer remain exact");
        check(History(*p.store).undo().ok && *p.store->getAutomationData(91,{})==bytes &&
              p.integer("SELECT count(*) FROM automation_segment_ranges")==0,"undo restores original automation bytes and absent slice metadata");
        check(History(*p.store).redo().ok,"redo curve surgery");
        const auto inserted=clip_edit::prepareInsertSilence(*p.store,2*q,q/2,48000);
        check(p.commit(inserted),"insert can split an already deleted nonlinear curve");
        const auto removed=clip_edit::prepareDeleteTime(*p.store,2*q,5*q/2,48000);
        check(p.commit(removed),"delete can split an already inserted nonlinear curve");
        engine::AutomationProgram twice(rows::readModel(*p.store),48000);same=twice.problems().empty();
        for(std::int64_t position=0;position<96000 && same;position+=19)
            same=std::abs(engine::valueAt(*next.lane(91),position)-engine::valueAt(*twice.lane(91),position))<3e-12;
        check(same,"repeated insert/delete retains the original slice domain");
    }
}
void boundaryClustersAndMetadata() {
    Project p;auto& db=p.store->db();
    db.exec("INSERT INTO automation_lanes(id,owner_kind,owner_id,param_ref,value_domain) VALUES(91,'track',21,'pan','real')");
    std::vector<AutomationPoint> points(6);
    for(std::size_t index=0;index<points.size();++index){points[index].time=static_cast<std::int64_t>(index)*q;points[index].value=static_cast<double>(index);points[index].point_id=index+1;points[index].curve=1;}
    points[3].time=2*q;
    check(p.store->putAutomationData(91,{},writeStream<AutomationPoint>(FourCC::Automation,points)),"write coincident points at the right boundary");
    check(p.commit(clip_edit::prepareDeleteTime(*p.store,q,2*q,48000)),"delete across existing boundary points");
    auto bytes=*p.store->getAutomationData(91,{});StreamReader<AutomationPoint> read(bytes,FourCC::Automation);
    engine::AutomationProgram next(rows::readModel(*p.store),48000);
    check(read.count()==6 && read.at(2)->point_id==3 && read.at(3)->point_id==4 &&
          engine::valueAt(*next.lane(91),24000)==3,"original coincident right-boundary cluster keeps its order and right-continuous value");
    check(History(*p.store).undo().ok,"undo boundary cluster");
    db.exec("UPDATE automation_data SET data=data;ALTER TABLE automation_segment_ranges ADD COLUMN future BLOB NOT NULL DEFAULT X''");
    const auto first=clip_edit::prepareInsertSilence(*p.store,q/2,q/2,48000);
    check(p.commit(first),"create a reusable source range");
    // Start with one long nonlinear segment, then split it twice inside its domain.
    check(History(*p.store).undo().ok,"restore original stream before nonlinear metadata case");
    points.resize(2);points[0].time=0;points[0].value=-1;points[0].curve=3;points[1].time=6*q;points[1].value=1;
    check(p.store->putAutomationData(91,{},writeStream<AutomationPoint>(FourCC::Automation,points)),"write nonlinear source for future metadata");
    check(p.commit(clip_edit::prepareInsertSilence(*p.store,4*q,q/2,48000)),"create original nonlinear range rows");
    db.exec("UPDATE automation_segment_ranges SET future=X'0001FF'");
    check(p.commit(clip_edit::prepareDeleteTime(*p.store,q,2*q,48000)),"split a range carrying future metadata into two slices");
    check(p.integer("SELECT count(*) FROM automation_segment_ranges WHERE hex(future)<>'0001FF'")==0,"both split slices retain unknown source-range columns");
}
void guardsAndCollapsedLoops() {
    Project p;auto& db=p.store->db();
    db.exec("INSERT INTO clips(id,track_id,kind,pos_ticks,length_ticks) VALUES(7,21,'midi',0,34594560)");
    auto stale=clip_edit::prepareDeleteTime(*p.store,q,2*q,48000);
    OpRequest rename;rename.opType="track.rename";rename.payload={{"id",21},{"name","Other"}};
    check(OpJournal(*p.store).commit(rename).ok,"intervening edit moves the journal head");
    const auto before=digestProject(*p.store).text;
    check(!clip_edit::commitPrepared(*p.store,stale).ok && digestProject(*p.store).text==before,"stale Delete Time refuses atomically");
    auto late=clip_edit::prepareDeleteTime(*p.store,q,2*q,48000);
    for(auto& change:late.requests[0].payload["changes"])
        if(change.at("table")=="clips" && change.at("before").is_null())change["after"]["track_id"]=999;
    check(!p.commit(late) && digestProject(*p.store).text==before,"late SQL failure rolls back earlier deletions and updates");
    db.exec("INSERT INTO session_state(key,value) VALUES('transport.loop','{\"on\":true,\"start\":5765760,\"end\":8648640,\"future\":7}')");
    check(p.commit(clip_edit::prepareDeleteTime(*p.store,q,2*q,48000)),"delete the complete saved loop");
    const auto loop=Payload::parse(db.execAndGet("SELECT value FROM session_state WHERE key='transport.loop'").getString());
    check(loop.at("on")==false && loop.at("start")==q && loop.at("end")==3*q/2 && loop.at("future")==7,"a removed loop disables and retains a positive bracket at the join");
    check(!clip_edit::prepareDeleteTime(*p.store,-1,q,48000).ok &&
          !clip_edit::prepareDeleteTime(*p.store,q,q,48000).ok &&
          !clip_edit::prepareDeleteTime(*p.store,0,q,0).ok,"invalid endpoints and render rates refuse");
    Project zero;zero.store->db().exec("INSERT INTO tempo_map(id,pos_ticks,bpm) VALUES(12,5765760,60)");
    check(zero.commit(clip_edit::prepareDeleteTime(*zero.store,0,2*q,48000)) &&
          zero.integer("SELECT count(*) FROM tempo_map")==1 && zero.integer("SELECT pos_ticks FROM tempo_map WHERE id=12")==0,"deletion from zero retains the end context as the new root event");
}
void timeClipboards() {
    for(int base=0;base<=1;++base)for(int curve=0;curve<=5;++curve) {
        Project p;auto& db=p.store->db();
        db.exec("INSERT INTO clips(id,track_id,kind,pos_ticks,length_ticks) VALUES(7,21,'midi',0,46126080);"
            "INSERT INTO clips(id,track_id,kind,time_base,pos_ns,length_ns) VALUES(8,22,'midi',1,500000000,1000000000)");
        {
            SQLite::Statement lane(db,"INSERT INTO automation_lanes(id,owner_kind,owner_id,param_ref,value_domain,time_base) VALUES(91,'track',21,'pan','real',?)");lane.bind(1,base);lane.exec();
        }
        AutomationPoint a{},b{};a.time=0;a.value=-1;a.curve=static_cast<std::uint8_t>(curve);a.tension=-0.6f;a.point_id=std::numeric_limits<std::uint64_t>::max();a.flags=3;
        b.time=base==0?8*q:4000000000;b.value=1;b.point_id=17;b.flags=5;
        auto bytes=writeStream<AutomationPoint>(FourCC::Automation,std::vector<AutomationPoint>{a,b},1,StreamFlags::SortedByTime|(base==1?StreamFlags::TimeIsNanos:0));
        bytes.push_back(std::byte{0xff});
        check(p.store->putAutomationData(91,{},bytes),"write global envelope for whole-Set clipboard");
        engine::AutomationProgram original(rows::readModel(*p.store),48000);const auto before=digestProject(*p.store).text;
        const auto copied=clip_edit::copyTime(*p.store,q,3*q,48000);
        if(!copied.ok)std::printf("copy: %s\n",copied.error.c_str());
        check(copied.ok && copied.material.images.size()==2 && copied.ticks==2*q && copied.nanos==1000000000 &&
              digestProject(*p.store).text==before,"Copy Time captures both material clocks and global context without writing");
        const auto pasted=clip_edit::preparePasteTime(*p.store,copied,4*q,48000);
        check(pasted.ok && pasted.requests.size()==1 && p.commit(pasted),"Paste Time is one whole-Set native edit");
        engine::AutomationProgram next(rows::readModel(*p.store),48000);bool same=next.problems().empty();
        for(std::int64_t position=0;position<240000 && same;position+=11) {
            const auto old=position<96000?position:position<144000?position-96000+24000:position-48000;
            same=std::abs(engine::valueAt(*original.lane(91),old)-engine::valueAt(*next.lane(91),position))<3e-12;
        }
        check(same,"Paste Time copies every source curve and returns to the unchanged destination on both joins");
        const auto after=digestProject(*p.store).text;
        check(History(*p.store).undo().ok && digestProject(*p.store).text==before,"Paste Time undo restores maps, global data and all original clip identities");
        check(History(*p.store).redo().ok && digestProject(*p.store).text==after,"Paste Time redo uses captured native values");
        check(History(*p.store).undo().ok,"restore source before Cut Time");
        const auto cut=clip_edit::prepareCutTime(*p.store,q,3*q,48000);
        check(cut.copy.ok && cut.edit.ok && p.commit(cut.edit),"Cut Time captures its complete clipboard and removal under one read snapshot");
        check(History(*p.store).undo().ok && digestProject(*p.store).text==before,"Cut Time undo restores the exact original Set");
        check(p.commit(clip_edit::prepareDuplicateTime(*p.store,q,3*q,48000)),"Duplicate Time copies the whole context at the selection's end");
        engine::AutomationProgram duplicate(rows::readModel(*p.store),48000);same=duplicate.problems().empty();
        for(std::int64_t position=0;position<240000 && same;position+=13) {
            const auto old=position<72000?position:position<120000?position-72000+24000:position-48000;
            same=std::abs(engine::valueAt(*original.lane(91),old)-engine::valueAt(*duplicate.lane(91),position))<3e-12;
        }
        check(same,"Duplicate Time preserves source and destination curve domains in either clock");
    }
}
void copiedTempoAndTuning() {
    Project p;auto& db=p.store->db();
    db.exec("DELETE FROM tempo_map;INSERT INTO tempo_map(id,pos_ticks,bpm) VALUES(10,0,120),(11,11531520,60),(12,23063040,240);"
        "INSERT INTO time_signature_map(id,pos_ticks,numerator,denominator) VALUES(21,5765760,3,4),(22,11531520,7,8),(23,23063040,5,4);"
        "INSERT INTO key_map(id,pos_ticks,root) VALUES(31,5765760,1),(32,11531520,2),(33,23063040,3);"
        "INSERT INTO tuning_systems(id,name,source) VALUES(1,'Three','edo');"
        "INSERT INTO tuning_degrees(tuning_id,degree,cents) VALUES(1,0,0),(1,1,400);"
        "INSERT INTO key_map_tunings VALUES(31,1),(32,1);INSERT INTO key_map_degrees VALUES(31,0),(32,1);"
        "ALTER TABLE key_map_degrees ADD COLUMN future BLOB NOT NULL DEFAULT X'';UPDATE key_map_degrees SET future=X'0001FF';"
        "INSERT INTO markers(id,pos_ticks,length_ticks,kind) VALUES(41,0,23063040,'cycle'),(42,11531520,NULL,'position');"
        "INSERT INTO arranger_sections(id,start_ticks,end_ticks) VALUES(51,0,23063040);"
        "INSERT INTO arranger_chain(id,ord,section_id) VALUES(61,0,51);"
        "INSERT INTO clips(id,track_id,kind,time_base,pos_ns,length_ns) VALUES(7,21,'midi',1,4000000000,1000000000)");
    const auto before=digestProject(*p.store).text;const auto copy=clip_edit::copyTime(*p.store,q,3*q,48000);
    check(copy.ok && copy.nanos==1500000000,"Copy Time captures the source tempo integral across an interior event");
    check(p.commit(clip_edit::preparePasteTime(*p.store,copy,5*q,48000)),"Paste Time at a different target tempo");
    check(p.integer("SELECT pos_ns FROM clips WHERE id=7")==5500000000,"linear objects move by copied 1.5 seconds rather than the target tempo's 0.5 seconds");
    check(p.integer("SELECT CAST(bpm AS INTEGER) FROM tempo_map WHERE pos_ticks=28828800")==120 &&
          p.integer("SELECT CAST(bpm AS INTEGER) FROM tempo_map WHERE pos_ticks=34594560")==60 &&
          p.integer("SELECT CAST(bpm AS INTEGER) FROM tempo_map WHERE pos_ticks=40360320")==240,"copied carry/interior tempo points and target end context are explicit");
    check(p.integer("SELECT numerator FROM time_signature_map WHERE pos_ticks=28828800")==3 &&
          p.integer("SELECT numerator FROM time_signature_map WHERE pos_ticks=34594560")==7 &&
          p.integer("SELECT numerator FROM time_signature_map WHERE pos_ticks=40360320")==5,"meter context follows the copied interval and then restores the destination");
    check(p.integer("SELECT count(*) FROM key_map_degrees d JOIN key_map k ON k.id=d.key_map_id WHERE k.pos_ticks IN(28828800,34594560) AND hex(d.future)='0001FF'")==2,"pasted tuned keys retain composite degree metadata");
    check(p.integer("SELECT count(*) FROM arranger_chain")==1 && p.integer("SELECT count(*) FROM arranger_sections")==2,"copied section boundaries leave the existing playback playlist unchanged");
    const auto after=digestProject(*p.store).text;
    check(History(*p.store).undo().ok && digestProject(*p.store).text==before &&
          History(*p.store).redo().ok && digestProject(*p.store).text==after,"compound map/tuning/marker paste has exact undo and redo");
    check(History(*p.store).undo().ok,"restore source for tuning identity check");
    db.exec("UPDATE tuning_degrees SET cents=450 WHERE tuning_id=1 AND degree=1");
    const auto changed=digestProject(*p.store).text;const auto refused=clip_edit::preparePasteTime(*p.store,copy,0,48000);
    check(!refused.ok && refused.error.find("tuning identity")!=std::string::npos && digestProject(*p.store).text==changed,"a reused or changed tuning cannot reinterpret an old clipboard");
    db.exec("UPDATE tuning_degrees SET cents=400 WHERE tuning_id=1 AND degree=1");
    check(p.commit(clip_edit::preparePasteTime(*p.store,copy,0,48000)),"paste at the protected zero root");
    check(p.integer("SELECT CAST(bpm AS INTEGER) FROM tempo_map WHERE pos_ticks=11531520")==120 &&
          p.integer("SELECT root FROM key_map WHERE pos_ticks=0")==1,"zero insertion replaces the root and restores old context at the far join");
    check(History(*p.store).undo().ok && digestProject(*p.store).text==before,"zero-root paste restores exact original root bindings and absence");
}

void timeClipboardEnvelopeEdges() {
    for(int scenario=0;scenario<4;++scenario) {
        Project p;auto& db=p.store->db();
        db.exec("INSERT INTO automation_lanes(id,owner_kind,owner_id,param_ref,value_domain,default_value) VALUES(91,'track',21,'pan','real',0.25)");
        auto write=[&](std::int64_t first,std::int64_t last,double low,double high) {
            AutomationPoint a{},b{};a.time=first;a.value=low;a.curve=3;a.tension=0.4f;a.point_id=9;b.time=last;b.value=high;b.point_id=10;
            auto blob=writeStream<AutomationPoint>(FourCC::Automation,std::vector<AutomationPoint>{a,b});blob.push_back(std::byte{0xff});
            if(!p.store->putAutomationData(91,{},blob))throw std::runtime_error("write clipboard edge");
        };
        if(scenario!=3)write(0,8*q,-1,1);
        engine::AutomationProgram source(rows::readModel(*p.store),48000);
        const auto copy=clip_edit::copyTime(*p.store,q,3*q,48000);
        db.exec("DELETE FROM automation_data");
        if(scenario==0)write(7*q,8*q,0.3,0.9);
        else if(scenario==1)write(0,q,0.3,0.9);
        else if(scenario==3)write(0,8*q,0.3,0.9);
        engine::AutomationProgram target(rows::readModel(*p.store),48000);const auto before=digestProject(*p.store).text;
        check(copy.ok && p.commit(clip_edit::preparePasteTime(*p.store,copy,4*q,48000)),"paste handles before-first, after-last, absent target and absent source envelopes");
        engine::AutomationProgram next(rows::readModel(*p.store),48000);bool same=next.problems().empty();
        for(std::int64_t position=0;position<264000 && same;position+=17) {
            const auto expected=position<96000?engine::valueAt(*target.lane(91),position):
                position<144000?engine::valueAt(*source.lane(91),position-96000+24000):
                    engine::valueAt(*target.lane(91),position-48000);
            same=std::abs(engine::valueAt(*next.lane(91),position)-expected)<3e-12;
        }
        check(same,"held/default values restore exactly at both clipboard joins without inventing a ramp");
        check(p.store->getAutomationData(91,{})->back()==std::byte{0xff},"target stream trailers survive merging and a newly created stream retains the source trailer");
        const auto after=digestProject(*p.store).text;
        p.store.reset();StoreError error{};p.store=Store::open(p.temporary.path()/"song.adi",error);
        check(p.store && digestProject(*p.store).text==after && History(*p.store).undo().ok &&
              digestProject(*p.store).text==before,"reopened Time clipboard history preserves absent automation and exact native state");
    }
}
void timeClipboardGuards() {
    Project p;auto& db=p.store->db();
    db.exec("INSERT INTO clips(id,track_id,kind,pos_ticks,length_ticks) VALUES(7,21,'midi',0,17297280)");
    const auto material=clip_edit::copyRange(*p.store,{{21},{0,q},{0,2*q}},48000);
    check(material.ok && p.commit(clip_edit::preparePasteRange(*p.store,material,{21},{0,7*q},48000)),"material paste reserves the synthetic identity of a clip trimmed only in the clipboard");
    check(History(*p.store).undo().ok,"restore source after material identity regression");
    db.exec("INSERT INTO automation_lanes(id,owner_kind,owner_id,param_ref,value_domain) VALUES(91,'track',21,'pan','real')");
    const auto copy=clip_edit::copyTime(*p.store,q,2*q,48000);
    const auto stale=clip_edit::preparePasteTime(*p.store,copy,4*q,48000);
    OpRequest rename;rename.opType="track.rename";rename.payload={{"id",21},{"name","Changed"}};
    check(OpJournal(*p.store).commit(rename).ok,"intervening native edit changes the clipboard preparation's expected head");
    const auto before=digestProject(*p.store).text;
    check(!clip_edit::commitPrepared(*p.store,stale).ok && digestProject(*p.store).text==before,"stale Paste Time refuses every material/map/automation write atomically");
    db.exec("UPDATE automation_lanes SET param_ref='volume' WHERE id=91");
    const auto ownerChanged=digestProject(*p.store).text;const auto refused=clip_edit::preparePasteTime(*p.store,copy,4*q,48000);
    check(!refused.ok && refused.error.find("lane changed")!=std::string::npos && digestProject(*p.store).text==ownerChanged,"copied automation cannot silently bind to a reused lane or another parameter");
    db.exec("UPDATE automation_lanes SET param_ref='pan' WHERE id=91");
    auto late=clip_edit::preparePasteTime(*p.store,copy,4*q,48000);
    for(auto& change:late.requests[0].payload["changes"])if(change.at("table")=="clips" && change.at("before").is_null())change["after"]["track_id"]=999;
    const auto unchanged=digestProject(*p.store).text;
    check(!p.commit(late) && digestProject(*p.store).text==unchanged,"late Time paste constraint failure rolls back created maps, curves and material");
    Project other;
    check(!clip_edit::preparePasteTime(*other.store,copy,0,48000).ok &&
          !clip_edit::preparePasteTime(*p.store,copy,0,44100).ok &&
          !clip_edit::copyTime(*p.store,2*q,q,48000).ok,"cross-project implicit mapping, changed render rate and inverted Time selections refuse");
}

}
int main(){try{compoundDeletion();automationCurves();boundaryClustersAndMetadata();guardsAndCollapsedLoops();timeClipboards();copiedTempoAndTuning();timeClipboardEnvelopeEdges();timeClipboardGuards();}catch(const std::exception& error){check(false,error.what());}std::printf("%d checks, %d failures\n",checks,failures);return failures?1:0;}
