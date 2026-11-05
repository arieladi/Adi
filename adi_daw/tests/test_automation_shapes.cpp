// SPDX-License-Identifier: AGPL-3.0-or-later
#include "temp_directory.hpp"
#include "adi/automation_shapes.hpp"
#include "adi/automation_lanes.hpp"
#include "adi/automation_edits.hpp"
#include "adi/digest.hpp"
#include "adi/history.hpp"
#include "adi/store.hpp"
#include <SQLiteCpp/SQLiteCpp.h>
#include <sqlite3.h>
#include <cmath>
#include <cstdio>
#include <limits>
#include <stdexcept>
#include <set>
#include <algorithm>
#include <string_view>
namespace {
using namespace adi;
using namespace adi::automation;
int checks=0,failures=0;
void check(bool ok,const char* label) {++checks;if(!ok){++failures;std::printf("FAIL: %s\n",label);}}
std::unique_ptr<Store> project(const std::filesystem::path& path) {
    StoreError e=StoreError::Ok;auto s=Store::createEmpty(path,e);if(!s)throw std::runtime_error("store");
    s->db().exec("INSERT INTO project(id,name) VALUES(1,'Shapes');INSERT INTO tracks(id,kind,name) VALUES(1,'audio','Track');"
        "INSERT INTO automation_lanes(id,owner_kind,owner_id,param_ref,min_value,max_value) VALUES(2,'track',1,'volume',0,1)");return s;
}
std::vector<std::byte> bytes(Store& s) {
    SQLite::Statement q(s.db(),"SELECT data FROM automation_data WHERE id=3");if(!q.executeStep())return {};
    const auto c=q.getColumn(0);const auto* b=static_cast<const std::byte*>(c.getBlob());return {b,b+c.getBytes()};
}
Payload point(std::uint64_t id,std::int64_t time,double value) {return {{"id",id},{"time",time},{"value",value},{"curve",1},{"tension",0}};}
void run() {
    ShapeSpec spec;spec.start=100;spec.end=1100;spec.stepsPerCycle=100;
    const auto sine=generateShape(spec);
    check(sine.size()==101 && sine.front().time==100 && sine.back().time==1100,"sine endpoints and bounded resolution");
    check(std::abs(sine[25].value-0.5)<1e-14 && sine[50].value==1 && sine[0].value==0,"sine quarter and half cycle");
    for(auto shape:{Shape::Sine,Shape::Triangle,Shape::Sawtooth,Shape::InverseSawtooth,Shape::Square,
        Shape::ExponentialRise,Shape::ExponentialFall,Shape::LogarithmicRise,Shape::LogarithmicFall,Shape::ADSR}) {
        spec.shape=shape;spec.cycles=2;spec.low=-60;spec.high=12;
        const auto p=generateShape(spec);bool valid=!p.empty();std::int64_t last=spec.start;
        for(const auto& v:p){valid=valid && v.time>=last && v.time<=spec.end && v.value>=spec.low && v.value<=spec.high && v.point_id==0;last=v.time;}
        check(valid,"every template has ordered finite bounded unassigned points");
    }
    spec={};spec.start=(std::int64_t{1}<<54)+1;spec.end=spec.start+300;spec.stepsPerCycle=3;spec.shape=Shape::Sine;
    const auto wide=generateShape(spec);
    check(wide[0].time==spec.start && wide[1].time==spec.start+100 && wide[2].time==spec.start+200 && wide[3].time==spec.end,"exact large int64 time division");
    spec={};spec.end=1000;spec.stepsPerCycle=10;spec.shape=Shape::Square;
    const auto square=generateShape(spec);
    check(square.size()==3 && square[0].curve==0 && square[0].value==1 && square[1].time==500 && square[1].value==0,"square uses exact hold discontinuity");
    spec.shape=Shape::Triangle;spec.stepsPerCycle=3;const auto triangle=generateShape(spec);
    check(triangle.size()==3 && triangle[1].time==500 && triangle[1].value==1,"triangle apex survives odd sample resolution");
    spec.shape=Shape::ExponentialFall;const auto ramp=generateShape(spec);
    check(ramp.size()==2 && ramp[0].curve==2 && ramp[0].tension==1 && ramp[0].value==1 && ramp[1].value==0,"ramp uses exact native curve descriptor");
    spec.shape=Shape::ADSR;const auto adsr=generateShape(spec);
    check(adsr.size()==5 && adsr[1].time==100 && adsr[1].value==1 && adsr[2].time==300 && adsr[2].value==0.7 && adsr[3].time==800 && adsr[4].value==0,"ADSR explicit stages");
    spec.attack=0;spec.decay=0;spec.release=0;const auto instant=generateShape(spec);
    check(instant[0].time==instant[1].time && instant.back().value==spec.sustain,"zero ADSR phases are explicit coincident steps");
    auto invalid=[&](ShapeSpec p){try{(void)generateShape(p);return false;}catch(const std::exception&){return true;}};
    spec={};spec.end=1000;
    auto bad=spec;bad.cycles=0;check(invalid(bad),"zero cycles refused");
    bad=spec;bad.stepsPerCycle=3000;check(invalid(bad),"unbounded resolution refused");
    bad=spec;bad.low=2;check(invalid(bad),"reversed value range refused");
    bad=spec;bad.high=std::numeric_limits<double>::infinity();check(invalid(bad),"nonfinite values refused");
    bad=spec;bad.shape=static_cast<Shape>(99);check(invalid(bad),"unknown template refused");
    bad=spec;bad.attack=9000;check(invalid(bad),"overlapping ADSR phases refused");
    bad=spec;bad.end=2;check(invalid(bad),"resolution beyond time units refused");

    test::TempDirectory temp("automation","shapes");auto s=project(temp.path()/"a.adi"),other=project(temp.path()/"b.adi");
    std::string error;const auto initial=digestProject(*s).text;
    const auto totalChanges=sqlite3_total_changes(s->db().getHandle());
    auto prepared=prepareShapeInsertion(*s,3,2,std::nullopt,spec,error);
    check(prepared && prepared->requests.size()==1 && !prepared->head && error.empty(),"prepare fresh root without writes");
    check(digestProject(*s).text==initial && bytes(*s).empty() && sqlite3_total_changes(s->db().getHandle())==totalChanges,"preparation never reserves IDs or writes rows");
    if(!prepared)return;
    check(!commitShapeInsertion(*other,*prepared).ok,"another open document refused");
    OpRequest rename;rename.opType="track.rename";rename.payload={{"id",1},{"name","Changed"}};
    check(OpJournal(*s).commit(rename).ok,"concurrent committed edit");
    const auto stale=commitShapeInsertion(*s,*prepared);
    check(!stale.ok && stale.stale && bytes(*s).empty(),"NULL captured head remains a real stale guard");
    prepared=prepareShapeInsertion(*s,3,2,std::nullopt,spec,error);
    check(prepared && commitShapeInsertion(*s,*prepared).ok,"fresh prepared shape commits");
    const auto inserted=bytes(*s);History history(*s);
    check(history.undo().ok && bytes(*s).empty(),"one undo restores original absence");
    check(history.redo().ok && bytes(*s)==inserted,"shape redo exact bytes");
    check(history.undo().ok,"return to empty slot");
    const auto outside=Payload::array({point(UINT64_MAX,10,0.1),point(1,500,0.2),point(7,2000,0.3)});
    check(OpJournal(*s).commit(writePoints(3,2,nullptr,outside)).ok,"existing outside and selected points");
    const auto before=bytes(*s);spec.start=100;spec.end=1100;
    prepared=prepareShapeInsertion(*s,3,2,std::nullopt,spec,error);
    check(prepared && prepared->requests.size()==2,"selected points produce one atomic delete/write batch");
    if(!prepared)return;
    check(commitShapeInsertion(*s,*prepared).ok,"replace selected range");
    const auto after=bytes(*s);StreamReader<AutomationPoint> r(after,FourCC::Automation);
    const auto all=r.all();check(all && all->front().point_id==UINT64_MAX && all->back().point_id==7,"outside points survive full-width IDs");
    bool unique=true;if(all){std::set<std::uint64_t> ids;for(const auto& p:*all)unique=unique&&ids.insert(p.point_id).second;}
    check(unique,"generated identities avoid all existing IDs");
    check(all && std::none_of(all->begin(),all->end(),[](const AutomationPoint& p){return p.point_id==1;}),"selected original point is removed");
    check(history.undo().ok && bytes(*s)==before,"range replacement is one exact undo");
    spec.high=2;check(!prepareShapeInsertion(*s,3,2,std::nullopt,spec,error),"shape respects lane maximum");spec.high=1;
    check(!prepareShapeInsertion(*s,4,2,std::nullopt,spec,error),"wrong addressed data ID refused");
    check(!prepareShapeInsertion(*s,3,999,std::nullopt,spec,error),"missing lane refused");
    check(!prepareShapeInsertion(*s,3,2,999,spec,error),"missing clip refused");
    {
        SQLite::Transaction outer(s->db(),SQLite::TransactionBehavior::DEFERRED);
        check(prepareShapeInsertion(*s,3,2,std::nullopt,spec,error).has_value() && sqlite3_get_autocommit(s->db().getHandle())==0,"caller-owned read snapshot retained");
    }
    // A second connection commits between the head and lane/data queries.
    auto consistent=project(temp.path()/"snapshot.adi");StoreError openError=StoreError::Ok;
    auto writer=Store::open(temp.path()/"snapshot.adi",openError);
    struct Trace {Store* writer;bool fired=false,committed=false;} trace{writer.get()};
    sqlite3_trace_v2(consistent->db().getHandle(),SQLITE_TRACE_STMT,
        [](unsigned,void* context,void* statement,void*)->int {
            auto& t=*static_cast<Trace*>(context);const char* sql=sqlite3_sql(static_cast<sqlite3_stmt*>(statement));
            if(!t.fired && sql && std::string_view(sql).starts_with("SELECT min_value,max_value,time_base")) {
                t.fired=true;
                try {t.committed=OpJournal(*t.writer).commit(writePoints(3,2,nullptr,Payload::array({point(88,500,0.2)}))).ok;}
                catch(...){t.committed=false;}
            }
            return 0;
        },&trace);
    const auto snapshot=prepareShapeInsertion(*consistent,3,2,std::nullopt,spec,error);
    sqlite3_trace_v2(consistent->db().getHandle(),0,nullptr,nullptr);
    check(trace.fired && trace.committed,"concurrent fixture committed between preparation queries");
    check(snapshot && snapshot->requests.size()==1 && !snapshot->head,"shape metadata and head use one read revision");
    check(snapshot && commitShapeInsertion(*consistent,*snapshot).stale,"concurrent prepared snapshot refuses stale commit");
    const auto client=s->clientId();s->setClientId("11111111111111111111111111111111");
    check(!commitShapeInsertion(*s,*prepared).ok,"changed open-client identity refused");s->setClientId(client);
}
} // namespace
int main(){try{run();}catch(const std::exception& e){++failures;std::printf("EXCEPTION: %s\n",e.what());}std::printf("%d checks, %d failures\n",checks,failures);return failures?1:0;}
