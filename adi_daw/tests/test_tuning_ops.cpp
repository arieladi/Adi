// SPDX-License-Identifier: AGPL-3.0-or-later
#include "adi/tuning_ops.hpp"
#include "adi/audio/io_audit.hpp"
#include "adi/store.hpp"
#include "adi/history.hpp"
#include "adi/digest.hpp"
#include "adi/textproj_store.hpp"
#include "temp_directory.hpp"
#include <SQLiteCpp/SQLiteCpp.h>
#include <sqlite3.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>
#include <stdexcept>
namespace {
using namespace adi;
int checks=0,failures=0;
void check(bool ok,const char* label){++checks;if(!ok){++failures;std::printf("FAIL: %s\n",label);}}
OpRequest request(const char* type,Payload payload){OpRequest result;result.opType=type;result.payload=std::move(payload);return result;}
engine::TuningDefinition edo(std::int64_t id,int steps) {
    engine::TuningDefinition result;result.id=id;result.name="Authored\nScale";result.source="edo";result.description="Description: \"root\"";
    for(int degree=0;degree<steps;++degree)result.degrees.push_back({degree,1200.0*degree/steps,degree==7?"E half-flat":""});
    return result;
}
engine::ClipScale rast(std::int64_t tuning=4){engine::ClipScale result;result.root=0;result.id="rast";result.mask=4095;result.tuningId=tuning;result.degrees={0,4,7,10,14,18,21};return result;}
OpRequest create(const engine::TuningDefinition& definition){return request("tuning.create",{{"id",definition.id},{"definition",tunings::definitionPayload(definition)}});}
OpRequest key(std::int64_t id,std::int64_t pos,const engine::ClipScale& scale){return request("project.insertKeyEvent",{{"id",id},{"pos",pos},{"scale",tunings::scalePayload(scale)}});}
OpRequest set(std::int64_t id,const engine::ClipScale& scale){return request("project.setKeyScale",{{"id",id},{"scale",tunings::scalePayload(scale)}});}
struct Fixture {
    test::TempDirectory temp{"tuning","ops"};std::unique_ptr<Store> store;
    Fixture(){StoreError error{};store=Store::createEmpty(temp.path()/"a.adi",error);if(!store)throw std::runtime_error("store");
        store->db().exec("INSERT INTO tracks(id,kind,name) VALUES(1,'midi','Scale');INSERT INTO clips(id,track_id,kind,pos_ticks,length_ticks) VALUES(10,1,'midi',0,5765760),(11,1,'midi',5765760,5765760)");}
    void commit(const OpRequest& op){const auto result=OpJournal(*store).commit(op);if(!result.ok)throw std::runtime_error(op.opType+": "+result.error);}
    void undo(){const auto result=History(*store).undo();if(!result.ok)throw std::runtime_error(result.error);}
    void redo(){const auto result=History(*store).redo();if(!result.ok)throw std::runtime_error(result.error);}
    std::string digest(){return digestProject(*store).text;}
    std::string projection(){return textproj::projectStore(*store).text;}
};
void lifecycle() {
    Fixture f;const auto original=f.digest();const auto definition=edo(4,24);const auto scale=rast();
    const auto result=OpJournal(*f.store).commit(std::vector<OpRequest>{create(definition),key(20,0,scale),key(21,5765760,engine::ClipScale{})});
    check(result.ok && History(*f.store).nextUndo()->opCount==3,"tuning and two key changes commit as one native action");
    const auto complete=f.digest();const auto context=engine::readClipScaleContext(*f.store,10),second=engine::readClipScaleContext(*f.store,11);
    check(context.context && context.context->contains(6350,6000) && !context.context->contains(6400,6000) && second.context && second.context->degrees().size()==12,"native key edits drive the actual inherited clip context across key changes");
    f.undo();check(f.digest()==original,"one undo removes the new key memberships and tuning rows exactly");f.redo();check(f.digest()==complete,"one redo restores all relational scale rows exactly");
    const auto held=*context.context;auto changed=definition;changed.name="Renamed";changed.degrees[7].name="Changed name";changed.degrees[7].cents=351.2345678901234;
    f.commit(request("tuning.replace",{{"id",4},{"definition",tunings::definitionPayload(changed)}}));
    const auto updated=engine::readClipScaleContext(*f.store,10);check(updated.context && updated.context->degrees()[7].cents==changed.degrees[7].cents && updated.context->degrees()[7].name=="Changed name" && held.degrees()[7].name=="E half-flat","replacement preserves full double cents while held contexts remain immutable");
    const auto replaced=f.digest();f.undo();check(f.digest()==complete,"replacement captures every original tuning row for undo");f.redo();check(f.digest()==replaced,"replacement redo reproduces authored values");
    auto implicit=engine::ClipScale{};implicit.root=5;f.commit(set(20,implicit));const auto detached=engine::readClipScaleContext(*f.store,10);
    check(detached.context && detached.context->degrees().size()==12 && detached.context->scale()->root==5 && !detached.context->scale()->tuningId,"assigning implicit scale removes tuning and degree rows without retuning notes");
    f.undo();check(f.digest()==replaced,"scale assignment undo restores tuning reference and exact selected degrees");f.redo();f.undo();
    f.commit(request("project.removeKeyEvent",{{"id",20}}));const auto removed=f.digest();
    check(!engine::readClipScaleContext(*f.store,10).context->scale()->tuningId,"removing a key restores implicit default inheritance");f.undo();check(f.digest()==replaced,"key removal inverse captures explicit identity, position, mask and membership");f.redo();check(f.digest()==removed,"key removal redo restores absence");
    f.commit(request("tuning.delete",{{"id",4}}));const auto deleted=f.digest();f.undo();check(f.digest()==removed,"unused tuning deletion restores its full definition on undo");f.redo();check(f.digest()==deleted,"unused tuning deletion redo restores absence");
    StoreError error{};const auto path=f.store->path();f.store.reset();f.store=Store::open(path,error);check(f.store && f.digest()==deleted,"native scale edits survive project reopen");f.undo();check(f.digest()==removed,"captured tuning inverse survives reopen");
}
void refusalsAndRollback() {
    Fixture f;f.commit(create(edo(4,24)));f.commit(key(20,0,rast()));const auto before=f.digest();const auto head=History(*f.store).head();
    check(!OpJournal(*f.store).commit(request("tuning.delete",{{"id",4}})).ok && f.digest()==before && History(*f.store).head()==head,"deleting a referenced tuning refuses without project or history changes");
    auto invalid=edo(4,12);const auto rejectedReplacement=OpJournal(*f.store).commit(request("tuning.replace",{{"id",4},{"definition",tunings::definitionPayload(invalid)}}));
    check(!rejectedReplacement.ok && f.digest()==before,"replacement cannot invalidate an existing key's selected degrees");
    if(rejectedReplacement.ok)f.undo(); // keep negative cases independent under defect plants
    auto same=key(21,0,rast());check(!OpJournal(*f.store).commit(same).ok && f.digest()==before,"duplicate key position never upserts or replaces an existing key");
    const auto transaction=OpJournal(*f.store).commit(std::vector<OpRequest>{create(edo(5,53)),key(21,0,rast(5))});
    check(!transaction.ok && f.digest()==before && History(*f.store).head()==head,"late failure rolls back earlier tuning creation and the entire action log");
    f.store->db().exec("CREATE TEMP TRIGGER refuse_degree BEFORE INSERT ON tuning_degrees WHEN NEW.degree=7 BEGIN SELECT RAISE(ABORT,'late tuning refusal'); END");
    auto replacement=edo(4,24);replacement.description="Updated";
    check(!OpJournal(*f.store).commit(request("tuning.replace",{{"id",4},{"definition",tunings::definitionPayload(replacement)}})).ok && f.digest()==before,"SQL failure after clearing degrees restores the original tuning and history");
    check(!OpJournal(*f.store).commit(create(edo(5,53))).ok && f.digest()==before,"SQL failure partway through new degrees leaves no partial definition");
    f.store->db().exec("DROP TRIGGER refuse_degree");
    auto empty=rast();empty.degrees.clear();f.commit(set(20,empty));check(engine::readClipScaleContext(*f.store,10).context->grid(6000,7200,6000,true).rows.empty(),"an explicit tuned key may intentionally have no selected scale degrees");
    auto huge=key(std::numeric_limits<std::int64_t>::max(),9007199254740993LL,engine::ClipScale{});f.commit(huge);
    SQLite::Statement high(f.store->db(),"SELECT pos_ticks FROM key_map WHERE id=9223372036854775807");check(high.executeStep() && high.getColumn(0).getInt64()==9007199254740993LL,"native key identities and tick positions retain signed 64-bit precision beyond double");
    std::vector<OpRequest> bad;
    for(int index=0;index<15;++index) {
        auto op=create(edo(8,24));auto& d=op.payload["definition"];
        if(index==0)op.payload["id"]=std::numeric_limits<std::uint64_t>::max();
        if(index==1)op.payload["id"]=0;
        if(index==2)d["period"]=0;
        if(index==3)d["source"]="kbm";
        if(index==4)d["degrees"]=Payload::array();
        if(index==5)d["degrees"][0]["cents"]=1.0;
        if(index==6)d["degrees"][7]["id"]=6;
        if(index==7)d["degrees"][7]["cents"]=299.0;
        if(index==8)d["degrees"][23]["cents"]=1200.0;
        if(index==9)d["degrees"][7]["id"]=7.5;
        if(index==10)d["period"]=std::numeric_limits<double>::infinity();
        if(index==11)d["extra"]=1;
        if(index==12)d["degrees"][1]["extra"]=1;
        if(index==13)d["name"]=std::string(65537,'x');
        if(index==14)d["description"]=false;
        bad.push_back(std::move(op));
    }
    for(int index=0;index<8;++index) {
        auto op=key(30,5765760,rast());auto& scale=op.payload["scale"];
        if(index==0)scale["degrees"].push_back(99);
        if(index==1)scale["degrees"].push_back(7);
        if(index==2)scale["tuning"]=88;
        if(index==3)scale["root"]=12;
        if(index==4)scale["mask"]=4096;
        if(index==5)op.payload["pos"]=-1;
        if(index==6){scale.erase("tuning");}
        if(index==7)scale["degrees"][1]=-1;
        bad.push_back(std::move(op));
    }
    const auto stable=f.digest();int rejected=0;for(const auto& op:bad)if(!OpJournal(*f.store).commit(op).ok && f.digest()==stable)++rejected;
    check(rejected==23,"malformed definitions, nested fields, precision domains, tuning references and memberships refuse without writes");
    unsigned queries=0;sqlite3_trace_v2(f.store->db().getHandle(),SQLITE_TRACE_STMT,[](unsigned,void* count,void*,void*)->int{++*static_cast<unsigned*>(count);return 0;},&queries);
    OpContext context{*f.store,f.store->db()};std::string error;Payload inverse;bool refused=true;
    // Handler-level guard: the journal itself is strictly an owner API.
    for(const auto& descriptor:tunings::ops()) {audio::CallbackScope callback;refused=refused && !descriptor.apply(context,Payload{},error) && error=="callback" && !descriptor.buildInverse(context,Payload{},inverse,error) && error=="callback";}
    sqlite3_trace_v2(f.store->db().getHandle(),0,nullptr,nullptr);check(refused && queries==0,"all new handlers and inverse capture refuse callbacks before SQL");
}
void sparseNative() {
    Fixture f;engine::TuningDefinition definition;definition.id=4;definition.name="Tritave";definition.source="scala";definition.periodCents=1901.955;
    definition.degrees={{0,0,"Unison"},{3,701.955,"Fifth"},{7,1200,"Octave"}};auto scale=rast();scale.degrees={0,3};
    const auto before=f.digest();const auto committed=OpJournal(*f.store).commit(std::vector<OpRequest>{create(definition),key(20,0,scale)});
    if(!committed.ok)throw std::runtime_error(committed.error);
    const auto context=engine::readClipScaleContext(*f.store,10);
    check(context.context && context.context->periodCents()==1901.955 && context.context->degrees()[1].id==3 && context.context->contains(7901.955,6000) && !context.context->contains(7200,6000),"native Scala provenance, sparse coordinates and non-octave period reach the real clip context");
    check(f.projection().find("period 1901.955")!=std::string::npos && f.projection().find("degree 3 cents 701.955 name Fifth")!=std::string::npos,"non-octave definition and sparse authored coordinates project without coercion");
    f.undo();check(f.digest()==before,"one native action undoes a sparse non-octave tuning and its key completely");
}
void projectionAndReplay() {
    Fixture source;const auto definition=edo(4,24);source.commit(create(definition));source.commit(create(edo(5,53)));source.commit(key(20,0,rast()));
    auto other=rast(5);other.root=2;other.degrees={0,9,18,22,31,40,49};source.commit(key(21,5765760,other));
    const auto both=rows::readModel(*source.store);auto reordered=both;
    std::reverse(reordered.tunings.begin(),reordered.tunings.end());std::reverse(reordered.tuningDegrees.begin(),reordered.tuningDegrees.end());
    std::reverse(reordered.keyEvents.begin(),reordered.keyEvents.end());std::reverse(reordered.keyTunings.begin(),reordered.keyTunings.end());
    for(auto& value:reordered.tunings)value.id=100-value.id;
    for(auto& value:reordered.tuningDegrees)value.tuningId=100-value.tuningId;
    for(auto& value:reordered.keyTunings)value.tuningId=100-value.tuningId;
    check(textproj::project(textproj::buildTree(both)).text==textproj::project(textproj::buildTree(reordered)).text,"same-named tuning definitions keep content-derived references across reversed identities and row ordering");
    const auto secondContext=engine::readClipScaleContext(*source.store,11);
    check(secondContext.context && secondContext.context->degrees().size()==53 && secondContext.context->includesDegree(49),"native per-key tuning assignment selects 53-EDO at the next clip boundary");
    auto updated=definition;updated.degrees[7].cents=351.2345678901234;updated.degrees[7].name="Exact\nname";source.commit(request("tuning.replace",{{"id",4},{"definition",tunings::definitionPayload(updated)}}));
    source.commit(set(21,engine::ClipScale{}));source.commit(request("project.removeKeyEvent",{{"id",21}}));source.commit(request("tuning.delete",{{"id",5}}));
    Fixture replay;replay.store->db().exec("INSERT INTO session_state(key,value) VALUES('selection','different'),('playhead','987'),('grid','1/7')");
    SQLite::Statement log(source.store->db(),"SELECT op_type,payload FROM ops ORDER BY seq");int decoded=0;
    while(log.executeStep()) {const auto data=log.getColumn(1);const auto* bytes=static_cast<const std::byte*>(data.getBlob());std::string error;const auto payload=decodeFromLog({bytes,static_cast<std::size_t>(data.getBytes())},error);
        if(!payload)throw std::runtime_error(error);
        const auto type=log.getColumn(0).getString();replay.commit(request(type.c_str(),*payload));++decoded;}
    check(decoded==8 && source.digest()==replay.digest() && source.projection()==replay.projection(),"actual binary op log replays all six handler shapes under different UI state with identical project and text projection");
    const auto text=source.projection();check(text.find("E half-flat")==std::string::npos && text.find("Exact\\nname")!=std::string::npos && text.find("351.2345678901234")!=std::string::npos && text.find("tuning \"")!=std::string::npos && text.find("degrees 0 4 7 10 14 18 21")!=std::string::npos,"projection covers author names, full-double degrees, definitions, memberships and tuning references");
    const auto model=rows::readModel(*source.store);check(model.tunings.size()==1 && model.tuningDegrees.size()==24 && model.keyEvents.size()==1 && model.keyTunings.size()==1 && model.keyDegrees.size()==7,"typed model reads all five newly writable tables");
    auto permuted=model;for(auto& row:permuted.tunings)row.id+=100;
    for(auto& row:permuted.tuningDegrees)row.tuningId+=100;
    for(auto& row:permuted.keyEvents)row.id+=200;
    for(auto& row:permuted.keyTunings){row.keyId+=200;row.tuningId+=100;}
    for(auto& row:permuted.keyDegrees)row.keyId+=200;
    std::reverse(permuted.tuningDegrees.begin(),permuted.tuningDegrees.end());std::reverse(permuted.keyDegrees.begin(),permuted.keyDegrees.end());
    check(textproj::project(textproj::buildTree(permuted)).text==text,"projection is independent of relational identities and input row ordering");
    auto missing=model;missing.tunings.clear();check(textproj::project(textproj::buildTree(missing)).text.find("!unresolved(tuning)")!=std::string::npos,"missing projection tuning target is explicit instead of a leaked database identity");
    bool covered=true;for(const auto& table:textproj::coverage())if(table.table=="key_map" || table.table.starts_with("tuning_") || table.table.starts_with("key_map_"))covered=covered && table.status==textproj::Coverage::Projected;
    check(covered,"coverage manifest no longer claims the writable tuning tables have no ops");
    History history(*replay.store);const auto final=replay.digest();bool roundtrip=true;for(int pass=0;pass<8;++pass)roundtrip=roundtrip && history.undo().ok;
    for(int pass=0;pass<8;++pass)roundtrip=roundtrip && history.redo().ok;
    check(roundtrip && replay.digest()==final,"all six native operations retain valid inverses through whole-log rewind and redo");
}
}
int main(){try{lifecycle();refusalsAndRollback();sparseNative();projectionAndReplay();}catch(const std::exception& exception){std::printf("EXCEPTION: %s\n",exception.what());check(false,"fixtures complete");}std::printf("tuning_ops: %d checks, %d failures\n",checks,failures);return failures?1:0;}
