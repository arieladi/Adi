// SPDX-License-Identifier: AGPL-3.0-or-later
#include "adi/engine/scale_timeline.hpp"
#include "adi/audio/io_audit.hpp"
#include "adi/store.hpp"
#include "adi/digest.hpp"
#include "temp_directory.hpp"
#include <SQLiteCpp/SQLiteCpp.h>
#include <sqlite3.h>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <new>
#include <stdexcept>
namespace {std::atomic<unsigned> callbackAllocations{0};}
void* operator new(std::size_t n){if(adi::audio::onAudioThread)++callbackAllocations;if(void* p=std::malloc(n?n:1))return p;throw std::bad_alloc();}
void* operator new[](std::size_t n){return operator new(n);}
void operator delete(void* p) noexcept{std::free(p);}
void operator delete[](void* p) noexcept{std::free(p);}
void operator delete(void* p,std::size_t) noexcept{std::free(p);}
void operator delete[](void* p,std::size_t) noexcept{std::free(p);}
namespace {
using namespace adi;using namespace adi::engine;
int checks=0,failures=0;
void check(bool ok,const char* label){++checks;if(!ok){++failures;std::printf("FAIL: %s\n",label);}}
TuningDefinition edo(std::int64_t id,int steps){TuningDefinition result;result.id=id;result.name=std::to_string(steps)+" EDO";result.source="edo";for(int degree=0;degree<steps;++degree)result.degrees.push_back({degree,1200.0*degree/steps,degree==7?"E half-flat":""});return result;}
ClipScale rast(std::int64_t id=4){ClipScale scale;scale.tuningId=id;scale.mask=0;scale.id="rast";scale.degrees={0,4,7,10,14,18,21};return scale;}
ScaleTimeline compiled(const std::vector<ScaleKeyChange>& changes={},const std::vector<TuningDefinition>& definitions={}){std::string error;const auto result=compileScaleTimeline(changes,definitions,error);if(!result)throw std::runtime_error(error);return *result;}
void boundariesAndLifetime() {
    const auto empty=compiled();check(empty.events().empty() && empty.atTicks(-1)->contains(6000,6000) && empty.atTicks(0)->degrees().size()==12 && !empty.atTicks(0)->includesDegree(1) && !empty.nextChangeAfterTicks(0),"empty global map has C major/12-TET through pre-roll and no invented boundary");
    auto second=rast(5);second.root=2;second.degrees={0,9,18,22,31,40,49};auto final=ClipScale{};final.root=5;
    std::vector<ScaleKeyChange> changes={{20,100,rast()},{21,9007199254740993LL,second},{22,std::numeric_limits<std::int64_t>::max(),final}};
    std::vector<TuningDefinition> definitions={edo(4,24),edo(5,53)};const auto timeline=compiled(changes,definitions);
    check(timeline.atTicks(99)->degrees().size()==12 && timeline.atTicks(100)->degrees().size()==24 && timeline.atTicks(100)->contains(6350,6000) && !timeline.atTicks(100)->contains(6400,6000),"key event applies inclusively at its exact tick rather than a previous/future context");
    check(timeline.atTicks(9007199254740992LL)->degrees().size()==24 && timeline.atTicks(9007199254740993LL)->degrees().size()==53 && timeline.atTicks(9007199254740993LL)->scale()->root==2,"adjacent ticks beyond 2^53 never narrow through a floating clock");
    check(timeline.atTicks(std::numeric_limits<std::int64_t>::min())->degrees().size()==12 && timeline.atTicks(std::numeric_limits<std::int64_t>::max())->scale()->root==5,"int64 pre-roll and terminal event lookup avoid arithmetic overflow");
    check(timeline.nextChangeAfterTicks(99)==100 && timeline.nextChangeAfterTicks(100)==9007199254740993LL && timeline.nextChangeAfterTicks(9007199254740993LL)==std::numeric_limits<std::int64_t>::max() && !timeline.nextChangeAfterTicks(std::numeric_limits<std::int64_t>::max()),"next boundary is strictly after the supplied tick with no tick-plus-one overflow");
    check(timeline.events().size()==3 && timeline.events()[0].keyId==20 && timeline.events()[1].posTicks==9007199254740993LL && timeline.events()[2].keyId==22,"event view retains explicit owner identities and exact arrangement ticks");
    definitions[0].degrees[7].name="Edited source";changes[0].scale.degrees.clear();check(timeline.atTicks(100)->degrees()[7].name=="E half-flat" && timeline.atTicks(100)->includesDegree(7),"published timeline owns its definitions and memberships independently of source edits");
    auto moved=compiled({{30,0,rast()}},{edo(4,24)});auto held=std::move(moved);check(!moved.atTicks(0) && moved.events().empty() && !moved.nextChangeAfterTicks(0) && held.atTicks(-1)->degrees().size()==12 && held.atTicks(0)->degrees().size()==24,"moved-from holder is inert while the retained generation preserves the exact zero boundary");
    bool good=true;{audio::CallbackScope callback;for(int pass=0;pass<100000;++pass) {
        good=good && timeline.atTicks(pass)->degrees().size()==(pass<100?12U:24U);
        good=good && timeline.nextChangeAfterTicks(pass)==(pass<100?100:9007199254740993LL);
        good=good && timeline.atTicks(9007199254740993LL)->includesDegree(49);
    }}
    check(good,"300,000 callback queries find immutable contexts and boundaries without allocation");
}
void validationAndBudgets() {
    std::string error;int rejected=0;
    for(int index=0;index<11;++index) {
        std::vector<ScaleKeyChange> changes={{20,100,rast()},{21,200,rast()}};std::vector<TuningDefinition> definitions={edo(4,24)};
        if(index==0)changes[0].id=0;
        if(index==1)changes[1].id=20;
        if(index==2)changes[0].posTicks=-1;
        if(index==3)changes[1].posTicks=100;
        if(index==4)changes[1].posTicks=99;
        if(index==5)changes[0].scale.local=true;
        if(index==6)definitions.clear();
        if(index==7)definitions.push_back(definitions.front());
        if(index==8)changes[1].scale.degrees.push_back(99);
        if(index==9)definitions[0].degrees[7].cents=100;
        if(index==10)changes[0].scale.root=12;
        if(!compileScaleTimeline(changes,definitions,error) && !error.empty())++rejected;
    }
    check(rejected==11,"invalid identities, positions, local flags, tuning definitions and referenced memberships refuse the whole timeline");
    std::vector<ScaleKeyChange> tooMany(65537);check(!compileScaleTimeline(tooMany,{},error) && error.find("event/tuning budget")!=std::string::npos,"event budget refuses before trying to compile invalid or excessive input rows");
    std::vector<TuningDefinition> tooManyTunings(1025);check(!compileScaleTimeline({},tooManyTunings,error) && error.find("event/tuning budget")!=std::string::npos,"definition-count budget bounds even unreferenced input metadata");
    const auto large=edo(4,65536);std::vector<ScaleKeyChange> repeated;auto scale=rast();scale.degrees.clear();
    for(int index=0;index<16;++index)repeated.push_back({index+1,index,scale});
    check(!compileScaleTimeline(repeated,std::vector<TuningDefinition>{large},error) && error.find("degree budget")!=std::string::npos,"repeated definitions count against aggregate degree budget before any context copies");
    auto text=edo(4,24);text.description.assign(1024*1024,'d');repeated.resize(9);
    check(!compileScaleTimeline(repeated,std::vector<TuningDefinition>{text},error) && error.find("text budget")!=std::string::npos,"repeated authored strings count against aggregate text budget");
    std::vector<ScaleKeyChange> maximum;maximum.reserve(65536);for(int index=0;index<65536;++index)maximum.push_back({index+1,index,{}});
    const auto max=compiled(maximum);check(max.events().size()==65536 && max.nextChangeAfterTicks(65534)==65535 && !max.nextChangeAfterTicks(65535),"largest event map compiles within degree budget and keeps its exact final boundary");
    auto unused=edo(8,12);unused.source="invalid but not referenced";const auto unaffected=compiled({{20,0,{}}},{unused});check(unaffected.atTicks(0)->degrees().size()==12,"unreferenced tuning contents do not change the active map");
    bool refused=false;{audio::CallbackScope callback;refused=!compileScaleTimeline({}, {},error) && error=="callback";}
    check(refused,"timeline owner compilation refuses callbacks before allocation");
}
struct Fixture {
    test::TempDirectory temp{"scale","timeline"};std::unique_ptr<Store> store;
    Fixture(){StoreError error{};store=Store::createEmpty(temp.path()/"a.adi",error);if(!store)throw std::runtime_error("store");
        store->db().exec("INSERT INTO tracks(id,kind,name) VALUES(1,'midi','Scale');INSERT INTO clips(id,track_id,kind,pos_ticks,length_ticks) VALUES(10,1,'midi',0,5765760);"
            "INSERT INTO key_map(id,pos_ticks,root,scale_id,scale_mask) VALUES(20,0,0,'rast',4095),(21,5765760,2,'other',0)");
        for(const auto& tuning:{edo(4,24),edo(5,53)}) {
            SQLite::Statement row(store->db(),"INSERT INTO tuning_systems(id,name,source,period_cents,description) VALUES(?,?,?,?,?)");row.bind(1,tuning.id);row.bind(2,tuning.name);row.bind(3,tuning.source);row.bind(4,tuning.periodCents);row.bind(5,tuning.description);row.exec();
            SQLite::Statement degree(store->db(),"INSERT INTO tuning_degrees(tuning_id,degree,cents,name) VALUES(?,?,?,?)");
            for(const auto& value:tuning.degrees){degree.reset();degree.bind(1,tuning.id);degree.bind(2,value.id);degree.bind(3,value.cents);degree.bind(4,value.name);degree.exec();}
        }
        store->db().exec("INSERT INTO key_map_tunings VALUES(20,4),(21,5);INSERT INTO key_map_degrees VALUES(20,0),(20,4),(20,7),(21,0),(21,9),(21,18)");
    }
};
void coherentStoreRead() {
    Fixture f;const auto before=digestProject(*f.store).text;const auto read=readScaleTimeline(*f.store);
    check(read.timeline && read.timeline->atTicks(0)->degrees().size()==24 && read.timeline->atTicks(5765760)->degrees().size()==53 && read.timeline->atTicks(5765760)->scale()->root==2,"global owner read resolves tuning changes within a continuing clip");
    check(digestProject(*f.store).text==before,"global scale read changes no project rows, identities or history");
    {SQLite::Transaction outer(f.store->db(),SQLite::TransactionBehavior::DEFERRED);const auto nested=readScaleTimeline(*f.store);check(nested.timeline && sqlite3_get_autocommit(f.store->db().getHandle())==0,"global reader preserves its caller-owned read transaction");}
    f.store->db().exec("UPDATE tuning_degrees SET name='Updated' WHERE tuning_id=4 AND degree=7;DELETE FROM key_map_degrees WHERE key_map_id=20");const auto updated=readScaleTimeline(*f.store);
    check(read.timeline && read.timeline->atTicks(0)->includesDegree(7) && read.timeline->atTicks(0)->degrees()[7].name=="E half-flat" && updated.timeline && !updated.timeline->atTicks(0)->includesDegree(7) && updated.timeline->atTicks(0)->degrees()[7].name=="Updated","held map generation remains immutable across owner SQL edits and empty membership");
    f.store->db().exec("CREATE TABLE clip_properties(clip_id INTEGER PRIMARY KEY,end_marker_ticks INTEGER,signature_numerator INTEGER,signature_denominator INTEGER,scale_root INTEGER,scale_id TEXT,scale_mask INTEGER);"
        "INSERT INTO clip_properties(clip_id,scale_root,scale_id,scale_mask) VALUES(10,5,'major',2741);INSERT INTO event_streams(clip_id,stream_kind,data) VALUES(10,'notes',X'0102')");
    unsigned noteReads=0;sqlite3_trace_v2(f.store->db().getHandle(),SQLITE_TRACE_STMT,[](unsigned,void* count,void*,void* raw)->int{const auto* text=static_cast<const char*>(raw);if(text && std::string_view(text).find("event_streams")!=std::string_view::npos)++*static_cast<unsigned*>(count);return 0;},&noteReads);
    const auto global=readScaleTimeline(*f.store);sqlite3_trace_v2(f.store->db().getHandle(),0,nullptr,nullptr);
    const auto local=readClipScaleContext(*f.store,10);check(global.timeline && global.timeline->atTicks(0)->degrees().size()==24 && local.context && local.context->degrees().size()==12 && local.context->scale()->local && noteReads==0,"clip-local override stays distinct from the global map and neither needs malformed note content");
    unsigned queries=0;sqlite3_trace_v2(f.store->db().getHandle(),SQLITE_TRACE_STMT,[](unsigned,void* count,void*,void*)->int{++*static_cast<unsigned*>(count);return 0;},&queries);
    bool refused=false;{audio::CallbackScope callback;const auto result=readScaleTimeline(*f.store);refused=!result.timeline && result.error=="callback";}
    sqlite3_trace_v2(f.store->db().getHandle(),0,nullptr,nullptr);check(refused && queries==0,"timeline Store read refuses callback before SQL or copying");
    f.store->db().exec("UPDATE tuning_degrees SET cents=100 WHERE tuning_id=5 AND degree=18");const auto broken=readScaleTimeline(*f.store);
    check(!broken.timeline && !broken.error.empty(),"malformed later tuning refuses the entire global map without a valid-looking prefix");
    check(read.timeline->atTicks(5765760)->includesDegree(18),"later corruption cannot damage a retained valid generation");
    Fixture missing;missing.store->db().exec("PRAGMA foreign_keys=OFF;DELETE FROM tuning_systems WHERE id=5");const auto absent=readScaleTimeline(*missing.store);check(!absent.timeline && !absent.error.empty(),"missing tuning reference is named without implicit twelve-TET substitution");
    Fixture orphan;orphan.store->db().exec("PRAGMA foreign_keys=OFF;DELETE FROM key_map WHERE id=20");const auto dangling=readScaleTimeline(*orphan.store);check(!dangling.timeline && !dangling.error.empty(),"orphan relational scale rows are diagnosed rather than dropped");
}
}
int main(){try{boundariesAndLifetime();validationAndBudgets();coherentStoreRead();check(callbackAllocations==0 && audio::callbackFileIo==0,"all realtime timeline queries and callback refusals allocate nothing and do no callback file I/O");}catch(const std::exception& exception){std::printf("EXCEPTION: %s\n",exception.what());check(false,"fixtures complete");}std::printf("scale_timeline: %d checks, %d failures\n",checks,failures);return failures?1:0;}
