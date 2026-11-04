// SPDX-License-Identifier: AGPL-3.0-or-later
#include "adi/engine/scala_import.hpp"
#include "adi/audio/io_audit.hpp"
#include "adi/store.hpp"
#include "adi/ops.hpp"
#include "adi/history.hpp"
#include "temp_directory.hpp"
#include <atomic>
#include <cmath>
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
ScalaImport parsed(std::string_view source){return parseScala(source,4,"Explicit display name");}
ScaleContext context(const ScalaImport& imported,std::vector<std::int64_t> members) {
    if(!imported.definition)throw std::runtime_error(imported.error);
    ClipScale scale;scale.tuningId=imported.definition->id;scale.degrees=std::move(members);std::string error;
    const auto compiled=compileScaleContext(scale,imported.definition,error);if(!compiled)throw std::runtime_error(error);return *compiled;
}
void examples() {
    const auto single=parsed("! single.scl\n! comment\n  Authored description ! is text  \n1\n2/1\n! tail\n");
    check(single.definition && single.error.empty() && single.line==0 && single.definition->degrees.size()==1 && single.definition->degrees[0].id==0 && single.definition->degrees[0].cents==0 && single.definition->periodCents==1200,"one-interval scale inserts unison and moves the sole listed interval to the period");
    check(single.definition && single.definition->name=="Explicit display name" && single.definition->source=="scala" && single.definition->description=="  Authored description ! is text  ","explicit name, Scala provenance and authored description spacing/bytes survive without inferred labels");
    const auto ratios=parsed("Pure intervals\n3\n5/4 E annotation\n3/2 ! fifth\n2\n");const auto pure=context(ratios,{0,1,2});
    check(ratios.definition && ratios.definition->degrees.size()==3 && std::abs(ratios.definition->degrees[1].cents-386.3137138648348)<1e-10 && std::abs(ratios.definition->degrees[2].cents-701.9550008653874)<1e-10,"integer and rational pitch syntax converts to double cents with no float narrowing");
    check(ratios.definition && ratios.definition->degrees[1].name.empty() && ratios.definition->degrees[2].id==2 && pure.contains(6701.9550008653874,6000),"numeric-line annotations are not invented degree names; compiled contexts retain exact degree coordinates");
    const auto tritave=parsed("Bohlen-Pierce fixture\n3\n701.955\n1200.0\n3/1\n");const auto odd=context(tritave,{0,1});
    check(tritave.definition && std::abs(tritave.definition->periodCents-1901.9550008653873)<1e-10 && odd.contains(6000+tritave.definition->periodCents,6000) && !odd.contains(7200,6000),"tritave period remains the actual final ratio instead of an implicit octave");
    const auto emptyDescription=parsed("! comment\n\n\n! between\n1 ! count annotation\n\t+1.2e3 ! decimal cents\n");
    check(emptyDescription.definition && emptyDescription.definition->description.empty() && emptyDescription.definition->periodCents==1200,"empty first non-comment description and blank/comment lines after it are accepted");
    const auto endings=parsed("\xef\xbb\xbf! BOM comment\r\nCRLF\r\n3\r\n100.0\r\n200.0\r\n1200.0");
    const auto bareCr=parsed("CRLF\r3\r100.0\r200.0\r1200.0\r");
    check(endings.definition && bareCr.definition && endings.definition->description==bareCr.definition->description && endings.definition->degrees[2].cents==bareCr.definition->degrees[2].cents,"UTF-8 BOM, CRLF, CR and a missing final newline have explicit equivalent parsing");
    const auto close=parsed("Near unison\n2\n9007199254740991/9007199254740990\n2/1\n");
    check(close.definition && close.definition->degrees[1].cents>0 && close.definition->degrees[1].cents<1e-10,"close exact-integer ratios survive through log1p rather than disappearing into rounded division");
    std::string source="Owned\n2\n123.45678901234567\n1200.0\n";auto owned=parsed(source);source.assign(source.size(),'x');
    check(owned.definition && owned.definition->description=="Owned" && owned.definition->degrees[1].cents==123.45678901234567,"returned definitions own their source text and full-double cent values");
    auto moved=std::move(owned);check(moved.definition && moved.definition->degrees[1].cents==123.45678901234567,"definition ownership survives movement beyond input lifetime");
}
void domainsAndBudgets() {
    const std::vector<std::string> bad={"","! only a comment\n","Description\n","Description\n0\n","Description\n-1\n","Description\n1.0\n2/1\n", "Description\n65537\n",
        "Description\n2\n100.0\n","Description\n1\n2/1\nextra\n","Description\n1\n1/0\n","Description\n1\n0/1\n","Description\n1\n1/1\n",
        "Description\n1\n3.0/2\n","Description\n1\n2/1/1\n","Description\n1\n-1.0\n","Description\n1\n0.0\n","Description\n1\nNaN\n","Description\n1\n1.0e999\n",
        "Description\n1\n1e3\n","Description\n2\n400.0\n300.0\n","Description\n2\n100.0\n100.0\n","Description\n1\n100.0cents\n","Description\n1\n9007199254740992\n"};
    int rejected=0;for(const auto& source:bad){const auto result=parsed(source);if(!result.definition && !result.error.empty())++rejected;}
    check(rejected==static_cast<int>(bad.size()),"23 malformed/truncated/ambiguous/order/ratio/range inputs refuse complete definitions");
    auto nul=std::string("Description\n1\n2/1\n");nul.insert(2,1,'\0');check(!parsed(nul).definition,"embedded NUL refuses before line interpretation");
    check(!parseScala("D\n1\n2/1\n",0,"Name").definition && !parseScala("D\n1\n2/1\n",-1,"Name").definition,"a parser never invents or reserves a project identity");
    check(!parseScala("D\n1\n2/1\n",4,std::string(65537,'n')).definition,"oversized explicit names are bounded before copying");
    check(!parsed(std::string(65537,'d')+"\n1\n2/1\n").definition,"physical line budget applies to descriptions");
    check(!parsed("D\n!"+std::string(65536,'c')+"\n1\n2/1\n").definition,"physical line budget also applies to skipped comments");
    const auto overBudget=parsed(std::string(8*1024*1024+1,'\n'));
    check(!overBudget.definition && overBudget.line==0 && overBudget.error.find("8 MiB")!=std::string::npos,"whole-input budget refuses before skipped-line work or tuning allocations");
    const auto tooMany=parsed("D\n65537\n");
    check(!tooMany.definition && tooMany.line==2 && tooMany.error.find("degree count")!=std::string::npos,"degree-count budget refuses before reading or allocating declared intervals");
    const auto problem=parsed("D\n2\n100.0\n50.0\n");check(!problem.definition && problem.line==4,"parse errors retain their one-based physical source line");
    const auto missing=parsed("D\n2\n100.0\n");check(!missing.definition && missing.line==3,"truncated input reports its last consumed physical line without partial degrees");
    std::string largest="Maximum\n65536\n";for(int degree=1;degree<=65536;++degree)largest+=std::to_string(degree)+".0\n";
    const auto maximum=parsed(largest);check(maximum.definition && maximum.definition->degrees.size()==65536 && maximum.definition->degrees.back().id==65535 && maximum.definition->periodCents==65536,"largest supported degree count stays bounded and keeps the final interval outside stored degrees");
}
void nativeNoteHandoff() {
    std::string source="24 equal\n24\n";for(int degree=1;degree<=24;++degree)source+=std::to_string(degree*50)+".0\n";
    const auto imported=parsed(source);const auto rast=context(imported,{0,4,7,10,14,18,21});const auto rows=rast.grid(6000,7199,6000,true);
    check(imported.definition && imported.definition->degrees.size()==24 && rows.rows.size()==7 && rows.rows[2].pitch.cents==6350 && rows.rows[2].midi && rows.rows[2].midi->key==64 && rows.rows[2].midi->tuningCents==-50,"Scala 24-EDO supplies the actual Rast Fold pitch and an exact native E-minus-fifty-cent representation");
    test::TempDirectory temp{"scala","native"};StoreError error{};auto store=Store::createEmpty(temp.path()/"a.adi",error);if(!store)throw std::runtime_error("store");
    auto op=[](const char* name,Payload payload){OpRequest request;request.opType=name;request.payload=std::move(payload);return request;};
    const auto result=OpJournal(*store).commit(std::vector<OpRequest>{op("track.create",{{"id",1},{"kind","midi"},{"name","Scale"},{"index",0}}),op("clip.create",{{"id",10},{"track",1},{"kind","midi"},{"pos",0},{"length",5765760}}),
        op("note.insert",{{"clip",10},{"note",77},{"start",0},{"dur",5765760},{"key",rows.rows[2].midi->key},{"cents",rows.rows[2].midi->tuningCents}})});
    if(!result.ok)throw std::runtime_error(result.error);
    const auto details=readClipDetails(*store,10);check(details.clip && details.clip->notes.size()==1 && details.clip->notes[0].key==64 && details.clip->notes[0].tuningCents==-50,"imported ruler pitch reaches the real native ANOT path without implicit note retuning");
    History history(*store);check(history.undo().ok && !readClipDetails(*store,10).clip && history.redo().ok && readClipDetails(*store,10).clip->notes[0].tuningCents==-50,"Scala-derived native note insertion retains the existing one-action undo/redo path");
}
void callbackRefusal() {
    const std::string source="D\n1\n2/1\n";bool refused=false;
    {audio::CallbackScope callback;const auto result=parseScala(source,4,"Name");refused=!result.definition && result.error=="callback";}
    check(refused && callbackAllocations==0 && audio::callbackFileIo==0,"owner parsing refuses callbacks before copying, allocation or file I/O");
}
}
int main(){try{examples();domainsAndBudgets();nativeNoteHandoff();callbackRefusal();}catch(const std::exception& exception){std::printf("EXCEPTION: %s\n",exception.what());check(false,"fixtures complete");}std::printf("scala_import: %d checks, %d failures\n",checks,failures);return failures?1:0;}
