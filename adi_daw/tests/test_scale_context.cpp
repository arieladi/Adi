// SPDX-License-Identifier: AGPL-3.0-or-later
#include "adi/engine/scale_context.hpp"
#include "adi/audio/io_audit.hpp"
#include "adi/store.hpp"
#include "adi/digest.hpp"
#include "temp_directory.hpp"
#include <SQLiteCpp/SQLiteCpp.h>
#include <sqlite3.h>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <new>
#include <stdexcept>
namespace {std::atomic<unsigned> allocations{0};}
void* operator new(std::size_t n){if(adi::audio::onAudioThread)++allocations;if(void* p=std::malloc(n?n:1))return p;throw std::bad_alloc();}
void* operator new[](std::size_t n){return operator new(n);}
void operator delete(void* p) noexcept{std::free(p);}
void operator delete[](void* p) noexcept{std::free(p);}
void operator delete(void* p,std::size_t) noexcept{std::free(p);}
void operator delete[](void* p,std::size_t) noexcept{std::free(p);}
namespace {
using namespace adi;using namespace adi::engine;
int checks=0,failures=0;
void check(bool ok,const char* label){++checks;if(!ok){++failures;std::printf("FAIL: %s\n",label);}}
TuningDefinition edo(std::int64_t id,int steps) {
    TuningDefinition result;result.id=id;result.name=std::to_string(steps)+"-EDO";result.source="edo";result.description="Authored tuning";
    for(int index=0;index<steps;++index)result.degrees.push_back({index,1200.0*index/steps,index==7?"E half-flat":index==0?"Unison":""});
    return result;
}
ScaleContext compile(const ClipScale& scale={},const std::optional<TuningDefinition>& tuning={}) {
    std::string error;auto result=compileScaleContext(scale,tuning,error);if(!result)throw std::runtime_error(error);return *result;
}
void basic() {
    const auto major=compile();check(major.degrees().size()==12 && major.periodCents()==1200 && major.tuningName()=="12-TET" && major.tuningSource()=="implicit","default context is an immutable implicit 12-TET major scale");
    check(major.contains(6000,6000) && !major.contains(6100,6000) && major.contains(6200,6000) && major.includesDegree(11) && !major.includesDegree(1),"literal pitch membership follows the relative twelve-bit mask");
    const auto all=major.grid(6000,7199,6000),folded=major.grid(6000,7199,6000,true);
    check(all.error.empty() && all.rows.size()==12 && folded.error.empty() && folded.rows.size()==7 && folded.rows[0].name=="C" && folded.rows[1].name=="D","ruler and scale Fold share the same degree membership and English pitch-class names");
    bool ids=true;const std::array<std::int64_t,7> expected{0,2,4,5,7,9,11};for(std::size_t i=0;i<expected.size();++i)ids=ids && folded.rows[i].pitch.degree==expected[i];
    check(ids && all.rows[0].midi && all.rows[0].midi->key==60 && all.rows[0].midi->tuningCents==0,"folded rows preserve degree identity while native middle C is key sixty");
    const auto snap=major.nearest(6100,6000,true),literal=major.match(6100,6000);
    check(snap && snap->degree==0 && snap->cents==6000 && snap->distanceCents==100 && literal && literal->degree==1 && !literal->inScale,"nearest scale member has an explicit lower-pitch tie rule; literal highlighting does not quantize notes");
    const auto below=major.match(-100,0);check(below && below->degree==11 && below->period==-1 && below->cents==-100,"negative pitch periods wrap without losing degree identity");
    check(major.contains(6000.00005,6000) && !major.contains(6000.1,6000) && !major.match(6050,6000,0),"membership uses the caller's bounded cent tolerance");
    ClipScale d;d.root=2;const auto transposed=compile(d);
    check(transposed.contains(6400,6200) && !transposed.contains(6500,6200) && transposed.contains(6600,6200) && transposed.degrees()[0].name=="D","clip root metadata transposes relative membership and fallback names");
    ClipScale empty;empty.mask=0;const auto none=compile(empty);
    check(none.grid(6000,7200,6000,true).rows.empty() && !none.nearest(6000,6000,true) && !none.contains(6000,6000) && none.degrees().size()==12,"an explicitly empty scale stays empty without inventing a fallback membership");
}
void microtonal() {
    ClipScale scale;scale.tuningId=4;scale.mask=0;scale.id="rast";scale.degrees={0,4,7,10,14,18,21};auto definition=edo(4,24);const auto rast=compile(scale,definition);
    check(rast.degrees().size()==24 && rast.includesDegree(7) && !rast.includesDegree(8) && rast.contains(6350,6000) && !rast.contains(6400,6000),"Rast on C uses its 24-TET degree membership rather than the ignored legacy mask");
    const auto folded=rast.grid(6000,7199,6000,true);
    check(folded.error.empty() && folded.rows.size()==7 && folded.rows[2].name=="E half-flat" && folded.rows[1].name=="4","authored microtonal degree names survive; unnamed rows use their degree ID");
    check(folded.rows[2].midi && folded.rows[2].midi->key==64 && folded.rows[2].midi->tuningCents==-50 && folded.rows[2].midi->errorCents==0,"E half-flat offers an exact native E-minus-fifty-cent representation without replacing its authored name");
    check(rast.tuningSource()=="edo" && rast.tuningDescription()=="Authored tuning" && rast.scale()->tuningId==4,"immutable context retains tuning provenance and the clip's explicit reference");
    definition.degrees[7].name="Changed";scale.degrees.clear();check(rast.degrees()[7].name=="E half-flat" && rast.includesDegree(7),"held contexts do not change when owner source data is edited");
    ClipScale fifty;fifty.tuningId=5;fifty.degrees={0,9,18,22,31,40,49};fifty.mask=4095;const auto context=compile(fifty,edo(5,53));
    const auto ideal=6000+1200.0*9/53;const auto native=encodeMidiPitch(ideal);
    check(context.contains(ideal,6000) && !context.contains(6200,6000) && native && native->errorCents<=0.0001 && context.contains(static_cast<double>(native->key)*100+native->tuningCents,6000),"53-EDO membership follows real cent offsets and reports bounded native float error");
    check(!encodeMidiPitch(ideal,0) && encodeMidiPitch(-1200,0) && encodeMidiPitch(13900,0) && !encodeMidiPitch(14000) && !encodeMidiPitch(6000,-1),"native pitch encoding names its precision and MIDI-range limits");
    TuningDefinition tritave;tritave.id=8;tritave.name="Tritave";tritave.source="scala";tritave.periodCents=1901.955;tritave.degrees={{0,0,"Unison"},{3,701.955,"Fifth"},{7,1200,"Octave"}};
    ClipScale odd;odd.tuningId=8;odd.degrees={0,3};const auto nonOctave=compile(odd,tritave);const auto rows=nonOctave.grid(6000,7901.96,6000,true);
    check(nonOctave.contains(6000+1901.955,6000) && !nonOctave.contains(7200,6000) && rows.rows.size()==3 && rows.rows[1].pitch.degree==3 && rows.rows[2].pitch.period==1,"non-octave periods and sparse authored degree IDs are preserved rather than coerced to octaves");
    check(nonOctave.contains(6100,6100) && !nonOctave.contains(6100,6000),"consumer supplies the absolute tonic anchor explicitly");
}
void boundsAndRealtime() {
    const auto context=compile();check(!context.grid(0,12700,0,false,127).error.empty() && context.grid(0,12700,0,false,127).rows.empty(),"ruler budget failure returns no partial grid");
    check(!context.grid(100,0,0).error.empty() && !context.grid(0,1,0,false,0).error.empty() && !context.nearest(1e300,0) && !context.match(0,0,-1),"invalid ranges, budgets, pitches and tolerances are bounded");
    TuningDefinition tiny;tiny.id=1;tiny.name="Tiny";tiny.source="scala";tiny.periodCents=1e-20;tiny.degrees={{0,0,""}};ClipScale small;small.tuningId=1;small.degrees={0};const auto dense=compile(small,tiny);
    check(!dense.nearest(1,0) && !dense.grid(0,100,0).error.empty(),"dense periods refuse unsupported repeat counts before integer conversion or unbounded enumeration");
    bool good=true;
    {audio::CallbackScope callback;for(int pass=0;pass<1000;++pass)for(int key=0;key<128;++key) {
        const auto pitch=static_cast<double>(key)*100;const auto found=context.match(pitch,6000);good=good && found.has_value();
        const auto nearest=context.nearest(pitch+30,6000,true);good=good && nearest && nearest->inScale;
    }}
    check(good,"realtime membership/matching/nearest queries allocate and mutate nothing");
    std::string error;ClipScale source;const std::optional<TuningDefinition> none;
    bool refused=false;{audio::CallbackScope callback;refused=!compileScaleContext(source,none,error) && error=="callback" && context.grid(0,100,0).error=="callback";}
    check(refused,"owner compilation and grid projection reject callback use before allocation");
}
void malformed() {
    ClipScale valid;valid.tuningId=4;valid.degrees={0,7};const auto definition=edo(4,24);std::string error;int rejected=0;
    for(int which=0;which<14;++which) {
        auto scale=valid;std::optional<TuningDefinition> tuning=definition;
        if(which==0)scale.root=12;
        if(which==1)tuning.reset();
        if(which==2)tuning->id=5;
        if(which==3)tuning->source="unknown";
        if(which==4)tuning->periodCents=0;
        if(which==5)tuning->periodCents=std::numeric_limits<double>::infinity();
        if(which==6)tuning->degrees.clear();
        if(which==7)tuning->degrees[0].id=1;
        if(which==8)tuning->degrees[0].cents=1;
        if(which==9)tuning->degrees[7].cents=250;
        if(which==10)tuning->degrees.back().cents=1200;
        if(which==11)scale.degrees.push_back(99);
        if(which==12)scale.degrees.push_back(7);
        if(which==13)tuning->degrees[7].id=6;
        if(!compileScaleContext(scale,tuning,error) && !error.empty())++rejected;
    }
    check(rejected==14,"bad roots, missing definitions, periods, row order/unison and membership references refuse whole contexts");
    ClipScale fallback;fallback.mask=4096;check(!compileScaleContext(fallback,{},error),"invalid implicit twelve-bit mask is not silently truncated");
    valid.mask=std::numeric_limits<std::int64_t>::min();check(compileScaleContext(valid,definition,error).has_value(),"a tuning-aware reader ignores the legacy compatibility mask as ADR-0178 requires");
}
struct Fixture {
    test::TempDirectory temp{"scale","context"};std::unique_ptr<Store> store;
    Fixture(){StoreError error{};store=Store::createEmpty(temp.path()/"a.adi",error);if(!store)throw std::runtime_error("store");
        store->db().exec("INSERT INTO tracks(id,kind,name) VALUES(1,'midi','Scale');INSERT INTO clips(id,track_id,kind,pos_ticks,length_ticks) VALUES(10,1,'midi',0,5765760),(11,1,'midi',5765760,5765760);"
            "INSERT INTO key_map(id,pos_ticks,root,scale_id,scale_mask) VALUES(1,0,0,'rast',4095),(2,5765760,2,'custom',0)");
        for(const auto& tuning:{edo(4,24),edo(5,53)}) {
            SQLite::Statement system(store->db(),"INSERT INTO tuning_systems(id,name,source,period_cents,description) VALUES(?,?,?,?,?)");system.bind(1,tuning.id);system.bind(2,tuning.name);system.bind(3,tuning.source);system.bind(4,tuning.periodCents);system.bind(5,tuning.description);system.exec();
            SQLite::Statement degree(store->db(),"INSERT INTO tuning_degrees(tuning_id,degree,cents,name) VALUES(?,?,?,?)");
            for(const auto& row:tuning.degrees){degree.reset();degree.bind(1,tuning.id);degree.bind(2,row.id);degree.bind(3,row.cents);degree.bind(4,row.name);degree.exec();}
        }
        store->db().exec("INSERT INTO key_map_tunings VALUES(1,4),(2,5);INSERT INTO key_map_degrees VALUES(1,0),(1,4),(1,7),(1,10),(1,14),(1,18),(1,21),(2,0),(2,9),(2,18)");
    }
};
void ownerReads() {
    Fixture f;const auto before=digestProject(*f.store).text;const auto first=readClipScaleContext(*f.store,10),second=readClipScaleContext(*f.store,11);
    check(first.context && second.context && first.context->degrees().size()==24 && second.context->degrees().size()==53 && second.context->scale()->root==2 &&
        first.context->contains(6350,6000) && second.context->includesDegree(9),"actual per-key tuning changes resolve at each clip's arrangement start");
    check(digestProject(*f.store).text==before,"owner scale reads reserve no identities and change no project/history rows");
    {SQLite::Transaction transaction(f.store->db(),SQLite::TransactionBehavior::DEFERRED);const auto nested=readClipScaleContext(*f.store,10);
        check(nested.context && sqlite3_get_autocommit(f.store->db().getHandle())==0,"scale and reused ClipDetails readers participate in one caller-owned read snapshot");}
    f.store->db().exec("UPDATE tuning_degrees SET name='New label' WHERE tuning_id=4 AND degree=7;DELETE FROM key_map_degrees WHERE key_map_id=1");
    const auto changed=readClipScaleContext(*f.store,10);
    check(first.context && first.context->degrees()[7].name=="E half-flat" && first.context->includesDegree(7) && changed.context && changed.context->degrees()[7].name=="New label" &&
        changed.context->grid(6000,7199,6000,true).rows.empty(),"held immutable contexts survive later owner tuning/name/membership edits");
    f.store->db().exec("CREATE TABLE clip_properties(clip_id INTEGER PRIMARY KEY,end_marker_ticks INTEGER,signature_numerator INTEGER,signature_denominator INTEGER,scale_root INTEGER,scale_id TEXT,scale_mask INTEGER);"
        "INSERT INTO clip_properties(clip_id,scale_root,scale_id,scale_mask) VALUES(10,5,'major',2741)");
    const auto local=readClipScaleContext(*f.store,10);
    check(local.context && local.context->degrees().size()==12 && local.context->scale()->local && local.context->scale()->root==5 && !local.context->scale()->tuningId && local.context->contains(6700,6500),"N33-compatible local Live scale overrides inherited microtonal tuning as N32 already defines");
    check(!readClipScaleContext(*f.store,999).context,"missing clip is an explicit owner-read error");
    unsigned queries=0;sqlite3_trace_v2(f.store->db().getHandle(),SQLITE_TRACE_STMT,[](unsigned,void* context,void*,void*)->int{++*static_cast<unsigned*>(context);return 0;},&queries);
    bool refused=false;{audio::CallbackScope callback;const auto read=readClipScaleContext(*f.store,10);refused=!read.context && read.error=="callback";}
    sqlite3_trace_v2(f.store->db().getHandle(),0,nullptr,nullptr);check(refused && queries==0,"scale context owner read refuses callback before database access");
}
void noteIndependenceAndClockErrors() {
    Fixture f;f.store->db().exec("INSERT INTO event_streams(clip_id,stream_kind,data) VALUES(10,'notes',X'0102')");
    const auto details=readClipDetails(*f.store,10);check(details.clip && !details.clip->notesProblem.empty(),"malformed note stream fixture is actually diagnosed by the normal note reader");
    unsigned noteReads=0;sqlite3_trace_v2(f.store->db().getHandle(),SQLITE_TRACE_STMT,[](unsigned,void* context,void*,void* sql)->int {
        const auto* text=static_cast<const char*>(sql);if(text && std::string_view(text).find("event_streams")!=std::string_view::npos)++*static_cast<unsigned*>(context);return 0;
    },&noteReads);
    const auto scale=readClipScaleContext(*f.store,10);sqlite3_trace_v2(f.store->db().getHandle(),0,nullptr,nullptr);
    check(scale.context && noteReads==0,"scale context reads no note blob and remains usable when unrelated note content is unreadable");
    f.store->db().exec("UPDATE tuning_degrees SET cents=450 WHERE tuning_id=4 AND degree=7");const auto broken=readClipScaleContext(*f.store,10);
    check(!broken.context && !broken.error.empty(),"malformed relational tuning never falls back silently to a twelve-bit scale");
    Fixture linear;linear.store->db().exec("INSERT INTO clips(id,track_id,kind,time_base,pos_ns,length_ns) VALUES(12,1,'midi',1,500000000,500000000)");
    const auto placed=readClipScaleContext(*linear.store,12);check(placed.context && placed.context->scale()->tuningId==5,"linear placement inherits its key through N32's existing step-tempo converter");
    linear.store->db().exec("INSERT INTO tempo_map(id,pos_ticks,bpm,curve) VALUES(1,0,120,1)");const auto ramp=readClipScaleContext(*linear.store,12);
    check(!ramp.context && !ramp.error.empty(),"ambiguous inherited linear/ramp context is named rather than guessed");
}
}
int main(){try{basic();microtonal();boundsAndRealtime();malformed();ownerReads();noteIndependenceAndClockErrors();check(allocations==0 && audio::callbackFileIo==0,"context queries and callback refusal allocate nothing and perform no callback I/O");}
catch(const std::exception& e){std::printf("EXCEPTION: %s\n",e.what());check(false,"fixture must complete");}
std::printf("scale_context: %d checks, %d failures\n",checks,failures);return failures?1:0;}
