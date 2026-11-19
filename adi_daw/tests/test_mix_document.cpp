// SPDX-License-Identifier: AGPL-3.0-or-later
#include "adi/ui/project_document.hpp"
#include "juce/builtin_host.hpp"
#include "juce/mix_function_host.hpp"
#include "temp_directory.hpp"
#include <SQLiteCpp/SQLiteCpp.h>
#include <algorithm>
#include <array>
#include <cstdio>
#include <optional>

using namespace adi;
namespace {
int checks=0,failures=0;
void check(bool ok,const char* label){++checks;if(!ok){++failures;std::printf("FAIL %s\n",label);}}
void observeLimiterReduction() {
    test::TempDirectory temp("mix_document", "limiter-reduction");
    std::string error;
    auto doc = ui::ProjectDocument::open(temp.path() / "project.adi", true, device::withBuiltinDevices(), error);
    if (!doc || !doc->addMixFunction(mix::Function::Limiter, 2)) {
        check(false, "insert a real limiter for reduction observation");
        return;
    }
    const auto limiter = [&]() -> std::optional<ui::DevicePanelData> {
        doc->tick(0);
        const auto publication = doc->parameterFeed().current();
        const auto found = std::find_if(publication->begin(), publication->end(),
            [](const auto& row) { return row.uid == "org.adidaw.mix.limiter"; });
        if (found != publication->end()) return *found;
        return std::nullopt;
    };
    const auto old = limiter();
    check(old && old->available, "reduction source device is available");
    if (!old || !old->available) return;
    auto* instance = doc->session().instanceFor(old->id);
    const auto source = instance ? device::limiterReductionSource(*instance) : nullptr;
    check(source && !source->observed(), "new native limiter has no telemetry demand");
    if (!source) return;
    const auto render = [](device::DeviceInstance& target, std::uint64_t block, int offset, int frames,
                           std::uint64_t revision = 1, bool exporting = false) {
        std::array<float, 256> left{}, right{};
        left.fill(1.5f); right.fill(-1.5f);
        std::array<const float*, 2> input{left.data(), right.data()};
        std::array<float*, 2> output{left.data(), right.data()};
        engine::TransportInfo clock;
        clock.playing = true;
        clock.timelineSample = 48000;
        clock.transportRevision = revision;
        engine::NodeIo io;
        io.in = input.data(); io.out = output.data();
        io.channels = 2;
        io.blockOffset = offset;
        io.frames = frames;
        io.transport = &clock;
        io.renderBlock = block;
        io.configuredLatency = target.latencySamples();
        io.exporting = exporting;
        target.process(io);
    };
    for (std::uint64_t block = 1; block <= 8; ++block) render(*instance, block, 0, 256);
    check(!source->observed(), "ordinary audio keeps the telemetry gate inactive before any viewer");

    auto first = doc->observeLimiterReduction(old->id, old->runtime);
    const auto epoch = source->observationEpoch();
    auto second = doc->observeLimiterReduction(old->id, old->runtime);
    check(first && second && epoch != 0 && epoch == source->observationEpoch() &&
          !first.read() && !second.read(), "two observers share a fresh capture without inherited audio");
    render(*instance, 9, 0, 96);
    const auto early = first.read();
    render(*instance, 9, 96, 160);
    const auto a = first.read(), b = second.read();
    check(early && a && b && early->observedFrames == 96 &&
          a->renderBlock == 9 && a->observedFrames == 256 && a->firstTimelineSample == 48000 &&
          a->advancing && a->transportRevision == 1 && a->reduction.available &&
          a->reduction.peakLoss > 0 && a->reduction.peakLoss >= early->reduction.peakLoss &&
          a->reduction.peakLoss == b->reduction.peakLoss && b->observedFrames == a->observedFrames,
          "both readers get the same actual peak accumulated across split callback segments");
    render(*instance, 9, 160, 96, 2);
    const auto discontinuity = second.read();
    check(discontinuity && discontinuity->observedFrames == 96 &&
          discontinuity->transportRevision == 2 && discontinuity->firstTimelineSample == 48160,
          "a same-callback transport discontinuity starts a fresh segment group");
    first.reset();
    render(*instance, 10, 0, 256);
    check(source->observationEpoch() == epoch && second.read() && second.read()->renderBlock == 10,
          "one hidden view leaves the remaining subscription active");
    second.reset();
    render(*instance, 11, 0, 256);
    check(!source->observed(), "the last hidden view turns off capture immediately");

    auto held = doc->observeLimiterReduction(old->id, old->runtime);
    check(held && source->observationEpoch() != epoch && !held.read(),
          "reopening after the last hide starts empty");
    if (a) source->publishSegment(epoch, *a);
    render(*instance, 12, 0, 256, 1, true);
    check(!held.read(), "in-flight old epochs and export callbacks cannot repopulate the live mailbox");
    render(*instance, 13, 0, 256);
    check(held.read() && held.read()->renderBlock == 13, "live source resumes after export without a stale reading");
    check(doc->deviceAction("device.setEnabled", {{"id", old->id}, {"enabled", false}}) &&
          !held && !held.read() && !source->observed() &&
          !doc->observeLimiterReduction(old->id, old->runtime),
          "graph bypass revokes held demand and refuses a fresh reader");
    check(doc->deviceAction("device.setEnabled", {{"id", old->id}, {"enabled", true}}),
          "graph bypass can be removed through the normal device route");
    const auto enabled = limiter();
    if (!enabled) return;
    held = doc->observeLimiterReduction(enabled->id, enabled->runtime);
    check(held && !held.read(), "enabling the graph device opens a fresh telemetry epoch");

    check(doc->deviceAction("device.remove", {{"id", old->id}}), "remove the observed limiter");
    check(!held && !held.read() && !source->observed() &&
          !doc->observeLimiterReduction(old->id, enabled->runtime),
          "retirement immediately revokes the held lease and refuses new readers");

    // Undo can restore the same device ID and retained runtime. A previously
    // revoked observer must still stay outside its new capture interval.
    check(doc->ops().undo().ok && doc->synchronise(), "Undo restores the retired limiter");
    const auto restored = limiter();
    auto* restoredInstance = doc->session().instanceFor(old->id);
    check(restored && restored->id == old->id && restoredInstance == instance,
          "Undo restores the original ID and runtime");
    if (!restored || !restoredInstance) return;
    auto revived = doc->observeLimiterReduction(restored->id, restored->runtime);
    check(revived && !revived.read() && !held && !held.read(),
          "the restored runtime admits only a fresh lease");
    render(*restoredInstance, 14, 0, 256);
    check(revived.read() && revived.read()->renderBlock == 14 && !held.read(),
          "only the fresh lease sees audio from the restored runtime");
    held.reset();
    check(source->observed() && revived.read(), "closing the stale lease leaves the restored runtime's fresh reader active");
    check(doc->ops().redo().ok && doc->synchronise() && !revived && !revived.read() && !source->observed(),
          "Redo retires the restored runtime and revokes its fresh reader");

    check(doc->addMixFunction(mix::Function::Compressor, 3), "add a genuinely different native function");
    doc->tick(0);
    const auto publication = doc->parameterFeed().current();
    const auto foreign = std::find_if(publication->begin(), publication->end(),
        [](const auto& row) { return row.uid == "org.adidaw.mix.compressor"; });
    check(foreign != publication->end() && foreign->id > old->id &&
          !foreign->runtime.sameInstance(old->runtime) && !revived && !revived.read() && !source->observed(),
          "a newly authored function uses an ID beyond the retained limiter");
    if (foreign == publication->end()) return;
    check(!doc->observeLimiterReduction(foreign->id, old->runtime) &&
          !doc->observeLimiterReduction(foreign->id, foreign->runtime),
          "the new device rejects both the retired token and non-limiter telemetry");
    check(doc->addMixFunction(mix::Function::Limiter, 3), "another native limiter can be observed independently");
    const auto replacement = limiter();
    check(replacement && !replacement->runtime.sameInstance(old->runtime),
          "the second limiter has a distinct runtime identity");
    if (!replacement) return;
    auto fresh = doc->observeLimiterReduction(replacement->id, replacement->runtime);
    revived.reset();
    auto* next = doc->session().instanceFor(replacement->id);
    check(fresh && !fresh.read() && next && next != instance,
          "the replacement owns an independent fresh subscription");
    if (!next) return;
    render(*next, 15, 0, 256);
    check(fresh.read() && fresh.read()->renderBlock == 15,
          "releasing a held stale lease leaves the replacement reading active");

    const auto secondSource = device::limiterReductionSource(*next);
    next->release();
    check(!fresh && !fresh.read() && secondSource && !secondSource->observed(),
          "driver release invalidates readers of the same retained runtime");
    next->prepare(48000, 256);
    auto rearmed = doc->observeLimiterReduction(replacement->id, replacement->runtime);
    fresh.reset();
    render(*next, 16, 0, 256);
    check(rearmed && rearmed.read() && rearmed.read()->renderBlock == 16,
          "reprepare requires a fresh lease and stale release cannot alter it");
    doc.reset();
    check(!rearmed && !rearmed.read() && secondSource && !secondSource->observed(),
          "closing the document revokes even an externally held view lease");
}


void replaceHighestFunction() {
    test::TempDirectory temp("mix_document", "removed-native-id");
    std::string error;
    auto doc = ui::ProjectDocument::open(temp.path() / "project.adi", true, device::withBuiltinDevices(), error);
    if (!doc || !doc->addMixFunction(mix::Function::Eq, 2)) {
        check(false, "add original EQ through the native function edit");
        return;
    }
    const auto find = [&](std::string_view uid) -> std::optional<ui::DevicePanelData> {
        const auto publication = doc->parameterFeed().current();
        for (const auto& device : *publication)
            if (device.uid == uid) return device;
        return {};
    };
    const auto eq = find("org.adidaw.mix.eq");
    check(eq && eq->available, "highest authored EQ is available in the shared device feed");
    if (!eq) return;
    auto* oldInstance = doc->session().instanceFor(eq->id);
    check(oldInstance && doc->deviceAction("device.remove", {{"id", eq->id}}) &&
              doc->addMixFunction(mix::Function::Limiter, 3), "remove the highest device and add the limiter on another track");
    const auto limiter = find("org.adidaw.mix.limiter");
    check(limiter && limiter->available && limiter->track == 3 && limiter->id > eq->id,
          "new authored device ID advances past the retired EQ");
    if (!limiter) return;
    doc->setParameterDemand(doc.get(), {limiter->id});
    auto* newInstance = doc->session().instanceFor(limiter->id);
    const auto panel = find("org.adidaw.mix.limiter");
    if (!panel) {
        check(false, "limiter panel remains available after requesting control metadata");
        return;
    }
    const auto group = panel->groups().find("0");
    check(newInstance && newInstance != oldInstance && newInstance->identity().uid == panel->uid &&
              group != panel->groups().end() && group->second.groupId == "limiter" && !panel->parameters().empty() &&
              std::none_of(panel->parameters().begin(), panel->parameters().end(), [](const auto& control) { return control.missing; }),
          "Device View and STACK share the new limiter's actual controls");
    check(!doc->deviceTargetCurrent(eq->id, eq->runtime, eq->instance),
          "removed EQ target cannot edit the new limiter");
    const bool undoAdd = doc->ops().undo().ok && doc->synchronise();
    const bool undoRemove = doc->ops().undo().ok && doc->synchronise();
    doc->tick(1);
    check(undoAdd && undoRemove && doc->session().instanceFor(eq->id) == oldInstance && find(eq->uid).has_value(),
          "Undo restores the original EQ runtime");
    const bool redoRemove = doc->ops().redo().ok && doc->synchronise();
    const bool redoAdd = doc->ops().redo().ok && doc->synchronise();
    doc->tick(2);
    check(redoRemove && redoAdd && doc->session().instanceFor(limiter->id) == newInstance && find(limiter->uid).has_value(),
          "Redo restores the same independently authored limiter runtime");
}
}
int main() {
    replaceHighestFunction();
    observeLimiterReduction();
    test::TempDirectory temp("mix_document","native-functions");std::string error;
    auto doc=ui::ProjectDocument::open(temp.path()/"project.adi",true,device::withBuiltinDevices(),error);
    check(doc!=nullptr,"native function document opens");if(!doc)return 2;
    auto count=[&] {return doc->store().db().execAndGet(
        "SELECT count(*) FROM devices d JOIN plugin_refs p ON p.id=d.plugin_ref_id WHERE p.uid LIKE 'org.adidaw.mix.%'").getInt();};
    check(count()==0,"fresh project has no reserved or silently enabled functions");
    check(!doc->addMixFunction(mix::Function::Deesser,2)&&count()==0,"unported core has an honest refusal");
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
