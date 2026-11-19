// SPDX-License-Identifier: AGPL-3.0-or-later
#include "temp_directory.hpp"
#include "adi/automation_edits.hpp"
#include "adi/automation_lanes.hpp"
#include "adi/blob.hpp"
#include "adi/engine/automation_snapshot.hpp"
#include "adi/engine/automation.hpp"
#include "adi/history.hpp"
#include "adi/row_capture.hpp"
#include "adi/store.hpp"
#include <SQLiteCpp/SQLiteCpp.h>
#include <array>
#include <cstdio>
#include <cstring>
#include <stdexcept>
namespace {
using namespace adi;
int checks=0,failures=0;
void check(bool good,const char* label){++checks;if(!good){++failures;std::printf("FAIL: %s\n",label);}}
Payload rows(SQLite::Database& db,const std::string& sql){Payload result=Payload::array();SQLite::Statement query(db,sql);while(query.executeStep())result.push_back(detail::captureQueryRow(query));return result;}
struct Fixture {
    test::TempDirectory temp{"automation","range_undo"};std::unique_ptr<Store> store;
    Fixture(){StoreError error{};store=Store::createEmpty(temp.path()/"song.adi",error);if(!store)throw std::runtime_error("create");auto& db=store->db();db.exec("INSERT INTO tracks(id,kind,name) VALUES(1,'audio','Track'); INSERT INTO clips(id,track_id,kind,time_base,pos_ticks,length_ticks) VALUES(1,1,'audio',0,0,1000); INSERT INTO automation_lanes(id,owner_kind,owner_id,param_ref) VALUES(2,'track',1,'volume'),(3,'track',1,'pan'); ALTER TABLE automation_segment_ranges ADD COLUMN future_blob BLOB NOT NULL DEFAULT X''; ALTER TABLE automation_segment_ranges ADD COLUMN future_text TEXT; ALTER TABLE automation_segment_ranges ADD COLUMN future_integer INTEGER");
        std::array<AutomationPoint,4> points{};for(std::size_t i=0;i<points.size();++i){auto& point=points[i];point.point_id=i==0?UINT64_MAX-2:static_cast<std::uint64_t>((i+1)*10);point.time=static_cast<std::int64_t>(i*100);point.value=.1*static_cast<double>(i+1);point.curve=static_cast<std::uint8_t>(i+2);point.tension=.25f;}
        const auto bytes=writeStream<AutomationPoint>(FourCC::Automation,points);for(const auto id:{10,11,12}){SQLite::Statement insert(db,"INSERT INTO automation_data(id,lane_id,clip_id,data) VALUES(?,?,?,?)");insert.bind(1,id);insert.bind(2,id==12?3:2);if(id==11)insert.bind(3,1);else insert.bind(3);insert.bind(4,bytes.data(),static_cast<int>(bytes.size()));insert.exec();for(int index=0;index<3;++index){SQLite::Statement range(db,"INSERT INTO automation_segment_ranges(id,data_id,clip_id,record_index,progress_begin,progress_end,source_first,source_last,future_blob,future_text,future_integer) VALUES(?,?,?,?,0.2,0.8,-0.123456789012345,0.987654321098765,?,?,?)");range.bind(1,id*10+index);range.bind(2,id);if(id==11)range.bind(3,1);else range.bind(3);range.bind(4,index);const std::array<unsigned char,3> opaque{0,static_cast<unsigned char>(index),255};range.bind(5,opaque.data(),index==1?0:static_cast<int>(opaque.size()));if(index==1)range.bind(6);else range.bind(6,std::string("opaque\0text",11));range.bind(7,INT64_MAX-index);range.exec();}}
    }
    std::vector<std::uint8_t> image(){auto& db=store->db();return Payload::to_cbor(Payload{{"lanes",rows(db,"SELECT * FROM automation_lanes ORDER BY id")},{"data",rows(db,"SELECT * FROM automation_data ORDER BY id")},{"ranges",rows(db,"SELECT * FROM automation_segment_ranges ORDER BY id")}});}
    Payload ranges(int data=10){return rows(store->db(),"SELECT * FROM automation_segment_ranges WHERE data_id="+std::to_string(data)+" ORDER BY record_index,id");}
    bool valid(){const auto lanes=engine::detail::readAutomationSnapshot(store->db(),{},{});for(const auto& lane:lanes)for(const auto& envelope:lane->envelopes)if(!envelope->problem.empty())return false;return true;}
    void commit(const OpRequest& request){const auto result=OpJournal(*store).commit(request);if(!result.ok)throw std::runtime_error(result.error);}
};
Payload point(std::uint64_t id,std::int64_t time,double value,int curve=2,double tension=.25){return {{"id",id},{"time",time},{"value",value},{"curve",curve},{"tension",tension}};}
void roundtrip(Fixture& fixture,const OpRequest& request){const auto before=fixture.image();fixture.commit(request);const auto after=fixture.image();History history(*fixture.store);check(history.undo().ok && fixture.image()==before,"Undo restores all lane/data/range bytes, identities, nullable addresses and unknown SQLite cells");check(history.redo().ok && fixture.image()==after,"Redo reproduces the complete forward automation image");check(history.undo().ok && fixture.image()==before,"a second Undo still restores every curved lane byte for byte");}
void lanes(){
    for(const auto& request:std::array<OpRequest,5>{automation::deleteLane(2),automation::clearLane(2),automation::setLaneEnabled(2,false),automation::setLaneVisible(2,true),automation::setDefaultValue(2,.25)}){Fixture fixture;roundtrip(fixture,request);check(fixture.valid(),"native arrangement and clip snapshots remain valid after lane Undo/Redo/Undo");}
    Fixture clear;const auto unrelated=clear.ranges(12);clear.commit(automation::clearLane(2));check(clear.ranges().empty() && clear.ranges(11).empty() && clear.ranges(12)==unrelated && clear.valid(),"clear removes only its arrangement and clip curve ranges and leaves no stale record index");
    Fixture restore;auto state=Payload{{"row",*detail::captureRow(restore.store->db(),"automation_lanes","id",2)},{"data",Payload::array()}};for(const int id:{10,11}){const auto row=*detail::captureRow(restore.store->db(),"automation_data","id",id);state["data"].push_back({{"id",id},{"clip",row.at("clip_id")},{"bytes",row.at("data")},{"ranges",restore.ranges(id)}});}state["row"]["visible"]=1;OpRequest request;request.opType="automation.restoreLane";request.payload={{"id",2},{"state",state}};roundtrip(restore,request);
    // Creation plus populated data in one real transaction exercises the
    // creation inverse without writing unjournalled changes after its commit.
    Fixture create;create.store->db().exec("DELETE FROM automation_lanes WHERE id=2");const auto before=create.image();automation::LaneSpec spec;spec.id=2;spec.ownerId=1;spec.paramRef="volume";std::vector<OpRequest> batch{automation::createLane(spec)};for(const auto& data:state["data"]){OpRequest dataRestore;dataRestore.opType="automation.restoreData";dataRestore.payload={{"id",data.at("id")},{"lane",2},{"clip",data.at("clip")},{"state",{{"present",true},{"bytes",data.at("bytes")},{"ranges",data.at("ranges")}}}};batch.push_back(std::move(dataRestore));}const auto result=OpJournal(*create.store).commit(batch);check(result.ok && create.ranges().size()==3 && create.ranges(11).size()==3,"lane creation and exact curved stream restoration commit as one transaction");const auto after=create.image();History history(*create.store);check(history.undo().ok && create.image()==before && history.redo().ok && create.image()==after && history.undo().ok && create.image()==before,"creation Undo/Redo/Undo preserves absence and the complete populated lane");
}
void edits(){
    for(const bool clip:{false,true}){const int data=clip?11:10;const Payload address=clip?Payload(1):Payload(nullptr);
        {Fixture fixture;const auto original=fixture.ranges(data);fixture.commit(automation::deletePoints(data,2,address,Payload::array({UINT64_MAX-2})));const auto current=fixture.ranges(data);check(current.size()==2 && current[0].at("id")==original[1].at("id") && current[0].at("record_index")==0 && current[1].at("id")==original[2].at("id") && current[1].at("record_index")==1 && fixture.valid(),"deleting a point reindexes surviving outgoing curve ranges by point identity");}
        {Fixture fixture;fixture.commit(automation::deletePoints(data,2,address,Payload::array({20})));const auto current=fixture.ranges(data);check(current.size()==1 && current[0].at("id")==data*10+2 && current[0].at("record_index")==1 && fixture.valid(),"deleting an interior point drops both replaced adjacent segments and reindexes the unaffected one");}
        {Fixture fixture;fixture.commit(automation::deletePoints(data,2,address,Payload::array({UINT64_MAX-2,20,30,40})));check(fixture.ranges(data).empty() && fixture.valid(),"deleting the last points leaves no orphaned curve ranges");}
        {Fixture fixture;const auto original=fixture.ranges(data);fixture.commit(automation::movePoints(data,2,address,Payload::array({point(UINT64_MAX-2,350,.1)})));const auto current=fixture.ranges(data);check(current.size()==2 && current[0].at("id")==original[1].at("id") && current[0].at("record_index")==0 && current[1].at("id")==original[2].at("id") && current[1].at("record_index")==1 && fixture.valid(),"moving a point across others carries untouched ranges to their new indices and drops changed adjacency");}
        {Fixture fixture;const auto original=fixture.ranges(data);fixture.commit(automation::movePoints(data,2,address,Payload::array({point(UINT64_MAX-2,10,.1)})));check(fixture.ranges(data)==original && fixture.valid(),"time-only movement with unchanged endpoint identities/values preserves the retained nonlinear shape");}
        {Fixture fixture;fixture.commit(automation::movePoints(data,2,address,Payload::array({point(20,100,.9)})));const auto current=fixture.ranges(data);check(current.size()==1 && current[0].at("record_index")==2 && fixture.valid(),"changing an endpoint value invalidates only its incident retained segment slices");}
        {Fixture fixture;fixture.commit(automation::writePoints(data,2,address,Payload::array({point(99,50,.15)})));const auto current=fixture.ranges(data);check(current.size()==2 && current[0].at("record_index")==2 && current[1].at("record_index")==3 && fixture.valid(),"inserting a new point removes the split range and shifts unaffected ranges without stale metadata");}
        {Fixture fixture;fixture.commit(automation::setCurve(data,2,address,Payload::array({20}),5,-.5));const auto current=fixture.ranges(data);check(current.size()==2 && current[0].at("record_index")==0 && current[1].at("record_index")==2 && fixture.valid(),"changing a curve discards its old retained source domain and preserves unrelated slices");}
        for(const auto& request:std::array<OpRequest,4>{automation::deletePoints(data,2,address,Payload::array({20})),automation::movePoints(data,2,address,Payload::array({point(UINT64_MAX-2,350,.1)})),automation::writePoints(data,2,address,Payload::array({point(99,50,.15)})),automation::setCurve(data,2,address,Payload::array({20}),5,-.5)}){Fixture fixture;roundtrip(fixture,request);}
    }
}
void atomic(){
    Fixture fixture;const auto before=fixture.image();const auto request=automation::clearLane(2);CommitOptions options;options.beforeCommit=[](SQLite::Database&,std::int64_t,std::string& error){error="range rollback";return false;};check(!OpJournal(*fixture.store).commit(std::span<const OpRequest>(&request,1),options).ok && fixture.image()==before,"late journal refusal rolls back source bytes and every curve-range row");
    const auto edit=automation::movePoints(10,2,nullptr,Payload::array({point(UINT64_MAX-2,350,.1)}));check(!OpJournal(*fixture.store).commit(std::span<const OpRequest>(&edit,1),options).ok && fixture.image()==before,"late point-edit refusal rolls back ordinal rewrites and unknown range cells");
}
void adversarial(){
    {Fixture fixture;fixture.store->db().exec("UPDATE automation_data SET data=X'00' WHERE id=11");const auto before=fixture.image();check(!OpJournal(*fixture.store).commit(automation::clearLane(2)).ok && fixture.image()==before,"a malformed later clip stream rolls back an earlier clear including its deleted curve rows");}

    for(int defect=0;defect<5;++defect){
        Fixture fixture;const auto before=fixture.image();const auto data=*detail::captureRow(fixture.store->db(),"automation_data","id",10);auto ranges=fixture.ranges();
        if(defect==0)ranges[0]["data_id"]=12;
        if(defect==1)ranges[0]["clip_id"]=1;
        if(defect==2)ranges[0]["id"]=120;
        if(defect==3)ranges[0].erase("id");
        OpRequest request;request.opType="automation.restoreData";request.payload={{"id",10},{"lane",2},{"clip",nullptr},{"state",{{"present",true},{"bytes",data.at("data")},{"ranges",ranges}}}};
        if(defect==4)request.payload["state"]["present"]=false;
        check(!OpJournal(*fixture.store).commit(request).ok && fixture.image()==before,"foreign curve address or conflicting range identity refuses atomically without touching another stream");
    }
    {
        Fixture fixture;const auto before=fixture.image();fixture.store->db().exec("CREATE TEMP TRIGGER range_refusal BEFORE INSERT ON main.automation_segment_ranges WHEN NEW.data_id=10 AND NEW.record_index=1 BEGIN SELECT RAISE(ABORT,'range reinsertion refused'); END");
        const auto request=automation::movePoints(10,2,nullptr,Payload::array({point(UINT64_MAX-2,350,.1)}));
        check(!OpJournal(*fixture.store).commit(request).ok && fixture.image()==before,"failure after deleting all ranges and reinserting one rolls back every stream byte and opaque row");
    }
    {
        Fixture fixture;fixture.store->db().exec("UPDATE automation_segment_ranges SET record_index=999 WHERE id=100");const auto before=fixture.image();
        check(!OpJournal(*fixture.store).commit(automation::setCurve(10,2,nullptr,Payload::array({20}),4,0)).ok && fixture.image()==before,"point editing refuses a preexisting out-of-bounds retained record instead of attaching it to another point");
        fixture.commit(automation::clearLane(2));check(fixture.ranges().empty() && fixture.valid(),"clear repairs invalid legacy range ordinals by removing their whole addressed streams' metadata");
        check(History(*fixture.store).undo().ok && fixture.image()==before,"clear Undo preserves even legacy invalid metadata exactly rather than silently normalizing it");
    }
    {
        Fixture fixture;const auto data=*detail::captureRow(fixture.store->db(),"automation_data","id",10);OpRequest request;request.opType="automation.restoreData";request.payload={{"id",10},{"lane",2},{"clip",nullptr},{"state",{{"present",true},{"bytes",data.at("data")}}}};
        const auto before=fixture.image();fixture.commit(request);check(fixture.ranges().empty() && fixture.valid(),"historical byte-only inverse packets do not retain ordinals for a different restored stream");check(History(*fixture.store).undo().ok && fixture.image()==before,"a legacy inverse's new inverse still captures the complete current curve ranges");
    }
    {
        Fixture fixture;auto row=*detail::captureRow(fixture.store->db(),"automation_data","id",10);const auto& binary=row.at("data").get_binary();StreamReader<AutomationPoint> reader({reinterpret_cast<const std::byte*>(binary.data()),binary.size()},FourCC::Automation);if(!reader.ok())throw std::runtime_error("future fixture read");auto header=reader.header();header.version=9;header.rec_size=40;header.flags|=0x100;
        std::vector<std::uint8_t> future(sizeof header);std::memcpy(future.data(),&header,sizeof header);for(std::uint32_t index=0;index<reader.count();++index){const auto first=sizeof header+static_cast<std::size_t>(index)*sizeof(AutomationPoint);future.insert(future.end(),binary.begin()+static_cast<std::ptrdiff_t>(first),binary.begin()+static_cast<std::ptrdiff_t>(first+sizeof(AutomationPoint)));future.insert(future.end(),8,static_cast<std::uint8_t>(0xa0u+index));}future.push_back(0xff);row["data"]=Payload::binary(future);detail::restoreRow(fixture.store->db(),"automation_data",row,"id");
        const auto before=fixture.image();fixture.commit(automation::movePoints(10,2,nullptr,Payload::array({point(UINT64_MAX-2,350,.1)})));const auto current=*detail::captureRow(fixture.store->db(),"automation_data","id",10);const auto& changed=current.at("data").get_binary();check(changed.size()==future.size() && changed.back()==0xff && changed[sizeof header+32]==0xa1 && changed[sizeof header+3*40+32]==0xa0 && fixture.ranges().size()==2,"wider future AAUT record tails and trailer travel with their points while retained range ordinals follow");check(History(*fixture.store).undo().ok && fixture.image()==before,"future AAUT bytes and complete unknown curve columns survive exact Undo");
    }
}
void audibleCurves(){
    for(const auto& request:std::array<OpRequest,5>{automation::deleteLane(2),automation::clearLane(2),automation::setLaneEnabled(2,false),automation::setLaneVisible(2,true),automation::setDefaultValue(2,.25)}){
        Fixture fixture;auto row=*detail::captureRow(fixture.store->db(),"automation_data","id",10);auto binary=row.at("data").get_binary();for(std::size_t index=0;index<4;++index){AutomationPoint value{};const auto offset=sizeof(StreamHeader)+index*sizeof(AutomationPoint);std::memcpy(&value,binary.data()+offset,sizeof value);value.time*=24024;std::memcpy(binary.data()+offset,&value,sizeof value);}row["data"]=Payload::binary(std::move(binary));detail::restoreRow(fixture.store->db(),"automation_data",row,"id");
        const auto evaluate=[&]{engine::AutomationProgram program(rows::readModel(*fixture.store),48000);const auto* lane=program.lane(2);if(!lane || !program.problems().empty())throw std::runtime_error("native retained curve compiler");std::array<double,12> values{};for(std::size_t index=0;index<values.size();++index)values[index]=engine::valueAt(*lane,static_cast<std::int64_t>(index)*2500);return values;};
        const auto before=evaluate();fixture.commit(request);History history(*fixture.store);check(history.undo().ok && evaluate()==before && history.redo().ok && history.undo().ok && evaluate()==before,"actual native nonlinear automation values survive each lane operation's Undo/Redo/Undo");
    }
}

}
int main(){try{lanes();edits();atomic();adversarial();audibleCurves();}catch(const std::exception& e){++failures;std::printf("EXCEPTION: %s\n",e.what());}std::printf("%d checks, %d failures\n",checks,failures);return failures?1:0;}
