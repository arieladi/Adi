// SPDX-License-Identifier: AGPL-3.0-or-later
#include "adi/ui/project_document.hpp"
#include "juce/builtin_host.hpp"
#include "temp_directory.hpp"
#include <SQLiteCpp/SQLiteCpp.h>
#include <algorithm>
#include <cstdio>
#include <optional>

using namespace adi;
namespace {
int checks=0,failures=0;
void check(bool ok,const char* label){++checks;if(!ok){++failures;std::printf("FAIL %s\n",label);}}
}
int main() {
    test::TempDirectory temp("mix_document","native-functions");std::string error;
    auto doc=ui::ProjectDocument::open(temp.path()/"project.adi",true,device::withBuiltinDevices(),error);
    check(doc!=nullptr,"native function document opens");if(!doc)return 2;
    auto count=[&] {return doc->store().db().execAndGet(
        "SELECT count(*) FROM devices d JOIN plugin_refs p ON p.id=d.plugin_ref_id WHERE p.uid LIKE 'org.adidaw.mix.%'").getInt();};
    check(count()==0,"fresh project has no reserved or silently enabled functions");
    check(!doc->addMixFunction(mix::Function::Compressor,2)&&count()==0,"unported core has an honest refusal");
    check(doc->addMixFunction(mix::Function::Limiter,2)&&count()==1,"opt-in real native limiter adds in one batch");
    doc->tick(0);
    const auto publication=doc->parameterFeed().current();
    auto found=std::find_if(publication->begin(),publication->end(),[](const auto& d){return d.uid=="org.adidaw.mix.limiter";});
    check(found!=publication->end()&&found->available&&!found->plugin,"native function published without external plugin disguise");
    if(found==publication->end())return 2;
    const auto shown=*found;auto* instance=doc->session().instanceFor(shown.id);
    check(instance&&instance->paramCount()==21&&shown.latency==instance->latencySamples(),"real controls and actual latency use common feed");
    check(!doc->addMixFunction(mix::Function::Limiter,2)&&count()==1,"duplicate + action refused");
    using K=engine::ParamEventKind;
    check(doc->parameterGestureExpected(shown.id,"0",K::Begin,0,shown.runtime,shown.instance)&&
          doc->parameterGestureExpected(shown.id,"0",K::Value,.2,shown.runtime,shown.instance)&&
          doc->parameterGestureExpected(shown.id,"0",K::End,.2,shown.runtime,shown.instance),"native parameter uses exact-instance common gesture");
    check(instance->getParam("0").real==6,"accepted value reaches reused core controls");
    const auto storedInput=[&]() -> std::optional<double> {
        SQLite::Statement row(doc->store().db(),"SELECT normalized_value FROM plugin_params WHERE device_id=? AND param_id='0'");
        row.bind(1,shown.id);return row.executeStep()?std::optional<double>{row.getColumn(0).getDouble()}:std::nullopt;
    };
    const auto undone=doc->ops().undo(); const bool undoSync=doc->synchronise();
    check(undone.ok&&undoSync&&!storedInput(),"undo immediately restores the original parameter-row presence");
    // Applied receipts deliberately queue until the document's normal drain. The graph/SQL
    // refresh is not a second setter authority for devices that were already loaded.
    doc->tick(1);
    const auto undoValue=doc->session().instanceFor(shown.id)->getParam("0").real;
    if(!undone.ok||!undoSync||undoValue!=0)std::printf("UNDO diagnostic: ok=%d sync=%d real=%.17g history=%s document=%s applied=%lld state_applied=%lld state_equal=%lld state_refused=%lld\n",
        undone.ok,undoSync,undoValue,undone.error.c_str(),doc->error().c_str(),
        static_cast<long long>(doc->parameterStats().applied),static_cast<long long>(doc->parameterStats().statesApplied),
        static_cast<long long>(doc->parameterStats().statesAppliedEqual),static_cast<long long>(doc->parameterStats().statesRefused));
    check(undone.ok&&undoSync&&undoValue==0,"one undo restores gesture start");
    const auto redone=doc->ops().redo();const bool redoSync=doc->synchronise();
    check(redone.ok&&redoSync&&storedInput()&&*storedInput()==.2,"redo immediately restores the committed normalized row");
    doc->tick(2);
    check(redone.ok&&redoSync&&doc->session().instanceFor(shown.id)->getParam("0").real==6,"redo restores full gesture");
    check(doc->parameterGestureExpected(shown.id,"73",K::Begin,0,shown.runtime,shown.instance)&&
          doc->parameterGestureExpected(shown.id,"73",K::Value,1,shown.runtime,shown.instance)&&
          doc->parameterGestureExpected(shown.id,"73",K::End,1,shown.runtime,shown.instance),
          "nonautomatable export policy uses the exact-instance common configuration gesture");
    check(instance->getParam("73").real==1,"configuration reaches native runtime exactly");
    const auto exportUndone=doc->ops().undo();const bool exportUndoSync=doc->synchronise();doc->tick(3);
    check(exportUndone.ok&&exportUndoSync&&doc->session().instanceFor(shown.id)->getParam("73").real==0,
          "one undo restores export configuration default through normal receipt drain");
    const auto exportRedone=doc->ops().redo();const bool exportRedoSync=doc->synchronise();doc->tick(4);
    check(exportRedone.ok&&exportRedoSync&&doc->session().instanceFor(shown.id)->getParam("73").real==1,
          "redo restores export configuration without an automation override");
    DeviceCopy copy;check(doc->copyDevice(shown.id,copy),"native function copy retains normal stored state");
    check(!doc->pasteDevice(copy,2,{})&&count()==1,"clipboard cannot evade one-function-per-channel rule");
    const bool pasted=doc->pasteDevice(copy,3,{});
    if(!pasted)std::printf("PASTE diagnostic: %s\n",doc->error().c_str());
    check(pasted&&count()==2,"copy to another channel creates independent function");
    check(doc->deviceAction("device.remove",{{"id",shown.id}})&&count()==1,"native off removes authored function");
    check(doc->ops().undo().ok&&doc->synchronise()&&count()==2,"undo removal restores function and state");
    check(!doc->parameterGestureExpected(shown.id,"0",K::Value,.9,shown.runtime,shown.instance),"old retained editor cannot address restored runtime");
    doc->tick(1);
    doc->views().save("analyzer",[&]{ui::WindowState s;s.mixViewTab=5;return s;}(),error);
    check(doc->views().load("analyzer").mixViewTab==5,"new analyzer tab persists without remapping old tabs");
    doc.reset();
    auto reopened=ui::ProjectDocument::open(temp.path()/"project.adi",false,device::withBuiltinDevices(),error);
    check(reopened!=nullptr,"configured native project reopens");
    if(reopened) {
        reopened->tick(0);auto* restored=reopened->session().instanceFor(shown.id);
        check(restored&&restored->getParam("73").real==1,"export configuration survives project close and reopen");
    }
    std::printf("mix document: %d checks, %d failures\n",checks,failures);return failures?1:0;
}
